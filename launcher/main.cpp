/**
 * @file main.cpp
 * @brief 启动器 (Launcher) 程序入口
 * @details 
 *   启动时将直接调用 RunLauncher()
 *   完成环境校验、微信进程创建、DLL 远程注入以及加载结果验证
 */

#include "launcher.hpp"

/**
 * @brief 使用wmain入口处理中文字符问题
 * @return 0 表示微信拉起并成功注入观察者 DLL；非 0 表示启动或注入失败
 */
int wmain() { 
    return wechatbot::launcher::RunLauncher(); 
}
