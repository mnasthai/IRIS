#include "monitor/native/native_bindings.hpp"
#include "monitor/native/target.hpp"
#include "monitor/config/version_profile.hpp"

namespace wechatbot::monitor {
namespace {
namespace sender = active_profile::sender;
namespace media = active_profile::media_send;

template<size_t Count>
bool MatchesSignatures(uintptr_t base, const sender::Signature (&entries)[Count]) noexcept {
    if (!base) return false;
    for (const auto& entry : entries)
        if (!VerifyEntry(reinterpret_cast<const void*>(base + entry.rva),
                         entry.bytes, sizeof(entry.bytes))) return false;
    return true;
}
} // namespace

bool MatchesNativeSender(uintptr_t verifiedBase) noexcept {
    return MatchesSignatures(verifiedBase, sender::kSignatures);
}

bool MatchesNativeMediaSender(uintptr_t verifiedBase) noexcept {
    return MatchesSignatures(verifiedBase, media::kSignatures);
}

NativeSenderApis BindNativeSenderApis(uintptr_t verifiedBase) noexcept {
    NativeSenderApis apis;
    auto& text = apis.text;
    text.factory = reinterpret_cast<NativeTextFactoryFn>(verifiedBase + sender::kTextFactoryRva);
    text.assign = reinterpret_cast<NativeStringAssignFn>(verifiedBase + sender::kStringAssignRva);
    text.allocate = reinterpret_cast<NativeAllocateFn>(verifiedBase + sender::kAllocateRva);
    text.free = reinterpret_cast<NativeFreeFn>(verifiedBase + sender::kSizedFreeRva);
    text.makeDelayed = reinterpret_cast<NativeDelayedFactoryFn>(verifiedBase + sender::kDelayedFactoryRva);
    text.initMetadata = reinterpret_cast<NativeMetadataInitFn>(verifiedBase + sender::kMetadataInitRva);
    text.start = reinterpret_cast<NativeStartFn>(verifiedBase + sender::kStartRva);
    text.referenceMessageInit = reinterpret_cast<NativeReferenceMessageInitFn>(verifiedBase + sender::kReferenceMessageInitRva);
    text.referenceMessageDestroy = reinterpret_cast<NativeReferenceMessageDestroyFn>(verifiedBase + sender::kReferenceMessageDestroyRva);
    text.quoteFromMessage = reinterpret_cast<NativeQuoteFromMessageFn>(verifiedBase + sender::kQuoteFromMessageRva);
    text.quoteDestroy = reinterpret_cast<NativeQuoteMessageDestroyFn>(verifiedBase + sender::kQuoteMessageDestroyRva);
    text.quoteAttach = reinterpret_cast<NativeQuoteAttachFn>(verifiedBase + sender::kQuoteAttachRva);
    text.metadataStaticText1 = reinterpret_cast<const void*>(verifiedBase + sender::kMetadataText1Rva);
    text.metadataStaticText2 = reinterpret_cast<const void*>(verifiedBase + sender::kMetadataText2Rva);
    text.metadataValue = sender::kMetadataValue;
    apis.media.common = text;
    apis.media.imageFactory = reinterpret_cast<NativeImageFactoryFn>(verifiedBase + media::kImageFactoryRva);
    apis.media.voiceFactory = reinterpret_cast<NativeVoiceFactoryFn>(verifiedBase + media::kVoiceFactoryRva);
    apis.media.wideAssign = reinterpret_cast<NativeWideAssignFn>(verifiedBase + media::kWideAssignRva);
    return apis;
}

} // namespace wechatbot::monitor
