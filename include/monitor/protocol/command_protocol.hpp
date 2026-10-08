#pragma once
#include "monitor/send/send_capabilities.hpp"
#include "monitor/send/send_gate.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace wechatbot::monitor {

/// 单条命令协议帧载荷的最大字节限制 (64KB)
inline constexpr uint32_t kCommandFrameLimit = 65536;

/**
 * @brief 命令协议执行器回调函数表
 */
struct CommandHandlers {
    std::function<NativeSendCapabilities()> probe;      ///< 探测发信能力与微信登录态的回调
    std::function<SendResult(const SendCommand&)> send;  ///< 执行发信指令的回调
};

/**
 * @brief 解析并分发单条 JSON 命令协议请求 (支持依赖注入)
 * @details 
 *   解析请求文本并执行严格校验：
 *   - `hello` / `hello_media`: 调用 handlers.probe() 生成能力快照响应；
 *   - `send_text` / `send_rich_text` / `send_media`: 校验参数并在会话一致时调用 handlers.send()；
 *   - 语法错误、未知字段或参数越界时直接返回标准的 JSON 错误响应。
 * @param payload 接收到的原始 JSON 文本
 * @param session 当前 DLL 运行实例的唯一会话 ID
 * @param handlers 绑定的能力探测与发信回调函数
 * @return std::string 格式化好的 JSON 响应字符串
 */
std::string HandleCommand(std::string_view payload, const std::string& session,
                          const CommandHandlers& handlers);

/**
 * @brief 原生后端便捷分发入口（内部自动绑定全局 NativeSender）
 */
std::string HandleCommand(std::string_view payload, const std::string& session);

} // namespace wechatbot::monitor
