#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace wechatbot::monitor::json {

template<class Integer>
void AppendInteger(std::string& output, Integer value, int base = 10) {
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    std::array<char, std::numeric_limits<Integer>::digits + 2> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
    output.append(buffer.data(), result.ptr);
}

// Byte lengths are explicit: embedded NUL is escaped instead of ending a value.
inline void AppendString(std::string& output, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    output += '"';
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20) {
                output += "\\u00";
                output += hex[byte >> 4];
                output += hex[byte & 15];
            } else output += character;
        }
    }
    output += '"';
}

// Schemas choose fields and close objects explicitly. The writer only borrows
// host-owned output; it has no knowledge of events, native objects or sessions.
class ObjectWriter {
public:
    explicit ObjectWriter(std::string& output) : output_(output) { output_ += '{'; }
    ObjectWriter(const ObjectWriter&) = delete;
    ObjectWriter& operator=(const ObjectWriter&) = delete;
    void Close() { output_ += '}'; }
    void String(std::string_view name, std::string_view value) {
        Field(name); AppendString(output_, value);
    }
    void Number(std::string_view name, uint64_t value) {
        Field(name); AppendInteger(output_, value);
    }
    void NumberString(std::string_view name, uint64_t value) {
        Field(name); output_ += '"'; AppendInteger(output_, value); output_ += '"';
    }
    void SignedString(std::string_view name, int64_t value) {
        Field(name); output_ += '"'; AppendInteger(output_, value); output_ += '"';
    }
    void Boolean(std::string_view name, bool value) {
        Field(name); output_ += value ? "true" : "false";
    }
    void Null(std::string_view name) { Field(name); output_ += "null"; }
    void Hex(std::string_view name, uint64_t value) {
        Field(name); output_ += "\"0x"; AppendInteger(output_, value, 16); output_ += '"';
    }
    void HexBytes(std::string_view name, std::span<const uint8_t> bytes) {
        constexpr std::string_view digits = "0123456789abcdef";
        Field(name); output_ += '"';
        for (const auto byte : bytes) {
            output_ += digits[byte >> 4];
            output_ += digits[byte & 15];
        }
        output_ += '"';
    }
    template<class Fields>
    void Object(std::string_view name, Fields&& fields) {
        Field(name);
        ObjectWriter nested(output_);
        std::forward<Fields>(fields)(nested);
        nested.Close();
    }
    template<class Elements>
    void Array(std::string_view name, Elements&& elements) {
        Field(name); output_ += '[';
        std::forward<Elements>(elements)(output_);
        output_ += ']';
    }
private:
    void Field(std::string_view name) {
        if (!std::exchange(first_, false)) output_ += ',';
        AppendString(output_, name);
        output_ += ':';
    }
    std::string& output_;
    bool first_ = true;
};

} // namespace wechatbot::monitor::json
