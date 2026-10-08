#include "monitor/config/IRISconfig.hpp"
#include "monitor/config/version_config.hpp"
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <vector>

namespace wechatbot::monitor {
namespace {
constexpr size_t kEnvironmentCapacity = 8192;
constexpr size_t kPathCapacity = 32768;

bool HasInvalidCharacters(const std::wstring& value) {
    return std::any_of(value.begin(), value.end(), [](wchar_t ch) {
        return ch < L' ' || ch == L'"' || ch == L'<' || ch == L'>' ||
               ch == L'|' || ch == L'*' || ch == L'?';
    });
}

bool IsValidUnicode(const std::wstring& value) {
    return value.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr) > 0;
}

bool IsLocalAbsolutePath(const std::wstring& value) {
    return value.size() >= 3 &&
        ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) &&
        value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

bool IsUncPath(const std::wstring& value) {
    if (value.size() < 5 || value[0] != L'\\' || value[1] != L'\\') return false;
    const auto serverEnd = value.find(L'\\', 2);
    if (serverEnd == std::wstring::npos || serverEnd <= 2 || serverEnd + 1 >= value.size() ||
        value[serverEnd + 1] == L'\\') return false;
    const auto shareEnd = value.find(L'\\', serverEnd + 1);
    const auto share = value.substr(serverEnd + 1,
        shareEnd == std::wstring::npos ? shareEnd : shareEnd - serverEnd - 1);
    return share != L"." && share != L"..";
}

bool InvalidComponent(const std::wstring& value) {
    if (value.empty() || value == L"." || value == L"..") return false;
    if (value.size() > 255 || value.back() == L' ' || value.back() == L'.') return true;
    const auto dot = value.find(L'.');
    const auto base = value.substr(0, dot);
    if (_wcsicmp(base.c_str(), L"CON") == 0 || _wcsicmp(base.c_str(), L"PRN") == 0 ||
        _wcsicmp(base.c_str(), L"AUX") == 0 || _wcsicmp(base.c_str(), L"NUL") == 0)
        return true;
    return base.size() == 4 && base[3] >= L'1' && base[3] <= L'9' &&
        (_wcsnicmp(base.c_str(), L"COM", 3) == 0 || _wcsnicmp(base.c_str(), L"LPT", 3) == 0);
}

std::string EnvironmentText(const std::wstring& value, std::string& reason,
                            const char* field) {
    if (value.empty()) return {};
    if (value.size() >= kEnvironmentCapacity || value.find(L'\0') != std::wstring::npos) {
        if (reason.empty()) reason = std::string(field) + "_invalid";
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > static_cast<int>(kEnvironmentCapacity)) {
        if (reason.empty()) reason = std::string(field) + "_invalid";
        return {};
    }
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr)) {
        if (reason.empty()) reason = std::string(field) + "_invalid";
        return {};
    }
    return result;
}

std::vector<std::string> TargetList(const std::string& value) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin <= value.size()) {
        const auto separator = value.find(';', begin);
        const auto end = separator == std::string::npos ? value.size() : separator;
        const auto first = value.find_first_not_of(" \t\r\n", begin);
        if (first != std::string::npos && first < end) {
            const auto last = value.find_last_not_of(" \t\r\n", end - 1);
            std::string target = value.substr(first, last - first + 1);
            if (std::find(result.begin(), result.end(), target) == result.end())
                result.push_back(std::move(target));
        }
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return result;
}

void RejectConfiguration(IrisConfig& config) {
    config.runtimeRoot.clear();
    config.logDirectory.clear();
    config.logPath.clear();
    config.sender = {};
    config.mediaInboundRoot.clear();
    config.commandPipe = config.sendTrace = config.sendContextTrace = false;
    config.sendSubmitTrace = config.mediaTrace = config.mediaReceive = false;
}

std::wstring ChildPath(const std::wstring& root, const wchar_t* relative) {
    return NormalizeAbsolutePath((std::filesystem::path(root) / relative).wstring());
}
} // namespace

std::wstring ReadObserverEnvironment(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return {};
    // A nonempty invalid marker distinguishes a failed read from an unset
    // path; invalid explicit settings must never select a default directory.
    if (count > kPathCapacity) return std::wstring(1, L'\0');
    std::wstring value(count, L'\0');
    if (GetEnvironmentVariableW(name, value.data(), count) != count - 1)
        return std::wstring(1, L'\0');
    value.resize(count - 1);
    return value;
}
std::wstring NormalizeAbsolutePath(const std::wstring& value) {
    if (value.empty() || value.size() >= kPathCapacity || HasInvalidCharacters(value) ||
        !IsValidUnicode(value)) return {};
    std::wstring path = value;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!IsLocalAbsolutePath(path) && !IsUncPath(path)) return {};
    // Device namespaces and alternate data streams are not directory roots.
    if (path.starts_with(L"\\\\.\\") || path.starts_with(L"\\\\?\\")) return {};
    const auto colon = path.find(L':', IsLocalAbsolutePath(path) ? 2 : 0);
    if (colon != std::wstring::npos) return {};
    size_t begin = IsLocalAbsolutePath(path) ? 3 : 2;
    while (begin < path.size()) {
        const auto end = path.find(L'\\', begin);
        if (InvalidComponent(path.substr(begin, end == std::wstring::npos ? end : end - begin)))
            return {};
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    auto normalized = std::filesystem::path(path).lexically_normal().wstring();
    while (normalized.size() > 3 && normalized.back() == L'\\') normalized.pop_back();
    return normalized.size() < kPathCapacity ? normalized : std::wstring{};
}

/**
* @brief 自动推导机器人数据的根目录
* @details - 如果是本地源码编译（在 build/.../bin 里）
* 自动把 runtime 提升到工程根目录 project/IRIS/runtime 防止重新编译时日志丢失
* 如果是独立打包发布的无源码环境 则直接在当前 exe 旁边创建 runtime 目录
*
*/
std::wstring FindIrisRuntimeRoot(const std::wstring& modulePath) {
    try {
        std::filesystem::path current = std::filesystem::path(modulePath).parent_path();

        while (current.has_relative_path()) {
            // 只要目录下有 CMakeLists.txt，或者目录名就叫 IRIS，或者有 WeixinMonitor.sln
            if (std::filesystem::exists(current / "CMakeLists.txt") ||
                std::filesystem::exists(current / "WeixinMonitor.sln") ||
                current.filename() == "IRIS") {
                return (current / "runtime").wstring();
            }
            if (current == current.parent_path()) break; // 到顶了
            current = current.parent_path();
        }
    } catch (...) {}

    // 如果是打包发布环境（没找到任何项目文件）就在 DLL 旁边建 runtime
    return (std::filesystem::path(modulePath).parent_path() / "runtime").wstring();
}

IrisConfig LoadIrisConfigFrom(const std::wstring& defaultRuntimeRoot,
                                    const EnvironmentReader& reader) {
    IrisConfig config;
    if (!reader) {
        config.invalidReason = "environment_reader_missing";
        return config;
    }
    // 路径整理
    const auto runtime = reader(L"WECHATBOT_RUNTIME_ROOT");
    const auto media = reader(L"WECHATBOT_MEDIA_ROOT");
    const auto inbound = reader(L"WECHATBOT_MEDIA_INBOUND_ROOT");
    const auto runtimeInput = runtime.empty() ? defaultRuntimeRoot : runtime;
    config.runtimeRoot = NormalizeAbsolutePath(runtimeInput);

    if (config.runtimeRoot.empty() || !IsLocalAbsolutePath(config.runtimeRoot))
        config.invalidReason = "runtime_root_invalid";

    // 路径组织
    if (config.invalidReason.empty()) {
        config.logDirectory = config.runtimeRoot; // 日志文件路径
        config.logPath = ChildPath(config.runtimeRoot, kObserverLogFilename);
        config.sender.mediaRoot = media.empty() ? ChildPath(config.runtimeRoot, L"media\\outbound")
                                              : NormalizeAbsolutePath(media);  //待发送媒体资源
        config.mediaInboundRoot = inbound.empty() ? ChildPath(config.runtimeRoot, L"media\\inbound")
                                                : NormalizeAbsolutePath(inbound); // 接受的媒体资源
        if (config.logPath.empty()) config.invalidReason = "log_path_invalid";
        else if (config.sender.mediaRoot.empty()) config.invalidReason = "media_root_invalid";
        else if (config.mediaInboundRoot.empty()) config.invalidReason = "media_inbound_root_invalid";
    }

    //发信策略与目标白名单
    auto& policy = config.sender.policy;
    policy.enabled = reader(L"WECHATBOT_NATIVE_SEND") == L"1";
    policy.accountId = EnvironmentText(reader(L"WECHATBOT_SEND_ACCOUNT"), config.invalidReason, "send_account");
    policy.continuous = reader(L"WECHATBOT_SEND_MODE") == L"continuous";
    policy.experimentalQuote = reader(L"WECHATBOT_EXPERIMENTAL_QUOTE") == L"1";
    policy.mediaEnabled = reader(L"WECHATBOT_MEDIA_SEND") == L"1";

    const auto targets = reader(L"WECHATBOT_SEND_TARGETS");
    const auto target = reader(L"WECHATBOT_SEND_TARGET");
    const auto text = reader(L"WECHATBOT_SEND_TEXT");
    if (policy.continuous)
        policy.targetIds = TargetList(EnvironmentText(targets, config.invalidReason, "send_targets"));
    else {
        policy.targetId = EnvironmentText(target, config.invalidReason, "send_target");
        policy.text = EnvironmentText(text, config.invalidReason, "send_text");
    }

    // 各个子系统与 IPC 功能开关
    config.commandPipe = reader(L"WECHATBOT_COMMAND_PIPE") == L"1";
    config.sendTrace = reader(L"WECHATBOT_SEND_TRACE") == L"1";
    config.sendContextTrace = reader(L"WECHATBOT_SEND_CONTEXT_TRACE") == L"1";
    config.sendSubmitTrace = reader(L"WECHATBOT_SEND_SUBMIT_TRACE") == L"1";
    config.mediaTrace = reader(L"WECHATBOT_MEDIA_TRACE") == L"1";
    config.mediaReceive = reader(L"WECHATBOT_MEDIA_RECEIVE") == L"1";

    // 配置出错 熔断
    if (!config.invalidReason.empty()) RejectConfiguration(config);
    return config;
}

IrisConfig LoadObserverConfig(HMODULE observerModule) {
    if (!observerModule) return LoadIrisConfigFrom({}, ReadObserverEnvironment);
    std::wstring modulePath(kPathCapacity, L'\0');
    const DWORD length = GetModuleFileNameW(observerModule, modulePath.data(),
                                           static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) return LoadIrisConfigFrom({}, ReadObserverEnvironment);
    modulePath.resize(length);
    const auto root = FindIrisRuntimeRoot(modulePath);
    return LoadIrisConfigFrom(root, ReadObserverEnvironment);
}
} // namespace wechatbot::monitor
