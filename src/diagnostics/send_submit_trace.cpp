#include "monitor/diagnostics/send_trace.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/native/target.hpp"
#include <MinHook.h>
#include <intrin.h>
#include <cstring>
#include <string_view>

namespace wechatbot::monitor {
SendSubmitFn g_originalSendSubmit = nullptr;
namespace {
volatile LONG traceBudget = 0;
volatile LONG imageTraceBudget = 0;
volatile LONG voiceTraceBudget = 0;
uintptr_t imageSize = 0;
namespace source = active_profile::text;
namespace media_source = active_profile::media_send;

// Independent one-shot slots. Unmarked ordinary traffic never consumes them.
LONG MarkerSlot(uint32_t type, uint32_t subtype, std::string_view content) {
    if (type == 1 && subtype == 0) {
        if (content.starts_with(kSendSubmitMarker)) return 1;
        if (content.find("SEND-AT-001") != std::string_view::npos) return 2;
        if (content.find("BOT-AT-001") != std::string_view::npos) return 8;
    }
    if (type == 49 && subtype == 57 &&
        content.find("SEND-QUOTE-001") != std::string_view::npos) return 4;
    if (type == 49 && subtype == 57 &&
        content.starts_with("BOT-QUOTE-")) return 16;
    return 0;
}
bool ClaimSlot(LONG slot) {
    LONG available = InterlockedCompareExchange(&traceBudget, 0, 0);
    while (available & slot) {
        const auto previous = InterlockedCompareExchange(&traceBudget, available & ~slot, available);
        if (previous == available) return true;
        available = previous;
    }
    return false;
}
bool ClaimSample(volatile LONG& budget) {
    LONG available = InterlockedCompareExchange(&budget, 0, 0);
    while (available > 0) {
        const auto previous = InterlockedCompareExchange(&budget, available - 1, available);
        if (previous == available) return true;
        available = previous;
    }
    return false;
}
bool AnyTraceAvailable() {
    return InterlockedCompareExchange(&traceBudget, 0, 0) > 0 ||
        InterlockedCompareExchange(&imageTraceBudget, 0, 0) > 0 ||
        InterlockedCompareExchange(&voiceTraceBudget, 0, 0) > 0;
}

template<class T> bool ReadAt(uintptr_t base, size_t offset, T& out) {
    if (!base || offset > UINTPTR_MAX - base) return false;
    const auto* address = reinterpret_cast<const void*>(base + offset);
    if (!IsReadableRange(address, sizeof(T))) return false;
    __try { memcpy(&out, address, sizeof(T)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint32_t Rva(uintptr_t address) {
    const auto base = reinterpret_cast<uintptr_t>(g_weixin);
    return address >= base && address - base < imageSize ? static_cast<uint32_t>(address - base) : 0;
}
TextRead InspectNativeBinary(uintptr_t object) {
    TextRead result{};
    result.status = ReadStatus::InvalidObject;
    uint64_t length = 0, capacity = 0;
    if (!ReadAt(object, 0x10, length) || !ReadAt(object, 0x18, capacity)) return result;
    result.lengthKnown = true;
    result.originalBytes = length;
    if (length > capacity || capacity > 0x4000000 || (capacity < 16 && length > 15)) {
        result.status = ReadStatus::InvalidLayout;
    } else {
        result.status = length ? ReadStatus::Ok : ReadStatus::Empty;
    }
    return result;
}
void CaptureCompletion(SubmitSnapshot& snapshot) {
    const auto address = static_cast<uintptr_t>(snapshot.completionAddress);
    snapshot.completionReadable = ReadAt(address, 0, snapshot.sharedPair) &&
        ReadAt(address, 0xD0, snapshot.completionValue) && ReadAt(address, 0xD8, snapshot.weakPair);
    constexpr size_t offsets[]{0x48, 0x88, 0xC8};
    for (size_t i = 0; i < 3; ++i) {
        if (!ReadAt(address, offsets[i], snapshot.callbackPayloads[i])) {
            snapshot.callbackReads[i] = ReadStatus::Unreadable;
            snapshot.completionReadable = false;
        } else if (!snapshot.callbackPayloads[i]) {
            snapshot.callbackReads[i] = ReadStatus::Empty;
        } else {
            snapshot.callbackReads[i] = ReadAt(snapshot.callbackPayloads[i], 0, snapshot.callbackVtables[i])
                ? ReadStatus::Ok : ReadStatus::Unreadable;
        }
    }
}
void CaptureExecutor(SubmitSnapshot& snapshot) {
    const auto base = reinterpret_cast<uintptr_t>(g_weixin);
    if (imageSize < kRootObjectPointerRva + sizeof(uintptr_t)) return;
    snapshot.rootPointerReadable = ReadAt(base, kRootObjectPointerRva, snapshot.rootAddress);
    if (!snapshot.rootPointerReadable) return;
    snapshot.executorPairReadable = ReadAt(snapshot.rootAddress, 0x310, snapshot.executorPair);
    if (!snapshot.executorPairReadable) return;
    snapshot.executorInnerReadable = ReadAt(snapshot.executorPair[0], 0x10, snapshot.executorInner);
    if (snapshot.executorInnerReadable)
        snapshot.executorFlagsReadable = ReadAt(snapshot.executorInner, 0xE9, snapshot.executorFlags);
}
void CaptureReference(ReferenceSnapshot& snapshot, uintptr_t object) {
    snapshot.idFlagReadable = ReadAt(object, source::kReferenceIdPresentOffset, snapshot.idPresent);
    if (snapshot.idFlagReadable && snapshot.idPresent == 1) {
        for (size_t i = 0; i < std::size(source::kReferenceIdStringOffsets); ++i)
            snapshot.idStrings[i].read = CopyNativeString(reinterpret_cast<const void*>(object + source::kReferenceIdStringOffsets[i]),
                                                   snapshot.idStrings[i].data(), snapshot.idStrings[i].capacity());
        snapshot.idScalarsReadable = ReadAt(object, 0x210, snapshot.idScalars) && ReadAt(object, 0x218, snapshot.idWide);
    }
    snapshot.messageFlagReadable = ReadAt(object, source::kReferenceMessagePresentOffset, snapshot.messagePresent);
    if (snapshot.messageFlagReadable && snapshot.messagePresent == 1) {
        const auto message = object + source::kReferenceMessageOffset;
        snapshot.messageVtableReadable = ReadAt(message, 0, snapshot.messageVtable);
        snapshot.messageVtableMatches = snapshot.messageVtableReadable &&
            snapshot.messageVtable == reinterpret_cast<uintptr_t>(g_weixin) + source::kReferenceMessageVtableRva;
        if (snapshot.messageVtableMatches) {
            for (size_t i = 0; i < std::size(source::kReferenceMessageStringOffsets); ++i)
                snapshot.messageStrings[i].read = CopyNativeString(reinterpret_cast<const void*>(message + source::kReferenceMessageStringOffsets[i]),
                                                             snapshot.messageStrings[i].data(), snapshot.messageStrings[i].capacity());
            constexpr size_t wideOffsets[]{0x08, 0xB8, 0xC8}, scalarOffsets[]{0x94, 0xC0, 0xD0};
            for (size_t i = 0; i < 3; ++i) {
                snapshot.messageWideReadable[i] = ReadAt(message, wideOffsets[i], snapshot.messageWide[i]);
                snapshot.messageScalarsReadable[i] = ReadAt(message, scalarOffsets[i], snapshot.messageScalars[i]);
            }
        }
    }
    for (size_t i = 0; i < std::size(source::kPartialStringOffsets); ++i)
        snapshot.partialStrings[i].read = CopyNativeString(reinterpret_cast<const void*>(object + source::kPartialStringOffsets[i]),
                                                    snapshot.partialStrings[i].data(), snapshot.partialStrings[i].capacity());
    snapshot.partialWideReadable = ReadAt(object, 0x6D0, snapshot.partialWide);
}
bool CaptureEnter(Event& event, const void* callable, const void* completion) {
    if (!g_weixin || !AnyTraceAvailable()) return false;
    const auto address = reinterpret_cast<uintptr_t>(callable);
    uint64_t vtable = 0, vector[3]{};
    if (!ReadAt(address, 0, vtable) ||
        vtable != reinterpret_cast<uintptr_t>(g_weixin) + kSendSubmitCallableVtableRva ||
        !ReadAt(address, 8, vector)) return false;
    const auto begin = vector[0], end = vector[1], capacity = vector[2];
    if (!begin || end <= begin || capacity < end || (end - begin) % 16 ||
        (capacity - begin) % 16 || (end - begin) / 16 > 1024) return false;
    const auto count = static_cast<uint32_t>((end - begin) / 16);
    // Bound inspection independently of each event budget. Media source
    // layouts are selected only by their exact vtable and type pair.
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        uint64_t pair[2]{}, sourceVtable = 0;
        uint32_t type = 0, subtype = 0;
        if (!ReadAt(begin, i * 16, pair) || !ReadAt(pair[0], 0, sourceVtable) ||
            !ReadAt(pair[0], 0x118, type) || !ReadAt(pair[0], 0x11C, subtype)) continue;

        const auto base = reinterpret_cast<uintptr_t>(g_weixin);
        const bool image = sourceVtable == base + media_source::kImageSourceVtableRva && type == 3 && subtype == 0;
        const bool voice = sourceVtable == base + media_source::kVoiceSourceVtableRva && type == 34 && subtype == 0;
        if (image || voice) {
            auto& budget = image ? imageTraceBudget : voiceTraceBudget;
            if (!ClaimSample(budget)) continue;
            event = MakeCallEvent(EventKind::SendSubmitEnter);
            auto& snapshot = event.Get<SubmitSnapshot>();
            snapshot.context = address;
            snapshot.begin = begin; snapshot.end = end; snapshot.count = count; snapshot.index = i;
            snapshot.callableVtable = vtable;
            snapshot.vectorCapacity = capacity;
            snapshot.completionAddress = reinterpret_cast<uint64_t>(completion);
            snapshot.sourceVtableRva = Rva(sourceVtable);
            snapshot.sourcePairAddress = begin + i * 16;
            snapshot.sourceObjectAddress = pair[0]; snapshot.sourceControlAddress = pair[1];
            snapshot.sourceVtable = sourceVtable; snapshot.sourceType = type; snapshot.sourceSubtype = subtype;
            // Both confirmed media sources inherit these base fields. Other
            // TextSource-only offsets remain untouched.
            snapshot.target.read = CopyNativeString(
                reinterpret_cast<const void*>(pair[0] + 0xB0), snapshot.target.data(), snapshot.target.capacity());
            snapshot.localUuid.read = CopyNativeString(
                reinterpret_cast<const void*>(pair[0] + 0x6F8), snapshot.localUuid.data(), snapshot.localUuid.capacity());
            if (image) {
                auto& media = snapshot.source.emplace<ImageSubmitSnapshot>();
                media.path.read = CopyNativeWideString(
                    reinterpret_cast<const void*>(pair[0] + 0x120), media.path.data(), media.path.capacity());
                media.dataRead = InspectNativeBinary(pair[0] + 0x768);
                media.widthReadable = ReadAt(pair[0], 0x788, media.width);
                media.heightReadable = ReadAt(pair[0], 0x78C, media.height);
            } else {
                auto& media = snapshot.source.emplace<VoiceSubmitSnapshot>();
                media.dataRead = CopyNativeBinary(
                    reinterpret_cast<const void*>(pair[0] + 0x758),
                    media.prefix.data(), media.prefix.size());
                media.lengthReadable = ReadAt(pair[0], 0x778, media.length);
                media.formatReadable = ReadAt(pair[0], 0x780, media.format);
            }
            CaptureCompletion(snapshot);
            CaptureExecutor(snapshot);
            return true;
        }

        if (sourceVtable != base + kSendContextSourceVtableRva ||
            !((type == 1 && subtype == 0) || (type == 49 && subtype == 57))) continue;
        char content[kTextCapacity]{};
        const auto contentRead = CopyNativeString(reinterpret_cast<const void*>(pair[0] + 0x758),
                                                  content, sizeof(content));
        if (contentRead.status != ReadStatus::Ok) continue;
        const LONG slot = MarkerSlot(type, subtype, std::string_view(content, contentRead.capturedBytes));
        if (!slot || !ClaimSlot(slot)) continue;
        // MakeCallEvent resets storage; copy the already captured text only once more.
        event = MakeCallEvent(EventKind::SendSubmitEnter);
        auto& snapshot = event.Get<SubmitSnapshot>();
        auto& text = snapshot.source.emplace<TextSubmitSnapshot>();
        snapshot.context = address;
        snapshot.begin = begin; snapshot.end = end; snapshot.count = count; snapshot.index = i;
        snapshot.callableVtable = vtable;
        snapshot.vectorCapacity = capacity;
        snapshot.completionAddress = reinterpret_cast<uint64_t>(completion);
        snapshot.sourceVtableRva = Rva(sourceVtable);
        snapshot.sourcePairAddress = begin + i * 16;
        snapshot.sourceObjectAddress = pair[0]; snapshot.sourceControlAddress = pair[1];
        snapshot.sourceVtable = sourceVtable; snapshot.sourceType = type; snapshot.sourceSubtype = subtype;
        snapshot.target.read = CopyNativeString(reinterpret_cast<const void*>(pair[0] + 0xB0),
                                                snapshot.target.data(), snapshot.target.capacity());
        text.content.read = CopyNativeString(reinterpret_cast<const void*>(pair[0] + 0x758),
                                             text.content.data(), text.content.capacity());
        snapshot.localUuid.read = CopyNativeString(reinterpret_cast<const void*>(pair[0] + 0x6F8),
                                                   snapshot.localUuid.data(), snapshot.localUuid.capacity());
        text.sourceTextBytesReadable = ReadAt(pair[0], 0x1C8, text.sourceTextBytes);
        text.sourceStateFlagReadable = ReadAt(pair[0], source::kStateFlagOffset, text.sourceStateFlag);
        text.atUserList.read = CopyNativeString(reinterpret_cast<const void*>(pair[0] + source::kAtUserListOffset),
                                                text.atUserList.data(), text.atUserList.capacity());
        text.copyFromUuid.read = CopyNativeString(reinterpret_cast<const void*>(pair[0] + source::kCopyFromUuidOffset),
                                                  text.copyFromUuid.data(), text.copyFromUuid.capacity());
        text.optionalText.read = CopyNativeString(reinterpret_cast<const void*>(pair[0] + source::kOptionalTextOffset),
                                                  text.optionalText.data(), text.optionalText.capacity());
        if (type == 49 && subtype == 57) CaptureReference(text.reference, pair[0]);
        CaptureCompletion(snapshot);
        CaptureExecutor(snapshot);
        return true;
    }
    return false;
}
} // namespace

void ConfigureSendSubmitTrace(uintptr_t size, bool enabled, bool mediaEnabled) {
    imageSize = size;
    InterlockedExchange(&traceBudget, enabled ? 31 : 0);
    InterlockedExchange(&imageTraceBudget, mediaEnabled ? 4 : 0);
    InterlockedExchange(&voiceTraceBudget, mediaEnabled ? 4 : 0);
}

void __fastcall HookSendSubmit(const void* callable, const void* completion) {
    const DWORD entryError = GetLastError();
    if (!AnyTraceAvailable()) {
        SetLastError(entryError);
        g_originalSendSubmit(callable, completion);
        return;
    }
    Event event{};
    const bool traced = CaptureEnter(event, callable, completion);
    if (traced) {
        event.Get<SubmitSnapshot>().directReturnRva = Rva(reinterpret_cast<uintptr_t>(_ReturnAddress()));
        event.header.stack.attempted = true;
        void* frames[24]{};
        event.header.stack.frameCount = CaptureStackBackTrace(1, 24, frames, nullptr);
        for (uint16_t i = 0; i < event.header.stack.frameCount; ++i)
            event.header.stack.rvas[i] = Rva(reinterpret_cast<uintptr_t>(frames[i]));
        QueueEvent(event);
    }
    SetLastError(entryError);
    g_originalSendSubmit(callable, completion);
    const DWORD returnError = GetLastError();
    if (traced) {
        // No post-call dereferences: this records normal return, not success.
        event.SetKind(EventKind::SendSubmitReturn);
        event.header.sequence = NextSequence();
        event.Get<SubmitSnapshot>().returnTick = GetTickCount64();
        QueueEvent(event);
    }
    SetLastError(returnError);
}

void EnableSendSubmitObservation(bool textEnabled, bool mediaEnabled) {
    if (!textEnabled && !mediaEnabled) return;
    void* target = reinterpret_cast<uint8_t*>(g_weixin) + kSendSubmitRva;
    if (!VerifyEntry(target, kSendSubmitEntry, sizeof(kSendSubmitEntry))) {
        AppendDirect("{\"kind\":\"send_submit_hook_disabled\",\"reason\":\"entry_signature_mismatch\"}");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookSendSubmit),
                      reinterpret_cast<void**>(&g_originalSendSubmit)) != MH_OK) {
        AppendDirect("{\"kind\":\"hook_error\",\"source\":\"send_submit\",\"stage\":\"create\"}");
        return;
    }
    ConfigureSendSubmitTrace(kExpectedImageSize, textEnabled, mediaEnabled);
    if (MH_EnableHook(target) != MH_OK) {
        ConfigureSendSubmitTrace(0, false, false);
        AppendDirect("{\"kind\":\"hook_error\",\"source\":\"send_submit\",\"stage\":\"enable\"}");
        return;
    }
    std::string trace = "{\"kind\":\"send_submit_trace_enabled\",\"text_enabled\":";
    trace += textEnabled ? "true" : "false";
    trace += ",\"media_enabled\":";
    trace += mediaEnabled ? "true" : "false";
    trace += ",\"max_calls\":5,\"max_media_calls_per_type\":4,\"max_frames\":24,"
             "\"max_inspected_items\":8,\"markers\":[\"SEND-SUBMIT-001\",\"SEND-AT-001\","
             "\"SEND-QUOTE-001\",\"BOT-AT-001\",\"BOT-QUOTE-*\"]}";
    AppendDirect(trace.c_str());
    AppendDirect("{\"kind\":\"hook_enabled\",\"source\":\"send_submit\",\"rva\":\"0x19ab920\"}");
}
} // namespace wechatbot::monitor
