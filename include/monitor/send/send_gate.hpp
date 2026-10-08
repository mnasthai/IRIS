#pragma once
/**
 * @file send_gate.hpp
 * @brief 发送门控中枢：负责发信鉴权、白名单过滤、时间戳检验、频率冷却与幂等防重
 * @details 
 *   SendGate 是主动发信的第一道安全防线。
 *   在发信指令触及微信核心对象前，严格执行前置校验，防止因网络重发、程序死循环或非法参数
 *   导致的高频轰炸或异常调用风险。
 */

#include "monitor/core/platform.hpp"
#include "monitor/send/quote.hpp"
#include <optional>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wechatbot::monitor {

/// @brief 多媒体发信指令载荷 (图片或语音)
struct MediaCommand {
    std::string kind;           ///< 媒体类型: "image" 或 "voice"
    std::string path;           ///< 媒体文件的本地物理绝对路径
    std::string sha256;         ///< 媒体文件 SHA-256 校验哈希 (64字符十六进制)
    uint64_t bytes = 0;         ///< 文件字节尺寸 (图片上限 20MB, 语音上限 1MB)
    uint32_t durationMs = 0;    ///< 语音时长 (毫秒，要求 20ms 的整数倍且 <= 60000ms)
    bool operator==(const MediaCommand&) const = default;
};

/**
 * @brief 校验多媒体发信命令参数是否合法
 * @param media 待校验的媒体指令
 * @return true 合法；false 包含非法字符、超长或不满足约束
 */
bool IsValidMediaCommand(const MediaCommand& media);

/// @brief 发信命令请求数据结构 (对应客户端传入的 JSON 指令对象)
struct SendCommand {
    std::string requestId;          ///< 客户端全局唯一请求 ID (用于去重)
    std::string attemptId;          ///< 单次重试尝试 ID
    std::string sessionId;          ///< 观察者会话 ID (由 hello 命令下发，校验是否为当前会话)
    std::string accountId;          ///< 期望登录的微信账号 ID (wxid，防止切号误发)
    std::string targetId;           ///< 发送目标联系人或群聊 ID (wxid_xxx 或 xxx@chatroom)
    std::string text;               ///< 发送的正文文本 (UTF-8 编码，上限 16384 字节)
    std::string createdAt;          ///< 客户端创建时间 (UTC ISO-8601 格式)
    std::string expiresAt;          ///< 客户端过期时间 (UTC ISO-8601 格式)
    std::string origin;             ///< 触发来源 (如 "manual", "auto_reply")
    std::string sourceEventKey;     ///< 关联的触发源事件键 (可选)
    std::string atUserList;         ///< 群聊时需要 @ 的群成员 wxid 列表 (逗号分隔)
    std::optional<QuoteText> quote; ///< 引用回复卡片元数据 (可选)
    std::optional<MediaCommand> media; ///< 多媒体参数 (可选，发图/语音时使用)
    bool operator==(const SendCommand&) const = default;
};

/// @brief 发信结果结构体
struct SendResult {
    std::string status = "rejected"; ///< 状态: "accepted"(成功), "rejected"(拒绝), "unknown"(超时或异常)
    std::string code;                ///< 机器可读原因码 (如 "invalid_timestamp", "rate_limited")
    std::string detail;              ///< 人类可读详细错误描述
};

/// @brief 发信安全策略配置
struct SendPolicy {
    bool enabled = false;               ///< 是否允许原生发信 (环境变量 WECHATBOT_NATIVE_SEND=1)
    std::string accountId;              ///< 允许发信的绑定账号 ID
    std::string targetId;               ///< 单次模式允许的目标联系人
    std::string text;                   ///< 单次模式允许的固定文本
    bool continuous = false;            ///< 是否启用持续多发模式 (WECHATBOT_SEND_CONTINUOUS=1)
    std::vector<std::string> targetIds; ///< 持续模式下的允许接收人白名单列表
    uint64_t minIntervalMs = 1000;      ///< 最小发信冷却时间间隔 (毫秒，默认 1000ms)
    size_t maxRecords = 4096;           ///< 幂等去重哈希表最大保留记录数
    bool experimentalQuote = false;     ///< 是否开启实验性引用回复 (WECHATBOT_EXPERIMENTAL_QUOTE=1)
    bool mediaEnabled = false;          ///< 是否启用多媒体发送功能
};

/**
 * @brief 解析 ISO-8601 UTC 字符串为 Windows FILETIME 100纳秒刻度
 * @param text ISO-8601 字符串 (如 "2026-10-08T12:00:00Z" 或带有毫秒小数)
 * @param[out] ticks 转换后的 Windows 100ns 刻度值
 * @return true 解析成功；false 格式非法
 */
bool ParseCommandTime(const std::string& text, uint64_t& ticks);

/// @brief 获取当前系统精确 UTC 时间 (返回 Windows 100ns 刻度)
uint64_t CommandNow();

/**
 * @brief 校验发信命令的时间窗口合法性
 * @details 要求：created_at < expires_at，生命周期 <= 600秒，未过期且不可为未来时间
 */
SendResult ValidateSendTime(const SendCommand& command, uint64_t now);

/**
 * @brief 发信安全门控控制器 (SendGate)
 * @details 
 *   控制发信准入条件：
 *   - 单次模式 (Single Mode): 仅允许在 DLL 整个生命周期内向固定目标发送一次单条消息；
 *   - 持续模式 (Continuous Mode): 允许在白名单受限范围内连续发信，并强制执行 1 秒冷却和 4096 条去重表。
 */
class SendGate {
public:
    explicit SendGate(SendPolicy policy) : policy_(std::move(policy)) {}

    /// @brief 检查当前门控是否空闲可用 (冷却期已过且无并发执行中的发信)
    bool Available() const;
    bool Available(uint64_t now, uint64_t monotonicMs) const;

    /**
     * @brief 经由门控安全检查并执行发信动作
     * @param command 发信命令参数
     * @param now 当前系统 UTC 时间戳刻度
     * @param submit 真正的发信提交闭包 (在门控通过后触发)
     * @return SendResult 综合校验与执行结果
     */
    SendResult Run(const SendCommand& command, uint64_t now,
                   const std::function<SendResult()>& submit);

    SendResult Run(const SendCommand& command, uint64_t now, uint64_t monotonicMs,
                   const std::function<SendResult()>& submit);

private:
    struct Record {
        SendCommand command;
        SendResult result;
        uint64_t retainUntil = 0; // 该去重记录过期淘汰的时间戳
    };

    bool PolicyReady() const;
    bool TargetAllowed(const std::string& target) const;
    void CleanupExpired(uint64_t now) const;

    SendPolicy policy_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, Record> records_;
    bool busy_ = false;
    bool hasLastStart_ = false;
    uint64_t lastStartMs_ = 0;
};

} // namespace wechatbot::monitor
