#include "monitor/native/target.hpp"
#include "monitor/core/memory.hpp"
#include <cstdio>
#include <cstring>

namespace wechatbot::monitor {

HMODULE g_weixin = nullptr;
namespace {
HMODULE g_realVersion = nullptr;
INIT_ONCE g_versionInit = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK InitRealVersion(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 13)
        return FALSE;
    wcscat_s(systemDirectory, L"\\version.dll");
    g_realVersion = LoadLibraryW(systemDirectory);
    return g_realVersion != nullptr;
}

FARPROC ResolveRealVersion(const char* name) {
    InitOnceExecuteOnce(&g_versionInit, InitRealVersion, nullptr, nullptr);
    return g_realVersion ? GetProcAddress(g_realVersion, name) : nullptr;
}
bool GetModuleFileVersion(HMODULE module, WORD& major, WORD& minor, WORD& build, WORD& revision) {
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto sizeFn = reinterpret_cast<SizeFn>(ResolveRealVersion("GetFileVersionInfoSizeW"));
    auto infoFn = reinterpret_cast<InfoFn>(ResolveRealVersion("GetFileVersionInfoW"));
    auto queryFn = reinterpret_cast<QueryFn>(ResolveRealVersion("VerQueryValueW"));
    if (!sizeFn || !infoFn || !queryFn)
        return false;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH))
        return false;
    DWORD ignored = 0;
    const DWORD size = sizeFn(path, &ignored);
    if (!size)
        return false;
    void* block = HeapAlloc(GetProcessHeap(), 0, size);
    if (!block)
        return false;
    bool ok = false;
    if (infoFn(path, 0, size, block)) {
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoSize = 0;
        if (queryFn(block, L"\\", reinterpret_cast<void**>(&info), &infoSize) &&
            info && infoSize >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == 0xFEEF04BD) {
            major = HIWORD(info->dwFileVersionMS);
            minor = LOWORD(info->dwFileVersionMS);
            build = HIWORD(info->dwFileVersionLS);
            revision = LOWORD(info->dwFileVersionLS);
            ok = true;
        }
    }
    HeapFree(GetProcessHeap(), 0, block);
    return ok;
}
} // namespace

bool VerifyTarget(HMODULE module, std::string& reason) {
    WORD major = 0, minor = 0, build = 0, revision = 0;
    if (!GetModuleFileVersion(module, major, minor, build, revision)) {
        reason = "unable to read Weixin.dll file version";
        return false;
    }
    char version[96]{};
    sprintf_s(version, "%u.%u.%u.%u", major, minor, build, revision);
    if (major != kVersionParts[0] || minor != kVersionParts[1] ||
        build != kVersionParts[2] || revision != kVersionParts[3]) {
        reason = "unexpected Weixin.dll version: ";
        reason += version;
        return false;
    }
    __try {
        const auto base = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            reason = "invalid DOS header";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.SizeOfImage != kExpectedImageSize) {
            reason = "PE identity mismatch";
            return false;
        }
        if (!VerifyEntry(base + kReceiveBatchRva, kReceiveEntry, sizeof(kReceiveEntry))) {
            reason = "receive entry signature mismatch";
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = "exception while validating target image";
        return false;
    }
    return true;
}

bool VerifyEntry(const void* address, const uint8_t* expected, size_t length) {
    __try {
        return IsReadableRange(address, length) && memcmp(address, expected, length) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace wechatbot::monitor
