#pragma once
/**
 * @file runtime.hpp
 * @brief 观察者运行时生命周期管理与停机协调接口
 * @details 
 *   定义 DLL 模块生命周期的启动、停止信号通知，以及逆序等待各工作子系统退出的核心 API。
 */

#include "monitor/core/platform.hpp"

namespace wechatbot::monitor {

/// @brief 全局停机标志位 (0: 运行中, 1: 请求停机)，使用原子互锁操作访问
extern volatile LONG g_stop;

/**
 * @brief 启动观察者运行时监控线程
 * @param IRISModule 本观察者 DLL (IRIS.dll) 的模块句柄
 * @note 
 *   仅在确认当前进程为微信主进程后，派生独立的后台 ObserverThread 线程执行重量级初始化。
 *   绝不在 DllMain 的 Loader Lock 作用域内直接执行挂钩或网络操作。
 */
void StartIRIS(HMODULE IRISModule);

/**
 * @brief 向所有工作线程广播停机信号
 * @details 原子将 g_stop 置为 1，促使主循环与管道轮询退出
 */
void SignalStop();

/**
 * @brief 优雅停机并逆序 Join 所有工作子系统
 * @details 
 *   按以下严格依赖顺序依次安全停止各个后台工作组件：
 *   1. 停止命名管道服务端 (不再接收新的外部指令)
 *   2. 等待原生发信任务空闲/完成 (确保已进入微信原生调用的任务安全返回)
 *   3. 停止多媒体接收管线
 *   4. 停止异步日志刷盘线程并排空队列
 *   5. 关闭并刷新 JSONL 事件日志文件
 * 
 * @param timeoutMs 各子系统 Join 操作的超时预算时间 (毫秒)，传 INFINITE 表示无限等待
 * @return true 所有组件均在超时前成功安全退出；false 发生超时 (仍有部分资源保持活动，便于调用方重试)
 * @warning 必须在 DllMain 之外调用，绝不可在 DLL_PROCESS_DETACH 中同步阻塞等待！
 */
bool StopObserverWorkers(DWORD timeoutMs) noexcept;

} // namespace wechatbot::monitor
