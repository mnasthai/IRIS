#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/send/send_gate.hpp"
#include <functional>
#include <string>

namespace wechatbot::monitor {
using EnvironmentReader = std::function<std::wstring(const wchar_t*)>;
using ProjectRootProbe = std::function<bool(const std::wstring&)>;

struct NativeSenderConfig {
    SendPolicy policy;
    std::wstring mediaRoot;
};

struct IrisConfig {
    std::wstring logDirectory, logPath;
    NativeSenderConfig sender;
    bool commandPipe = false, sendTrace = false, sendContextTrace = false;
    bool sendSubmitTrace = false, mediaTrace = false, mediaReceive = false;
    std::wstring mediaInboundRoot;
    std::wstring runtimeRoot;
    // Invalid configuration is a value, not an exception or a silent fallback.
    // The bootstrap must reject it before opening logs, files, or native hooks.
    std::string invalidReason;
};

std::wstring ReadObserverEnvironment(const wchar_t* name);
// Returns empty for relative, malformed, device, or over-limit paths.
std::wstring NormalizeAbsolutePath(const std::wstring& path);
// Runtime root discovery walks up from the DLL directory until it finds a
// project marker (CMakeLists.txt / WeixinMonitor.sln, or a directory named
// IRIS) and returns that root's runtime subdirectory. Without a marker it
// falls back to <DLL directory>/runtime.
std::wstring FindIrisRuntimeRoot(const std::wstring& modulePath);
// defaultRuntimeRoot is already a runtime directory, not a project root.
IrisConfig LoadIrisConfigFrom(const std::wstring& defaultRuntimeRoot,const EnvironmentReader& reader);
IrisConfig LoadObserverConfig(HMODULE observerModule);
} // namespace wechatbot::monitor
