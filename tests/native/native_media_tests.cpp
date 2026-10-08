// Exercises native media construction with local fake APIs only. No Weixin
// function is resolved or called by this test.
#include "monitor/native/native_media.hpp"
#include "monitor/config/version_profile.hpp"
#include <cstddef>
#include <cstring>
#include <new>
#include <string>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);

namespace {
namespace profile = active_profile::text;

struct FakeControl {
    void** vtable{};
    volatile LONG strong{};
    volatile LONG weak{};
};
static_assert(offsetof(FakeControl, strong) == 8 && offsetof(FakeControl, weak) == 12);

struct FakePayload {
    void** vtable{};
    NativeSharedPair source{};
};

struct FakeState {
    alignas(8) unsigned char source[0x800]{};
    FakeControl sourceControl{};
    FakePayload payload{};
    std::string uuid = "media-factory-uuid";
    std::string target, audio;
    std::wstring path;
    unsigned imageFactoryCalls{}, voiceFactoryCalls{}, assignCalls{}, wideAssignCalls{};
    unsigned allocateCalls{}, freeCalls{}, delayedCalls{}, metadataCalls{}, startCalls{};
    unsigned sourceDestroyed{}, controlDeleted{}, payloadDestroyed{};
    bool failAssign{}, failWideAssign{}, failAllocate{}, throwFactory{}, throwStart{};
    bool invalidFactoryReturn{};
    bool voiceInputTerminated{}, voiceDeepCopied{}, metadataMatched{};
};

FakeState* state = nullptr;
void* sourceVtable[1]{}, *controlVtable[2]{}, *payloadVtable[5]{};
constexpr char metadata1[] = "NativeMediaOne", metadata2[] = "NativeMediaTwo";

template<class T>
T Read(const void* object, size_t offset) {
    T value{};
    memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
    return value;
}

template<class T>
void Write(void* object, size_t offset, const T& value) {
    memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}

std::string ReadNarrow(const void* object) {
    const auto length = Read<uint64_t>(object, 0x10);
    const auto capacity = Read<uint64_t>(object, 0x18);
    const char* bytes = capacity < 16 ? static_cast<const char*>(object)
                                     : Read<const char*>(object, 0);
    return std::string(bytes, static_cast<size_t>(length));
}

std::wstring ReadWide(const void* object) {
    const auto length = Read<uint64_t>(object, 0x10);
    const auto capacity = Read<uint64_t>(object, 0x18);
    const wchar_t* units = capacity < 8 ? static_cast<const wchar_t*>(object)
                                       : Read<const wchar_t*>(object, 0);
    return std::wstring(units, static_cast<size_t>(length));
}

void WriteNarrow(void* object, const std::string& value) {
    memset(object, 0, 0x20);
    const uint64_t length = value.size(), capacity = length < 16 ? 15 : length;
    if (length < 16) memcpy(object, value.data(), static_cast<size_t>(length));
    else { const char* pointer = value.data(); Write(object, 0, pointer); }
    Write(object, 0x10, length); Write(object, 0x18, capacity);
}

void WriteWide(void* object, const std::wstring& value) {
    memset(object, 0, 0x20);
    const uint64_t length = value.size(), capacity = length < 8 ? 7 : length;
    if (length < 8) memcpy(object, value.data(), static_cast<size_t>(length) * sizeof(wchar_t));
    else { const wchar_t* pointer = value.data(); Write(object, 0, pointer); }
    Write(object, 0x10, length); Write(object, 0x18, capacity);
}

void ReleaseFakeSource(NativeSharedPair pair) {
    auto* control = static_cast<FakeControl*>(pair.control);
    if (!control || InterlockedDecrement(&control->strong) != 0) return;
    reinterpret_cast<void (__fastcall*)(void*)>(control->vtable[0])(control);
    if (InterlockedDecrement(&control->weak) == 0)
        reinterpret_cast<void (__fastcall*)(void*)>(control->vtable[1])(control);
}

void __fastcall DestroySource(void*) { ++state->sourceDestroyed; }
void __fastcall DeleteControl(void*) { ++state->controlDeleted; }
void __fastcall DestroyPayload(void* raw, bool) {
    auto* payload = static_cast<FakePayload*>(raw);
    ReleaseFakeSource(payload->source);
    payload->source = {};
    ++state->payloadDestroyed;
}

void Reset(FakeState& value) {
    state = &value;
    sourceVtable[0] = nullptr;
    controlVtable[0] = reinterpret_cast<void*>(&DestroySource);
    controlVtable[1] = reinterpret_cast<void*>(&DeleteControl);
    for (void*& entry : payloadVtable) entry = nullptr;
    payloadVtable[4] = reinterpret_cast<void*>(&DestroyPayload);
}

void ConstructSource() {
    memset(state->source, 0, sizeof(state->source));
    void** table = sourceVtable;
    Write(state->source, 0, table);
    WriteNarrow(state->source + profile::kLocalUuidOffset, state->uuid);
    state->sourceControl = {controlVtable, 1, 1};
}

NativeSharedPair* __fastcall ImageFactory(NativeSharedPair* out) {
    ++state->imageFactoryCalls;
    if (state->throwFactory) throw 17;
    ConstructSource();
    *out = {state->source, &state->sourceControl};
    return state->invalidFactoryReturn ? nullptr : out;
}

NativeSharedPair* __fastcall VoiceFactory(
    NativeSharedPair* out, const void* target, const void* audio,
    const uint32_t* duration, const void* metadata) {
    ++state->voiceFactoryCalls;
    if (state->throwFactory) throw 18;
    const auto audioLength = Read<uint64_t>(audio, 0x10);
    const auto audioCapacity = Read<uint64_t>(audio, 0x18);
    const char* audioData = audioCapacity < 16 ? static_cast<const char*>(audio)
                                              : Read<const char*>(audio, 0);
    state->voiceInputTerminated = audioData[audioLength] == 0;
    state->target = ReadNarrow(target);
    state->audio.assign(audioData, static_cast<size_t>(audioLength));
    state->metadataMatched = Read<uint32_t>(metadata, 0) == 4 &&
        Read<uint32_t>(metadata, 4) == 3 && Read<uint64_t>(metadata, 8) == 30000 &&
        Read<uint8_t>(metadata, 0x10) == 0 && Read<uint64_t>(metadata, 0x28) == 0 &&
        Read<uint64_t>(metadata, 0x30) == 15;
    ConstructSource();
    WriteNarrow(state->source + profile::kTargetOffset, state->target);
    WriteNarrow(state->source + 0x758, state->audio);
    Write(state->source, 0x778, *duration);
    Write(state->source, 0x780, uint32_t{4});
    state->voiceDeepCopied = Read<const char*>(state->source + 0x758, 0) != audioData;
    *out = {state->source, &state->sourceControl};
    return out;
}

void* __fastcall Assign(void* destination, const void* bytes, uint64_t length) {
    ++state->assignCalls;
    if (state->failAssign) return nullptr;
    state->target.assign(static_cast<const char*>(bytes), static_cast<size_t>(length));
    WriteNarrow(destination, state->target);
    return destination;
}

void* __fastcall WideAssign(void* destination, const void* source) {
    ++state->wideAssignCalls;
    if (state->failWideAssign) return nullptr;
    state->path = ReadWide(source);
    WriteWide(destination, state->path);
    return destination;
}

void* __fastcall Allocate(size_t bytes) {
    ++state->allocateCalls;
    return state->failAllocate ? nullptr : ::operator new(bytes);
}
void __fastcall Free(void* allocation, size_t) {
    ++state->freeCalls;
    ::operator delete(allocation);
}

NativeDelayedHolder* __fastcall MakeDelayed(
    void*, NativeDelayedHolder* out, const NativeSharedPairVector* sources, unsigned char) {
    ++state->delayedCalls;
    auto pair = *sources->begin;
    InterlockedIncrement(&static_cast<FakeControl*>(pair.control)->strong);
    state->payload = {payloadVtable, pair};
    Write(out, profile::kDelayedPayloadOffset, static_cast<void*>(&state->payload));
    return out;
}

NativeMetadata* __fastcall InitMetadata(
    NativeMetadata* out, const void*, const void*, uint32_t) {
    ++state->metadataCalls;
    return out;
}

NativeStartupHandle* __fastcall Start(
    NativeDelayedHolder*, NativeStartupHandle* out, NativeCallableHolder*,
    NativeCallableHolder*, NativeCallableHolder*, const NativeMetadata*) {
    ++state->startCalls;
    if (state->throwStart) throw 23;
    return out;
}

NativeMediaApi Api() {
    NativeMediaApi api{};
    api.common.assign = Assign;
    api.common.allocate = Allocate;
    api.common.free = Free;
    api.common.makeDelayed = MakeDelayed;
    api.common.initMetadata = InitMetadata;
    api.common.start = Start;
    api.common.metadataStaticText1 = metadata1;
    api.common.metadataStaticText2 = metadata2;
    api.common.metadataValue = 0x216;
    api.imageFactory = ImageFactory;
    api.voiceFactory = VoiceFactory;
    api.wideAssign = WideAssign;
    return api;
}

NativeMediaRequest ImageRequest() {
    NativeMediaRequest request{};
    request.targetUtf8 = "wxid_media_target";
    request.kind = NativeMediaKind::image;
    request.imagePath = L"C:\\媒体样本\\受控图片-abcdef.png";
    request.authorizedUiThreadId = GetCurrentThreadId();
    return request;
}

NativeMediaRequest VoiceRequest(const std::string& audio) {
    NativeMediaRequest request{};
    request.targetUtf8 = "123456789@chatroom";
    request.kind = NativeMediaKind::voice;
    request.audioBytes = audio;
    request.durationMs = 4700;
    request.authorizedUiThreadId = GetCurrentThreadId();
    return request;
}

bool Released(const FakeState& value) {
    return value.sourceControl.strong == 0 && value.sourceControl.weak == 0 &&
           value.sourceDestroyed == 1 && value.controlDeleted == 1;
}
} // namespace

void TestNativeMedia() {
    FakeState image{};
    Reset(image);
    auto imageRequest = ImageRequest();
    imageRequest.beforeStart = [&] {
        return Read<uint64_t>(image.source, profile::kTypeOffset) == 3 &&
               ReadNarrow(image.source + profile::kTargetOffset) == imageRequest.targetUtf8 &&
               ReadWide(image.source + 0x120) == imageRequest.imagePath && image.startCalls == 0;
    };
    auto result = SubmitNativeMediaOnce(Api(), imageRequest);
    Check(result.status == NativeAttemptStatus::start_returned && result.startEntered &&
          result.startReturned && result.cleanupComplete && result.localUuid == image.uuid &&
          image.imageFactoryCalls == 1 && image.voiceFactoryCalls == 0 &&
          image.assignCalls == 1 && image.wideAssignCalls == 1 && image.startCalls == 1 &&
          image.payloadDestroyed == 1 && Released(image),
          "image source uses exact factory, target, UTF-16 path and shared lifecycle once");

    std::string silk("\x02#!SILK_V3", 10);
    silk.append("\0\xff\xc0\x80", 4);
    silk.append(32, '\x5a');
    FakeState voice{};
    Reset(voice);
    auto voiceRequest = VoiceRequest(silk);
    voiceRequest.beforeStart = [&] {
        return Read<uint64_t>(voice.source, profile::kTypeOffset) == 34 &&
               ReadNarrow(voice.source + profile::kTargetOffset) == voiceRequest.targetUtf8 &&
               ReadNarrow(voice.source + 0x758) == silk &&
               Read<uint32_t>(voice.source, 0x778) == 4700 &&
               Read<uint32_t>(voice.source, 0x780) == 4 && voice.startCalls == 0;
    };
    result = SubmitNativeMediaOnce(Api(), voiceRequest);
    Check(result.status == NativeAttemptStatus::start_returned && result.cleanupComplete &&
          voice.voiceFactoryCalls == 1 && voice.imageFactoryCalls == 0 &&
          voice.voiceInputTerminated && voice.voiceDeepCopied && voice.metadataMatched &&
          voice.audio == silk && voice.startCalls == 1 && Released(voice),
          "voice factory receives terminated length-aware binary, duration pointer and exact metadata");

    FakeState prepareFailure{};
    prepareFailure.failWideAssign = true;
    Reset(prepareFailure);
    result = SubmitNativeMediaOnce(Api(), ImageRequest());
    Check(result.status == NativeAttemptStatus::preparation_failed &&
          result.stage == NativeSubmitStage::media_prepare && !result.startEntered &&
          result.cleanupComplete && prepareFailure.startCalls == 0 && Released(prepareFailure),
          "media preparation failure releases the returned source and never starts");

    FakeState invalidFactoryReturn{};
    invalidFactoryReturn.invalidFactoryReturn = true;
    Reset(invalidFactoryReturn);
    result = SubmitNativeMediaOnce(Api(), ImageRequest());
    Check(result.status == NativeAttemptStatus::preparation_failed &&
          result.stage == NativeSubmitStage::media_factory && result.cleanupComplete &&
          invalidFactoryReturn.startCalls == 0 && Released(invalidFactoryReturn),
          "normally returned invalid image factory result still releases source once");

    FakeState guardFailure{};
    Reset(guardFailure);
    auto guarded = ImageRequest();
    guarded.beforeStart = [] { return false; };
    result = SubmitNativeMediaOnce(Api(), guarded);
    Check(result.status == NativeAttemptStatus::preparation_failed &&
          result.stage == NativeSubmitStage::before_start && result.cleanupComplete &&
          guardFailure.startCalls == 0 && guardFailure.payloadDestroyed == 1 && Released(guardFailure),
          "fresh media guard rejection uses shared delayed/vector cleanup");

    FakeState startFailure{};
    startFailure.throwStart = true;
    Reset(startFailure);
    result = SubmitNativeMediaOnce(Api(), ImageRequest());
    Check(result.status == NativeAttemptStatus::unknown_after_start && result.startEntered &&
          !result.startReturned && !result.cleanupComplete && startFailure.startCalls == 1 &&
          startFailure.payloadDestroyed == 1 && Released(startFailure),
          "throw after media Start stays unknown without retry and still drops known references once");

    FakeState wrongThread{};
    Reset(wrongThread);
    auto wrongThreadRequest = ImageRequest();
    wrongThreadRequest.authorizedUiThreadId = GetCurrentThreadId() == 1 ? 2 : 1;
    result = SubmitNativeMediaOnce(Api(), wrongThreadRequest);
    Check(result.status == NativeAttemptStatus::ui_thread_required &&
          wrongThread.imageFactoryCalls == 0 && wrongThread.voiceFactoryCalls == 0,
          "media thread gate rejects before either native factory");

    FakeState invalid{};
    Reset(invalid);
    auto invalidRequest = VoiceRequest(silk);
    invalidRequest.durationMs = 0;
    Check(SubmitNativeMediaOnce(Api(), invalidRequest).status == NativeAttemptStatus::invalid_argument &&
          invalid.voiceFactoryCalls == 0, "zero-duration voice is rejected before native input construction");
    invalidRequest = ImageRequest();
    invalidRequest.audioBytes = silk;
    Check(SubmitNativeMediaOnce(Api(), invalidRequest).status == NativeAttemptStatus::invalid_argument &&
          invalid.imageFactoryCalls == 0, "mixed image and voice fields are rejected before factories");

    FakeState factoryFailure{};
    factoryFailure.throwFactory = true;
    Reset(factoryFailure);
    result = SubmitNativeMediaOnce(Api(), ImageRequest());
    Check(result.status == NativeAttemptStatus::preparation_failed && !result.cleanupComplete &&
          factoryFailure.imageFactoryCalls == 1 && factoryFailure.startCalls == 0 &&
          factoryFailure.sourceDestroyed == 0,
          "throwing media factory leaves unknown partial ownership untouched and never retries");
}
