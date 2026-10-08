#pragma once
#include "monitor/diagnostics/event.hpp"

namespace wechatbot::monitor {
void ConfigureMediaReceiveTrace(bool enabled);
void CaptureMediaReceive(const Event& message, const uint8_t* item);
BinarySnapshot CopyMediaBuffer(const uint8_t* item, size_t offset, uint32_t bit);
std::string SerializeMediaReceiveEvent(const Event& event);
} // namespace wechatbot::monitor
