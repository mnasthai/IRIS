#pragma once
#include <cstdint>

namespace wechatbot::monitor {
enum class ReadStatus : uint8_t {
    NotRead, Ok, Empty, Missing, InnerMissing, InvalidObject, InvalidWrapper,
    InvalidLayout, Unreadable, Exception, Truncated, InvalidUtf8
};
struct TextRead {
    ReadStatus status{};
    bool lengthKnown{};
    uint64_t originalBytes{};
    uint32_t capturedBytes{};
};
} // namespace wechatbot::monitor
