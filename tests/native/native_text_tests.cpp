// All native entry points are local fakes. This test never loads Weixin,
// injects code, touches another process, or sends a message.
#include "monitor/native/native_text.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);

namespace {
enum class ControlKind : unsigned { source, startupFirst, startupSecond, referenceBacking, count };
enum class QuoteFault { none, initReturn, initThrow, assignReturn, assignThrow,
                        convertReturn, convertThrow, missingBacking, textMismatch, scalarMismatch,
                        referenceDestroyThrow, attachThrow, attachMissingBacking, destroyThrow,
                        digestReturn, digestThrow };
constexpr std::array<size_t, 6> quoteStringOffsets{0x10, 0x30, 0x50, 0x70, 0xE0, 0x100};
constexpr std::array<size_t, 5> referenceStringOffsets{0x18, 0x38, 0x58, 0x180, 0x1C0};

struct FakeControl {
    void** vtable = nullptr;
    volatile LONG strong = 0;
    volatile LONG weak = 0;
    ControlKind kind = ControlKind::source;
};
static_assert(offsetof(FakeControl, strong) == 8);
static_assert(offsetof(FakeControl, weak) == 12);

struct FakeSource { alignas(16) unsigned char bytes[0x798]{}; };
struct FakePayload {
    void** vtable = nullptr;
    NativeSharedPair pair{};
};
struct FakeReferenceBacking {
    bool alive = false;
    std::array<std::string, 5> strings;
};

struct FakeState {
    FakeSource source{};
    FakeControl sourceControl{};
    FakeControl startupFirst{};
    FakeControl startupSecond{};
    FakeControl referenceBackingControl{};
    FakeReferenceBacking referenceBacking{};
    NativeSharedPair vectorSlot{};
    FakePayload payload{};
    int startupRawFirst = 1;
    int startupRawSecond = 2;
    unsigned factoryCalls = 0;
    unsigned assignCalls = 0;
    unsigned allocateCalls = 0;
    unsigned freeCalls = 0;
    unsigned delayedCalls = 0;
    unsigned metadataCalls = 0;
    unsigned startCalls = 0;
    unsigned referenceInitCalls = 0;
    unsigned referenceDestroyCalls = 0;
    unsigned quoteFromMessageCalls = 0;
    unsigned quoteAttachCalls = 0;
    unsigned quoteDestroyCalls = 0;
    std::array<unsigned, static_cast<size_t>(ControlKind::count)> destroyed{};
    std::array<unsigned, static_cast<size_t>(ControlKind::count)> deleted{};
    bool allocationOutstanding = false;
    bool payloadDestroyed = false;
    bool failAllocate = false;
    bool invalidFactoryReturn = false;
    bool invalidDelayedReturn = false;
    bool throwInDelayed = false;
    bool throwInStart = false;
    QuoteFault quoteFault = QuoteFault::none;
    size_t quoteAssignFaultIndex = 3;
    void* liveReference = nullptr;
    bool referenceAlive = false;
    std::array<std::string, 5> referenceStrings;
    std::string convertedRawContent;
    void* liveQuote = nullptr;
    bool quoteAlive = false;
    bool quoteAttached = false;
    std::array<std::string, 6> quoteStrings;
    std::array<std::string, 6> attachedQuoteStrings;
    std::string localUuid = "00000000-1111-4222-8333-444444444444";
    std::string assignedTarget;
    std::string assignedText;
    std::string assignedMembers;
    std::string assignedPartialDigest;
};

FakeState* state = nullptr;
void* controlVtable[2]{};
void* payloadVtable[5]{};
constexpr char metadata1[] = "native_text_tests.cpp";
constexpr char metadata2[] = "TestNativeText";

template<class T>
T Read(const void* base, size_t offset) {
    T value{};
    memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return value;
}

template<class T>
void Write(void* base, size_t offset, const T& value) {
    memcpy(static_cast<unsigned char*>(base) + offset, &value, sizeof(value));
}

void WriteFakeString(void* destination, const std::string& value) {
    memset(destination, 0, 0x20);
    Write(destination, 0x10, static_cast<uint64_t>(value.size()));
    Write(destination, 0x18, static_cast<uint64_t>(value.size() < 16 ? 15 : value.size()));
    if (value.size() < 16) memcpy(destination, value.data(), value.size());
    else Write(destination, 0, value.data());
}

std::string ReadFakeString(const void* value) {
    const auto length = Read<uint64_t>(value, 0x10);
    const auto* bytes = Read<uint64_t>(value, 0x18) < 16
        ? static_cast<const char*>(value) : Read<const char*>(value, 0);
    return {bytes, static_cast<size_t>(length)};
}

void InitControl(FakeControl& control, ControlKind kind) {
    control.vtable = controlVtable;
    control.strong = 1;
    control.weak = 1;
    control.kind = kind;
}

void ReleaseFakePair(NativeSharedPair& pair);

void __fastcall FakeControlDestroy(void* pointer) {
    auto& control = *static_cast<FakeControl*>(pointer);
    ++state->destroyed[static_cast<size_t>(control.kind)];
    if (control.kind == ControlKind::source && state->quoteAttached) {
        NativeSharedPair backing = Read<NativeSharedPair>(
            state->source.bytes + 0x248, active_profile::text::kReferenceBackingPairOffset);
        ReleaseFakePair(backing);
    } else if (control.kind == ControlKind::referenceBacking) {
        Check(state->referenceBacking.alive, "the cloned W backing message is destroyed once");
        state->referenceBacking.alive = false;
        for (auto& value : state->referenceBacking.strings) value.clear();
    }
}

void __fastcall FakeControlDelete(void* pointer) {
    auto& control = *static_cast<FakeControl*>(pointer);
    ++state->deleted[static_cast<size_t>(control.kind)];
}

void ReleaseFakePair(NativeSharedPair& pair) {
    auto* control = static_cast<FakeControl*>(pair.control);
    pair = {};
    if (!control || InterlockedDecrement(&control->strong) != 0) return;
    FakeControlDestroy(control);
    if (InterlockedDecrement(&control->weak) == 0) FakeControlDelete(control);
}

void __fastcall FakePayloadDestroy(void* pointer, bool releaseStorage) {
    auto& payload = *static_cast<FakePayload*>(pointer);
    Check(releaseStorage, "heap delayed payload requests storage release");
    Check(!state->payloadDestroyed, "delayed payload destroyed once");
    state->payloadDestroyed = true;
    ReleaseFakePair(payload.pair);
}

void ResetVtables() {
    controlVtable[0] = reinterpret_cast<void*>(&FakeControlDestroy);
    controlVtable[1] = reinterpret_cast<void*>(&FakeControlDelete);
    memset(payloadVtable, 0, sizeof(payloadVtable));
    payloadVtable[4] = reinterpret_cast<void*>(&FakePayloadDestroy);
}

NativeSharedPair* __fastcall FakeFactory(NativeSharedPair* out) {
    ++state->factoryCalls;
    memset(&state->source, 0, sizeof(state->source));
    const char* uuidBytes = state->localUuid.data();
    const uint64_t uuidLength = state->localUuid.size();
    const uint64_t uuidCapacity = state->localUuid.capacity();
    Write(&state->source, 0x6F8, uuidBytes);
    Write(&state->source, 0x6F8 + 0x10, uuidLength);
    Write(&state->source, 0x6F8 + 0x18, uuidCapacity);
    InitControl(state->sourceControl, ControlKind::source);
    out->raw = &state->source;
    out->control = &state->sourceControl;
    return state->invalidFactoryReturn ? nullptr : out;
}

void* __fastcall FakeAssign(void* destination, const void* bytes, uint64_t length) {
    ++state->assignCalls;
    const std::string copied(static_cast<const char*>(bytes), static_cast<size_t>(length));
    auto* base = state->source.bytes;
    if (destination == base + 0xB0) state->assignedTarget = copied;
    else if (destination == base + 0x758) state->assignedText = copied;
    else if (destination == base + 0x778) state->assignedMembers = copied;
    else if (destination == base + 0x6D8) {
        Check(state->quoteAttached && !state->referenceAlive && !state->quoteAlive &&
              state->referenceDestroyCalls == 1 && state->quoteDestroyCalls == 1,
              "partial digest assignment follows independent Q copy and both temporary destructions");
        if (state->quoteFault == QuoteFault::digestThrow) throw 96;
        state->assignedPartialDigest = copied;
        WriteFakeString(destination, state->assignedPartialDigest);
        return state->quoteFault == QuoteFault::digestReturn ? nullptr : destination;
    }
    else if (state->referenceAlive) {
        for (size_t i = 0; i < referenceStringOffsets.size(); ++i) {
            if (destination != static_cast<unsigned char*>(state->liveReference) + referenceStringOffsets[i]) continue;
            if (i == state->quoteAssignFaultIndex && state->quoteFault == QuoteFault::assignThrow) {
                // Model an abnormal native mutation that invalidates cleanup
                // assumptions; the caller must not guess that W is intact.
                state->referenceAlive = false;
                throw 92;
            }
            state->referenceStrings[i] = copied;
            WriteFakeString(destination, state->referenceStrings[i]);
            if (i == state->quoteAssignFaultIndex && state->quoteFault == QuoteFault::assignReturn) {
                state->referenceAlive = false;
                return nullptr;
            }
            return destination;
        }
        Check(false, "reference assignment addresses a constructed W string slot");
    }
    else Check(false, "assign targets only the constructed NativeStrings");
    Write(destination, 0x10, length);
    const uint64_t capacity = length < 16 ? 15 : length;
    Write(destination, 0x18, capacity);
    return destination;
}

void* __fastcall FakeReferenceMessageInit(void* message) {
    ++state->referenceInitCalls;
    Check(reinterpret_cast<uintptr_t>(message) % 8 == 0 && !state->referenceAlive,
          "temporary W is pointer-aligned and initialized once");
    if (state->quoteFault == QuoteFault::initThrow) throw 91;
    if (state->quoteFault == QuoteFault::initReturn) return nullptr;
    memset(message, 0, active_profile::reference_message::kObjectSize);
    for (const size_t offset : referenceStringOffsets) WriteFakeString(static_cast<unsigned char*>(message) + offset, {});
    state->liveReference = message;
    state->referenceAlive = true;
    return message;
}

void __fastcall FakeReferenceMessageDestroy(void* message) {
    ++state->referenceDestroyCalls;
    Check(state->referenceAlive && message == state->liveReference,
          "only a completely constructed W is destroyed once");
    state->referenceAlive = false;
    state->liveReference = nullptr;
    for (auto& value : state->referenceStrings) value.clear();
    memset(message, 0xDD, active_profile::reference_message::kObjectSize);
    if (state->quoteFault == QuoteFault::referenceDestroyThrow) throw 97;
}

void* __fastcall FakeQuoteFromMessage(const void* original, void* message) {
    ++state->quoteFromMessageCalls;
    Check(state->referenceAlive && original == state->liveReference && !state->quoteAlive &&
          reinterpret_cast<uintptr_t>(message) % 8 == 0,
          "ToQ receives a complete W and previously unconstructed aligned Q storage");
    if (state->quoteFault == QuoteFault::convertReturn) return nullptr;
    Check(Read<uint32_t>(original, 0x0C) == 1 && Read<uint32_t>(original, 0x10) == 0 &&
          Read<uint32_t>(original, 0x144) == 0 && Read<uint32_t>(original, 0x174) == 0,
          "W carries text type/subtype and keeps default local ID and optional field 13");
    const auto& fields = state->referenceStrings;
    for (size_t i = 0; i < referenceStringOffsets.size(); ++i)
        Check(ReadFakeString(static_cast<const unsigned char*>(original) + referenceStringOffsets[i]) == fields[i],
              "ToQ reads all original strings through the native W layout");
    state->convertedRawContent = fields[3];
    const std::string conversation = IsGroupTarget(fields[0]) ? fields[0] :
        IsGroupTarget(fields[1]) ? fields[1] : fields[0] == "wxid_current_account" ? fields[1] : fields[0];
    std::string normalizedText = fields[3];
    if (IsGroupTarget(fields[0])) {
        const std::string prefix = fields[2] + ":\n";
        Check(normalizedText.starts_with(prefix), "incoming group W contains the exact member prefix");
        normalizedText.erase(0, prefix.size());
    }
    memset(message, 0, active_profile::text::kReferenceObjectSize);
    state->quoteStrings = {fields[0], fields[1], fields[2], conversation, normalizedText, fields[4]};
    if (state->quoteFault == QuoteFault::textMismatch) state->quoteStrings[4] = "wrong normalized text";
    for (size_t i = 0; i < quoteStringOffsets.size(); ++i)
        WriteFakeString(static_cast<unsigned char*>(message) + quoteStringOffsets[i], state->quoteStrings[i]);
    Write(message, 0x08, uint64_t{1});
    Write(message, 0xB8, Read<uint64_t>(original, 0x148));
    Write(message, 0xC8, Read<uint64_t>(original, 0x150));
    Write(message, 0xD0, Read<uint32_t>(original, 0x164));
    if (state->quoteFault == QuoteFault::scalarMismatch) Write(message, 0xB8, uint64_t{0});
    if (state->quoteFault != QuoteFault::missingBacking) {
        // Only ToQ creates backing ownership. These strings must survive both
        // W destruction and temporary Q destruction through the source's pair.
        state->referenceBacking.strings = fields;
        state->referenceBacking.alive = true;
        InitControl(state->referenceBackingControl, ControlKind::referenceBacking);
        Write(message, active_profile::text::kReferenceBackingPairOffset,
              NativeSharedPair{&state->referenceBacking, &state->referenceBackingControl});
    }
    state->liveQuote = message;
    state->quoteAlive = true;
    if (state->quoteFault == QuoteFault::convertThrow) {
        // Native construction may unwind its clone and leave stale output
        // bytes. A failed ToQ must not trigger a second Q destruction.
        auto backing = Read<NativeSharedPair>(message, active_profile::text::kReferenceBackingPairOffset);
        ReleaseFakePair(backing);
        state->quoteAlive = false;
        state->liveQuote = nullptr;
        for (auto& value : state->quoteStrings) value.clear();
        throw 98;
    }
    return message;
}

void __fastcall FakeQuoteAttach(void* referencedData, const void* message) {
    ++state->quoteAttachCalls;
    Check(state->quoteAlive && !state->referenceAlive && state->referenceDestroyCalls == 1 &&
          state->referenceBacking.alive && state->referenceBackingControl.strong == 1 &&
          state->referenceBacking.strings[3] == state->convertedRawContent && message == state->liveQuote &&
          referencedData == state->source.bytes + 0x1F0,
          "attach borrows Q whose cloned backing survives W destruction");
    if (state->quoteFault == QuoteFault::attachThrow) throw 93;
    auto* copy = state->source.bytes + 0x248;
    memcpy(copy, message, active_profile::text::kReferenceObjectSize);
    const auto backing = Read<NativeSharedPair>(message, active_profile::text::kReferenceBackingPairOffset);
    Check(backing.raw == &state->referenceBacking && backing.control == &state->referenceBackingControl,
          "Q owns the complete backing message pair before attachment");
    if (state->quoteFault == QuoteFault::attachMissingBacking)
        Write(copy, active_profile::text::kReferenceBackingPairOffset, NativeSharedPair{});
    else InterlockedIncrement(&state->referenceBackingControl.strong);
    for (size_t i = 0; i < quoteStringOffsets.size(); ++i) {
        state->attachedQuoteStrings[i] = ReadFakeString(static_cast<const unsigned char*>(message) + quoteStringOffsets[i]);
        WriteFakeString(copy + quoteStringOffsets[i], state->attachedQuoteStrings[i]);
    }
    state->source.bytes[0x688] = 1;
    state->quoteAttached = true;
}

void __fastcall FakeQuoteDestroy(void* message) {
    ++state->quoteDestroyCalls;
    Check(state->quoteAlive && message == state->liveQuote,
          "only a completely initialized Q is destroyed once");
    state->quoteAlive = false;
    state->liveQuote = nullptr;
    auto backing = Read<NativeSharedPair>(message, active_profile::text::kReferenceBackingPairOffset);
    ReleaseFakePair(backing);
    for (auto& value : state->quoteStrings) value.clear();
    memset(message, 0xDD, active_profile::text::kReferenceObjectSize);
    if (state->quoteFault == QuoteFault::destroyThrow) throw 94;
}

void* __fastcall FakeAllocate(size_t bytes) {
    ++state->allocateCalls;
    Check(bytes == sizeof(NativeSharedPair), "single vector allocation is one pair");
    if (state->failAllocate) return nullptr;
    Check(!state->allocationOutstanding, "only one native vector allocation");
    state->allocationOutstanding = true;
    return &state->vectorSlot;
}

void __fastcall FakeFree(void* allocation, size_t bytes) {
    ++state->freeCalls;
    Check(allocation == &state->vectorSlot && bytes == sizeof(NativeSharedPair) &&
          state->allocationOutstanding,
          "native vector storage freed by matching API");
    state->allocationOutstanding = false;
}

NativeDelayedHolder* __fastcall FakeMakeDelayed(
    void* unused, NativeDelayedHolder* out, const NativeSharedPairVector* sources,
    unsigned char unusedFlag) {
    ++state->delayedCalls;
    Check(!unused && unusedFlag == 0, "unused delayed arguments stay zero");
    Check(sources && sources->begin == &state->vectorSlot &&
          sources->end == sources->begin + 1 && sources->capacityEnd == sources->end,
          "delayed receives exact single-pair vector");
    Check(sources->begin->raw == &state->source &&
          sources->begin->control == &state->sourceControl,
          "vector retains factory source identity");
    const uint64_t expectedType = state->quoteAttached ? active_profile::text::kQuoteTypeAndSubtype : 1;
    Check(Read<uint64_t>(&state->source, 0x118) == expectedType &&
          Read<uint64_t>(&state->source, 0x1C8) == state->assignedText.size() &&
          state->source.bytes[0x108] == 0 && Read<uint64_t>(&state->source, 0x130) == 0,
          "text or quote fields and factory defaults reach delayed construction");
    if (state->quoteAttached) {
        Check(!state->referenceAlive && !state->quoteAlive && state->referenceDestroyCalls == 1 &&
              state->quoteDestroyCalls == 1 && state->referenceBacking.alive &&
              state->referenceBackingControl.strong == 1 &&
              state->referenceBacking.strings[3] == state->convertedRawContent &&
              ReadFakeString(state->source.bytes + 0x248 + 0xE0) == state->attachedQuoteStrings[4],
              "delayed receives complete Q and backing data after both temporaries are destroyed");
    }
    state->payload.vtable = payloadVtable;
    state->payload.pair = *sources->begin;
    InterlockedIncrement(&state->sourceControl.strong);
    Write(out, 0x38, static_cast<void*>(&state->payload));
    if (state->throwInDelayed) {
        // Model native unwinding that destroys the partial delayed payload but
        // leaves a stale pointer in the caller-provided output buffer.
        FakePayloadDestroy(&state->payload, true);
        throw 17;
    }
    return state->invalidDelayedReturn ? nullptr : out;
}

NativeMetadata* __fastcall FakeInitMetadata(
    NativeMetadata* out, const void* first, const void* second, uint32_t value) {
    ++state->metadataCalls;
    Check(first == metadata1 && second == metadata2 && value == 0x216,
          "metadata uses caller-supplied process-lifetime text");
    Write(out, 0, first);
    Write(out, 8, second);
    Write(out, 0x10, value);
    return out;
}

NativeStartupHandle* __fastcall FakeStart(
    NativeDelayedHolder* delayed, NativeStartupHandle* out,
    NativeCallableHolder* onValue, NativeCallableHolder* onError,
    NativeCallableHolder* onComplete, const NativeMetadata* metadata) {
    ++state->startCalls;
    Check(Read<void*>(delayed, 0x38) == &state->payload,
          "start receives constructed delayed payload");
    Check(!Read<void*>(onValue, 0x38) && !Read<void*>(onError, 0x38) &&
          !Read<void*>(onComplete, 0x38), "three callback holders are valid and empty");
    Check(Read<const void*>(metadata, 0) == metadata1 &&
          Read<const void*>(metadata, 8) == metadata2,
          "start receives initialized metadata");
    if (state->quoteAttached) {
        const auto backing = Read<NativeSharedPair>(
            state->source.bytes + 0x248, active_profile::text::kReferenceBackingPairOffset);
        Check(!state->referenceAlive && !state->quoteAlive && state->referenceDestroyCalls == 1 &&
              state->quoteDestroyCalls == 1 && state->referenceBacking.alive &&
              backing.raw == &state->referenceBacking && backing.control == &state->referenceBackingControl &&
              state->referenceBackingControl.strong == 1 &&
              state->referenceBacking.strings[3] == state->convertedRawContent,
              "Start retains the complete backing clone with neither temporary W nor Q alive");
    }
    InitControl(state->startupFirst, ControlKind::startupFirst);
    Write(out, 0x08, NativeSharedPair{&state->startupRawFirst, &state->startupFirst});
    if (state->throwInStart) {
        // Model native unwinding that releases its partial output but leaves
        // stale bytes in the caller's buffer. The caller must not inspect or
        // release this output after the exception.
        NativeSharedPair partial{&state->startupRawFirst, &state->startupFirst};
        ReleaseFakePair(partial);
        throw 42;
    }
    InitControl(state->startupSecond, ControlKind::startupSecond);
    Write(out, 0x18, NativeSharedPair{&state->startupRawSecond, &state->startupSecond});
    return out;
}

NativeTextApi Api() {
    return {FakeFactory, FakeAssign, FakeAllocate, FakeFree, FakeMakeDelayed,
            FakeInitMetadata, FakeStart, metadata1, metadata2, 0x216,
            FakeReferenceMessageInit, FakeReferenceMessageDestroy, FakeQuoteFromMessage,
            FakeQuoteDestroy, FakeQuoteAttach};
}

NativeTextRequest Request(const std::string& target, const std::string& text) {
    return {target, text, GetCurrentThreadId()};
}

void CheckSourceReleased(const FakeState& value, const char* message) {
    const auto index = static_cast<size_t>(ControlKind::source);
    Check(value.sourceControl.strong == 0 && value.sourceControl.weak == 0 &&
          value.destroyed[index] == 1 && value.deleted[index] == 1, message);
}

void TestNativeQuoteText() {
    const std::string target = "123456789@chatroom";
    const std::string reply = "BOT-QUOTE-001 回复🙂";
    QuoteText original{};
    original.messageId = 5620547460830845497ULL;
    original.fromId = target;
    original.toId = "wxid_current_account";
    original.senderId = "wxid_quote_sender";
    original.conversationId = target;
    original.text = "QUOTE-ORIGINAL 原文🙂";
    original.msgSource = "<msgsource><alnode><fr>1</fr></alnode></msgsource>";
    original.timestamp = UINT32_MAX;

    // Incoming group, the maximum incoming body, own group, received private,
    // and own private distinguish exactly when the raw member prefix is needed.
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        FakeState success{};
        state = &success;
        auto quote = original;
        if (scenario == 1) {
            quote.text.assign(kNativeTextMaxBytes, 'x');
            quote.msgSource.clear();
            quote.timestamp = 0;
        } else if (scenario == 2) {
            quote.fromId = quote.toId;
            quote.toId = target;
            quote.senderId = quote.fromId;
        } else if (scenario == 3) {
            quote.fromId = quote.senderId;
            quote.conversationId = quote.fromId;
        } else if (scenario == 4) {
            quote.fromId = quote.toId;
            quote.toId = "wxid_private_target";
            quote.senderId = quote.fromId;
            quote.conversationId = quote.toId;
        }
        const bool group = IsGroupTarget(quote.conversationId);
        auto request = Request(quote.conversationId, reply);
        request.quote = &quote;
        if (group) request.atUserListUtf8 = "wxid_member";
        request.beforeStart = [&success] {
            Check(success.quoteAttached && !success.referenceAlive && !success.quoteAlive &&
                  success.referenceDestroyCalls == 1 && success.quoteDestroyCalls == 1 &&
                  success.referenceBacking.alive && success.referenceBackingControl.strong == 1,
                  "pre-Start guard sees only the source-owned complete quote and backing clone");
            return true;
        };
        const auto result = SubmitNativeTextOnce(Api(), request);
        Check(result.status == NativeAttemptStatus::start_returned && result.cleanupComplete &&
              success.referenceInitCalls == 1 && success.referenceDestroyCalls == 1 &&
              success.quoteFromMessageCalls == 1 && success.quoteAttachCalls == 1 &&
              success.quoteDestroyCalls == 1 && success.assignCalls == (group ? 9u : 8u) &&
              success.allocateCalls == 1 && success.startCalls == 1,
              "quoted text constructs W, converts Q, releases both and starts exactly once");
        const std::string rawExpected = scenario <= 1 ? quote.senderId + ":\n" + quote.text : quote.text;
        Check(success.convertedRawContent == rawExpected,
              "only received group originals gain the raw member prefix, including a full-size body");
        const std::array<std::string, 6> expected{
            quote.fromId, quote.toId, quote.senderId, quote.conversationId, quote.text, quote.msgSource};
        Check(success.attachedQuoteStrings == expected && success.assignedText == reply &&
              success.assignedMembers == request.atUserListUtf8 &&
              success.assignedPartialDigest == "d41d8cd98f00b204e9800998ecf8427e",
              "source owns the exact quote strings while reply, mentions and partial digest stay separate");
        const auto* q = success.source.bytes + 0x248;
        Check(Read<uint64_t>(q, 0x08) == 1 && Read<uint64_t>(q, 0xB8) == quote.messageId &&
              Read<uint64_t>(q, 0xC8) == static_cast<uint64_t>(quote.timestamp) * 1000 &&
              Read<uint32_t>(q, 0xD0) == quote.timestamp && Read<uint32_t>(q, 0x94) == 0,
              "quote scalar widths preserve the full server ID, millisecond conversion and default local ID");
        CheckSourceReleased(success, "normal quote path releases source ownership once");
        const auto backing = static_cast<size_t>(ControlKind::referenceBacking);
        Check(!success.referenceBacking.alive && success.referenceBackingControl.strong == 0 &&
              success.referenceBackingControl.weak == 0 && success.destroyed[backing] == 1 &&
              success.deleted[backing] == 1,
              "the source releases the last backing reference after Start, exactly once");
    }

    for (const auto fault : {QuoteFault::initReturn, QuoteFault::initThrow,
                            QuoteFault::assignReturn, QuoteFault::assignThrow,
                            QuoteFault::convertReturn, QuoteFault::convertThrow,
                            QuoteFault::missingBacking, QuoteFault::textMismatch, QuoteFault::scalarMismatch,
                            QuoteFault::referenceDestroyThrow,
                            QuoteFault::attachThrow, QuoteFault::attachMissingBacking, QuoteFault::destroyThrow,
                            QuoteFault::digestReturn, QuoteFault::digestThrow}) {
        FakeState failure{};
        failure.quoteFault = fault;
        state = &failure;
        auto request = Request(target, reply);
        request.quote = &original;
        const auto result = SubmitNativeTextOnce(Api(), request);
        const bool complete = fault == QuoteFault::missingBacking || fault == QuoteFault::textMismatch ||
                              fault == QuoteFault::scalarMismatch || fault == QuoteFault::attachMissingBacking;
        const bool destroyReference = fault != QuoteFault::initReturn && fault != QuoteFault::initThrow &&
                                      fault != QuoteFault::assignReturn && fault != QuoteFault::assignThrow;
        const bool destroyQuote = destroyReference && fault != QuoteFault::convertReturn &&
                                  fault != QuoteFault::convertThrow;
        const bool sourceUncertain = fault == QuoteFault::attachThrow || fault == QuoteFault::digestReturn ||
                                     fault == QuoteFault::digestThrow;
        const bool expectedFailure = result.status == NativeAttemptStatus::preparation_failed &&
            result.stage == NativeSubmitStage::quote_prepare && !result.startEntered &&
            !result.startReturned && result.cleanupComplete == complete &&
            failure.allocateCalls == 0 && failure.delayedCalls == 0 && failure.startCalls == 0;
        if (!expectedFailure)
            std::fprintf(stderr, "quote fault=%u status=%u stage=%u entered=%u returned=%u cleanup=%u expected_cleanup=%u init=%u assign=%u convert=%u w_destroy=%u attach=%u q_destroy=%u allocate=%u delayed=%u start=%u\n",
                static_cast<unsigned>(fault), static_cast<unsigned>(result.status), static_cast<unsigned>(result.stage),
                static_cast<unsigned>(result.startEntered), static_cast<unsigned>(result.startReturned),
                static_cast<unsigned>(result.cleanupComplete), static_cast<unsigned>(complete),
                failure.referenceInitCalls, failure.assignCalls, failure.quoteFromMessageCalls,
                failure.referenceDestroyCalls, failure.quoteAttachCalls, failure.quoteDestroyCalls,
                failure.allocateCalls, failure.delayedCalls, failure.startCalls);
        Check(expectedFailure,
              "every quote preparation failure stops before delayed/Start and reports cleanup certainty");
        Check(failure.referenceDestroyCalls == static_cast<unsigned>(destroyReference) &&
              failure.quoteDestroyCalls == static_cast<unsigned>(destroyQuote),
              "W/Q cleanup never destroys partial initialization/mutation or retries a destructor");
        if (complete)
            Check(failure.quoteAttachCalls == (fault == QuoteFault::attachMissingBacking ? 1u : 0u),
                  "incomplete temporary or attached Q is rejected at the first failed ownership/field check");
        if (sourceUncertain) {
            const auto index = static_cast<size_t>(ControlKind::source);
            Check(failure.sourceControl.strong == 1 && failure.destroyed[index] == 0 &&
                  failure.deleted[index] == 0,
                  "abnormal quote attach or source assignment leaves uncertain source ownership untouched");
        } else CheckSourceReleased(failure, "quote failure releases a source whose ownership remains known");
    }

    for (size_t assignment = 0; assignment < referenceStringOffsets.size(); ++assignment) {
        for (const auto fault : {QuoteFault::assignReturn, QuoteFault::assignThrow}) {
            FakeState failure{};
            failure.quoteFault = fault;
            failure.quoteAssignFaultIndex = assignment;
            state = &failure;
            auto request = Request(target, reply);
            request.quote = &original;
            const auto result = SubmitNativeTextOnce(Api(), request);
            Check(result.status == NativeAttemptStatus::preparation_failed &&
                  result.stage == NativeSubmitStage::quote_prepare &&
                  !result.startEntered && !result.startReturned && !result.cleanupComplete,
                  "failure in any W string slot retains unknown cleanup and never enters Start");
            Check(failure.referenceInitCalls == 1 && failure.assignCalls == assignment + 3 &&
                  failure.referenceDestroyCalls == 0 && failure.quoteFromMessageCalls == 0 &&
                  failure.quoteAttachCalls == 0 && failure.quoteDestroyCalls == 0 &&
                  failure.allocateCalls == 0 && failure.delayedCalls == 0 && failure.startCalls == 0,
                  "each failed W assignment stops immediately without guessing W destruction eligibility");
            CheckSourceReleased(failure, "W assignment failure preserves known source cleanup");
        }
    }

    for (unsigned missing = 0; missing < 5; ++missing) {
        FakeState rejected{};
        state = &rejected;
        auto api = Api();
        if (missing == 0) api.referenceMessageInit = nullptr;
        else if (missing == 1) api.referenceMessageDestroy = nullptr;
        else if (missing == 2) api.quoteFromMessage = nullptr;
        else if (missing == 3) api.quoteDestroy = nullptr;
        else api.quoteAttach = nullptr;
        auto request = Request(target, reply);
        request.quote = &original;
        Check(SubmitNativeTextOnce(api, request).status == NativeAttemptStatus::api_unavailable &&
              rejected.factoryCalls == 0 && rejected.referenceInitCalls == 0,
              "each missing quote API is rejected before the source factory");
    }
    for (unsigned invalid = 0; invalid < 10; ++invalid) {
        FakeState rejected{};
        state = &rejected;
        auto quote = original;
        switch (invalid) {
        case 0: quote.messageId = 0; break;
        case 1: quote.messageType = 49; break;
        case 2: quote.conversationId = "different@chatroom"; break;
        case 3: quote.fromId = "invalid:id"; break;
        case 4: quote.senderId = target; break;
        case 5: quote.text.clear(); break;
        case 6: quote.text = std::string("bad\xC0\xAF", 5); break;
        case 7: quote.msgSource = std::string("bad\xED\xA0\x80", 6); break;
        case 8: quote.text = std::string("bad\0text", 8); break;
        case 9: quote.msgSource.assign(8193, 'x'); break;
        }
        auto request = Request(target, reply);
        request.quote = &quote;
        Check(SubmitNativeTextOnce(Api(), request).status == NativeAttemptStatus::invalid_argument &&
              rejected.factoryCalls == 0 && rejected.referenceInitCalls == 0,
              "invalid quote fields and UTF-8 are rejected before all native construction");
    }
}
} // namespace

void TestNativeText() {
    ResetVtables();
    const std::string target = "wxid_private_target";
    const std::string text = "SEND-SUBMIT-001 你好🙂\n第二行";

    FakeState success{};
    state = &success;
    auto result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::start_returned && result.startEntered &&
          result.startReturned && result.cleanupComplete &&
          result.stage == NativeSubmitStage::start, "normal Start return is recorded without delivery claim");
    Check(result.localUuid == success.localUuid,
          "factory source UUID is captured for local request correlation");
    Check(success.factoryCalls == 1 && success.assignCalls == 2 && success.allocateCalls == 1 &&
          success.delayedCalls == 1 && success.metadataCalls == 1 && success.startCalls == 1,
          "normal candidate invokes each native stage exactly once");
    Check(success.assignedTarget == target && success.assignedText == text &&
          success.freeCalls == 1 && !success.allocationOutstanding && success.payloadDestroyed,
          "UTF-8 bytes and all native temporary ownership are preserved");
    CheckSourceReleased(success, "normal path releases vector, delayed and original source references");
    for (const auto kind : {ControlKind::startupFirst, ControlKind::startupSecond}) {
        const auto index = static_cast<size_t>(kind);
        Check(success.destroyed[index] == 1 && success.deleted[index] == 1,
              "normal path releases both startup handle pairs");
    }

    FakeState groupSuccess{};
    state = &groupSuccess;
    const std::string groupTarget = "123456789@chatroom";
    const std::string groupText = "BOT-GROUP-001 你好🙂\n第二行";
    result = SubmitNativeTextOnce(Api(), Request(groupTarget, groupText));
    Check(result.status == NativeAttemptStatus::start_returned && result.cleanupComplete &&
          groupSuccess.startCalls == 1 && groupSuccess.assignedTarget == groupTarget &&
          groupSuccess.assignedText == groupText,
          "ordinary group text preserves exact target and body through the native source pipeline");
    CheckSourceReleased(groupSuccess, "group path preserves source ownership cleanup");

    FakeState missingFactory{};
    state = &missingFactory;
    auto missingFactoryApi = Api();
    missingFactoryApi.factory = nullptr;
    result = SubmitNativeTextOnce(missingFactoryApi, Request(target, text));
    Check(result.status == NativeAttemptStatus::api_unavailable && missingFactory.factoryCalls == 0,
          "text still requires its own factory after common lifecycle extraction");

    FakeState mentioned{};
    state = &mentioned;
    auto mentionedRequest = Request(groupTarget, groupText);
    mentionedRequest.atUserListUtf8 = "wxid_member_a,wxid_member_b";
    result = SubmitNativeTextOnce(Api(), mentionedRequest);
    Check(result.status == NativeAttemptStatus::start_returned && result.cleanupComplete &&
          mentioned.startCalls == 1 && mentioned.assignCalls == 3 &&
          mentioned.assignedMembers == mentionedRequest.atUserListUtf8 && mentioned.assignedText == groupText,
          "real at-list uses the native string assign independently of display text");
    CheckSourceReleased(mentioned, "mention source cleanup preserves ownership");
    FakeState invalidMention{};
    state = &invalidMention;
    mentionedRequest.atUserListUtf8 = "member,member";
    Check(SubmitNativeTextOnce(Api(), mentionedRequest).status == NativeAttemptStatus::invalid_argument &&
          invalidMention.factoryCalls == 0, "invalid at-list is rejected before native construction");

    FakeState missingUuid{};
    missingUuid.localUuid.clear();
    state = &missingUuid;
    result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::preparation_failed &&
          result.stage == NativeSubmitStage::local_uuid && !result.startEntered &&
          !result.startReturned && result.cleanupComplete && result.localUuid.empty(),
          "missing factory UUID fails before source mutation or Start");
    Check(missingUuid.assignCalls == 0 && missingUuid.allocateCalls == 0 &&
          missingUuid.startCalls == 0,
          "invalid UUID prevents all downstream native preparation");
    CheckSourceReleased(missingUuid, "invalid UUID releases the returned factory source");

    FakeState invalidFactoryReturn{};
    invalidFactoryReturn.invalidFactoryReturn = true;
    state = &invalidFactoryReturn;
    result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::preparation_failed &&
          result.stage == NativeSubmitStage::source_factory && result.cleanupComplete &&
          invalidFactoryReturn.startCalls == 0,
          "normally returned invalid factory result is still caller-owned");
    CheckSourceReleased(invalidFactoryReturn, "invalid factory return releases source once");

    FakeState allocationFailure{};
    allocationFailure.failAllocate = true;
    state = &allocationFailure;
    result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::preparation_failed && !result.startEntered &&
          !result.startReturned && result.cleanupComplete &&
          result.stage == NativeSubmitStage::vector_allocate,
          "construction failure remains known-before-Start");
    Check(allocationFailure.startCalls == 0 && allocationFailure.delayedCalls == 0 &&
          allocationFailure.freeCalls == 0, "construction failure performs no start or invalid free");
    CheckSourceReleased(allocationFailure, "construction failure releases factory source");

    FakeState invalidDelayedReturn{};
    invalidDelayedReturn.invalidDelayedReturn = true;
    state = &invalidDelayedReturn;
    result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::preparation_failed &&
          result.stage == NativeSubmitStage::delayed_factory && result.cleanupComplete &&
          invalidDelayedReturn.startCalls == 0 && invalidDelayedReturn.payloadDestroyed &&
          invalidDelayedReturn.freeCalls == 1 && !invalidDelayedReturn.allocationOutstanding,
          "normally returned invalid delayed result still releases caller-owned output");
    CheckSourceReleased(invalidDelayedReturn, "invalid delayed return releases source once");

    FakeState delayedException{};
    delayedException.throwInDelayed = true;
    state = &delayedException;
    result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::preparation_failed && !result.startEntered &&
          !result.startReturned && !result.cleanupComplete &&
          result.stage == NativeSubmitStage::delayed_factory,
          "throwing delayed factory leaves its output ownership unresolved");
    Check(delayedException.startCalls == 0 && delayedException.payloadDestroyed &&
          delayedException.freeCalls == 1 && !delayedException.allocationOutstanding,
          "delayed factory unwind is not followed by caller-side delayed destruction");
    CheckSourceReleased(delayedException,
                        "delayed factory exception releases vector and original source references");

    FakeState startException{};
    startException.throwInStart = true;
    state = &startException;
    result = SubmitNativeTextOnce(Api(), Request(target, text));
    Check(result.status == NativeAttemptStatus::unknown_after_start && result.startEntered &&
          !result.startReturned && !result.cleanupComplete && result.stage == NativeSubmitStage::start,
          "exception after entering Start is permanently unknown");
    Check(startException.startCalls == 1 && startException.payloadDestroyed &&
          startException.freeCalls == 1 && !startException.allocationOutstanding,
          "Start exception still releases delayed and vector ownership");
    CheckSourceReleased(startException, "Start exception releases all source references");
    const auto first = static_cast<size_t>(ControlKind::startupFirst);
    const auto second = static_cast<size_t>(ControlKind::startupSecond);
    Check(startException.destroyed[first] == 1 && startException.deleted[first] == 1 &&
          startException.startupFirst.strong == 0 && startException.startupFirst.weak == 0 &&
          startException.destroyed[second] == 0 && startException.deleted[second] == 0,
          "Start exception leaves caller-side stale output untouched after native unwind");

    FakeState guardRejected{};
    state = &guardRejected;
    unsigned guardCalls = 0;
    auto guardedRequest = Request(target, text);
    guardedRequest.beforeStart = [&guardCalls] { ++guardCalls; return false; };
    result = SubmitNativeTextOnce(Api(), guardedRequest);
    Check(result.status == NativeAttemptStatus::preparation_failed && !result.startEntered &&
          !result.startReturned && result.cleanupComplete &&
          result.stage == NativeSubmitStage::before_start && guardCalls == 1,
          "fresh pre-Start guard can reject after preparation without entering Start");
    Check(guardRejected.startCalls == 0 && guardRejected.payloadDestroyed &&
          guardRejected.freeCalls == 1 && !guardRejected.allocationOutstanding,
          "guard rejection releases delayed and vector ownership");
    CheckSourceReleased(guardRejected, "guard rejection releases all source references");

    FakeState rejected{};
    state = &rejected;
    auto request = Request(target, text);
    request.authorizedUiThreadId = GetCurrentThreadId() == 1 ? 2 : 1;
    result = SubmitNativeTextOnce(Api(), request);
    Check(result.status == NativeAttemptStatus::ui_thread_required && rejected.factoryCalls == 0,
          "unverified calling thread is rejected before native construction");
    const std::string invalidText("bad\0text", 8);
    request = Request(target, invalidText);
    result = SubmitNativeTextOnce(Api(), request);
    Check(result.status == NativeAttemptStatus::invalid_argument && rejected.factoryCalls == 0,
          "embedded NUL is rejected before native construction");

    TestNativeQuoteText();
}
