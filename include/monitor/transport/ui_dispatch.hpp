#pragma once
#include "monitor/core/platform.hpp"
#include <functional>
#include <memory>

namespace wechatbot::monitor {
// 调度执行结果状态码
enum class UiDispatchStatus {
    completed,              // 任务在 UI 线程成功执行完成
    cancelled_before_start, // 任务在被 UI 线程取出前已超时或取消
    timeout_after_start,    // 任务已在 UI 线程开始执行，但调用端等待超时 (副作用可能已发生，严禁自动重试)
    busy,                   // 当前派发槽位被占用
    unavailable,            // 派发器未就绪或已被解绑
    exception               // 任务执行过程中抛出异常
};

struct UiDispatchResult {
    UiDispatchStatus status = UiDispatchStatus::unavailable;
    DWORD executionThreadId = 0; // 执行线程 ID；若为 0 表示任务未曾开始
    DWORD win32Error = ERROR_SUCCESS;
};

/**
 * @brief 进程内单任务 UI 线程消息循环派发桥接器 (UiDispatcher)
 * @details 
 *   微信桌面端基于 Qt 5.15.14 开发，底层网络和核心对象均具备强烈的 UI 线程亲和性（Thread Affinity）。
 *   若直接在后台工作线程（如命名管道监听线程）调用微信原生发信函数，会导致崩溃或死锁。
 * 
 *   本类通过 Windows 钩子机制解决跨线程安全调度：
 *   1. 使用 SetWindowsHookExW(WH_GETMESSAGE, ...) 为目标 UI 线程挂载消息钩子；
 *   2. 使用 RegisterWindowMessageW 注册专用的自定义窗口消息；
 *   3. 外部发信请求通过 PostThreadMessage 投递到 UI 线程的消息队列；
 *   4. 当微信主界面的 Qt 消息泵（GetMessage/PeekMessage）轮询到该消息时，在 WH_GETMESSAGE 回调中执行任务闭包；
 *   5. 通过 Win32 Event 和原子状态机同步等待完成、超时拦截及优雅逆序解绑。
 */
class UiDispatcher final {
public:
    UiDispatcher();
    ~UiDispatcher();
    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;
    UiDispatcher(UiDispatcher&&) = delete;
    UiDispatcher& operator=(UiDispatcher&&) = delete;

    /**
     * @brief 绑定目标 UI 线程
     * @param threadId 微信主 UI 线程 ID (通常由 EnumWindows 寻找 Qt51514QWindowIcon 类名窗口获取)
     * @return true 绑定并挂钩成功；false 失败 (可通过 GetLastError() 获取原因)
     */
    bool Bind(DWORD threadId) noexcept;

    /**
     * @brief 派发任务至 UI 线程执行
     * @param action 需在 UI 线程执行的可调用闭包 (内部必须通过值捕获所有参数)
     * @param timeoutMs 调用方同步等待的最长毫秒数
     * @return UiDispatchResult 调度执行结果
     */
    UiDispatchResult Execute(std::function<void()> action, DWORD timeoutMs) noexcept;

    /**
     * @brief 解绑并卸载 WH_GETMESSAGE 钩子
     */
    bool Unbind() noexcept;
    DWORD BoundThreadId() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace wechatbot::monitor
