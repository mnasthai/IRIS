/**
 * @file dllmain.cpp
 * @brief DLL (IRIS.dll) 的入口点
 * @details
 *   当操作系统的模块加载器调用 DllMain 时，会持有进程级的全局加载锁 Loader Lock
 *   在此锁保护期间
 *   - 严禁调用任何可能导致线程同步等待的操作 如 WaitForSingleObject, Join
 *   - 严禁调用可能间接加载其他 DLL 的 API
 *   - 严禁初始化复杂的套接字或网络/管道通信
 * 
 *   因此
 *   - DLL_PROCESS_ATTACH 通过 CreateThread 迅速脱离 Loader Lock将所有重量级初始化交由独立的 ObserverThread 执行
 *
 *   - DLL_PROCESS_DETACH 仅保留一个标记全局停机信号 SignalStop()
 */


#include "monitor/runtime/runtime.hpp"

/**
 * @brief Windows 动态库入口主函数
 * @param module 当前被加载的 DLL 实例句柄
 * @param reason 触发调用的原因枚举 (ATTACH / DETACH 等)
 * @param lpReserved 隐式/显式加载保留标志
 * @return BOOL 始终返回 TRUE 表示加载成功
 */
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    using namespace wechatbot::monitor;
    if (reason == DLL_PROCESS_ATTACH) {
        // 优化性能 通知系统当本进程创建/销毁线程时 无需回调本 DLL 的 DllMain (忽略 ATTACH/DETACH 通知)
        DisableThreadLibraryCalls(module);

        // 引导线程 退出 DllMain 释放 Loader Lock
        StartIRIS(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        // 在 Loader Lock 作用域下严禁等待线程或卸载 MinHook 钩子！
        // 仅标记停机信号
        SignalStop();
    }
    return TRUE;
}
