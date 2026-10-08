#pragma once
#include <array>
#include <string_view>

namespace wechatbot::monitor {
inline bool IsGroupTarget(std::string_view target) noexcept {
    return target.ends_with("@chatroom");
}
// Internal IDs only. Names, unsupported domains and list separators are not
// recipients. Python uses the same prefix alphabet and 128-character limit.
inline bool IsValidRecipient(std::string_view target) noexcept {
    if (IsGroupTarget(target)) target.remove_suffix(std::string_view("@chatroom").size());
    if (target.empty() || target.size() > 128) return false;
    for (const char ch : target) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    }
    return true;
}
inline bool IsValidMentionList(std::string_view members) noexcept {
    if (members.empty()) return true;
    std::array<std::string_view, 16> seen{};
    size_t count = 0;
    while (!members.empty()) {
        const auto comma = members.find(',');
        const auto member = members.substr(0, comma);
        if (count == seen.size() || !IsValidRecipient(member) || IsGroupTarget(member) || member == "filehelper") return false;
        for (size_t i = 0; i < count; ++i) if (seen[i] == member) return false;
        seen[count++] = member;
        if (comma == std::string_view::npos) return true;
        members.remove_prefix(comma + 1);
        if (members.empty()) return false;
    }
    return false;
}
} // namespace wechatbot::monitor
