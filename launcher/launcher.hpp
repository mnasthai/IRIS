#pragma once
/**
 * @file launcher.hpp
 * @brief 微信观察者注入启动器公共接口
 * @details 
 *   定义启动器配置、远程模块加载探测状态机以及注入主流程函数。
 *   主要负责安全拉起微信官方客户端 (Weixin.exe) 并通过远程线程将 IRIS.dll 注入目标进程。
 */

#include "monitor/core/platform.hpp"
#include "monitor/config/IRISconfig.hpp"
#include <string>
#include <cstdint>

namespace wechatbot::launcher {

/// @brief 目标 DLL 模块文件名；由构建系统注入，避免与产物名不一致
#ifdef MONITOR_OBSERVER_MODULE
inline constexpr wchar_t kIRISName[] = MONITOR_OBSERVER_MODULE;
#else
inline constexpr wchar_t kIRISName[] = L"IRIS.dll";
#endif

/// @brief 微信可执行文件名的固定默认路径（可用 WECHATBOT_WEIXIN_EXE 覆盖）
inline constexpr wchar_t kDefaultWeixinExe[] =
    L"C:\\Program Files (x86)\\Tencent\\WeChat\\Weixin.exe";

/// @brief 远程进程模块探测状态枚举
enum class ModuleProbeState { 
    Found,              ///< 成功在目标进程中找到了指定路径的 DLL 模块
    Missing,            ///< 模块在目标进程中不存在 (尚未加载)
    SnapshotFailed,     ///< 创建 Toolhelp 模块快照失败 (进程可能已退出或无权限)
    EnumerationFailed   ///< 遍历模块链表过程中发生错误
};

/// @brief 远程模块探测结果详情
struct ModuleProbe {
    ModuleProbeState state = ModuleProbeState::Missing; ///< 探测状态
    DWORD error = ERROR_SUCCESS;                        ///< Win32 错误码 (GetLastError)
    std::wstring sameNamePath;                          ///< 若存在同名但不同路径的模块，记录其实际路径 (防注入劫持/错位)
};

/// @brief 启动器运行配置
struct LauncherConfig {
    std::wstring weixinExe;         ///< 微信主程序 Weixin.exe 的规范化绝对路径
    std::wstring weixinDirectory;   ///< 微信主程序所在的安装目录
    // std::wstring obsoleteProxy;     ///< 旧版版本 DLL 路径 (version.dll，检测防冲突)
    std::string invalidReason;      ///< 配置非法时的错误原因描述
};

/**
 * @brief 读取并校验启动器配置
 * @param reader std::function
 * @return LauncherConfig
 */
LauncherConfig LoadLauncherConfig(const wechatbot::monitor::EnvironmentReader& reader);

/**
 * @brief 获取当前启动器程序所在的绝对目录路径
 * @return std::wstring 目录绝对路径 (末尾不含反斜杠)
 */
std::wstring GetLauncherDirectory();

/**
 * @brief 在远程目标进程中查询指定模块的加载基地址 (Module Base Address)
 * @param processId 目标进程的 PID
 * @param moduleName 模块文件名 (如 L"kernel32.dll")
 * @return uintptr_t 模块基地址 (为 0 表示未找到或查询失败)
 */
uintptr_t FindRemoteModuleBase(DWORD processId, const wchar_t* moduleName);

/**
 * @brief 根据内存函数指针反查其宿主模块 (DLL) 的基地址与名称
 * @param address 函数指针内存地址 (例如 LoadLibraryW 的函数指针)
 * @param[out] moduleBase 查找到的宿主模块基地址
 * @param[out] moduleName 查找到的宿主模块文件名
 * @return true 查询成功；false 失败
 */
bool GetOwningModule(const void* address, uintptr_t& moduleBase, std::wstring& moduleName);

/**
 * @brief 探测远程进程是否已经加载了指定路径的模块
 * @param processId 目标进程 PID
 * @param expectedPath 预期的 DLL 绝对路径
 * @return ModuleProbe 探测详细结果
 */
ModuleProbe ProbeRemoteModule(DWORD processId, const wchar_t* expectedPath);

/**
 * @brief 循环等待直到远程进程完全加载目标观察者 DLL，或超时报错
 * @param process 目标进程句柄
 * @param processId 目标进程 PID
 * @param observerPath 观察者 DLL 的完整路径
 * @param threadExitCode 远程注入线程 (LoadLibraryW) 的退出码
 * @param[out] error 若加载失败，存储详细的诊断诊断信息
 * @return true 确认模块已在远程进程模块表中就绪；false 失败或超时
 */
bool WaitForObserverModule(HANDLE process, DWORD processId, const std::wstring& observerPath,
                           DWORD threadExitCode, std::wstring& error);

/**
 * @brief 启动器总入口流程
 * @details 
 *   执行完整生命周期：加载配置 -> 检查防重 -> 拉起微信 -> 远程注入 DLL -> 验证挂载 -> 打印就绪信息
 * @return int 退出码：0 成功，非 0 失败
 */
int RunLauncher();

} // namespace wechatbot::launcher
