#include "monitor/protocol/command_protocol.hpp"
#include "monitor/core/json_writer.hpp"
#include "monitor/config/version_profile.hpp"
#include <map>
#include <array>
#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace wechatbot::monitor {
namespace {

/// JSON 基础标量类型
enum class Type { Text, Number, Boolean, Null };

/// 解析后的基础标量值
struct Value {
    Type type;
    std::string text;
};

using Fields = std::map<std::string, Value>;

/// 协议支持的操作动词
enum class Operation {
    Hello,          ///< 基础探测发信能力与登录微信号
    HelloMedia,     ///< 扩展探测多媒体（图片/语音）发信能力
    SendText,       ///< 发送纯文本消息
    SendRichText,   ///< 发送富文本（含艾特 @成员 或 引用回复卡片）
    SendMedia       ///< 发送多媒体消息（图片或语音）
};

struct HelloRequest {
    Operation operation;
    std::string requestId;
};

struct SendRequest {
    SendCommand command;
};

using Request = std::variant<HelloRequest, SendRequest>;

std::optional<Operation> ReadOperation(std::string_view name) {
    if (name == "hello") return Operation::Hello;
    if (name == "hello_media") return Operation::HelloMedia;
    if (name == "send_text") return Operation::SendText;
    if (name == "send_rich_text") return Operation::SendRichText;
    if (name == "send_media") return Operation::SendMedia;
    return std::nullopt;
}

bool IsHello(Operation operation) {
    return operation == Operation::Hello || operation == Operation::HelloMedia;
}

/**
 * @brief 零依赖轻量递归下降扁平 JSON 解析器 (Parser)
 * @details 
 *   设计与安全性特性：
 *   - 专为 IPC 命令协议定制，仅接受扁平键值对 JSON 对象（v1 协议规范）；
 *   - 限制键值对上限不超过 32 个，防止超长对象消耗内存；
 *   - 拒绝重复 Key，消除歧义解析；
 *   - 完整支持 Unicode 转义符（包括 UTF-16 代理对如 Emoji 表情 `\uD83D\uDE0A` 正确转为 4 字节 UTF-8）；
 *   - 遇到任何语法不符立即抛出 std::invalid_argument 快速失败。
 */
class Parser {
    std::string_view input;
    size_t pos = 0;

    [[noreturn]] void Bad() const { throw std::invalid_argument("invalid flat JSON request"); }
    char Peek() const { return pos < input.size() ? input[pos] : '\0'; }
    void Space() { while (Peek() == ' ' || Peek() == '\t' || Peek() == '\r' || Peek() == '\n') ++pos; }
    void Take(char c) { if (Peek() != c) Bad(); ++pos; }

    /// 解析 4 位十六进制字符 (\uXXXX)
    uint32_t Hex() {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = Peek();
            uint32_t digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (digit > 15) Bad();
            ++pos; value = value * 16 + digit;
        }
        return value;
    }

    /// 将 21 位 Unicode 码点编码为 UTF-8 字节并追加至字符串
    void Utf8(std::string& text, uint32_t code) {
        if (code <= 0x7f) text += static_cast<char>(code);
        else if (code <= 0x7ff) {
            text += static_cast<char>(0xc0 | (code >> 6)); text += static_cast<char>(0x80 | (code & 63));
        } else if (code <= 0xffff) {
            text += static_cast<char>(0xe0 | (code >> 12)); text += static_cast<char>(0x80 | ((code >> 6) & 63));
            text += static_cast<char>(0x80 | (code & 63));
        } else {
            text += static_cast<char>(0xf0 | (code >> 18)); text += static_cast<char>(0x80 | ((code >> 12) & 63));
            text += static_cast<char>(0x80 | ((code >> 6) & 63)); text += static_cast<char>(0x80 | (code & 63));
        }
    }

    /// 解析带引号的 JSON 字符串（支持各种转义序列及代理对）
    std::string String() {
        Take('"'); std::string result;
        while (Peek() != '"') {
            const unsigned char c = static_cast<unsigned char>(Peek());
            if (c < 0x20) Bad(); // 禁止未转义的控制字符
            ++pos;
            if (c != '\\') { result += static_cast<char>(c); continue; }
            const char escape = Peek(); if (!escape) Bad(); ++pos;
            switch (escape) {
            case '"': case '\\': case '/': result += escape; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                uint32_t code = Hex();
                // 处理 UTF-16 代理项高半区 (High Surrogate)
                if (code >= 0xd800 && code <= 0xdbff) {
                    Take('\\'); Take('u'); const uint32_t low = Hex();
                    if (low < 0xdc00 || low > 0xdfff) Bad();
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) Bad();
                Utf8(result, code); break;
            }
            default: Bad();
            }
        }
        Take('"'); return result;
    }

    /// 解析字段值 (字符串、字面量 true/false/null、整数)
    Value ReadValue() {
        if (Peek() == '"') return {Type::Text, String()};
        for (const auto literal : {"true", "false", "null"}) {
            const std::string_view word(literal);
            if (input.substr(pos, word.size()) == word) {
                pos += word.size(); return {word == "null" ? Type::Null : Type::Boolean, std::string(word)};
            }
        }
        const size_t start = pos;
        if (Peek() < '0' || Peek() > '9') Bad();
        if (Peek() == '0') ++pos;
        else while (Peek() >= '0' && Peek() <= '9') ++pos;
        return {Type::Number, std::string(input.substr(start, pos - start))};
    }

public:
    explicit Parser(std::string_view value) : input(value) {}

    Fields Parse() {
        Fields fields; Space(); Take('{'); Space();
        if (Peek() == '}') { ++pos; Space(); if (pos != input.size()) Bad(); return fields; }
        for (;;) {
            if (fields.size() >= 32) Bad(); // 限制最多 32 个字段
            auto key = String(); Space(); Take(':'); Space(); auto value = ReadValue();
            if (!fields.emplace(std::move(key), std::move(value)).second) Bad(); // 拒绝重复 key
            Space(); if (Peek() == '}') { ++pos; break; }
            Take(','); Space();
        }
        Space(); if (pos != input.size()) Bad(); return fields;
    }
};

/**
 * @brief 严格白名单校验请求中的所有键名，杜绝未识别或拼写错误的字段
 */
void ValidateKeys(const Fields& fields, Operation operation) {
    constexpr std::array<std::string_view, 3> base{"op", "protocol_version", "request_id"};
    constexpr std::array<std::string_view, 9> send{
        "attempt_id", "observer_session_id", "expected_account_id", "target_id", "text",
        "created_at", "expires_at", "origin", "source_event_key"};
    constexpr std::array<std::string_view, 10> rich{
        "at_user_list", "quote_message_id", "quote_from_id", "quote_to_id", "quote_sender_id",
        "quote_conversation_id", "quote_text", "quote_timestamp", "quote_msg_source", "quote_message_type"};
    constexpr std::array<std::string_view, 5> media{
        "media_kind", "media_path", "media_sha256", "media_bytes", "duration_ms"};

    for (const auto& [key, value] : fields) {
        if (std::find(base.begin(), base.end(), key) != base.end()) continue;
        if (!IsHello(operation) && std::find(send.begin(), send.end(), key) != send.end() &&
            (operation != Operation::SendMedia || key != "text")) continue;
        if (operation == Operation::SendRichText && std::find(rich.begin(), rich.end(), key) != rich.end()) continue;
        if (operation == Operation::SendMedia && std::find(media.begin(), media.end(), key) != media.end()) continue;
        throw std::invalid_argument("unknown or unsupported request field");
    }
}

std::string Text(const Fields& fields, const char* key, size_t max = kCommandFrameLimit,
                 bool allowEmpty = false) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.type != Type::Text || (!allowEmpty && it->second.text.empty()) ||
        it->second.text.size() > max || it->second.text.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string("invalid ") + key);
    return it->second.text;
}

uint64_t Unsigned(const Fields& fields, const char* key, Type type, uint64_t maximum) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.type != type || it->second.text.empty())
        throw std::invalid_argument(std::string("invalid ") + key);
    const auto& value = it->second.text;
    uint64_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() || number > maximum ||
        (value.size() > 1 && value[0] == '0'))
        throw std::invalid_argument(std::string("invalid ") + key);
    return number;
}

std::optional<QuoteText> ReadQuote(const Fields& fields) {
    constexpr std::array<std::string_view, 9> keys{
        "quote_message_id", "quote_from_id", "quote_to_id", "quote_sender_id",
        "quote_conversation_id", "quote_text", "quote_timestamp", "quote_msg_source", "quote_message_type"};
    bool present = false;
    for (const auto& [key, value] : fields) {
        if (!key.starts_with("quote_")) continue;
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            throw std::invalid_argument("unknown quote field");
        present = true;
    }
    if (!present) return std::nullopt;
    QuoteText quote;
    quote.messageId = Unsigned(fields, "quote_message_id", Type::Text, (std::numeric_limits<uint64_t>::max)());
    quote.fromId = Text(fields, "quote_from_id", 256);
    quote.toId = Text(fields, "quote_to_id", 256);
    quote.senderId = Text(fields, "quote_sender_id", 256);
    quote.conversationId = Text(fields, "quote_conversation_id", 256);
    quote.text = Text(fields, "quote_text", 16384);
    quote.msgSource = Text(fields, "quote_msg_source", 8192, true);
    quote.timestamp = static_cast<uint32_t>(Unsigned(fields, "quote_timestamp", Type::Number, UINT32_MAX));
    quote.messageType = static_cast<uint32_t>(Unsigned(fields, "quote_message_type", Type::Number, UINT32_MAX));
    if (!IsValidQuote(quote, quote.conversationId)) throw std::invalid_argument("invalid quote reference");
    return quote;
}

std::string Error(const std::string& request, const char* code, const char* detail) {
    std::string result;
    json::ObjectWriter writer(result);
    writer.String("op", "error");
    writer.Number("protocol_version", 1);
    if (request.empty()) writer.Null("request_id");
    else writer.String("request_id", request);
    writer.String("error_code", code);
    writer.String("error_detail", detail);
    writer.Close();
    return result;
}

SendRequest DecodeSend(const Fields& fields, Operation operation, const std::string& request) {
    const auto quote = ReadQuote(fields);
    if (operation == Operation::SendRichText && !fields.contains("at_user_list") && !quote)
        throw std::invalid_argument("Rich text requires mention or quote metadata");
    SendCommand command;
    command.requestId = request;
    command.attemptId = Text(fields, "attempt_id", 128);
    command.sessionId = Text(fields, "observer_session_id", 128);
    command.accountId = Text(fields, "expected_account_id", 256);
    command.targetId = Text(fields, "target_id", 512);
    if (operation == Operation::SendMedia) {
        MediaCommand media;
        media.kind = Text(fields, "media_kind", 8);
        media.path = Text(fields, "media_path", 4096);
        media.sha256 = Text(fields, "media_sha256", 64);
        media.bytes = Unsigned(fields, "media_bytes", Type::Number, 20 * 1024 * 1024);
        media.durationMs = static_cast<uint32_t>(Unsigned(fields, "duration_ms", Type::Number, 60000));
        if (!IsValidMediaCommand(media)) throw std::invalid_argument("Invalid media description");
        command.media = std::move(media);
    } else command.text = Text(fields, "text", 16384);
    if (fields.contains("at_user_list")) command.atUserList = Text(fields, "at_user_list", 2063);
    if (!IsValidMentionList(command.atUserList) ||
        (!command.atUserList.empty() && !IsGroupTarget(command.targetId)))
        throw std::invalid_argument("Mentions require distinct private member IDs and a group target");
    command.quote = quote;
    if (quote && !IsValidQuote(*quote, command.targetId))
        throw std::invalid_argument("Quote must belong to the target conversation");
    command.createdAt = Text(fields, "created_at", 64);
    command.expiresAt = Text(fields, "expires_at", 64);
    command.origin = Text(fields, "origin", 16);
    if (command.origin != "manual" && command.origin != "ai" && command.origin != "game")
        throw std::invalid_argument("Unsupported command origin");
    const auto source = fields.find("source_event_key");
    if (source != fields.end() && source->second.type != Type::Null)
        command.sourceEventKey = Text(fields, "source_event_key", 512);
    return {std::move(command)};
}

Request DecodeRequest(const Fields& fields, Operation operation, const std::string& request) {
    ValidateKeys(fields, operation);
    if (IsHello(operation)) return HelloRequest{operation, request};
    return DecodeSend(fields, operation, request);
}

std::string HelloResponse(const HelloRequest& request,
                          const std::string& session, const NativeSendCapabilities& capability) {
    std::string result;
    json::ObjectWriter writer(result);
    writer.String("op", request.operation == Operation::HelloMedia ? "hello_media" : "hello");
    writer.Number("protocol_version", 1);
    writer.String("request_id", request.requestId);
    writer.String("observer_session_id", session);
    writer.String("target_version", kTargetVersion);
    writer.String("mode", capability.sendText ? "send_enabled" : "read_only");
    if (capability.accountId.empty()) writer.Null("account_id");
    else writer.String("account_id", capability.accountId);
    writer.Boolean("account_verified", capability.accountVerified);
    writer.Boolean("send_text", capability.sendText);
    writer.Boolean("send_group_text", capability.sendGroupText);
    writer.Boolean("send_mention", capability.sendMention);
    writer.Boolean("send_quote", capability.sendQuote);
    writer.Number("max_text_bytes", capability.maxTextBytes);
    if (request.operation == Operation::HelloMedia) {
        writer.Boolean("send_image", capability.sendImage);
        writer.Boolean("send_voice", capability.sendVoice);
    }
    writer.Close();
    return result;
}

SendResult ExecuteSend(const SendCommand& command, const std::string& session,
                       const CommandHandlers& handlers) {
    if (command.sessionId != session)
        return {"rejected", "session_mismatch", "No native send was attempted"};
    if (!handlers.send)
        return {"rejected", "native_sender_unavailable", "No native send was attempted"};
    return handlers.send(command);
}

std::string SendResponse(const SendCommand& command, const std::string& session,
                         const SendResult& result) {
    std::string response;
    json::ObjectWriter writer(response);
    writer.String("op", "send_result");
    writer.Number("protocol_version", 1);
    writer.String("request_id", command.requestId);
    writer.String("attempt_id", command.attemptId);
    writer.String("observer_session_id", session);
    writer.String("status", result.status);
    if (result.code.empty()) writer.Null("error_code");
    else writer.String("error_code", result.code);
    if (result.detail.empty()) writer.Null("error_detail");
    else writer.String("error_detail", result.detail);
    writer.Close();
    return response;
}
} // namespace

std::string HandleCommand(std::string_view payload, const std::string& session,
                          const CommandHandlers& handlers) {
    std::string request;
    try {
        if (payload.empty() || payload.size() > kCommandFrameLimit)
            return Error({}, "invalid_frame", "Frame length must be 1..65536 bytes");
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, payload.data(),
                                 static_cast<int>(payload.size()), nullptr, 0))
            return Error({}, "invalid_utf8", "Request must be UTF-8");
        
        // 1. 扁平 JSON 解析
        const auto fields = Parser(payload).Parse();
        request = Text(fields, "request_id", 128);
        
        // 2. 协议版本校验（当前仅支持版本 1）
        const auto version = fields.find("protocol_version");
        if (version == fields.end() || version->second.type != Type::Number || version->second.text != "1")
            return Error(request, "unsupported_protocol", "Only command protocol 1 is supported");
        
        // 3. 操作动词校验
        const auto operation = ReadOperation(Text(fields, "op", 32));
        if (!operation)
            return Error(request, "unsupported_operation", "Unknown command operation");
        
        // 4. 解码请求结构体
        const auto decoded = DecodeRequest(fields, *operation, request);
        
        // 5. 分发执行：hello / hello_media 探测能力
        if (const auto* hello = std::get_if<HelloRequest>(&decoded)) {
            const auto capability = handlers.probe ? handlers.probe() : NativeSendCapabilities{};
            return HelloResponse(*hello, session, capability);
        }
        
        // 6. 分发执行：发信请求
        const auto& command = std::get<SendRequest>(decoded).command;
        return SendResponse(command, session, ExecuteSend(command, session, handlers));
    } catch (const std::invalid_argument& error) {
        return Error(request, "invalid_request", error.what());
    }
}

} // namespace wechatbot::monitor
