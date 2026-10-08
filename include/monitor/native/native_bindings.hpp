#pragma once
#include "monitor/native/native_media.hpp"
#include <cstdint>

namespace wechatbot::monitor {

struct NativeSenderApis {
    NativeTextApi text;
    NativeMediaApi media;
};

// Production callers must first verify the exact target image with VerifyTarget.
// These checks compare the active profile's entry bytes; they do not establish
// target identity, account readiness, UI-thread authorization or send permission.
// Media signatures are checked independently so media failure cannot disable text.
bool MatchesNativeSender(uintptr_t verifiedBase) noexcept;
bool MatchesNativeMediaSender(uintptr_t verifiedBase) noexcept;

// Preconditions: verifiedBase is the live, VerifyTarget-approved image base and
// MatchesNativeSender(verifiedBase) succeeded. This only binds exact-profile
// addresses and process-lifetime static metadata; it calls no Weixin functions.
// The returned media entries are inert until MatchesNativeMediaSender also
// succeeds. The caller retains all existing policy, account and UI-thread gates.
NativeSenderApis BindNativeSenderApis(uintptr_t verifiedBase) noexcept;

} // namespace wechatbot::monitor
