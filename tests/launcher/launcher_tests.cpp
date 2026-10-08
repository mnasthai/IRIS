// Only inspect this test process. Never launch or inject into Weixin.
#include "launcher.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
using namespace wechatbot::launcher;

void Require(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s, Win32=%lu\n", message, GetLastError());
        exit(1);
    }
}

int wmain() {
    const auto noEnvironment = [](const wchar_t*) { return std::wstring{}; };
    const auto defaults = LoadLauncherConfig(noEnvironment);
    Require(defaults.invalidReason.empty() &&
            defaults.weixinExe == L"C:\\Program Files (x86)\\Tencent\\WeChat\\Weixin.exe",
            "documented default executable path retained");
    const auto alternate = LoadLauncherConfig([](const wchar_t* name) {
        return wcscmp(name, L"WECHATBOT_WEIXIN_EXE") == 0 ? L"D:/微信/Weixin/Weixin.exe" : std::wstring{};
    });
    Require(alternate.invalidReason.empty() && alternate.weixinExe == L"D:\\微信\\Weixin\\Weixin.exe" &&
            alternate.weixinDirectory == L"D:\\微信\\Weixin", "explicit executable path and installation directory");
    for (const auto* invalid : {L"Weixin.exe", L"D:Weixin.exe", L"D:\\other.exe", L"D:\\bad?path\\Weixin.exe"}) {
        const auto rejected = LoadLauncherConfig([invalid](const wchar_t*) { return std::wstring(invalid); });
        Require(rejected.invalidReason == "weixin_exe_invalid" && rejected.weixinExe.empty(),
                "invalid override has no default fallback");
    }
    const DWORD self = GetCurrentProcessId();
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    wchar_t kernelPath[MAX_PATH]{};
    Require(kernel && GetModuleFileNameW(kernel, kernelPath, MAX_PATH), "local system module path");
    Require(ProbeRemoteModule(self, kernelPath).state == ModuleProbeState::Found,
            "exact loaded module lookup");
    const auto missingPath = GetLauncherDirectory() + L"\\no-such-module.dll";
    auto missing = ProbeRemoteModule(self, missingPath.c_str());
    Require(missing.state == ModuleProbeState::Missing && !missing.error,
            "successful snapshot with absent module is distinct");
    auto failed = ProbeRemoteModule(0xFFFFFFFF, kernelPath);
    Require(failed.state != ModuleProbeState::Found && failed.error != ERROR_SUCCESS,
            "invalid PID retains snapshot error");
    uintptr_t ownerBase = 0;
    std::wstring ownerName;
    auto address = GetProcAddress(kernel, "LoadLibraryW");
    Require(GetOwningModule(reinterpret_cast<const void*>(address), ownerBase, ownerName),
            "LoadLibraryW actual owner resolves");
    Require(FindRemoteModuleBase(self, ownerName.c_str()) == ownerBase, "owner matches enumerated module");
    std::wstring error;
    Require(WaitForObserverModule(GetCurrentProcess(), self, kernelPath, 0, error),
            "module identity succeeds independently of truncated DWORD return");

    const std::wstring observer = GetLauncherDirectory() + L"\\" + kIRISName;
    HMODULE loaded = LoadLibraryW(observer.c_str());
    Require(loaded != nullptr, "built observer DLL loads in benign test host");
    Require(ProbeRemoteModule(self, observer.c_str()).state == ModuleProbeState::Found,
            "observer file path matches module enumeration");
    Require(FreeLibrary(loaded) != FALSE, "test host releases observer");
    puts("Launcher tests passed: local DLL load, path lookup, module absence/errors, owner resolution.");
    return 0;
}
