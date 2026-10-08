#include "monitor/send/send_gate.hpp"
#include "monitor/send/recipient.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace wechatbot::monitor {
namespace {
// Windows FILETIME 基础时间单位：1 tick = 100 纳秒，每秒包含 10,000,000 ticks
constexpr uint64_t kTicksPerSecond = 10000000ULL;
// 指令最大有效生命周期：600 秒（10 分钟）
constexpr uint64_t kMaximumLifetime = 600ULL * kTicksPerSecond;
// 指令过期后的去重缓存保留时间：60 秒，用于防止过期后网络延迟重发导致重复执行
constexpr uint64_t kRetentionAfterExpiry = 60ULL * kTicksPerSecond;
}

/**
 * @brief 校验媒体发送指令参数的合法性
 * @details 
 *   - 路径不可为空且长度不超过 4096 字节，不能包含空字符 ('\0')；
 *   - SHA-256 必须为标准的 64 位十六进制小写字符串；
 *   - 图片 (image)：大小在 0 ~ 20MB 之间，不可指定音频时长 durationMs；
 *   - 语音 (voice)：大小在 0 ~ 1MB 之间，时长在 20ms ~ 60000ms (60秒) 之间，
 *     且必须为 20ms 的整数倍（对应微信 SILK 编码的单帧时间步长）。
 */
bool IsValidMediaCommand(const MediaCommand& media) {
    if (media.path.empty() || media.path.size() > 4096 || media.path.find('\0') != std::string::npos ||
        media.sha256.size() != 64 || !std::all_of(media.sha256.begin(), media.sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) return false;
    if (media.kind == "image") return media.bytes > 0 && media.bytes <= 20 * 1024 * 1024 && !media.durationMs;
    return media.kind == "voice" && media.bytes > 0 && media.bytes <= 1024 * 1024 &&
        media.durationMs > 0 && media.durationMs <= 60000 && media.durationMs % 20 == 0;
}

/**
 * @brief 解析 ISO-8601 UTC 格式的时间戳字符串为 Windows FILETIME ticks (100ns)
 * @param text 格式如 "2026-10-08T12:00:00Z" 或带有亚秒 ".123456Z"
 * @param[out] ticks 解析成功后输出对应的 100 纳秒时间步长值
 * @return true 解析成功；false 格式不合法或超出有效范围
 */
bool ParseCommandTime(const std::string& text, uint64_t& ticks) {
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') return false;
    const auto number = [&text](size_t at, size_t size) -> int {
        int value = 0;
        for (size_t i = at; i < at + size; ++i) {
            if (text[i] < '0' || text[i] > '9') return -1;
            value = value * 10 + text[i] - '0';
        }
        return value;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (year < 1601 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    size_t pos = 19;
    uint64_t fraction = 0;
    if (text[pos] == '.') {
        ++pos;
        const auto begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (pos - begin >= 6) return false;
            fraction = fraction * 10 + text[pos++] - '0';
        }
        if (pos == begin) return false;
        // 补齐至 7 位数字（1 秒 = 10,000,000 个 100ns）
        for (size_t digits = pos - begin; digits < 7; ++digits) fraction *= 10;
    }
    if (text.substr(pos) != "Z" && text.substr(pos) != "+00:00") return false;
    SYSTEMTIME time{};
    time.wYear = static_cast<WORD>(year); time.wMonth = static_cast<WORD>(month);
    time.wDay = static_cast<WORD>(day); time.wHour = static_cast<WORD>(hour);
    time.wMinute = static_cast<WORD>(minute); time.wSecond = static_cast<WORD>(second);
    FILETIME file{};
    if (!SystemTimeToFileTime(&time, &file)) return false;
    ticks = (static_cast<uint64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    ticks += fraction;
    return true;
}

/**
 * @brief 获取当前高精度的系统 UTC 时间（FILETIME 100ns 单位）
 */
uint64_t CommandNow() {
    FILETIME file{}; GetSystemTimePreciseAsFileTime(&file);
    return (static_cast<uint64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
}

/**
 * @brief 校验指令时间有效性窗口（防重放与过期拦截）
 * @param command 待校验的发送指令
 * @param now 当前高精度系统时间（FILETIME ticks）
 * @return SendResult 若状态为空表示校验通过；若不为空表示被拒并附带原因
 */
SendResult ValidateSendTime(const SendCommand& command, uint64_t now) {
    uint64_t created = 0, expires = 0;
    if (!ParseCommandTime(command.createdAt, created) || !ParseCommandTime(command.expiresAt, expires) ||
        expires <= created) return {"rejected", "invalid_timestamp", "Invalid UTC command lifetime"};
    if (expires - created > kMaximumLifetime)
        return {"rejected", "invalid_timestamp", "Command lifetime exceeds 600 seconds"};
    if (created > now) return {"rejected", "command_not_yet_valid", "created_at is in the future"};
    if (expires <= now) return {"rejected", "command_expired", "Command expired before native start"};
    return {"", "", ""};
}

/**
 * @brief 检查当前策略配置是否完整可用
 */
bool SendGate::PolicyReady() const {
    if (!policy_.enabled || policy_.accountId.empty()) return false;
    if (!policy_.continuous)
        return IsValidRecipient(policy_.targetId) && !IsGroupTarget(policy_.targetId) &&
               !policy_.text.empty() && policy_.text.size() <= 1024;
    if (!policy_.maxRecords) return false;
    return std::any_of(policy_.targetIds.begin(), policy_.targetIds.end(), [](const std::string& target) {
        return IsValidRecipient(target);
    });
}

/**
 * @brief 检查指定接收方是否在策略允许的白名单内
 */
bool SendGate::TargetAllowed(const std::string& target) const {
    if (!IsValidRecipient(target)) return false;
    if (!policy_.continuous) return !IsGroupTarget(target) && target == policy_.targetId;
    return std::find(policy_.targetIds.begin(), policy_.targetIds.end(), target) !=
           policy_.targetIds.end();
}

/**
 * @brief 清理已超过保留时间的去重历史记录（仅在 continuous 模式下执行）
 */
void SendGate::CleanupExpired(uint64_t now) const {
    if (!policy_.continuous) return;
    for (auto item = records_.begin(); item != records_.end();) {
        if (item->second.retainUntil <= now) item = records_.erase(item);
        else ++item;
    }
}

/**
 * @brief 探测当前安全门是否就绪且可接受新的发信调用
 */
bool SendGate::Available() const {
    return Available(CommandNow(), GetTickCount64());
}

/**
 * @brief 探测当前安全门在给定时间点是否就绪
 * @param now 当前 UTC 时间戳 (FILETIME ticks)
 * @param monotonicMs 单调递增系统运行时间 (毫秒)，用于频控冷却计算
 */
bool SendGate::Available(uint64_t now, uint64_t monotonicMs) const {
    std::lock_guard lock(mutex_);
    if (!PolicyReady() || busy_) return false;
    CleanupExpired(now);
    if (!policy_.continuous) return records_.empty();
    if (records_.size() >= policy_.maxRecords) return false;
    if (!hasLastStart_) return true;
    return monotonicMs >= lastStartMs_ &&
           monotonicMs - lastStartMs_ >= policy_.minIntervalMs;
}

/**
 * @brief 执行发送请求的主入口，提供频控、鉴权、幂等去重与生命周期校验
 */
SendResult SendGate::Run(const SendCommand& command, uint64_t now,
                         const std::function<SendResult()>& submit) {
    return Run(command, now, GetTickCount64(), submit);
}

/**
 * @brief 带单调时间戳的 Run 实现
 * @details 
 *   执行步骤：
 *   1. 【时间窗口校验】：检查 createdAt 与 expiresAt，拦截过期或未来指令；
 *   2. 【加锁并审查状态】：
 *      - 检查策略开关是否启用；
 *      - 清理超期历史记录；
 *      - 幂等去重：若 requestId 已存在且内容完全一致，直接返回之前的结果；若内容不同，报 request_conflict；
 *      - 账号与目标校验：确保指令所属微信号与当前已登录微信一致，且接收人在白名单中；
 *      - 媒体/引用/艾特参数专项校验；
 *      - 频控冷却校验：连续发送模式下检查距离上次发起的毫秒数是否满足 minIntervalMs；
 *      - 标记 busy_ = true 并记录初始状态为 in_progress；
 *   3. 【解锁并执行提交闭包】：脱离互斥锁调用底层的 submit()，防止阻塞其他查询；
 *   4. 【重新加锁归档结果】：将 submit 的最终状态回写至去重缓存，解除 busy_ 标志并返回。
 */
SendResult SendGate::Run(const SendCommand& command, uint64_t now, uint64_t monotonicMs,
                         const std::function<SendResult()>& submit) {
    // 步骤 1: 校验指令生命周期窗口
    auto lifetime = ValidateSendTime(command, now);
    if (!lifetime.status.empty()) return lifetime;
    uint64_t expires = 0;
    ParseCommandTime(command.expiresAt, expires); // 前面已校验过格式，此处必定成功

    {
        std::lock_guard lock(mutex_);
        if (!policy_.enabled) return {"rejected", "native_sender_unavailable", "Native sending is disabled"};
        
        // 步骤 2: 空闲状态下先清理过期的去重记录
        if (!busy_) CleanupExpired(now);

        // 步骤 3: 幂等去重检查 (Idempotency)
        const auto duplicate = records_.find(command.requestId);
        if (duplicate != records_.end()) {
            if (duplicate->second.command == command) return duplicate->second.result;
            return {"rejected", "request_conflict", "Request content or attempt identity differs"};
        }

        // 步骤 4: 登录账号与白名单目标校验
        if (policy_.accountId.empty() || policy_.accountId != command.accountId)
            return {"rejected", "account_not_verified", "Command account differs from the configured account"};
        if (!TargetAllowed(command.targetId))
            return {"rejected", "target_not_allowed", "Target is not an allowed private account or chatroom"};

        // 步骤 5: 媒体、艾特、引用合法性审查
        if (command.media) {
            if (!policy_.continuous || !policy_.mediaEnabled)
                return {"rejected", "native_media_sender_unavailable", "Media sending is disabled"};
            if (!IsValidMediaCommand(*command.media) || !command.text.empty() ||
                !command.atUserList.empty() || command.quote)
                return {"rejected", "invalid_media", "Invalid or mixed media command"};
        }
        if (!IsValidMentionList(command.atUserList) || (!command.atUserList.empty() &&
            (!policy_.continuous || !IsGroupTarget(command.targetId))))
            return {"rejected", "invalid_mentions", "Mentions require a chatroom and 1..16 distinct private member IDs"};
        if (command.quote && (!policy_.continuous || !IsValidQuote(*command.quote, command.targetId)))
            return {"rejected", "invalid_quote", "Quote requires a valid original text from the same conversation"};
        if (command.quote && !policy_.experimentalQuote)
            return {"rejected", "native_quote_sender_unavailable", "Experimental quote sender is disabled"};

        // 步骤 6: 发送模式与文本长度审查
        if (policy_.continuous) {
            if (!command.media && (command.text.empty() || command.text.size() > 16384))
                return {"rejected", "text_not_allowed", "Continuous text must be 1..16384 UTF-8 bytes"};
            if (command.origin != "manual" && command.origin != "ai" && command.origin != "game")
                return {"rejected", "origin_not_allowed", "Unsupported continuous-send origin"};
        } else {
            // 单次首测模式：仅允许严格匹配配置中设定的测试文本和人工触发
            if (policy_.text.empty() || command.text != policy_.text || command.text.size() > 1024)
                return {"rejected", "text_not_allowed", "Text differs from the configured first-test text"};
            if (command.origin != "manual")
                return {"rejected", "origin_not_allowed", "This first-test backend accepts manual commands only"};
            if (!records_.empty())
                return {"rejected", "single_send_limit", "One command has already been reserved in this session"};
        }

        // 步骤 7: 并发限制与频控冷却 (Rate Limiting)
        if (busy_) return {"rejected", "busy", "Another native send is in progress"};
        if (policy_.continuous && hasLastStart_ &&
            (monotonicMs < lastStartMs_ || monotonicMs - lastStartMs_ < policy_.minIntervalMs))
            return {"rejected", "rate_limited", "Native send cooldown has not elapsed"};
        if (policy_.continuous && records_.size() >= policy_.maxRecords)
            return {"rejected", "capacity_unavailable", "Request deduplication capacity is full"};

        // 步骤 8: 登记预留记录（防止防重窗期间并发穿透）
        const uint64_t retainUntil = expires > (std::numeric_limits<uint64_t>::max)() -
            kRetentionAfterExpiry ? (std::numeric_limits<uint64_t>::max)() :
            expires + kRetentionAfterExpiry;
        records_.emplace(command.requestId, Record{
            command, {"unknown", "in_progress", "The command may be executing"}, retainUntil});
        busy_ = true;
        if (policy_.continuous) {
            hasLastStart_ = true;
            lastStartMs_ = monotonicMs;
        }
    }

    // 步骤 9: 释放互斥锁，实际执行底层原生提交
    SendResult result;
    try { result = submit(); }
    catch (...) { result = {"unknown", "dispatch_exception", "An exception occurred after reserving the command"}; }

    // 步骤 10: 重新加锁，将执行结果更新到去重记录中并解除 busy 状态
    std::lock_guard lock(mutex_);
    busy_ = false;
    auto record = records_.find(command.requestId);
    if (record == records_.end())
        return {"unknown", "request_record_missing", "Reserved request result could not be stored"};
    record->second.result = std::move(result);
    return record->second.result;
}
} // namespace wechatbot::monitor
