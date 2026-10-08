#include "monitor/send/send_service.hpp"
#include "monitor/send/native_send_result.hpp"
#include <chrono>
#include <utility>

namespace wechatbot::monitor {

/**
 * @brief 单次发送执行上下文 (Attempt)
 * @details 包含发送指令输入、绑定的 UI 线程 ID、已预备好的多媒体文件句柄，以及最终产出的发信结果。
 *          通过 std::shared_ptr 在工作线程与 UI 线程间传递所有权。
 */
struct SendService::Attempt {
    const SendCommand command;
    const DWORD thread;
    const std::shared_ptr<PreparedMediaFile> media;
    SendResult result;
    Attempt(const SendCommand& input, DWORD uiThread, std::shared_ptr<PreparedMediaFile> file)
        : command(input), thread(uiThread), media(std::move(file)) {}
};

/**
 * @brief 在途任务 RAII 租约 (ActionLease)
 * @details 当任务提交到 UI 调度器时创建，当闭包执行完毕并析构时自动触发 owner->ReleaseAction()。
 *          保证即使发信发生超时或异常，引用计数也能准确归零，不会造成 WaitIdle 永久阻塞。
 */
struct SendService::ActionLease {
    SendService* owner;
    explicit ActionLease(SendService* service) : owner(service) {}
    ~ActionLease() { owner->ReleaseAction(); }
};

SendService::SendService(SendPolicy policy, SendServicePorts ports)
    : policy_(std::move(policy)), ports_(std::move(ports)), gate_(policy_) {}

/**
 * @brief 登记一个在途操作并获取其 RAII 租约
 * @return 成功返回租约智能指针；若服务已处于关闭中 (closing_) 则返回空，拒绝接收新操作
 */
std::shared_ptr<SendService::ActionLease> SendService::RegisterAction() {
    std::lock_guard lock(actionsMutex_);
    if (closing_) return {};
    auto lease = std::make_shared<ActionLease>(this);
    ++outstandingActions_;
    return lease;
}

/**
 * @brief 释放一个在途操作计数
 * @details 当计数清零时通知所有正在等待 WaitIdle 的线程
 */
void SendService::ReleaseAction() noexcept {
    std::lock_guard lock(actionsMutex_);
    --outstandingActions_;
    if (!outstandingActions_) idle_.notify_all();
}

/**
 * @brief 停机等待屏障：阻止新任务进入，并等待所有在途操作完成
 * @param timeoutMs 等待超时毫秒数
 * @return true 全部在途任务在时限内排空；false 超时退出
 */
bool SendService::WaitIdle(DWORD timeoutMs) noexcept {
    try {
        std::unique_lock lock(actionsMutex_);
        closing_ = true; // 设置关闭标记，后续的 RegisterAction() 均会失败
        if (timeoutMs == INFINITE) {
            idle_.wait(lock, [this] { return !outstandingActions_; });
            return true;
        }
        return idle_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return !outstandingActions_; });
    } catch (...) { return false; }
}

/**
 * @brief 记录发送诊断日志
 */
void SendService::Log(const char* kind, const SendCommand& command, const SendResult& result,
                      const NativeAttemptResult* native) const {
    if (ports_.log) ports_.log(kind, command.requestId, command.attemptId, result, native);
}

/**
 * @brief 探测当前微信实例的发信能力与就绪状态
 * @details 
 *   1. 检查底层停止信号与微信原生接口签名是否有效；
 *   2. 查找并绑定微信 Qt UI 线程；
 *   3. 调度一个轻量只读任务到 UI 线程，通过 readSession 读取当前登录的微信 wxid 及执行器状态；
 *   4. 根据 SendGate 门禁可用性与策略配置，汇总返回当前可用的发信能力（纯文本、群艾特、引用回复、图片、语音）。
 */
NativeSendCapabilities SendService::Probe() {
    std::lock_guard lock(calls_);
    NativeSendCapabilities result;
    result.maxTextBytes = policy_.continuous ? 16384 : 1024;
    if (ports_.stopped()) return result;
    const auto thread = ports_.bindUi();
    if (!thread || !ports_.senderMatches()) return result;
    auto snapshot = std::make_shared<NativeSessionSnapshot>();
    auto lease = RegisterAction();
    if (!lease) return result;
    // 调度至 UI 线程读取会话快照（超时时限 1000ms）
    const auto dispatched = ports_.dispatch([this, thread, snapshot, lease] {
        (void)lease;
        *snapshot = ports_.readSession(thread);
    }, 1000);
    if (dispatched.status != UiDispatchStatus::completed) return result;
    result.accountId = snapshot->accountId;
    result.accountVerified = snapshot->accountReady;
    result.sendText = gate_.Available(ports_.now(), ports_.monotonicMs()) &&
        snapshot->accountReady && snapshot->executorReady && snapshot->accountId == policy_.accountId;
    result.sendGroupText = result.sendText && policy_.continuous;
    result.sendMention = result.sendGroupText;
    result.sendQuote = result.sendText && policy_.continuous && policy_.experimentalQuote;
    result.sendImage = result.sendText && policy_.continuous && policy_.mediaEnabled && ports_.mediaMatches();
    result.sendVoice = result.sendImage;
    return result;
}

/**
 * @brief 执行发送请求入口
 * @details 首先经过 SendGate 的频控、白名单和幂等性门禁；若准入，则在回调中触发 Dispatch
 */
SendResult SendService::Run(const SendCommand& command) {
    std::lock_guard lock(calls_);
    const auto result = gate_.Run(command, ports_.now(), ports_.monotonicMs(),
                                 [this, &command] { return Dispatch(command); });
    Log("native_send_request", command, result);
    return result;
}

/**
 * @brief 准备发送上下文并派发至微信 UI 线程
 * @details 
 *   - 耗时且密集的 IO 操作（如校验本地图片文件、计算 SHA-256、准备 SILK 语音音频）在当前工作线程执行，
 *     绝不在微信 UI 线程进行，防止导致微信界面卡死；
 *   - 准备完毕后构造 Attempt 对象，通过 ports_.dispatch 投递至微信 Qt UI 线程执行，
 *     并设置 1500 毫秒的严格同步等待超时；
 *   - 超时后绝不读取 attempt->result（因为 UI 线程可能正在写入，避免数据竞争），
 *     直接映射并返回超时错误状态。
 */
SendResult SendService::Dispatch(const SendCommand& command) {
    if (ports_.stopped() || !ports_.senderMatches())
        return {"rejected", "native_sender_unavailable", "Native profile, main thread or runtime unavailable"};
    const auto thread = ports_.bindUi();
    if (!thread)
        return {"rejected", "native_sender_unavailable", "Native profile, main thread or runtime unavailable"};
    
    // 媒体文件预处理（工作线程执行）
    std::shared_ptr<PreparedMediaFile> media;
    if (command.media) {
        if (!ports_.mediaMatches())
            return {"rejected", "native_media_sender_unavailable", "Media entry signature mismatch"};
        auto prepared = ports_.prepareMedia(*command.media);
        if (!prepared.file)
            return {"rejected", prepared.error, "Media spool validation failed before native preparation"};
        media = std::move(prepared.file);
    }

    auto attempt = std::make_shared<Attempt>(command, thread, std::move(media));
    auto lease = RegisterAction();
    if (!lease) return *MapUiDispatchFailure(UiDispatchStatus::unavailable);
    
    // 派发至微信 Qt UI 线程执行 Execute (超时设为 1500ms)
    const auto dispatched = ports_.dispatch([this, attempt, lease] {
        (void)lease;
        Execute(attempt);
    }, 1500);
    
    // 检查调度执行状态
    if (auto failure = MapUiDispatchFailure(dispatched.status)) {
        if (failure->status == "unknown") Log("native_send_result", command, *failure);
        return *failure; // 超时或失败时，严禁读取可能仍在写入中的 attempt->result
    }
    return attempt->result;
}

/**
 * @brief 真正运行在微信 UI 线程上的发信执行体
 * @details 
 *   1. 【二次时间校验】：指令在派发队列中排队可能消耗时间，进入执行体后先重新校验当前时间；
 *   2. 【登录与执行器校验】：调用 readSession 确认当前登录账号未发生变更，且发送器处于空闲就绪态；
 *   3. 【底层原生提交】：构造 beforeStart 闭包，调用 ports_.submit 执行原生构造与发送；
 *   4. 【结果映射与日志】：将底层原生提交结果 NativeAttemptResult 映射为标准 SendResult 并归档日志。
 */
void SendService::Execute(const std::shared_ptr<Attempt>& attempt) {
    const auto& command = attempt->command;
    auto& result = attempt->result;
    
    // 1. 二次校验时间窗口
    result = ValidateSendTime(command, ports_.now());
    if (!result.status.empty()) return;

    // 2. 检查会话状态与账号匹配
    const auto state = ports_.readSession(attempt->thread);
    if (!state.accountReady || state.accountId != command.accountId) {
        result = {"rejected", "account_not_verified", state.reason}; return;
    }
    if (!state.executorReady || ports_.stopped() || !ports_.senderMatches()) {
        result = {"rejected", "native_sender_unavailable", state.reason}; return;
    }

    // 3. 构建 beforeStart 前置守卫并调用底层原生提交
    const auto beforeStart = [this, attempt] { return BeforeStart(attempt); };
    const auto native = ports_.submit(command, attempt->thread, attempt->media, beforeStart);
    
    // 4. 映射原生执行结果
    result = MapNativeAttemptResult(native);
    Log("native_send_result", command, result, &native);
}

/**
 * @brief 原生发信调用前一刻的最终环境核验与文件句柄锁定
 * @details 该函数在微信底层 start() 真正被调用前由 NativeSubmit 回调：
 *          - 最后一次检查系统是否停机、会话是否正常、指令是否过期；
 *          - 对于图片消息，由于微信内部是由后台异步上传线程去读取文件内容的，
 *            为防止 UI 动作返回后文件被外部修改或删除导致微信底层读取崩溃，
 *            在此处通过 RetainImage 将该图片的只读句柄永久持有驻留至进程退出。
 */
bool SendService::BeforeStart(const std::shared_ptr<Attempt>& attempt) {
    const auto& command = attempt->command;
    const auto current = ports_.readSession(attempt->thread);
    const bool ready = !ports_.stopped() && current.accountReady &&
        current.accountId == command.accountId && current.executorReady &&
        ValidateSendTime(command, ports_.now()).status.empty();
    if (!ready || (command.media && !ports_.mediaMatches())) return false;
    
    // 图片发送必须成功锁定只读文件句柄
    return !command.media || command.media->kind != "image" || RetainImage(attempt->media);
}

/**
 * @brief 锁定图片文件句柄，防止微信异步上传时文件被删除或修改
 * @details 将 PreparedMediaFile 加入 retainedImages_ 映射表中，
 *          利用其内部具有 FILE_SHARE_READ 属性的 Win32 文件句柄锁定底层磁盘文件。
 */
bool SendService::RetainImage(const std::shared_ptr<PreparedMediaFile>& file) {
    std::lock_guard lock(mediaFiles_);
    if (retainedImages_.contains(file->path)) return true;
    if (retainedImages_.size() >= 4096) return false; // 防止缓存膨胀溢出
    retainedImages_.emplace(file->path, file);
    return true;
}

} // namespace wechatbot::monitor
