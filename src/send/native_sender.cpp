#include "monitor/send/native_sender.hpp"
#include "monitor/core/json_writer.hpp"
#include "monitor/send/send_service.hpp"
#include "monitor/config/IRISconfig.hpp"
#include "monitor/config/version_profile.hpp"
#include "monitor/protocol/command_protocol.hpp"
#include "monitor/native/native_bindings.hpp"
#include "monitor/native/native_text.hpp"
#include "monitor/native/native_media.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/runtime/runtime.hpp"
#include <atomic>
#include <cwchar>
#include <utility>

namespace wechatbot::monitor {
namespace {

/**
 * @brief 窗口枚举上下文结构体，用于定位微信的主 Qt UI 线程
 */
struct WindowSearch {
    DWORD process = GetCurrentProcessId(); ///< 当前进程 ID
    DWORD thread = 0;                     ///< 找到的目标 UI 线程 ID
    bool ambiguous = false;               ///< 是否发现多个不同线程持有同名窗口（防歧义）
};

/**
 * @brief Win32 EnumWindows 回调函数：定位属于当前进程且类名为 Qt51514QWindowIcon 的窗口
 * @details 
 *   微信桌面端主界面是基于 Qt 5.15.14 构建的，其顶层窗口类名为 "Qt51514QWindowIcon"。
 *   通过枚举当前进程的所有窗口并对比类名，能够准确获取承载 Qt 消息循环的 UI 线程 ID。
 *   若发现多个不同线程创建了该类名窗口，则置 ambiguous 为 true 放弃绑定，防止投递到错误的子线程。
 */
BOOL CALLBACK FindWindowThread(HWND window, LPARAM argument) {
    auto& found = *reinterpret_cast<WindowSearch*>(argument);
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != found.process) return TRUE; // 忽略其他进程的窗口
    
    wchar_t name[64]{};
    if (GetClassNameW(window, name, 64) && wcscmp(name, active_profile::sender::kWindowClass) == 0) {
        if (found.thread && found.thread != thread) found.ambiguous = true; // 歧义检测
        else found.thread = thread;
    }
    return TRUE;
}

/**
 * @brief 获取微信主 UI 线程 ID
 * @return 目标 UI 线程 ID；若未找到或存在多线程歧义则返回 0
 */
DWORD MainUiThread() {
    WindowSearch found;
    if (!EnumWindows(FindWindowThread, reinterpret_cast<LPARAM>(&found)) || found.ambiguous) return 0;
    return found.thread;
}

/**
 * @brief 将当前系统高精度时间转换为标准的 Unix 纪元时间戳 (毫秒)
 * @details Windows FILETIME 纪元为 1601-01-01，Unix 纪元为 1970-01-01。
 *          两者相差 116444736000000000 个 100ns 刻度；除以 10000 即可转换为毫秒。
 */
uint64_t ObservedUnixMs() noexcept {
    constexpr uint64_t epoch = 116444736000000000ULL;
    const uint64_t now = CommandNow();
    return now >= epoch ? (now - epoch) / 10000 : 0;
}

/**
 * @brief 发送全链路结构化诊断日志输出
 * @details 使用零堆内存分配的 ObjectWriter 直接格式化为 JSON 文本并追加至日志文件
 */
void Log(const char* kind, const std::string& request, const std::string& attempt,
         const SendResult& result, const NativeAttemptResult* native = nullptr) noexcept {
    try {
        std::string line;
        json::ObjectWriter fields(line);
        fields.String("kind", kind);
        fields.String("request_id", request);
        fields.String("attempt_id", attempt);
        fields.Number("observed_unix_ms", ObservedUnixMs());
        fields.String("status", result.status);
        fields.String("reason", result.code);
        if (native) {
            fields.String("local_uuid", native->localUuid);
            fields.Number("native_stage", static_cast<unsigned>(native->stage));
            fields.Boolean("start_entered", native->startEntered);
            fields.Boolean("start_returned", native->startReturned);
            fields.Boolean("cleanup_complete", native->cleanupComplete);
        }
        fields.Close();
        AppendDirect(line.c_str());
    } catch (...) {}
}

/**
 * @brief 根据微信模块特征校验结果动态调整发信策略
 * @details 只有当多媒体签名匹配且配置了合法的媒体暂存根目录时，才启用 mediaEnabled
 */
SendPolicy ReadyPolicy(const NativeSenderConfig& config, uintptr_t base) {
    auto policy = config.policy;
    policy.mediaEnabled = policy.mediaEnabled && MatchesNativeMediaSender(base) && !config.mediaRoot.empty();
    return policy;
}

/**
 * @brief 发送后端具体实现结构体 (Backend)
 * @details 
 *   生命周期管理说明：
 *   Backend 对象在首次初始化时通过 new Backend() 堆分配，其指针保存在原子变量中，
 *   设计为常驻整个宿主进程生命周期（不执行 delete）。
 *   设计意图：即使用户在后台工作线程中发起了 Observer 卸载，某些在 UI 线程中已超时
 *   但仍在执行的原生回调仍可能安全访问 Backend 中的虚函数或端口，绝不提前析构以防野指针崩溃。
 */
struct Backend {
    const uintptr_t base;                       ///< WeChatWin.dll 基地址
    const NativeSenderApis apis;                 ///< 微信底层逆向函数指针绑定表
    const std::wstring mediaRoot;               ///< 本地多媒体文件暂存目录
    UiDispatcher dispatcher;                    ///< Qt UI 线程消息泵调度器
    SendService service;                        ///< 发信业务中枢服务

    Backend(uintptr_t address, const NativeSenderConfig& config)
        : base(address), apis(BindNativeSenderApis(base)), mediaRoot(config.mediaRoot),
          service(ReadyPolicy(config, base), Ports()) {}

    /**
     * @brief 绑定 UI 线程至 UiDispatcher 钩子
     */
    DWORD BindUi() {
        const DWORD thread = MainUiThread();
        if (!thread) return 0;
        const DWORD bound = dispatcher.BoundThreadId();
        if (bound && bound != thread) return 0;
        return dispatcher.Bind(thread) ? thread : 0;
    }

    /**
     * @brief 底层原生发信分发
     * @details 根据指令类型将请求分流至 SubmitNativeMediaOnce 或 SubmitNativeTextOnce
     */
    NativeAttemptResult Submit(const SendCommand& command, DWORD thread,
                              const std::shared_ptr<PreparedMediaFile>& media,
                              const std::function<bool()>& beforeStart) {
        if (command.media) {
            NativeMediaRequest request;
            request.targetUtf8 = command.targetId;
            request.kind = command.media->kind == "image" ? NativeMediaKind::image : NativeMediaKind::voice;
            request.imagePath = request.kind == NativeMediaKind::image ? std::wstring_view(media->path) : std::wstring_view{};
            request.audioBytes = media->audio;
            request.durationMs = media->durationMs;
            request.authorizedUiThreadId = thread;
            request.beforeStart = beforeStart;
            return SubmitNativeMediaOnce(apis.media, request);
        }
        NativeTextRequest request{command.targetId, command.text, thread};
        request.atUserListUtf8 = command.atUserList;
        request.quote = command.quote ? &*command.quote : nullptr;
        request.beforeStart = beforeStart;
        return SubmitNativeTextOnce(apis.text, request);
    }

    /**
     * @brief 组装并注入 SendService 所需的平台适配端口 (Ports)
     */
    SendServicePorts Ports() {
        SendServicePorts ports;
        ports.now = CommandNow;
        ports.monotonicMs = [] { return GetTickCount64(); };
        ports.stopped = [] { return InterlockedCompareExchange(&g_stop, 0, 0) != 0; };
        ports.senderMatches = [this] { return MatchesNativeSender(base); };
        ports.mediaMatches = [this] { return MatchesNativeMediaSender(base); };
        ports.bindUi = [this] { return BindUi(); };
        ports.dispatch = [this](std::function<void()> action, DWORD timeout) {
            return dispatcher.Execute(std::move(action), timeout);
        };
        ports.readSession = [this](DWORD thread) { return ReadNativeSession(base, thread); };
        ports.prepareMedia = [this](const MediaCommand& command) { return PrepareMediaFile(command, mediaRoot); };
        ports.submit = [this](const SendCommand& command, DWORD thread,
                              const std::shared_ptr<PreparedMediaFile>& media,
                              const std::function<bool()>& beforeStart) {
            return Submit(command, thread, media, beforeStart);
        };
        ports.log = Log;
        return ports;
    }
};

/// 全局原子指针，保存常驻进程的 Backend 单例
std::atomic<Backend*> backend{nullptr};

} // namespace

void InitializeNativeSender(HMODULE module, const NativeSenderConfig& config) {
    if (backend.load() || !module) return;
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (!MatchesNativeSender(base)) {
        Log("native_sender_disabled", "", "", {"rejected", "entry_signature_mismatch", ""});
        return;
    }
    backend.store(new Backend(base, config), std::memory_order_release);
    Log("native_sender_ready", "", "", {"read_only", "native_backend_initialized", ""});
}

NativeSendCapabilities ProbeNativeSender() {
    auto* adapter = backend.load(std::memory_order_acquire);
    return adapter ? adapter->service.Probe() : NativeSendCapabilities{};
}

SendResult RunNativeSender(const SendCommand& command) {
    auto* adapter = backend.load(std::memory_order_acquire);
    if (adapter) return adapter->service.Run(command);
    SendResult result{"rejected", "native_sender_unavailable", "No native backend was initialized"};
    Log("native_send_request", command.requestId, command.attemptId, result);
    return result;
}

bool WaitNativeSenderIdle(DWORD timeoutMs) noexcept {
    auto* adapter = backend.load(std::memory_order_acquire);
    return !adapter || adapter->service.WaitIdle(timeoutMs);
}

std::string HandleCommand(std::string_view payload, const std::string& session) {
    return HandleCommand(payload, session, CommandHandlers{ProbeNativeSender, RunNativeSender});
}

} // namespace wechatbot::monitor
