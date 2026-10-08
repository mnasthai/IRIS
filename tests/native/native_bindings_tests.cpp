// Uses reserved local memory containing profile signatures only. Bound function
// pointers are compared as addresses and are never invoked.
#include "monitor/native/native_bindings.hpp"
#include "monitor/config/version_profile.hpp"
#include <cstring>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);

namespace {
namespace sender = active_profile::sender;
namespace media = active_profile::media_send;

class FakeBindingImage {
public:
    FakeBindingImage() {
        bytes_ = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kExpectedImageSize, MEM_RESERVE, PAGE_READWRITE));
        Check(bytes_ != nullptr, "reserve local native binding image");
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        pageSize_ = system.dwPageSize;
        Fill(sender::kSignatures);
        Fill(media::kSignatures);
    }
    ~FakeBindingImage() { if (bytes_) VirtualFree(bytes_, 0, MEM_RELEASE); }
    FakeBindingImage(const FakeBindingImage&) = delete;
    FakeBindingImage& operator=(const FakeBindingImage&) = delete;
    uintptr_t Base() const { return reinterpret_cast<uintptr_t>(bytes_); }
    unsigned char* At(uintptr_t rva) const { return bytes_ + rva; }

private:
    template<size_t Count> void Fill(const sender::Signature (&entries)[Count]) {
        for (const auto& entry : entries) {
            const uintptr_t first = entry.rva / pageSize_ * pageSize_;
            const uintptr_t last = (entry.rva + sizeof(entry.bytes) - 1) / pageSize_ * pageSize_;
            for (uintptr_t page = first; page <= last; page += pageSize_)
                Check(VirtualAlloc(bytes_ + page, pageSize_, MEM_COMMIT, PAGE_READWRITE) != nullptr,
                      "commit only native signature pages");
            memcpy(At(entry.rva), entry.bytes, sizeof(entry.bytes));
        }
    }
    unsigned char* bytes_ = nullptr;
    DWORD pageSize_ = 0;
};

void CheckTextBindings(const NativeTextApi& api, uintptr_t base) {
    Check(reinterpret_cast<uintptr_t>(api.factory) == base + sender::kTextFactoryRva &&
          reinterpret_cast<uintptr_t>(api.assign) == base + sender::kStringAssignRva &&
          reinterpret_cast<uintptr_t>(api.allocate) == base + sender::kAllocateRva &&
          reinterpret_cast<uintptr_t>(api.free) == base + sender::kSizedFreeRva &&
          reinterpret_cast<uintptr_t>(api.makeDelayed) == base + sender::kDelayedFactoryRva &&
          reinterpret_cast<uintptr_t>(api.initMetadata) == base + sender::kMetadataInitRva &&
          reinterpret_cast<uintptr_t>(api.start) == base + sender::kStartRva,
          "text and shared lifecycle entries retain exact-profile addresses");
    Check(reinterpret_cast<uintptr_t>(api.referenceMessageInit) == base + sender::kReferenceMessageInitRva &&
          reinterpret_cast<uintptr_t>(api.referenceMessageDestroy) == base + sender::kReferenceMessageDestroyRva &&
          reinterpret_cast<uintptr_t>(api.quoteFromMessage) == base + sender::kQuoteFromMessageRva &&
          reinterpret_cast<uintptr_t>(api.quoteDestroy) == base + sender::kQuoteMessageDestroyRva &&
          reinterpret_cast<uintptr_t>(api.quoteAttach) == base + sender::kQuoteAttachRva,
          "quote entries retain the complete W to Q route");
    Check(api.metadataStaticText1 == reinterpret_cast<const void*>(base + sender::kMetadataText1Rva) &&
          api.metadataStaticText2 == reinterpret_cast<const void*>(base + sender::kMetadataText2Rva) &&
          api.metadataValue == sender::kMetadataValue,
          "metadata binds image-lifetime static text and original value");
}
} // namespace

void TestNativeBindings() {
    Check(!MatchesNativeSender(0) && !MatchesNativeMediaSender(0), "null binding base is rejected");
    FakeBindingImage image;
    const auto base = image.Base();
    Check(MatchesNativeSender(base) && MatchesNativeMediaSender(base),
          "all native entry signatures match the synthetic image");

    const auto apis = BindNativeSenderApis(base);
    CheckTextBindings(apis.text, base);
    CheckTextBindings(apis.media.common, base);
    Check(reinterpret_cast<uintptr_t>(apis.media.imageFactory) == base + media::kImageFactoryRva &&
          reinterpret_cast<uintptr_t>(apis.media.voiceFactory) == base + media::kVoiceFactoryRva &&
          reinterpret_cast<uintptr_t>(apis.media.wideAssign) == base + media::kWideAssignRva,
          "media factory and wide assignment entries retain exact-profile addresses");

    for (const auto& entry : sender::kSignatures) {
        image.At(entry.rva)[0] ^= 1;
        Check(!MatchesNativeSender(base) && MatchesNativeMediaSender(base),
              "each changed sender entry is rejected without changing the media check");
        image.At(entry.rva)[0] ^= 1;
    }
    for (const auto& entry : media::kSignatures) {
        image.At(entry.rva)[0] ^= 1;
        Check(MatchesNativeSender(base) && !MatchesNativeMediaSender(base),
              "each changed media entry is rejected while text signatures remain valid");
        image.At(entry.rva)[0] ^= 1;
    }
}
