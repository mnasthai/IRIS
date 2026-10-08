#include "monitor/runtime/runtime.hpp"
#include "monitor/config/IRISconfig.hpp"
#include "monitor/receive/receive.hpp"
#include "monitor/diagnostics/send_trace.hpp"
#include "monitor/native/target.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/transport/command_pipe.hpp"
#include "monitor/send/native_sender.hpp"
#include "monitor/media/media_receive_native.hpp"
#include <MinHook.h>
#include <cwchar>

namespace wechatbot::monitor {

/// @brief 全局原子停机标志位 (0: 运行中, 1: 停机中)
volatile LONG g_stop = 0;  // 强迫 cpu 每次读取的时候去检查

namespace {

/**
 * @brief 判断当前进程是否为微信主界面进程 (Weixin.exe)
 * @details 
 *   现代微信桌面端采用多进程架构 (多进程模型类似 Chromium)：
 *   - 主进程 无 --type= 标识，负责渲染主窗口、网络引擎及核心会话
 *   - 渲染子进程 带有命令行参数 `--type=renderer` 或 `--type=gpu-process`
 *   - 崩溃处理守护进程 带有 `--crashpad-handler`
 * 
 *   若不加区分，注入器会导致所有子进程均加载 DLL 并尝试绑定相同的命令管道与日志文件，造成端口冲突
 */
bool IsMainWeixinProcess() {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return false;
    const wchar_t* base = wcsrchr(executable, L'\\');
    base = base ? base + 1 : executable;
    if (_wcsicmp(base, L"Weixin.exe") != 0) return false;
    const wchar_t* commandLine = GetCommandLineW();
    return commandLine && !wcsstr(commandLine, L"--type=") &&
           !wcsstr(commandLine, L"--crashpad-handler");
}

/**
 * @brief IRIS核心子系统初始化装配链
 * @param config 从注册表或环境变量加载的运行配置
 * @return true 全部核心组件挂钩并启动成功 false 初始化失败 (输出 hook_error)
 */
bool InitializeIRIS(const IrisConfig& config) {
    // 1. 发射观察者启动事件 JSON
    const std::string start = "{\"kind\":\"observer_start\",\"schema_version\":2,"
        "\"build\":\"" + std::string(kMonitorBuild) + "\",\"target_version\":\"" + kTargetVersion +
        "\",\"mode\":\"read_only\",\"pid\":" + std::to_string(GetCurrentProcessId()) + "}";
    AppendDirect(start.c_str());

    // 2. 轮询等待核心业务模块 Weixin.dll 完成动态装载
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        g_weixin = GetModuleHandleW(L"Weixin.dll");
        if (g_weixin) break;
        Sleep(100);
    }
    if (!g_weixin) return false;

    // 3. 版本签名校验 对比模块大小及入口 16 字节特征码
    std::string reason;
    if (!VerifyTarget(g_weixin, reason)) {
        std::string line = "{\"kind\":\"observer_disabled\",\"reason\":";
        AppendJsonString(line, reason.c_str());
        line += "}";
        AppendDirect(line.c_str());
        return false;
    }

    // 4. 启动异步无锁的日志刷盘线程 (256 槽位环形缓冲 不阻塞 Hook 热路径)
    if (!StartLogger()) {
        AppendDirect("{\"kind\":\"hook_error\",\"stage\":\"logger_thread\"}");
        return false;
    }

    // 5. 初始化原生发信 API 函数指针表
    InitializeNativeSender(g_weixin, config.sender);

    // 6. 初始化 MinHook 核心引擎
    const MH_STATUS initialize = MH_Initialize();
    if (initialize != MH_OK && initialize != MH_ERROR_ALREADY_INITIALIZED) {
        AppendDirect("{\"kind\":\"hook_error\",\"stage\":\"initialize\"}");
        return false;
    }

    // 7. 挂载各个业务子系统的 Hook 钩子
    if (!EnableReceiveObservation(config.mediaTrace)) return false; // 接收消息 Hook (必选)
    EnableSendObservation(config.sendTrace);                        // 发送信令链路观测 (可选)
    EnableSendContextObservation(config.sendContextTrace);          // 发送上下文观测 (可选)
    EnableSendSubmitObservation(config.sendSubmitTrace, config.mediaTrace); // 原生提交观测
    EnableNativeMediaReceive(config.mediaReceive, config.mediaInboundRoot); // 多媒体下载完成拦截

    // 8. 启动 Windows 命名管道服务端 外部进程 IPC 通信
    StartCommandPipe(config.commandPipe); // 失败时内部自报告
    return true;
}

/**
 * @brief 独立的后台常驻监控线程
 * @param module 本 DLL 模块基址 (HMODULE)
 * @details
 *   脱离 DllMain Loader Lock 后的顶层主管线程 (Supervisor)
 *   负责执行完整的会话开启、日志文件创建、系统初始化，并进入保活等待
 *   当收到退出信号时，主管线程协调各工作组件优雅停机
 */
DWORD WINAPI ObserverThread(void* module) {
    try {
        const auto config = LoadObserverConfig(static_cast<HMODULE>(module));
        if (!config.invalidReason.empty()) {
            OutputDebugStringA(("WeChat observer configuration rejected: " + config.invalidReason).c_str());
            return 0;
        }
        BeginSession();
        if (!OpenLog(config.logDirectory, config.logPath)) return 0;
        if (InitializeIRIS(config)) {
            // 系统进入稳定运行态，每 100ms 检查一次全局退出标志位 g_stop
            while (!InterlockedCompareExchange(&g_stop, 0, 0)) Sleep(100);
        }
    } catch (...) {
        try { AppendDirect("{\"kind\":\"observer_disabled\",\"reason\":\"startup_exception\"}"); }
        catch (...) { OutputDebugStringA("WeChat observer startup exception"); }
    }
    // 若有缓慢的 UI 任务或文件读写尚未完成 循环尝试 Join 直至彻底干净
    while (!StopObserverWorkers(1000)) Sleep(100);
    return 0;
}

} // namespace

void StartIRIS(HMODULE IRISModule) {
    if (!IsMainWeixinProcess()) return;
    // 仅引导线程的创建发生在 Loader Lock 下 脱钩后的 ObserverThread 自行管理后续生命周期
    HANDLE thread = CreateThread(nullptr, 0, ObserverThread, IRISModule, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void SignalStop() { 
    InterlockedExchange(&g_stop, 1); 
}

/**
 * @brief 清理停机
 * @details 
 *   严格按照各子系统之间的依赖反向停机：
 *   1. StopCommandPipe: 关闭管道连接，拒绝新进请求
 *   2. WaitNativeSenderIdle: 等待正在 UI 线程执行的原生发信指令返回
 *   3. StopNativeMediaReceive: 停止媒体下载管线
 *   4. StopLogger: 等待剩余日志全部写入磁盘
 *   5. CloseLog: 关闭 observer.jsonl 文件句柄
 */
bool StopObserverWorkers(DWORD timeoutMs) noexcept {
    SignalStop();
    const ULONGLONG began = GetTickCount64();
    const auto remaining = [began, timeoutMs]() -> DWORD {
        if (timeoutMs == INFINITE) return INFINITE;
        const ULONGLONG elapsed = GetTickCount64() - began;
        return elapsed >= timeoutMs ? DWORD{0} : timeoutMs - static_cast<DWORD>(elapsed);
    };
    if (!StopCommandPipe(remaining())) return false;
    if (!WaitNativeSenderIdle(remaining())) return false;
    if (!StopNativeMediaReceive(remaining())) return false;
    if (!StopLogger(remaining())) return false;
    return CloseLog();
}
} // namespace wechatbot::monitor
