#include "monitor/transport/command_pipe.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/runtime/runtime.hpp"
#include "monitor/core/worker_thread.hpp"
#include <sddl.h>
#include <algorithm>
#include <vector>
#include <mutex>

#pragma comment(lib, "advapi32.lib")

namespace wechatbot::monitor {
namespace {

/**
 * @brief 管道后台工作线程状态单例
 */
struct PipeWorkerState {
    WorkerThread worker;
    std::mutex lifecycle;
};

PipeWorkerState& PipeWorker() {
    static auto* state = new PipeWorkerState;
    return *state;
}

using Handle = UniqueHandle;

/**
 * @brief Win32 本地堆内存 (LocalAlloc/LocalFree) 的 RAII 自动释放封装
 */
struct LocalMemory {
    void* value = nullptr;
    ~LocalMemory() { if (value) LocalFree(value); }
};

/**
 * @brief 检查是否已收到全局停止信号或超时
 */
bool Stopped(volatile LONG& stop, ULONGLONG deadline) {
    return InterlockedCompareExchange(&stop, 0, 0) || GetTickCount64() >= deadline;
}

/**
 * @brief 等待异步重叠 I/O 操作完成或安全取消
 * @details 
 *   核心防护要点：
 *   - 以 100ms 为步长分段等待 hEvent，同时监测全局 stop 停止信号与 deadline 截止时间；
 *   - 若发生超时或收到停止信号，调用 Win32 CancelIoEx(pipe, &overlapped) 发起内核异步取消；
 *   - 关键防崩溃：发起 CancelIoEx 后，必须调用 GetOverlappedResult(pipe, &overlapped, ..., TRUE)
 *     同步等待 Windows 内核彻底完成取消回调并交还缓冲区所有权。
 *     若不等待直接释放局部缓冲区，内核后续写入将直接引发野指针崩溃！
 */
bool FinishIo(HANDLE pipe, OVERLAPPED& overlapped, DWORD& bytes,
              volatile LONG& stop, ULONGLONG deadline) {
    while (!Stopped(stop, deadline)) {
        const DWORD wait = WaitForSingleObject(overlapped.hEvent,
            static_cast<DWORD>((std::min)(100ULL, deadline - GetTickCount64())));
        if (wait == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &overlapped, &bytes, FALSE) != FALSE;
        if (wait != WAIT_TIMEOUT) break;
    }
    // 异步取消必须保证 OVERLAPPED 及其内存指针在内核返回前有效
    CancelIoEx(pipe, &overlapped);
    GetOverlappedResult(pipe, &overlapped, &bytes, TRUE);
    return false;
}

/**
 * @brief 在命名管道上基于 OVERLAPPED 执行完整的固定长度字节传输（读或写）
 * @param pipe 命名管道句柄
 * @param data 缓冲区指针
 * @param length 待传输的总字节数
 * @param writing true 为写入，false 为读取
 * @param stop 全局停止信号
 * @param deadline 单次传输的最大截止时间
 * @return true 传输完毕；false 中途失败或超时
 */
bool Transfer(HANDLE pipe, void* data, DWORD length, bool writing,
              volatile LONG& stop, ULONGLONG deadline) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get()) return false;
    DWORD offset = 0;
    while (offset < length && !Stopped(stop, deadline)) {
        ResetEvent(event.get());
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD transferred = 0;
        auto* buffer = static_cast<unsigned char*>(data) + offset;
        const BOOL done = writing ? WriteFile(pipe, buffer, length - offset, &transferred, &operation) :
                                    ReadFile(pipe, buffer, length - offset, &transferred, &operation);
        if (!done && (GetLastError() != ERROR_IO_PENDING ||
                      !FinishIo(pipe, operation, transferred, stop, deadline))) return false;
        if (!transferred) return false;
        offset += transferred;
    }
    return offset == length;
}

void LogFailure(const char* stage, DWORD error) {
    std::string line = "{\"kind\":\"command_pipe_error\",\"stage\":";
    AppendJsonString(line, stage);
    line += ",\"win32_error\":" + std::to_string(error) + "}";
    AppendDirect(line.c_str());
}

/**
 * @brief 将宽字符串按 UTF-8 转换为窄字符串
 * @details 管道名由会话 ID 拼接而成，实际只含 ASCII 字符。但逐字符窄化
 *          （std::string(name.begin(), name.end())）会把每个 wchar_t 截断为一个
 *          字节，一旦出现非 ASCII 就静默产生损坏的名称，因此这里走标准转换。
 * @param value 待转换的宽字符串
 * @return 转换后的 UTF-8 字符串；输入为空或转换失败时返回空串
 */
std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
                             result.data(), bytes, nullptr, nullptr))
        return {};
    return result;
}

/**
 * @brief 管道服务后台线程函数
 * @note 每次请求都是短连接：服务端应答后会等待客户端断开，再服务下一个客户端。
 */
DWORD WINAPI PipeThread(void*) {
    try {
        const auto session = CurrentSessionId();
        const std::wstring name = L"\\\\.\\pipe\\wechatbot-" + std::wstring(session.begin(), session.end());
        RunCommandPipe(name, session, g_stop, 0, true);
    } catch (...) {
        LogFailure("unhandled_exception", ERROR_UNHANDLED_EXCEPTION);
    }
    return 0;
}
} // namespace

bool RunCommandPipe(const std::wstring& name, const std::string& session,
                    volatile LONG& stop, DWORD durationMs, bool logReady) {
    const auto fail = [logReady](const char* stage) {
        if (logReady) LogFailure(stage, GetLastError());
        return false;
    };

    // 1. 获取当前进程令牌以提取运行微信的用户 SID
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return fail("process_token");
    Handle token(rawToken);
    DWORD tokenBytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &tokenBytes);
    if (!tokenBytes) return fail("token_size");
    std::vector<unsigned char> tokenBuffer(tokenBytes);
    if (!GetTokenInformation(token.get(), TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes))
        return fail("token_user");
    LocalMemory sid, descriptor;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenBuffer.data())->User.Sid,
                               reinterpret_cast<LPWSTR*>(&sid.value))) return fail("user_sid");

    // 2. 组装 SDDL 访问控制列表：仅允许 LocalSystem(SY) 完全控制与当前用户 SID 读写(GRGW)
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" +
                             std::wstring(static_cast<const wchar_t*>(sid.value)) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
            reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr)) return fail("pipe_acl");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};

    // 3. 创建命名管道服务端
    // 特性：双向全双工、重叠异步、首实例保证、拒绝远程网络客户端
    Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, kCommandFrameLimit + 4, kCommandFrameLimit + 4, 0, &security));
    if (!pipe) return fail("create_pipe");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get()) return fail("create_event");

    if (logReady) {
        std::string line = "{\"kind\":\"command_pipe_ready\",\"protocol_version\":1,\"pipe\":";
        AppendJsonString(line, ToUtf8(name).c_str());
        line += ",\"mode\":\"read_only\",\"account_verified\":false,\"send_text\":false}";
        AppendDirect(line.c_str());
    }

    const ULONGLONG deadline = durationMs ? GetTickCount64() + durationMs : MAXULONGLONG;

    // 4. 服务端主循环：等待客户端连接并处理请求
    while (!Stopped(stop, deadline)) {
        ResetEvent(event.get());
        OVERLAPPED connection{};
        connection.hEvent = event.get();
        bool connected = ConnectNamedPipe(pipe.get(), &connection) != FALSE;
        if (!connected) {
            const DWORD error = GetLastError();
            DWORD ignored = 0;
            if (error == ERROR_PIPE_CONNECTED) connected = true;
            else if (error == ERROR_IO_PENDING) connected = FinishIo(pipe.get(), connection, ignored, stop, deadline);
            else return fail("connect_pipe");
        }
        if (!connected) break;

        // 5. 帧读取：先读 4 字节小端无符号整数表示请求长度 (限制单次请求不超过 1000ms 时间预算)
        const auto requestDeadline = (std::min)(deadline, GetTickCount64() + 1000);
        uint32_t length = 0; // Windows x64 线路字节序为小端序 (Little Endian)
        if (Transfer(pipe.get(), &length, sizeof(length), false, stop, requestDeadline)) {
            std::string payload;
            bool read = false;
            if (length && length <= kCommandFrameLimit) {
                payload.resize(length);
                read = Transfer(pipe.get(), payload.data(), length, false, stop, requestDeadline);
            }

            // 6. 分发命令并执行
            if (read || !length || length > kCommandFrameLimit) {
                auto reply = HandleCommand(payload, session);
                const auto replyDeadline = (std::min)(deadline, GetTickCount64() + 1000);
                length = static_cast<uint32_t>(reply.size());
                
                // 7. 回写 4 字节小端响应长度及响应 JSON 载荷
                if (Transfer(pipe.get(), &length, sizeof(length), true, stop, replyDeadline))
                    Transfer(pipe.get(), reply.data(), length, true, stop, replyDeadline);
                
                // 8. 有界排空与等待客户端断开 (EOF Drain)
                // 绝不调用 FlushFileBuffers（防止恶意客户端故意不读挂死 DLL 卸载），
                // 而是等待客户端读完关闭连接后，安全 DisconnectNamedPipe
                char ignored;
                Transfer(pipe.get(), &ignored, 1, false, stop, replyDeadline);
            }
        }
        DisconnectNamedPipe(pipe.get());
    }
    return true;
}

bool StartCommandPipe(bool enabled) {
    if (!enabled) return true;
    auto& state = PipeWorker();
    std::lock_guard lock(state.lifecycle);
    if (state.worker.Start(PipeThread)) return true;
    LogFailure("create_thread", GetLastError());
    return false;
}

bool StopCommandPipe(DWORD timeoutMs) noexcept {
    auto& state = PipeWorker();
    std::lock_guard lock(state.lifecycle);
    return state.worker.Join(timeoutMs) == WAIT_OBJECT_0;
}

} // namespace wechatbot::monitor
