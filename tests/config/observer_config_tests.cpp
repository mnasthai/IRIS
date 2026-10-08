// Pure injected configuration tests: no environment edits, files, or Weixin.
#include "monitor/config/IRISconfig.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace wechatbot::monitor;
namespace fs = std::filesystem;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using Environment = std::map<std::wstring, std::wstring>;
EnvironmentReader Reader(const Environment& values) {
    return [&values](const wchar_t* name) {
        const auto found = values.find(name);
        return found == values.end() ? std::wstring{} : found->second;
    };
}
void RequireRejected(const IrisConfig& value, const char* reason) {
    Require(value.invalidReason == reason, "stable invalid configuration reason");
    Require(value.runtimeRoot.empty() && value.logDirectory.empty() && value.logPath.empty() &&
            value.sender.mediaRoot.empty() && value.mediaInboundRoot.empty(), "invalid paths are cleared");
    Require(!value.sender.policy.enabled && !value.sender.policy.mediaEnabled && !value.commandPipe &&
            !value.sendTrace && !value.sendContextTrace && !value.sendSubmitTrace &&
            !value.mediaTrace && !value.mediaReceive, "invalid configuration enables no capabilities");
}
} // namespace

void TestObserverConfig() {
    // Printed first so a transient failure's output still shows whether this
    // test was entered at all.
    puts("[config] start");
    const Environment empty;
    const auto defaults = LoadIrisConfigFrom(L"D:\\copy\\runtime", Reader(empty));
    Require(defaults.invalidReason.empty(), "default root accepted");
    Require(defaults.logDirectory == L"D:\\copy\\runtime" &&
            defaults.logPath == L"D:\\copy\\runtime\\observer-4.1.13.12.jsonl", "default log derives from runtime");
    Require(defaults.sender.mediaRoot == L"D:\\copy\\runtime\\media\\outbound" &&
            defaults.mediaInboundRoot == L"D:\\copy\\runtime\\media\\inbound", "default media shares runtime root");
    Require(!defaults.sender.policy.enabled && !defaults.commandPipe, "safe default flags");

    const Environment configured{
        {L"WECHATBOT_RUNTIME_ROOT", L"C:/new copy/缓存/../runtime"},
        {L"WECHATBOT_MEDIA_ROOT", L"D:\\outbound"},
        {L"WECHATBOT_MEDIA_INBOUND_ROOT", L"D:\\inbound"},
        {L"WECHATBOT_NATIVE_SEND", L"1"}, {L"WECHATBOT_SEND_ACCOUNT", L"wxid_self"},
        {L"WECHATBOT_SEND_MODE", L"continuous"}, {L"WECHATBOT_EXPERIMENTAL_QUOTE", L"1"},
        {L"WECHATBOT_MEDIA_SEND", L"1"},
        {L"WECHATBOT_SEND_TARGETS", L" room@chatroom ;wxid_a;\troom@chatroom\r\n;;wxid_b "},
        {L"WECHATBOT_COMMAND_PIPE", L"1"}, {L"WECHATBOT_SEND_TRACE", L"1"},
        {L"WECHATBOT_SEND_CONTEXT_TRACE", L"1"}, {L"WECHATBOT_SEND_SUBMIT_TRACE", L"1"},
        {L"WECHATBOT_MEDIA_TRACE", L"1"}, {L"WECHATBOT_MEDIA_RECEIVE", L"1"},
    };
    const auto values = LoadIrisConfigFrom(L"D:\\unused\\runtime", Reader(configured));
    Require(values.invalidReason.empty() && values.runtimeRoot == L"C:\\new copy\\runtime", "explicit runtime normalized");
    Require(values.sender.mediaRoot == L"D:\\outbound" && values.mediaInboundRoot == L"D:\\inbound", "explicit media overrides");
    Require(values.sender.policy.enabled && values.sender.policy.continuous &&
            values.sender.policy.experimentalQuote && values.sender.policy.mediaEnabled,
            "sender configuration requests are preserved");
    Require(values.sender.policy.targetIds == std::vector<std::string>{"room@chatroom", "wxid_a", "wxid_b"},
            "target trimming, deduplication, and order");
    Require(values.commandPipe && values.sendTrace && values.sendContextTrace &&
            values.sendSubmitTrace && values.mediaTrace && values.mediaReceive, "all trace flags captured");
    auto inactiveFields = configured;
    inactiveFields[L"WECHATBOT_SEND_TARGET"] = std::wstring(8192, L'a');
    inactiveFields[L"WECHATBOT_SEND_TEXT"] = std::wstring(1, static_cast<wchar_t>(0xD800));
    Require(LoadIrisConfigFrom(L"D:\\runtime", Reader(inactiveFields)).invalidReason.empty(),
            "continuous mode does not validate inactive single-send fields");

    auto flags = configured;
    for (const auto* flag : {L"WECHATBOT_NATIVE_SEND", L"WECHATBOT_MEDIA_SEND", L"WECHATBOT_COMMAND_PIPE",
                            L"WECHATBOT_SEND_TRACE", L"WECHATBOT_SEND_CONTEXT_TRACE",
                            L"WECHATBOT_SEND_SUBMIT_TRACE", L"WECHATBOT_MEDIA_TRACE", L"WECHATBOT_MEDIA_RECEIVE"})
        flags[flag] = L"1 ";
    flags[L"WECHATBOT_EXPERIMENTAL_QUOTE"] = L"true";
    const auto disabled = LoadIrisConfigFrom(L"D:\\runtime", Reader(flags));
    Require(disabled.invalidReason.empty() && !disabled.sender.policy.enabled &&
            !disabled.sender.policy.mediaEnabled && !disabled.sender.policy.experimentalQuote &&
            !disabled.commandPipe && !disabled.sendTrace && !disabled.sendContextTrace &&
            !disabled.sendSubmitTrace && !disabled.mediaTrace && !disabled.mediaReceive,
            "flags require exactly one character 1");

    const Environment single{{L"WECHATBOT_NATIVE_SEND", L"1"}, {L"WECHATBOT_SEND_TARGET", L"wxid_one"},
                             {L"WECHATBOT_SEND_TEXT", L"你好🙂"}};
    const auto one = LoadIrisConfigFrom(L"D:\\runtime", Reader(single));
    Require(one.sender.policy.targetId == "wxid_one" && one.sender.policy.text == "你好🙂" &&
            one.sender.policy.targetIds.empty() && !one.sender.policy.continuous, "single send UTF-8 policy");

    for (const auto* path : {L"relative", L"C:relative", L"\\rooted", L"C:\\bad?name",
                             L"C:\\runtime\\NUL.txt", L"C:\\runtime. ", L"\\\\.\\pipe\\name"})
        Require(NormalizeAbsolutePath(path).empty(), "invalid absolute path rejected");
    Require(NormalizeAbsolutePath(std::wstring(32768, L'a')).empty(), "path capacity boundary");
    std::wstring longestPath = L"D:\\";
    while (longestPath.size() < 32767) {
        const auto remaining = 32767 - longestPath.size();
        longestPath.append(remaining > 255 ? 255 : remaining, L'a');
        if (longestPath.size() < 32767) longestPath += L'\\';
    }
    Require(!NormalizeAbsolutePath(longestPath).empty(), "32767-character absolute path accepted");
    RequireRejected(LoadIrisConfigFrom(longestPath, Reader(empty)), "log_path_invalid");
    Require(NormalizeAbsolutePath(L"C:\\数据\\x\\..\\runtime\\") == L"C:\\数据\\runtime", "unicode path normalization");
    auto invalid = configured;
    invalid[L"WECHATBOT_RUNTIME_ROOT"] = L"relative";
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "runtime_root_invalid");
    invalid = configured;
    invalid[L"WECHATBOT_RUNTIME_ROOT"] = L"\\\\server\\share\\runtime";
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "runtime_root_invalid");
    invalid = configured;
    invalid[L"WECHATBOT_MEDIA_ROOT"] = L"relative";
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "media_root_invalid");
    invalid = configured;
    invalid[L"WECHATBOT_MEDIA_INBOUND_ROOT"] = std::wstring(32768, L'a');
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "media_inbound_root_invalid");
    invalid = configured;
    invalid[L"WECHATBOT_MEDIA_ROOT"] = std::wstring(L"C:\\bad") + L'\0' + L"path";
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "media_root_invalid");
    invalid = configured;
    invalid[L"WECHATBOT_SEND_ACCOUNT"] = std::wstring(1, static_cast<wchar_t>(0xD800));
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "send_account_invalid");
    invalid = single;
    invalid[L"WECHATBOT_SEND_TEXT"] = std::wstring(8191, L'a');
    Require(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)).invalidReason.empty(), "8191 UTF-16 characters accepted");
    invalid[L"WECHATBOT_SEND_TEXT"] = std::wstring(8192, L'a');
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "send_text_invalid");
    invalid[L"WECHATBOT_SEND_TEXT"] = std::wstring(2730, L'中');
    Require(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)).invalidReason.empty(), "UTF-8 under 8192 bytes accepted");
    invalid[L"WECHATBOT_SEND_TEXT"] += L"aa";
    Require(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)).sender.policy.text.size() == 8192,
            "exact UTF-8 8192-byte boundary accepted");
    invalid[L"WECHATBOT_SEND_TEXT"] = std::wstring(2731, L'中');
    RequireRejected(LoadIrisConfigFrom(L"D:\\runtime", Reader(invalid)), "send_text_invalid");

    // Runtime root discovery walks up from the module's directory until it finds
    // a marker file (CMakeLists.txt / WeixinMonitor.sln) or a directory named
    // IRIS; without a marker it falls back to <module directory>/runtime.
    // The scratch directory is per process so concurrent or repeated runs cannot
    // collide, and it is removed before and after use.
    // NOTE: multi-level paths are written with forward slashes. A Windows string
    // such as "build\\Release\\bin" would be one file name containing
    // backslashes, not three nested directories, so nothing would be created and
    // the ancestor scan would silently fall through to a parent project.
    const auto temporary = std::filesystem::temp_directory_path() /
        ("wechatbot-observer-config-test-" + std::to_string(GetCurrentProcessId()));
    const auto projectRoot = temporary / "project";
    const auto markedRoot = projectRoot / "marked";
    const auto plainRoot = temporary / "plain";
    std::error_code cleanupError;
    std::filesystem::remove_all(temporary, cleanupError);
    std::filesystem::create_directories(projectRoot / "build/Release/bin", cleanupError);
    std::filesystem::create_directories(markedRoot / "build/Release/bin", cleanupError);
    std::filesystem::create_directories(plainRoot / "build/Release/bin", cleanupError);
    { std::ofstream(projectRoot / "CMakeLists.txt") << "# project marker\n"; }
    { std::ofstream(markedRoot / "WeixinMonitor.sln") << "\n"; }
    Require(std::filesystem::exists(projectRoot / "CMakeLists.txt"),
            "temporary project marker was written");
    Require(std::filesystem::exists(markedRoot / "WeixinMonitor.sln"),
            "temporary solution marker was written");

    // With a marker directly above the module, discovery stops at that project
    // instead of continuing towards the drive root.
    Require(FindIrisRuntimeRoot((projectRoot / "build/Release/bin/IRIS.dll").wstring()) ==
            (projectRoot / "runtime").wstring(), "runtime root derives from the project root marker");
    Require(FindIrisRuntimeRoot((markedRoot / "build/Release/bin/IRIS.dll").wstring()) ==
            (markedRoot / "runtime").wstring(), "solution marker also identifies the project root");

    // The no-marker fallback applies only when no ancestor carries a project
    // marker. That is environment dependent: the CMake presets point TEMP/TMP at
    // <repository>/build/observer-cmake/tmp (to avoid MSVC D8050 on non-ASCII
    // temp paths), so under CTest the scratch directory sits inside this very
    // repository and the scan legitimately stops at the repository root. Resolve
    // the expected enclosing project first, then assert against it.
    std::error_code ancestorError;
    const auto temporaryCanonical = std::filesystem::weakly_canonical(temporary, ancestorError);
    fs::path enclosingProject;
    for (auto current = temporaryCanonical; !current.empty(); current = current.parent_path()) {
        if (std::filesystem::exists(current / "CMakeLists.txt") ||
            std::filesystem::exists(current / "WeixinMonitor.sln") ||
            current.filename() == L"IRIS") {
            enclosingProject = current;
            break;
        }
        if (!current.has_relative_path() || current == current.parent_path()) break;
    }

    const auto modulePath = plainRoot / "build/Release/bin/IRIS.dll";
    const auto discovered = FindIrisRuntimeRoot(modulePath.wstring());
    if (!enclosingProject.empty()) {
        const auto expectedRuntime = (enclosingProject / "runtime").wstring();
        Require(_wcsicmp(discovered.c_str(), expectedRuntime.c_str()) == 0,
                "ancestor project marker selects that project's runtime directory");
        printf("  ancestor scan: enclosing project at %ls -> runtime %ls\n",
               enclosingProject.wstring().c_str(), discovered.c_str());
    } else {
        const auto expectedFallback = (plainRoot / "build/Release/bin/runtime").wstring();
        if (discovered != expectedFallback) {
            // Written to a UTF-8 file: console redirection re-encodes wide text
            // with the active code page and would corrupt non-ASCII temp paths.
            std::ofstream report("observer-config-fallback.txt", std::ios::binary);
            auto dump = [&report](const char* label, const std::wstring& value) {
                report << label;
                for (const wchar_t ch : value) report << static_cast<char>(ch);  // ASCII paths only
                report << "\n";
            };
            report << "temporary_bytes=" << temporary.u8string().size() << "\n";
            dump("discovered=", discovered);
            dump("expected=", expectedFallback);
            report.close();
        }
        // Compare as paths, not as raw wide strings: operator/ appends with the
        // preferred separator ('\'), while expectedFallback carries the literal
        // '/' from the "build/Release/bin/runtime" component above. The two
        // strings then differ by one character although they name the same path,
        // which made this branch fail on any host whose TEMP lies outside the
        // repository (including CI runners). path comparison is element-wise and
        // therefore separator-agnostic.
        Require(fs::path(discovered) == fs::path(expectedFallback),
                "no marker falls back to the module directory");
        Require(FindIrisRuntimeRoot((plainRoot / "IRIS.dll").wstring()) ==
                (plainRoot / "runtime").wstring(), "portable layout uses the DLL directory");
        puts("  ancestor scan: no marker above TEMP, module-directory fallback verified");
    }
    std::filesystem::remove_all(temporary, cleanupError);
    std::map<std::wstring, unsigned> reads;
    const auto once = [&reads](const wchar_t* name) { ++reads[name]; return std::wstring{}; };
    Require(LoadIrisConfigFrom(L"D:\\runtime", once).invalidReason.empty(), "counted reader accepted");
    Require(reads.size() == 17, "all observer settings read");
    for (const auto& [name, count] : reads) Require(count == 1, "each setting read once");
    RequireRejected(LoadIrisConfigFrom(L"relative", Reader(empty)), "runtime_root_invalid");
    puts("Observer configuration tests passed: injected environment, flags, roots, paths, and bounds.");
}
