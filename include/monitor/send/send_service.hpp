#pragma once
#include "monitor/media/media_file.hpp"
#include "monitor/native/native_session.hpp"
#include "monitor/native/native_submit.hpp"
#include "monitor/send/send_capabilities.hpp"
#include "monitor/send/send_gate.hpp"
#include "monitor/transport/ui_dispatch.hpp"
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace wechatbot::monitor {

/**
 * @brief 发送服务端口抽象 (SendServicePorts)
 * @details 采用端口-适配器（Ports and Adapters / 六角架构）依赖注入模式：
 *          将真实的 Win32 API 与微信内存逆向操作抽象为一组可调用闭包。
 *          - 生产环境：注入真正的 NativeSender 和 UiDispatcher；
 *          - 单元测试：注入模拟 mock 函数，无需启动微信进程即可实现确定性全分支测试。
 *          除可选的日志接口 log 外，所有端口均须由调用方完整提供。
 */
struct SendServicePorts {
    std::function<uint64_t()> now;                           ///< 获取当前 UTC 时间戳 (FILETIME 100ns 单位)
    std::function<uint64_t()> monotonicMs;                   ///< 获取系统单调递增运行时间 (毫秒)，用于频控冷却
    std::function<bool()> stopped;                           ///< 检查运行时是否收到全局停止信号
    std::function<bool()> senderMatches;                     ///< 校验微信原生发信函数特征码是否匹配
    std::function<bool()> mediaMatches;                      ///< 校验微信原生多媒体发信函数特征码是否匹配
    std::function<DWORD()> bindUi;                           ///< 获取并绑定微信 Qt UI 主线程 ID (失败返回 0)
    std::function<UiDispatchResult(std::function<void()>, DWORD)> dispatch; ///< 将闭包跨线程调度至 UI 线程并同步等待
    std::function<NativeSessionSnapshot(DWORD)> readSession; ///< 读取微信会话与账号登录快照 (仅限 UI 线程调用)
    std::function<MediaFileResult(const MediaCommand&)> prepareMedia; ///< 校验并解析待发送媒体文件 (仅限工作线程)
    std::function<NativeAttemptResult(const SendCommand&, DWORD,
        const std::shared_ptr<PreparedMediaFile>&, const std::function<bool()>&)> submit; ///< 执行原生发信提交
    std::function<void(const char*, const std::string&, const std::string&,
        const SendResult&, const NativeAttemptResult*)> log; ///< 结构化诊断日志输出槽
};

/**
 * @brief 发送业务中枢服务 (SendService)
 * @details 
 *   职责：
 *   1. 充当业务门禁与底层原生提交之间的协调者；
 *   2. 维护发信任务生命周期（通过 ActionLease 实现优雅停机屏障 WaitIdle）；
 *   3. 负责跨线程调度：在后台工作线程准备媒体文件，然后派发至微信 Qt UI 线程执行原生对象构造与发送；
 *   4. 图片生命周期驻留：通过 RetainImage 保持图片文件的独占读取句柄，防止微信异步上传过程中文件被外部篡改或删除。
 */
class SendService final {
public:
    /**
     * @brief 构造发信服务
     * @param policy 发送安全策略 (包含限流、白名单、模式等)
     * @param ports 平台与底层接口端口集合
     */
    SendService(SendPolicy policy, SendServicePorts ports);
    SendService(const SendService&) = delete;
    SendService& operator=(const SendService&) = delete;

    /**
     * @brief 探测当前发信能力与登录会话状态
     * @details 会短暂派发快照任务至 UI 线程读取当前微信号、是否就绪、以及文本/图片/语音发送开关状态
     */
    NativeSendCapabilities Probe();

    /**
     * @brief 执行发信指令
     * @details 依次经过 SendGate 门禁审核、参数校验、跨线程调度到 UI 线程并执行微信原生发信
     */
    SendResult Run(const SendCommand& command);

    /**
     * @brief 优雅停机等待屏障 (Shutdown Barrier)
     * @details 
     *   1. 标记 closing_ = true，拒绝所有新入队的任务请求；
     *   2. 阻塞等待所有已进入调度管道（包括等待超时未完成的）在途任务释放 ActionLease；
     *   3. 确保在退出或卸载时不会发生悬空指针访问崩溃。
     * @param timeoutMs 最长等待毫秒数，若为 INFINITE 则无限等待
     * @return true 所有任务已排空 (空闲)；false 超时退出
     */
    bool WaitIdle(DWORD timeoutMs) noexcept;

private:
    struct Attempt;
    struct ActionLease;

    /// 登记并获取一个在途任务的租约 (RAII 自动计数)
    std::shared_ptr<ActionLease> RegisterAction();
    /// 释放一个在途任务租约并在全部清空时唤醒 WaitIdle
    void ReleaseAction() noexcept;

    /// 调度单次发送任务至 UI 线程
    SendResult Dispatch(const SendCommand& command);
    /// 实际在 UI 线程上执行的发信过程
    void Execute(const std::shared_ptr<Attempt>& attempt);
    /// 原生发信 Start 之前的最终状态检查与图片文件锁定
    bool BeforeStart(const std::shared_ptr<Attempt>& attempt);
    /// 将发送的图片加入防篡改/防删除句柄驻留缓存
    bool RetainImage(const std::shared_ptr<PreparedMediaFile>& file);

    /// 输出结构化诊断日志
    void Log(const char* kind, const SendCommand& command, const SendResult& result,
             const NativeAttemptResult* native = nullptr) const;

    const SendPolicy policy_;       ///< 发信策略（白名单、频控等）
    const SendServicePorts ports_;  ///< 底层平台/逆向接口
    SendGate gate_;                 ///< 安全校验与频控门禁

    std::mutex calls_;              ///< 序列化外部发信并发调用的锁
    std::mutex actionsMutex_;       ///< 保护在途任务计数与停机状态的锁
    std::condition_variable idle_;  ///< 在途任务清零时的条件变量
    size_t outstandingActions_ = 0; ///< 当前正在执行或调度的在途任务计数
    bool closing_ = false;          ///< 是否处于正在关闭状态

    std::mutex mediaFiles_;         ///< 保护图片驻留缓存的锁
    std::unordered_map<std::wstring, std::shared_ptr<PreparedMediaFile>> retainedImages_; ///< 进程生命周期的图片文件持有句柄映射表
};

} // namespace wechatbot::monitor
