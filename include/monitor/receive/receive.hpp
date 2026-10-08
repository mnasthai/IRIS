#pragma once
/**
 * @file receive.hpp
 * @brief 微信批量消息接收 (ReceiveBatch) MinHook 拦截接口
 * @details 
 *   微信网络同步模块在收到聊天消息包时，会调用底层的批量消息处理函数。
 *   本模块通过 MinHook 挂钩该函数，在调用原逻辑前透明捕获消息快照并压入异步日志队列。
 */

#include "monitor/receive/receive_snapshot.hpp"

namespace wechatbot::monitor {

/**
 * @brief 微信原生批量接收函数签名定义
 * @note 
 *   采用 x64 MSVC 默认的 __fastcall 调用约定：
 *   - RCX: context 微信内部上下文指针
 *   - RDX: vector 指向包含 AddMsg 结构体的 std::vector<AddMsg> 容器指针
 *   - R8:  flag3 批处理控制标志位 1
 *   - R9:  flag4 批处理控制标志位 2
 *   返回值: 状态字节 (通常非 0 表示成功处理)
 */
using ReceiveBatchFn = unsigned char(__fastcall*)(void*, void*, unsigned char, unsigned char);

/// @brief 保存微信被覆盖前的原始函数跳板指针 (Trampoline)
extern ReceiveBatchFn g_originalReceiveBatch;

/**
 * @brief MinHook 拦截函数入口
 * @details 
 *   1. 保存 entryError = GetLastError()，防止探测逻辑污染系统错误码；
 *   2. 调用 ObserveBatch 解析 vector 消息数据并排队至日志环形缓冲区；
 *   3. 恢复 entryError，调用原始微信逻辑 g_originalReceiveBatch；
 *   4. 捕获返回值并组装 ReturnValue 事件，恢复原调用的 GetLastError() 并返回。
 */
unsigned char __fastcall HookReceiveBatch(void* context, void* vector,
                                          unsigned char flag3, unsigned char flag4);

/**
 * @brief 启用接收消息观测 Hook
 * @param mediaTraceEnabled 是否同时启用多媒体追踪观测
 * @return true 挂钩成功；false 挂钩失败
 */
bool EnableReceiveObservation(bool mediaTraceEnabled);

} // namespace wechatbot::monitor
