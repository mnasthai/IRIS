#pragma once
#include "monitor/core/json_writer.hpp"
#include "monitor/diagnostics/event.hpp"

namespace wechatbot::monitor::event_encoding {

using json::AppendInteger;
using JsonObjectWriter = json::ObjectWriter;

template<size_t N>
void Text(JsonObjectWriter& fields, std::string_view name, const CapturedText<N>& value) {
    fields.String(name, value.view());
}
inline void Read(JsonObjectWriter& fields, std::string_view name, const TextRead& read) {
    fields.Object(name, [&](JsonObjectWriter& nested) {
        nested.String("status", StatusName(read.status));
        if (read.lengthKnown) nested.Number("original_bytes", read.originalBytes);
        else nested.Null("original_bytes");
        nested.Number("captured_bytes", read.capturedBytes);
    });
}
inline void Stack(JsonObjectWriter& fields, const StackSnapshot& stack) {
    fields.Object("call_stack", [&](JsonObjectWriter& nested) {
        nested.String("scope", "weixin_rva");
        nested.Array("rvas", [&](std::string& output) {
            const auto frames = std::span(stack.rvas).first(
                (std::min)(size_t{stack.frameCount}, stack.rvas.size()));
            bool first = true;
            for (const auto rva : frames) {
                if (!std::exchange(first, false)) output += ',';
                if (!rva) output += "null";
                else {
                    output += "\"0x";
                    AppendInteger(output, rva, 16);
                    output += '"';
                }
            }
        });
    });
}
inline void Session(JsonObjectWriter& fields) { fields.String("session_id", CurrentSessionId()); }

} // namespace wechatbot::monitor::event_encoding
