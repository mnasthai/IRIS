#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/protocol/command_protocol.hpp"
#include <string>

namespace wechatbot::monitor {

/**
 * @brief 运行命名管道命令服务端主循环
 * @details 
 *   执行步骤：
 *   1. 获取当前进程用户令牌并组装 SDDL 访问控制描述符（仅允许 SYSTEM 与当前用户读写）；
 *   2. 创建重叠异步具名管道 `\\.\pipe\wechatbot-<session>`；
 *   3. 循环等待客户端连接；
 *   4. 读取 4 字节包长及 JSON 命令请求；
 *   5. 调用 HandleCommand 解析并分发指令；
 *   6. 回写 4 字节包长及 JSON 响应，并有界等待客户端读取完毕后断开；
 *   7. 收到 stop 停止信号或超时后退出。
 * @param name 管道名称 (如 L"\\\\.\\pipe\\wechatbot-xxxxxxxx")
 * @param session 当前观察者会话 ID 字符串
 * @param stop 全局原子停止标记
 * @param durationMs 最大运行持续毫秒数 (0 表示无限循环直至收到 stop 信号)
 * @param logReady 是否输出 command_pipe_ready 诊断就绪日志
 * @return true 正常退出；false 初始化失败
 */
bool RunCommandPipe(const std::wstring& name, const std::string& session,
                    volatile LONG& stop, DWORD durationMs = 0, bool logReady = false);

/**
 * @brief 启动命名管道后台监听线程
 * @param enabled 是否启用管道监听
 * @return true 启动成功；false 启动失败
 */
bool StartCommandPipe(bool enabled);

/**
 * @brief 停止命名管道监听并阻塞等待线程退出
 * @details 必须在 DllMain 之外调用，超时时保留线程资源防止崩溃
 * @param timeoutMs 最长等待毫秒数
 * @return true 成功退出并汇合；false 等待超时
 */
bool StopCommandPipe(DWORD timeoutMs) noexcept;

} // namespace wechatbot::monitor
