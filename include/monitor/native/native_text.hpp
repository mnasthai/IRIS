#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/config/version_profile.hpp"
#include "monitor/send/quote.hpp"
#include "monitor/native/native_submit.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace wechatbot::monitor {

inline constexpr size_t kNativeTextMaxBytes = 16 * 1024;

struct NativeSharedPair {
    void* raw = nullptr;
    void* control = nullptr;
};

struct NativeSharedPairVector {
    NativeSharedPair* begin = nullptr;
    NativeSharedPair* end = nullptr;
    NativeSharedPair* capacityEnd = nullptr;
};

// The native layouts are all pointer-aligned. Keeping the declared alignment at
// 8 preserves their exact machine sizes; callers may place them in more highly
// aligned storage without changing any offset.
struct alignas(8) NativeCallableHolder { unsigned char bytes[active_profile::text::kCallableHolderSize]{}; };
struct alignas(8) NativeDelayedHolder { unsigned char bytes[active_profile::text::kDelayedHolderSize]{}; };
struct alignas(8) NativeStartupHandle { unsigned char bytes[active_profile::text::kStartupHandleSize]{}; };
struct alignas(8) NativeMetadata { unsigned char bytes[active_profile::text::kMetadataSize]{}; };

using NativeTextFactoryFn = NativeSharedPair* (__fastcall*)(NativeSharedPair* out);
using NativeStringAssignFn = void* (__fastcall*)(void* destination, const void* bytes,
                                                  uint64_t byteLength);
using NativeAllocateFn = void* (__fastcall*)(size_t bytes);
using NativeFreeFn = void (__fastcall*)(void* allocation, size_t bytes);
using NativeDelayedFactoryFn = NativeDelayedHolder* (__fastcall*)(
    void* unused, NativeDelayedHolder* out, const NativeSharedPairVector* sources,
    unsigned char unusedFlag);
using NativeMetadataInitFn = NativeMetadata* (__fastcall*)(
    NativeMetadata* out, const void* staticText1, const void* staticText2, uint32_t value);
using NativeStartFn = NativeStartupHandle* (__fastcall*)(
    NativeDelayedHolder* delayed, NativeStartupHandle* out,
    NativeCallableHolder* onValue, NativeCallableHolder* onError,
    NativeCallableHolder* onComplete, const NativeMetadata* metadata);
using NativeReferenceMessageInitFn = void* (__fastcall*)(void* message);
using NativeReferenceMessageDestroyFn = void (__fastcall*)(void* message);
// Converts a complete W into a newly constructed Q, including its owned backing
// message pair. The output storage must not contain an already constructed Q.
using NativeQuoteFromMessageFn = void* (__fastcall*)(const void* message, void* quote);
using NativeQuoteMessageDestroyFn = void (__fastcall*)(void* message);
using NativeQuoteAttachFn = void (__fastcall*)(void* referencedData, const void* message);

// Exact-version entry points are resolved and verified by the caller. The two
// metadata pointers must refer to process-lifetime native/static text. Sampling
// pointers and temporary host strings are not valid metadata inputs.
struct NativeTextApi {
    NativeTextFactoryFn factory = nullptr;          // 0x6DFD90
    NativeStringAssignFn assign = nullptr;          // 0x1F5880
    NativeAllocateFn allocate = nullptr;            // 0x71C5F3C
    NativeFreeFn free = nullptr;                     // 0x71C5F80
    NativeDelayedFactoryFn makeDelayed = nullptr;   // 0x197F730
    NativeMetadataInitFn initMetadata = nullptr;    // 0x196D80
    NativeStartFn start = nullptr;                   // 0x19AAA80
    const void* metadataStaticText1 = nullptr;
    const void* metadataStaticText2 = nullptr;
    uint32_t metadataValue = 0;
    NativeReferenceMessageInitFn referenceMessageInit = nullptr;
    NativeReferenceMessageDestroyFn referenceMessageDestroy = nullptr;
    NativeQuoteFromMessageFn quoteFromMessage = nullptr;
    NativeQuoteMessageDestroyFn quoteDestroy = nullptr;
    NativeQuoteAttachFn quoteAttach = nullptr;
};

struct NativeTextRequest {
    std::string_view targetUtf8;
    std::string_view textUtf8;
    // The integration layer must rediscover and authorize the current client's
    // UI thread for this session. This component verifies, but does not dispatch.
    DWORD authorizedUiThreadId = 0;
    // Invoked after native preparation and immediately before Start. Production
    // integration must provide a fresh account/executor/expiry/stop check. An
    // empty callback is permitted for isolated tests and trusted internal use.
    // Its captures must remain valid for this synchronous function call.
    std::function<bool()> beforeStart;
    std::string_view atUserListUtf8;
    const QuoteText* quote = nullptr;
};

// Executes exactly one already-authorized native attempt. A normal Start return
// is not a delivery acknowledgement. Once Start is entered, every exception or
// abnormal return is unknown and must never be retried automatically.
NativeAttemptResult SubmitNativeTextOnce(const NativeTextApi& api,
                                      const NativeTextRequest& request) noexcept;

// Shared implementation boundary for source types that use the already
// verified single-source vector, delayed, metadata and Start lifecycle.
// Callers must finish type-specific source construction before transferring it
// here; the function consumes and clears ``source`` on every releasable path.
namespace native_detail {
// A native out-parameter is caller-owned only after its call returns normally.
// EnteredWithoutReturn is deliberately distinct from a returned invalid value.
enum class NativeCallPhase : uint8_t { NotEntered, EnteredWithoutReturn, Returned };
bool IsValidNativeUtf8NoNul(std::string_view value) noexcept;
bool CompleteNativeCommonApi(const NativeTextApi& api) noexcept;
bool ReadNativeSourceUuid(const NativeSharedPair& source, std::string& output) noexcept;
bool ReleaseNativeSource(NativeSharedPair& source) noexcept;
NativeAttemptResult SubmitPreparedNativeSourceOnce(
    const NativeTextApi& api, NativeSharedPair& source, std::string localUuid,
    const std::function<bool()>& beforeStart, bool sourceSafeToRelease = true,
    bool priorCleanupComplete = true) noexcept;
} // namespace native_detail

static_assert(sizeof(NativeSharedPair) == active_profile::text::kSharedPairSize);
static_assert(sizeof(NativeSharedPairVector) == active_profile::text::kSharedPairVectorSize);
static_assert(sizeof(NativeCallableHolder) == active_profile::text::kCallableHolderSize);
static_assert(sizeof(NativeDelayedHolder) == active_profile::text::kDelayedHolderSize);
static_assert(sizeof(NativeStartupHandle) == active_profile::text::kStartupHandleSize);
static_assert(sizeof(NativeMetadata) == active_profile::text::kMetadataSize);

} // namespace wechatbot::monitor
