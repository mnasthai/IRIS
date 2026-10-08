#pragma once
#include <cstddef>
#include <string>

namespace wechatbot::monitor {
// A capability snapshot; querying the native backend is outside the protocol.
struct NativeSendCapabilities {
    std::string accountId;
    bool accountVerified = false;
    bool sendText = false;
    size_t maxTextBytes = 16384;
    bool sendGroupText = false;
    bool sendMention = false;
    bool sendQuote = false;
    bool sendImage = false;
    bool sendVoice = false;
};
} // namespace wechatbot::monitor
