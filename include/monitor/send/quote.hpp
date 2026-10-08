#pragma once
#include "monitor/send/recipient.hpp"
#include <cstdint>
#include <string>

namespace wechatbot::monitor {
struct QuoteText {
    uint64_t messageId = 0;
    std::string fromId, toId, senderId, conversationId, text, msgSource;
    uint32_t timestamp = 0;
    uint32_t messageType = 1;
    bool operator==(const QuoteText&) const = default;
};
inline bool IsValidQuote(const QuoteText& quote, std::string_view target) noexcept {
    return quote.messageId && quote.messageType == 1 &&
        quote.conversationId == target && IsValidRecipient(quote.conversationId) &&
        IsValidRecipient(quote.fromId) && IsValidRecipient(quote.toId) &&
        IsValidRecipient(quote.senderId) && !IsGroupTarget(quote.senderId) &&
        !quote.text.empty() && quote.text.size() <= 16384 && quote.text.find('\0') == std::string::npos &&
        quote.msgSource.size() <= 8192 && quote.msgSource.find('\0') == std::string::npos;
}
} // namespace wechatbot::monitor
