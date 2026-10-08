#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/send/send_gate.hpp"
#include "monitor/send/send_capabilities.hpp"

namespace wechatbot::monitor {

struct NativeSenderConfig;

/**
 * @brief 初始化微信原生发信后端 (Native Sender Backend)
 * @details 
 *   由 Observer 工作线程在完成目标进程环境核验 (VerifyTarget) 之后、挂载消息观察钩子之前单次调用。
 *   主要完成：
 *   1. 校验目标模块基址 (base) 与发信函数签名特征码；
 *   2. 创建常驻进程生命周期的 Backend 适配器实例（绑定 UiDispatcher 与 SendService）。
 * @param module WeChatWin.dll 的模块句柄（基地址）
 * @param config 原生发送配置（包括发信安全策略、多媒体缓存目录等）
 */
void InitializeNativeSender(HMODULE module, const NativeSenderConfig& config);

/**
 * @brief 探测当前原生发送后端的能力与状态
 * @return NativeSendCapabilities 返回微信号、登录就绪态以及各项发送功能是否开放
 */
NativeSendCapabilities ProbeNativeSender();

/**
 * @brief 执行一条发信指令
 * @details 经由 SendGate 校验与限流，通过 UiDispatcher 调度至微信 Qt UI 线程执行原生对象构造与投递
 * @param command 发送指令包 (文本/媒体/艾特/引用)
 * @return SendResult 执行结果（accepted/rejected/failed）
 */
SendResult RunNativeSender(const SendCommand& command);

/**
 * @brief 阻塞等待原生发送后端排空所有在途任务 (Shutdown Barrier)
 * @details 由工作线程退出流程调用（绝对严禁在 DllMain Loader Lock 下调用）。
 *          关闭新任务准入，并阻塞等待已派发到 UI 线程的任务执行结束或超时。
 * @param timeoutMs 最长等待毫秒数
 * @return true 后端已空闲；false 等待超时
 */
bool WaitNativeSenderIdle(DWORD timeoutMs) noexcept;

} // namespace wechatbot::monitor
