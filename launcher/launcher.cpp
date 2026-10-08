#include "launcher.hpp"
#include "monitor/config/version_config.hpp"
#include "monitor/config/IRISconfig.hpp"
#include <tlhelp32.h>

#include <cwchar>
#include <string>
#include <filesystem>

namespace wechatbot::launcher {

/**
 * @brief 解析并校验启动器运行配置
 * @details 优先从环境变量读取 WECHATBOT_WEIXIN_EXE 若未配置则回退到 kDefaultWeixinExe
 *          校验文件是否存在且文件名必须为 "Weixin.exe"
 */
LauncherConfig LoadLauncherConfig(const wechatbot::monitor::EnvironmentReader& reader) {
    LauncherConfig config;
    if (!reader){
        config.invalidReason = "environment_reader_missing";
        return config;
    }
    const auto overridePath = reader(L"WECHATBOT_WEIXIN_EXE");
    config.weixinExe = wechatbot::monitor::NormalizeAbsolutePath(
        overridePath.empty() ? kDefaultWeixinExe : overridePath);
    const std::filesystem::path executable(config.weixinExe);
    if (config.weixinExe.empty() || _wcsicmp(executable.filename().c_str(), L"Weixin.exe") != 0) {
        config.weixinExe.clear();
        config.invalidReason = "weixin_exe_invalid";
        return config;
    }
    config.weixinDirectory = executable.parent_path().wstring();
    // // 检查之前的 version.dll（已经舍弃） 代理注入劫持文件 防冲突
    // config.obsoleteProxy = (executable.parent_path() / L"version.dll").wstring();
    return config;
}

/**
 * @brief 获取启动器自身所在的物理目录路径
 * @details 调用 GetModuleFileNameW(nullptr, ...) 获取当前 exe 完整路径并剥离文件名
 */
std::wstring GetLauncherDirectory() {
    wchar_t path[32768]{};  // MAX_PATH = 260 字符限制
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path))
        return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return {};
    *slash = 0; // *slash指向最后一个\的地址 *slash = 0 直接就地写入 \0 截断 截断文件名 保留目录部分
    return path;
}

/**
 * @brief 比较两个文件路径是否完全一致 (不区分大小写)
 */
bool SamePath(const wchar_t* left, const wchar_t* right) {
    const auto fullLeft = wechatbot::monitor::NormalizeAbsolutePath(left ? left : L"");
    const auto fullRight = wechatbot::monitor::NormalizeAbsolutePath(right ? right : L"");
    return !fullLeft.empty() && !fullRight.empty() && _wcsicmp(fullLeft.c_str(), fullRight.c_str()) == 0;
}

/**
 * @brief 检测指定路径的微信进程是否已在系统中运行
 * @details 
 *   通过 Windows Toolhelp32 API (CreateToolhelp32Snapshot) 遍历当前系统所有活跃进程
 *   若发现同名进程且其物理路径完全匹配 说明微信已被拉起
 *   必须完全关闭现有微信后再行拉起 以确保捕获完整的启动与初始化阶段
 */
bool IsTargetAlreadyRunning(const std::wstring& executablePath) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Weixin.exe") != 0)
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length) &&
                SamePath(path, executablePath.c_str()))
                found = true;
            CloseHandle(process);
            if (found)
                break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

/**
 * @brief 通过 Toolhelp32 快照在指定远程进程中查找特定模块的加载基地址
 * @param processId 远程进程 PID
 * @param moduleName 待查找的模块名
 * @return uintptr_t 模块基地址 (若未找到返回 0)
 */
uintptr_t FindRemoteModuleBase(DWORD processId, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               processId);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32W entry{sizeof(entry)};
    uintptr_t result = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

/**
 * @brief 根据内存中的函数指针反查其所在的宿主模块信息
 * @details 
 *   在现代 Windows (Win10/Win11) 中 某些 API 可能被转发至 API-Set 虚拟 DLL
 *   (如 api-ms-win-core-libraryloader-*) 不能盲目假设 LoadLibraryW 一定来自 kernel32.dll
 *   使用 GetModuleHandleExW 传入地址可精准解析出真正拥有该代码段的 DLL 模块
 */
bool GetOwningModule(const void* address, uintptr_t& moduleBase,
                     std::wstring& moduleName) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return false;

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;

    const wchar_t* name = wcsrchr(path, L'\\');
    moduleName.assign(name ? name + 1 : path);
    moduleBase = reinterpret_cast<uintptr_t>(module);
    return true;
}

/**
 * @brief 探测远程进程是否已经加载了指定路径的模块
 * @details 
 *   通过 Toolhelp32 快照遍历远程进程的模块链表
 *   若路径完全匹配则判定为 Found；若存在同名模块但路径不符（例如加载了系统目录下的同名文件）
 *   记录 sameNamePath 以便定位潜在的 DLL 劫持或路径错乱问题
 */
ModuleProbe ProbeRemoteModule(DWORD processId, const wchar_t* expectedPath) {
    ModuleProbe probe;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        probe.state = ModuleProbeState::SnapshotFailed;
        probe.error = GetLastError();
        return probe;
    }
    MODULEENTRY32W entry{sizeof(entry)};
    const wchar_t* expectedName = wcsrchr(expectedPath, L'\\');
    expectedName = expectedName ? expectedName + 1 : expectedPath;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (SamePath(entry.szExePath, expectedPath)) {
                probe.state = ModuleProbeState::Found;
                break;
            }
            if (_wcsicmp(entry.szModule, expectedName) == 0)
                probe.sameNamePath = entry.szExePath;
        } while (Module32NextW(snapshot, &entry));
        if (probe.state != ModuleProbeState::Found) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                probe.state = ModuleProbeState::EnumerationFailed;
                probe.error = error;
            }
        }
    } else {
        probe.state = ModuleProbeState::EnumerationFailed;
        probe.error = GetLastError();
    }
    CloseHandle(snapshot);
    return probe;
}

std::wstring HexCode(DWORD value) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08lX", value);
    return buffer;
}

const wchar_t* ProbeStateName(ModuleProbeState state) {
    switch (state) {
    case ModuleProbeState::Found: return L"found";
    case ModuleProbeState::SnapshotFailed: return L"snapshot_failed";
    case ModuleProbeState::EnumerationFailed: return L"enumeration_failed";
    default: return L"module_absent";
    }
}

/**
 * @brief 轮询等待目标 DLL 出现在远程进程的模块链表中
 * @details 
 *   由于 Windows 加载器在执行 DllMain 时具有异步并发特性，CreateRemoteThread 返回不代表模块已经
 *   被完整登记在 Toolhelp 模块链表中。本函数最多重试 100 次（约 2 秒），以确认模块真正装载完毕。
 */
bool WaitForObserverModule(HANDLE process, DWORD processId, const std::wstring& observerPath,
                           DWORD threadExitCode, std::wstring& error) {
    ModuleProbe probe;
    DWORD processExit = STILL_ACTIVE;
    DWORD processError = ERROR_SUCCESS;
    unsigned attempts = 0;
    for (; attempts < 100; ++attempts) {
        if (!GetExitCodeProcess(process, &processExit)) {
            processError = GetLastError();
            break;
        }
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
            break;
        probe = ProbeRemoteModule(processId, observerPath.c_str());
        if (probe.state == ModuleProbeState::Found)
            return true;
        // 模块列表可能在快照期间正在被系统修改，仅在瞬态错误时重试
        if (probe.error != ERROR_SUCCESS && probe.error != ERROR_BAD_LENGTH &&
            probe.error != ERROR_PARTIAL_COPY && probe.error != ERROR_NO_MORE_FILES)
            break;
        Sleep(20);
    }
    error = L"Observer verification failed: pid=" + std::to_wstring(processId) +
        L", thread_exit32=" + HexCode(threadExitCode) + L", probe=" + ProbeStateName(probe.state) +
        L", win32=" + std::to_wstring(probe.error) + L", process_exit=" + HexCode(processExit) +
        L", process_query_error=" + std::to_wstring(processError) +
        L", attempts=" + std::to_wstring(attempts);
    if (!probe.sameNamePath.empty())
        error += L", same_name_path=" + probe.sameNamePath;
    error += L"\nExpected DLL: " + observerPath;
    return false;
}

/**
 * @brief 核心注入函数：将观察者 DLL 注入微信进程
 * @details 
 *   远程线程注入 (Classic Remote Thread Injection)
 *   
 *   1. 【OpenProcess】以必要特权（创建线程、VM读写操作、查询信息）打开微信目标进程
 *   2. 【动态解析 LoadLibraryW】获取本地 LoadLibraryW 地址，并根据 GetOwningModule 计算其相对宿主 DLL 偏移
 *   3. 【对齐远程基地址】轮询等待微信进程中该系统 DLL 初始化就绪，利用相对偏移算出远程进程中的 LoadLibraryW 地址
 *   4. 【VirtualAllocEx】在微信虚拟内存中为观察者 DLL 路径字符串申请内存空间
 *   5. 【WriteProcessMemory】将本地 DLL 绝对路径写入微信内存
 *   6. 【CreateRemoteThread】让微信以远程路径为入参，派生线程调用 LoadLibraryW
 *   7. 【WaitForSingleObject & VirtualFreeEx】等待注入线程执行完毕，释放申请的远程字符串内存
 *   8. 【WaitForObserverModule】通过 Toolhelp 快照双重确认目标 DLL 确实已加载就绪
 */
bool LoadIRIS(DWORD processId, const std::wstring& IRISPath,
                  std::wstring& error) {
    // 步骤 1: 打开微信进程
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ,
                                 FALSE, processId);
    if (!process) {
        error = L"OpenProcess failed, error=" + std::to_wstring(GetLastError());
        return false;
    }

    // 步骤 2: 解析 LoadLibraryW 及其所属模块
    const HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");  // kernel32.dll 的首地址
    const uintptr_t localLoadLibrary = localKernel32  // LoadLibraryW 偏移 无符号指针整类型 为了后续计算
    // 处理可能的 API-Set 转发
        ? reinterpret_cast<uintptr_t>(GetProcAddress(localKernel32, "LoadLibraryW"))  //找到具体的 KERNELBASE.dll 模块(函数)
        : 0; // 否则返回 0;
    uintptr_t localOwnerBase = 0;
    std::wstring ownerName;
    if (!localLoadLibrary ||
        !GetOwningModule(reinterpret_cast<const void*>(localLoadLibrary),
                         localOwnerBase, ownerName)) {
        error = L"Could not resolve local LoadLibraryW";
        CloseHandle(process);
        return false;
    }

    // 步骤 3: 处理微信程序 等待并获取远程微信进程中相同模块的基址
    uintptr_t remoteOwnerBase = 0;  // weixin

    // 轮询等待 等待微信的ntdll初始化完成
    for (unsigned attempt = 0; attempt < 500 && !remoteOwnerBase; ++attempt) {
        // 如果微信程序挂了 程序就应该是 进程 "未激活Not Signaled" --> "内核对象已激发Signaled" 其中就完全不阻塞 WAIT_OBJECT = 0
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(process, &exitCode);
            error = L"Weixin exited before " + ownerName +
                    L" became available, exit=" + std::to_wstring(exitCode);
            break;
        }
        remoteOwnerBase = FindRemoteModuleBase(processId, ownerName.c_str());  //遍历微信模块列表 找出 KERNELBASE.dll 的首地址
        if (!remoteOwnerBase)
            Sleep(10);
    }
    // 超时
    if (!remoteOwnerBase) {
        if (error.empty())
            error = L"Timed out waiting for remote " + ownerName;
        CloseHandle(process);
        return false;
    }
    // 远程 LoadLibraryW 地址 = 远程模块基址 + 本地函数相对偏移
    const auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteOwnerBase + (localLoadLibrary - localOwnerBase));

    // 步骤 4: 在远程微信中申请内存存放 DLL 路径
    const SIZE_T byteCount = (IRISPath.size() + 1) * sizeof(wchar_t);  // 构建dll 字节大小 (size + 终止符(+1)) * wcahr
    // 申请一块地址
    void* remotePath = VirtualAllocEx(process, nullptr, byteCount,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        error = L"VirtualAllocEx failed, error=" + std::to_wstring(GetLastError());
        CloseHandle(process);
        return false;
    }

    // 步骤 5: 将 DLL 路径写入微信内存
    SIZE_T written = 0;
    // 跨进程
    if (!WriteProcessMemory(process, remotePath, IRISPath.c_str(), byteCount,
                            &written) || written != byteCount) {
        error = L"WriteProcessMemory failed, error=" + std::to_wstring(GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // 步骤 6: 启动微信进程执行线程 LoadLibraryW(remotePath)
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary,
                                       remotePath, 0, nullptr);
    if (!thread) {
        error = L"CreateRemoteThread failed, error=" + std::to_wstring(GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // 步骤 7: 等待远程线程执行完成 (最多 15 秒)
    const DWORD wait = WaitForSingleObject(thread, 15000);
    const DWORD waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    DWORD exitCode = 0;
    const BOOL gotExitCode = GetExitCodeThread(thread, &exitCode);
    const DWORD exitError = gotExitCode ? ERROR_SUCCESS : GetLastError();
    CloseHandle(thread);

    // 释放远程路径内存
    if (wait == WAIT_OBJECT_0)
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    if (wait != WAIT_OBJECT_0 || !gotExitCode) {
        error = L"Remote LoadLibraryW wait failed: wait=" + HexCode(wait) +
                L", win32=" + std::to_wstring(waitError) +
                L", thread_query_error=" + std::to_wstring(exitError);
        CloseHandle(process);
        return false;
    }

    // 步骤 8: 确认目标模块已完全挂载入微信模块表
    const bool loaded = WaitForObserverModule(process, processId, IRISPath, exitCode, error);
    CloseHandle(process);
    return loaded;
}

/**
 * @brief 启动器全流程主入口
 * @details
 *   1. 读取并校验 Launcher 配置与 IRIS 配置
 *   2. 检查 Weixin.exe 与 IRIS.dll 文件完整性
 *   3. 检测微信是否已经在运行 (防多实例冲突)
 *   4. 调用 CreateProcessW 拉起全新微信客户端
 *   5. 执行 LoadObserver 完成注入与模块挂载校验
 *   6. 成功后打印微信 PID 与事件日志文件路径
 */
int RunLauncher() {
    // 加载启动器配置确定运行环境参数
    const auto config = LoadLauncherConfig(wechatbot::monitor::ReadObserverEnvironment);
    if (!config.invalidReason.empty()) {
        fprintf(stderr, "！启动器配置出错了 %s\n", config.invalidReason.c_str());
        return 1;
    }
    const std::wstring directory = GetLauncherDirectory();
    if (directory.empty()) {
        fwprintf(stderr, L"！无法确定启动器目录\n");
        return 1;
    }
    // 确定运行是目录
    const std::wstring IRISPath = directory + L"\\" + kIRISName;
    const auto runtimeRoot = wechatbot::monitor::FindIrisRuntimeRoot(IRISPath);
    // 加载IRIS配置
    const auto iris_config = wechatbot::monitor::LoadIrisConfigFrom(
        runtimeRoot, wechatbot::monitor::ReadObserverEnvironment);
    if (!iris_config.invalidReason.empty()) {
        fprintf(stderr, "! IRIS配置失败: %s\n", iris_config.invalidReason.c_str());
        return 1;
    }

    if (GetFileAttributesW(config.weixinExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Weixin.exe 不存在: %ls\n", config.weixinExe.c_str());
        return 1;
    }
    if (GetFileAttributesW(IRISPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"IRIS DLL 不存在: %ls\n", IRISPath.c_str());
        return 1;
    }

    // if (GetFileAttributesW(config.obsoleteProxy.c_str()) != INVALID_FILE_ATTRIBUTES) {
    //     fwprintf(stderr,
    //              L"Remove the unused proxy before continuing: %ls\n"
    //              L"The explicit launcher does not use an app-local version.dll.\n",
    //              config.obsoleteProxy.c_str());
    //     return 1;
    // }

    if (IsTargetAlreadyRunning(config.weixinExe)) {
        fwprintf(stderr,
                 L"Weixin.exe 已经在运行 请完全退出后在尝试运行\n");
        return 1;
    }

    // 创建微信进程
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring commandLine = L"\"";
    commandLine += config.weixinExe;
    commandLine += L"\"";
    if (!CreateProcessW(config.weixinExe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, config.weixinDirectory.c_str(), &startup, &process)) {
        fwprintf(stderr, L"CreateProcessW failed, error=%lu\n", GetLastError());
        return 1;
    }

    // 执行注入
    std::wstring error;
    const bool loaded = LoadIRIS(process.dwProcessId, IRISPath, error);
    if (!loaded) {
        // 注入失败则清理终止已启动的微信进程
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        fwprintf(stderr, L"IRIS load failed: %ls\n", error.c_str());
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }

    // 成功打印信息
    wprintf(L"IRIS loaded. Weixin PID=%lu\n", process.dwProcessId);
    wprintf(L"Log: %ls\n", iris_config.logPath.c_str());
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

} // namespace wechatbot::launcher
