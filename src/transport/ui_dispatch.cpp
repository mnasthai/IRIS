#include "monitor/transport/ui_dispatch.hpp"
#include <atomic>
#include <limits>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "user32.lib")

namespace wechatbot::monitor {
namespace {

/**
 * @brief Win32 SRWLOCK (Slim Reader/Writer Lock) 的独占锁 RAII 封装
 * @details 相比于 std::mutex，SRWLock 是轻量级无额外堆分配的 Win32 原生锁，无跨 DLL ABI 兼容隐患
 */
struct Lock {
    SRWLOCK& value;
    explicit Lock(SRWLOCK& lock) noexcept : value(lock) { AcquireSRWLockExclusive(&value); }
    ~Lock() { ReleaseSRWLockExclusive(&value); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

/**
 * @brief Win32 内核句柄 (HANDLE) 的 RAII 自动关闭封装
 */
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) noexcept : value(handle) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Release() noexcept { return std::exchange(value, nullptr); }
};

/**
 * @brief Win32 GetLastError() 保护器
 * @details 记录当前线程的 LastError，在作用域退出时恢复，确保调度器的内部操作不污染外部调用者的错误码
 */
struct PreserveError {
    DWORD value = GetLastError();
    ~PreserveError() { SetLastError(value); }
};

/// 任务生命周期状态机
enum class Phase {
    pending,   ///< 已入队，等待 UI 线程消息泵取出
    running,   ///< 已由 UI 线程取出并正在执行
    done,      ///< 已执行完毕（包括成功或抛出异常）
    cancelled  ///< 在 UI 线程取出前已超时或被显式取消
};

/**
 * @brief 单次 UI 调度任务控制块
 */
struct Task {
    const UINT_PTR token;                   ///< 唯一消息令牌标识符
    std::function<void()> action;           ///< 需在 UI 线程执行的目标闭包
    Handle finished{CreateEventW(nullptr, TRUE, FALSE, nullptr)}; ///< 任务完成/取消通知事件 (Manual Reset)
    std::atomic<Phase> phase{Phase::pending};///< 原子状态流转
    DWORD executionThreadId = 0;             ///< 实际执行任务的线程 ID (进入 running 阶段时发布)
    bool threw = false;                      ///< 任务执行过程中是否捕获到 C++ 异常
    Task(UINT_PTR id, std::function<void()> work) : token(id), action(std::move(work)) {}
};

/**
 * @brief 映射已结束任务的状态码
 */
UiDispatchResult FinishedResult(const std::shared_ptr<Task>& task) noexcept {
    const auto phase = task->phase.load(std::memory_order_acquire);
    if (phase == Phase::done) {
        return {task->threw ? UiDispatchStatus::exception : UiDispatchStatus::completed,
                task->executionThreadId,
                static_cast<DWORD>(task->threw ? ERROR_UNHANDLED_EXCEPTION : ERROR_SUCCESS)};
    }
    if (phase == Phase::cancelled)
        return {UiDispatchStatus::cancelled_before_start, 0, ERROR_CANCELLED};
    return {UiDispatchStatus::unavailable, 0, ERROR_GEN_FAILURE};
}
} // namespace

/**
 * @brief UiDispatcher 内部实现类 (Impl)
 * @details 采用 PIMPL 模式与 std::enable_shared_from_this，保证即便外层 Facade 析构，
 *          仍在 Windows 钩子回调中的未决操作仍可安全引用底层状态。
 */
struct UiDispatcher::Impl : std::enable_shared_from_this<UiDispatcher::Impl> {
    struct Registry {
        SRWLOCK lock = SRWLOCK_INIT;
        std::unordered_map<DWORD, std::shared_ptr<Impl>> bindings; ///< 线程 ID 到 Impl 的全局映射表
        std::atomic<UINT_PTR> nextToken{1};                        ///< 全局单调递增的令牌分配器
    };

    SRWLOCK lock = SRWLOCK_INIT;     ///< 保护当前 Impl 实例状态的锁
    DWORD threadId = 0;              ///< 当前绑定的目标 UI 线程 ID
    HANDLE thread = nullptr;         ///< 目标 UI 线程内核句柄（用于存活检测）
    HHOOK hook = nullptr;            ///< 安装在目标线程上的 WH_GETMESSAGE 钩子句柄
    UINT message = 0;                ///< 向 Windows 注册的自定义窗口消息 ID
    bool closing = false;            ///< 是否处于解绑/清理流程中
    std::shared_ptr<Task> active;    ///< 当前正在调度或执行的单一任务

    ~Impl() { if (thread) CloseHandle(thread); }

    /**
     * @brief 全局单例注册表
     * @details 采用堆分配指针且不主动释放，确保在进程退出期间 Windows 钩子卸载过程中始终有效
     */
    static Registry& Registrations() {
        static auto* registry = new Registry;
        return *registry;
    }

    /**
     * @brief 分配下一个全局唯一的任务令牌 token
     */
    static UINT_PTR NextToken() noexcept {
        auto& next = Registrations().nextToken;
        auto token = next.load(std::memory_order_relaxed);
        while (token != (std::numeric_limits<UINT_PTR>::max)()) {
            if (next.compare_exchange_weak(token, token + 1, std::memory_order_relaxed))
                return token;
        }
        return 0; // 达到最大值时禁止回绕，防止陈旧消息识别错乱
    }

    /**
     * @brief 绑定并挂钩目标 UI 线程
     * @param target 目标线程 ID
     * @param[out] error 失败时的 Win32 错误码
     * @return true 成功挂载 WH_GETMESSAGE 钩子；false 失败
     */
    bool Bind(DWORD target, DWORD& error) {
        if (!target) { error = ERROR_INVALID_PARAMETER; return false; }
        Handle candidate(OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, target));
        if (!candidate.value) { error = GetLastError(); return false; }
        const auto processId = GetProcessIdOfThread(candidate.value);
        if (!processId) { error = GetLastError(); return false; }
        if (processId != GetCurrentProcessId()) { error = ERROR_ACCESS_DENIED; return false; }
        // 确保目标线程仍然存活（未收到终止信号）
        if (WaitForSingleObject(candidate.value, 0) != WAIT_TIMEOUT) {
            error = ERROR_INVALID_THREAD_ID; return false;
        }
        // 发送无害的 WM_NULL 消息检测目标线程是否拥有消息队列（若无消息队列将失败）
        if (!PostThreadMessageW(target, WM_NULL, 0, 0)) { error = GetLastError(); return false; }
        // 注册全局唯一的自定义窗口消息
        const UINT registered = RegisterWindowMessageW(
            L"wechatbot.UiDispatcher.4AF77C01-5F29-4C2B-8EA5-24170355090D");
        if (!registered) { error = GetLastError(); return false; }
        
        auto& registry = Registrations();
        Lock registryGuard(registry.lock);
        Lock stateGuard(lock);
        if (hook) {
            if (!closing && threadId == target && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT)
                return true;
            error = ERROR_BUSY; return false;
        }
        if (registry.bindings.contains(target)) { error = ERROR_BUSY; return false; }
        
        // 先在全局表中占位
        registry.bindings.emplace(target, shared_from_this());
        // 安装目标线程专属的 WH_GETMESSAGE 消息钩子
        HHOOK installed = SetWindowsHookExW(WH_GETMESSAGE, Hook, nullptr, target);
        if (!installed) {
            error = GetLastError();
            registry.bindings.erase(target);
            return false;
        }
        threadId = target;
        thread = candidate.Release();
        message = registered;
        hook = installed;
        closing = false;
        return true;
    }

    /**
     * @brief 解绑并卸载 WH_GETMESSAGE 钩子
     * @param requireIdle 若为 true，在任务处于 running 阶段时拒绝解绑并返回 ERROR_BUSY
     * @param[out] error 失败时的 Win32 错误码
     */
    bool Detach(bool requireIdle, DWORD& error) noexcept {
        auto& registry = Registrations();
        HHOOK installed = nullptr;
        {
            Lock registryGuard(registry.lock);
            Lock stateGuard(lock);
            if (!hook) return true;
            if (closing) { error = ERROR_BUSY; return false; }
            if (active) {
                auto phase = active->phase.load(std::memory_order_acquire);
                if (phase == Phase::running && requireIdle) { error = ERROR_BUSY; return false; }
                if (phase == Phase::pending) {
                    if (active->phase.compare_exchange_strong(phase, Phase::cancelled,
                            std::memory_order_acq_rel)) {
                        SetEvent(active->finished.value);
                    } else if (phase == Phase::running && requireIdle) {
                        error = ERROR_BUSY; return false;
                    }
                }
            }
            closing = true;
            installed = hook;
        }
        // 关键安全原则：在调用 UnhookWindowsHookEx 时绝不持有自身互斥锁，防止 Windows 系统回调产生死锁
        const bool removed = UnhookWindowsHookEx(installed) != FALSE;
        const DWORD removalError = removed ? ERROR_SUCCESS : GetLastError();
        {
            Lock registryGuard(registry.lock);
            Lock stateGuard(lock);
            if (!removed) {
                closing = false;
                error = removalError;
                return false;
            }
            registry.bindings.erase(threadId);
            hook = nullptr;
            message = 0;
            threadId = 0;
            CloseHandle(thread);
            thread = nullptr;
            active.reset();
            closing = false;
        }
        return true;
    }

    /**
     * @brief 实际在目标 UI 线程上下文中执行任务闭包
     */
    void Run(const std::shared_ptr<Task>& task, DWORD incomingError) noexcept {
        {
            Lock stateGuard(lock);
            if (closing || active != task || GetCurrentThreadId() != threadId) return;
            if (task->phase.load(std::memory_order_acquire) != Phase::pending) return;
            // 记录实际执行的线程 ID
            task->executionThreadId = GetCurrentThreadId();
            auto expected = Phase::pending;
            // 原子 CAS 转为 running 态，若已被取消则放弃执行
            if (!task->phase.compare_exchange_strong(expected, Phase::running,
                    std::memory_order_acq_rel)) return;
        }
        bool threw = false;
        {
            auto action = std::move(task->action);
            SetLastError(incomingError);
            try { action(); }
            catch (...) { threw = true; } // 吞掉并标记异常，严禁 C++ 异常逃逸至 Windows 系统栈
        }
        {
            Lock stateGuard(lock);
            task->threw = threw;
            task->phase.store(Phase::done, std::memory_order_release);
            if (active == task) active.reset();
        }
        // 唤醒在工作线程中同步等待的 WaitForSingleObject
        SetEvent(task->finished.value);
    }

    /**
     * @brief 消息匹配与派发入口
     */
    void Message(const MSG& msg, DWORD incomingError) noexcept {
        std::shared_ptr<Task> task;
        {
            Lock stateGuard(lock);
            // 校验消息窗口为空（线程消息）、消息 ID 匹配、以及 wParam 包含对应的令牌 token
            if (closing || !hook || msg.hwnd || msg.message != message || msg.lParam != 0 ||
                !active || msg.wParam != active->token) return;
            task = active;
        }
        Run(task, incomingError);
    }

    /**
     * @brief WH_GETMESSAGE Windows 钩子静态回调函数
     * @details 当微信 Qt 消息泵（如 GetMessageW/PeekMessageW）检索到消息时触发此函数：
     *          - 仅当 code >= 0 且 removed == PM_REMOVE（消息已被从队列取出）时处理；
     *          - 根据当前线程 ID 从 Registrations 映射表中查找对应的 Impl 实例；
     *          - 调用 Message 进行任务触发；
     *          - 无论发生何种情况，必须将控制权传递给 CallNextHookEx，确保不打乱微信自身的事件传递。
     */
    static LRESULT CALLBACK Hook(int code, WPARAM removed, LPARAM rawMessage) noexcept {
        const DWORD incomingError = GetLastError();
        try {
            if (code >= 0 && removed == PM_REMOVE && rawMessage) {
                std::shared_ptr<Impl> binding;
                auto& registry = Registrations();
                {
                    Lock registryGuard(registry.lock);
                    const auto found = registry.bindings.find(GetCurrentThreadId());
                    if (found != registry.bindings.end()) binding = found->second;
                }
                if (binding) binding->Message(*reinterpret_cast<const MSG*>(rawMessage), incomingError);
            }
        } catch (...) {
            // 严禁异常逃逸
        }
        SetLastError(incomingError);
        try { return CallNextHookEx(nullptr, code, removed, rawMessage); }
        catch (...) { SetLastError(incomingError); return 0; }
    }

    /**
     * @brief 派发任务并在调用线程同步等待执行完成或超时
     */
    UiDispatchResult Execute(std::function<void()> action, DWORD timeout, DWORD incomingError) {
        if (!action) return {UiDispatchStatus::unavailable, 0, ERROR_INVALID_PARAMETER};
        std::shared_ptr<Task> task;
        bool inlineCall = false;
        {
            Lock stateGuard(lock);
            if (!hook || closing || WaitForSingleObject(thread, 0) != WAIT_TIMEOUT)
                return {UiDispatchStatus::unavailable, 0, ERROR_INVALID_THREAD_ID};
            // 单任务派发模型：若已有前序任务在 pending 或 running 中，拒绝并发
            if (active) {
                const auto phase = active->phase.load(std::memory_order_acquire);
                if (phase == Phase::pending || phase == Phase::running)
                    return {UiDispatchStatus::busy, 0, ERROR_BUSY};
                active.reset();
            }
            const auto token = NextToken();
            if (!token) return {UiDispatchStatus::unavailable, 0, ERROR_ARITHMETIC_OVERFLOW};
            task = std::make_shared<Task>(token, std::move(action));
            if (!task->finished.value)
                return {UiDispatchStatus::unavailable, 0, GetLastError()};
            active = task;
            
            // 若当前调用线程本身就是目标 UI 线程，则直接内联就地执行，避免消息队列死锁
            inlineCall = GetCurrentThreadId() == threadId;
            if (!inlineCall && !PostThreadMessageW(threadId, message, token, 0)) {
                const DWORD error = GetLastError();
                task->phase.store(Phase::cancelled, std::memory_order_release);
                active.reset();
                return {UiDispatchStatus::unavailable, 0, error};
            }
        }
        
        // 内联调用分支
        if (inlineCall) {
            Run(task, incomingError);
            return FinishedResult(task);
        }
        
        // 跨线程等待分支：阻塞等待 finished 事件通知
        const DWORD wait = WaitForSingleObject(task->finished.value, timeout);
        if (wait == WAIT_OBJECT_0) return FinishedResult(task);
        
        // 超时分支处理：
        const DWORD error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        auto expected = Phase::pending;
        // 尝试 CAS 将状态从 pending 转为 cancelled（抢在 UI 线程取出执行之前取消）
        if (task->phase.compare_exchange_strong(expected, Phase::cancelled, std::memory_order_acq_rel)) {
            {
                Lock stateGuard(lock);
                if (active == task) active.reset();
            }
            SetEvent(task->finished.value);
            return {UiDispatchStatus::cancelled_before_start, 0, error};
        }
        
        // 若状态已被 UI 线程推进为 running，说明任务已开始执行但等待超时
        // 此时必须返回 timeout_after_start，提示调用端“副作用可能已发生，严禁自动重试”！
        if (expected == Phase::running)
            return {UiDispatchStatus::timeout_after_start, task->executionThreadId, error};
        
        return FinishedResult(task); // 完成/取消与超时的竞态赢家已发布结果
    }
};

UiDispatcher::UiDispatcher() : impl_(std::make_shared<Impl>()) { Impl::Registrations(); }

UiDispatcher::~UiDispatcher() {
    PreserveError preserve;
    DWORD ignored = ERROR_SUCCESS;
    // 析构防御性解绑钩子
    impl_->Detach(false, ignored);
}

bool UiDispatcher::Bind(DWORD threadId) noexcept {
    const DWORD previous = GetLastError();
    DWORD error = ERROR_SUCCESS;
    bool bound = false;
    try { bound = impl_->Bind(threadId, error); }
    catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    SetLastError(bound ? previous : error);
    return bound;
}

UiDispatchResult UiDispatcher::Execute(std::function<void()> action, DWORD timeoutMs) noexcept {
    PreserveError preserve;
    try { return impl_->Execute(std::move(action), timeoutMs, preserve.value); }
    catch (...) { return {UiDispatchStatus::unavailable, 0, ERROR_NOT_ENOUGH_MEMORY}; }
}

bool UiDispatcher::Unbind() noexcept {
    const DWORD previous = GetLastError();
    DWORD error = ERROR_SUCCESS;
    const bool detached = impl_->Detach(true, error);
    SetLastError(detached ? previous : error);
    return detached;
}

DWORD UiDispatcher::BoundThreadId() const noexcept {
    PreserveError preserve;
    Lock stateGuard(impl_->lock);
    return impl_->closing ? 0 : impl_->threadId;
}

} // namespace wechatbot::monitor
