// Exercises the production capture with local objects, no WeChat or installed hooks.
#include "monitor/diagnostics/send_trace.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/native/target.hpp"
#include <cstring>
#include <string>
#include <vector>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);
namespace {
unsigned calls = 0;
const void* expectedCallable = nullptr;
const void* expectedCompletion = nullptr;
bool throwFromOriginal = false;
void __fastcall Original(const void* callable, const void* completion) {
    ++calls;
    Check(callable == expectedCallable && completion == expectedCompletion, "submit arguments unchanged");
    Check(GetLastError() == 3141, "submit entry LastError preserved");
    SetLastError(5926);
    if (throwFromOriginal) throw 42;
}
void Call(const void* callable, const void* completion) {
    expectedCallable = callable; expectedCompletion = completion;
    const auto before = calls;
    SetLastError(3141);
    HookSendSubmit(callable, completion);
    Check(calls == before + 1 && GetLastError() == 5926, "submit called once with returned LastError");
}
void NoEvent() {
    Event absent{};
    Check(!PopEvent(absent), "nonmatching or exhausted submit trace emits no event");
}
const SubmitSnapshot& Submit(const Event& event) { return event.Get<SubmitSnapshot>(); }
const TextSubmitSnapshot& TextSource(const Event& event) {
    return std::get<TextSubmitSnapshot>(Submit(event).source);
}
const ImageSubmitSnapshot& ImageSource(const Event& event) {
    return std::get<ImageSubmitSnapshot>(Submit(event).source);
}
const VoiceSubmitSnapshot& VoiceSource(const Event& event) {
    return std::get<VoiceSubmitSnapshot>(Submit(event).source);
}
template<class T> void Put(void* memory, size_t offset, const T& value) {
    memcpy(static_cast<unsigned char*>(memory) + offset, &value, sizeof(value));
}
void Text(void* memory, size_t offset, const std::string& value) {
    unsigned char native[0x20]{};
    const uint64_t size = value.size(), capacity = size < 16 ? 15 : size;
    if (size < 16) memcpy(native, value.data(), static_cast<size_t>(size));
    else { const char* ptr = value.data(); Put(native, 0, ptr); }
    Put(native, 0x10, size); Put(native, 0x18, capacity);
    memcpy(static_cast<unsigned char*>(memory) + offset, native, sizeof(native));
}
void Wide(void* memory, size_t offset, const std::u16string& value) {
    unsigned char native[0x20]{};
    const uint64_t size = value.size(), capacity = size < 8 ? 7 : size;
    if (size < 8) memcpy(native, value.data(), static_cast<size_t>(size) * sizeof(char16_t));
    else { const char16_t* ptr = value.data(); Put(native, 0, ptr); }
    Put(native, 0x10, size); Put(native, 0x18, capacity);
    memcpy(static_cast<unsigned char*>(memory) + offset, native, sizeof(native));
}
void Binary(void* memory, size_t offset, const std::vector<uint8_t>& value) {
    unsigned char native[0x20]{};
    const uint64_t size = value.size(), capacity = size < 16 ? 15 : size;
    if (size < 16) memcpy(native, value.data(), static_cast<size_t>(size));
    else { const uint8_t* ptr = value.data(); Put(native, 0, ptr); }
    Put(native, 0x10, size); Put(native, 0x18, capacity);
    memcpy(static_cast<unsigned char*>(memory) + offset, native, sizeof(native));
}
} // namespace

std::string TestSendSubmit() {
    const auto previousBase = g_weixin;
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, kExpectedImageSize, MEM_RESERVE, PAGE_READWRITE));
    Check(image != nullptr, "reserve fake image address space");
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto page = kRootObjectPointerRva / system.dwPageSize * system.dwPageSize;
    Check(VirtualAlloc(image + page, system.dwPageSize, MEM_COMMIT, PAGE_READWRITE) != nullptr,
          "commit only one fake root pointer page");
    g_weixin = reinterpret_cast<HMODULE>(image);
    const auto base = reinterpret_cast<uintptr_t>(image);
    g_originalSendSubmit = Original;
    alignas(8) unsigned char source[0x798]{}, callable[0x20]{}, completion[0xE8]{};
    alignas(8) unsigned char root[0x320]{}, executor[0x18]{}, inner[0xF0]{};
    uint64_t sourceControl[2]{123, 456};
    const uint64_t pair[]{reinterpret_cast<uintptr_t>(source), reinterpret_cast<uintptr_t>(sourceControl)};
    const auto pairAddress = reinterpret_cast<uintptr_t>(pair);
    const uint64_t vector[]{pairAddress, pairAddress + sizeof(pair), pairAddress + sizeof(pair)};
    Put(source, 0, base + kSendContextSourceVtableRva);
    Put(source, 0x118, uint32_t{1});
    const std::string target = "a_long_private_target_id", uuid = "uuid-local-only";
    const std::string text = "SEND-SUBMIT-001 你好🙂\n第二行";
    Text(source, 0xB0, target); Text(source, 0x758, text); Text(source, 0x6F8, uuid);
    Put(source, 0x1C8, static_cast<uint64_t>(text.size()));
    Put(callable, 0, base + kSendSubmitCallableVtableRva); Put(callable, 8, vector);
    const uint64_t shared[]{0x111, 0x222}, weak[]{0x333, 0x444};
    Put(completion, 0, shared); Put(completion, 0xD8, weak); Put(completion, 0xD0, uint64_t{77});
    uint64_t callback = base + 0x123456;
    Put(completion, 0x48, reinterpret_cast<uintptr_t>(&callback));
    Put(completion, 0xC8, uint64_t{1}); // unreadable payload, distinct from empty second callback
    Put(image, kRootObjectPointerRva, reinterpret_cast<uintptr_t>(root));
    const uint64_t executorPair[]{reinterpret_cast<uintptr_t>(executor), 0x555};
    Put(root, 0x310, executorPair); Put(executor, 0x10, reinterpret_cast<uintptr_t>(inner));
    inner[0xE9] = 1;

    ConfigureSendSubmitTrace(kExpectedImageSize, false, false);
    Call(callable, completion); NoEvent();
    ConfigureSendSubmitTrace(kExpectedImageSize, true, false);
    Call(nullptr, completion); NoEvent();
    const std::string unrelated = "ordinary unmarked text";
    Text(source, 0x758, unrelated); Call(callable, completion); NoEvent();
    Text(source, 0x758, text);
    Put(source, 0x118, uint32_t{3}); Call(callable, completion); NoEvent();
    Put(source, 0x118, uint32_t{1});
    Put(callable, 0, uint64_t{1}); Call(callable, completion); NoEvent();
    Put(callable, 0, base + kSendSubmitCallableVtableRva);
    Put(callable, 0x10, pairAddress + 1); Call(callable, completion); NoEvent();
    Put(callable, 8, vector);

    Call(callable, completion);
    Event enter{}, returned{};
    Check(PopEvent(enter) && PopEvent(returned), "marked submit emits entry and normal return");
    Check(enter.Kind() == EventKind::SendSubmitEnter && returned.Kind() == EventKind::SendSubmitReturn &&
          enter.header.callId == returned.header.callId && returned.header.sequence > enter.header.sequence,
          "submit diagnostic correlation");
    Check(Submit(enter).sourceObjectAddress == pair[0] && Submit(enter).sourceControlAddress == pair[1] &&
          TextSource(enter).content.read.status == ReadStatus::Ok && TextSource(enter).content.view() == text &&
          Submit(enter).target.view() == target && Submit(enter).localUuid.view() == uuid,
          "submit captures exact identity plus bounded Unicode native strings");
    Check(Submit(enter).completionReadable && Submit(enter).sharedPair[1] == shared[1] &&
          Submit(enter).weakPair[0] == weak[0] && Submit(enter).completionValue == 77 &&
          Submit(enter).callbackReads[0] == ReadStatus::Ok &&
          Submit(enter).callbackReads[1] == ReadStatus::Empty &&
          Submit(enter).callbackReads[2] == ReadStatus::Unreadable,
          "completion fields distinguish valid empty and unreadable callback payloads");
    Check(Submit(enter).executorFlagsReadable && Submit(enter).executorFlags == 1 &&
          Submit(enter).executorPair[0] == executorPair[0], "executor captured without native getter");
    Check(sourceControl[0] == 123 && sourceControl[1] == 456 &&
          Submit(returned).returnTick >= enter.header.tick,
          "observation leaves source references untouched and records separate return tick");
    const auto json = SerializeEvent(enter) + SerializeEvent(returned);
    Check(json.find("\"kind\":\"send_submit_enter\"") != std::string::npos &&
          json.find("\"vtable_rva\":\"0x123456\"") != std::string::npos &&
          json.find("\"normal_return\":true") != std::string::npos &&
          json.find("\"msg_type\"") == std::string::npos,
          "dedicated submit schema does not masquerade as messages or delivery");
    Call(callable, completion); NoEvent();
    ConfigureSendSubmitTrace(kExpectedImageSize, true, false);
    Call(callable, reinterpret_cast<const void*>(1));
    Check(PopEvent(enter) && PopEvent(returned) && !Submit(enter).completionReadable,
          "unreadable completion is logged as unknown without affecting original call");
    Check(SerializeEvent(enter).find("\"completion_shared_raw\":null") != std::string::npos,
          "unreadable fields are not reported as zero values");
    // Each rich marker has its own slot, independent of the legacy sample.
    const std::string mentions = "wxid_member_a,wxid_member_b";
    const std::string copyUuid = "quoted-local-uuid", optional = "optional-snapshot";
    Text(source, 0x778, mentions); Text(source, 0x738, copyUuid); Text(source, 0x120, optional);
    const std::string atText = "SEND-AT-001 @member-a @member-b";
    Text(source, 0x758, atText);
    Call(callable, completion);
    Check(PopEvent(enter) && PopEvent(returned) && Submit(enter).sourceType == 1 &&
          TextSource(enter).atUserList.read.status == ReadStatus::Ok &&
          TextSource(enter).atUserList.view() == mentions &&
          TextSource(enter).copyFromUuid.view() == copyUuid &&
          TextSource(enter).optionalText.view() == optional,
          "rich trace snapshots at-list and optional native strings exactly");
    Check(SerializeEvent(enter).find("\"at_user_list\":\"wxid_member_a,wxid_member_b\"") != std::string::npos,
          "at-list serialized separately from msgsource XML");
    Call(callable, completion); NoEvent();
    Put(source, 0x118, uint32_t{49}); Put(source, 0x11C, uint32_t{57});
    Put(source, 0x240, uint8_t{1}); Put(source, 0x688, uint8_t{1});
    Text(source, 0x1F0, target); Text(source, 0x220, uuid);
    Put(source, 0x218, uint64_t{18446744073709551614ULL});
    Put(source, 0x248, base + profiles::active::text::kReferenceMessageVtableRva);
    Text(source, 0x2B8, target);
    Text(source, 0x6D8, unrelated);
    const std::string quoteText = "SEND-QUOTE-001 引用测试🙂";
    Text(source, 0x758, quoteText);
    Call(callable, completion);
    Check(PopEvent(enter) && PopEvent(returned) && Submit(enter).sourceType == 49 &&
          Submit(enter).sourceSubtype == 57 &&
          TextSource(enter).reference.messageVtableMatches &&
          TextSource(enter).reference.idWide == 18446744073709551614ULL &&
          std::string(TextSource(enter).reference.messageStrings[3].data()) == target,
          "known quote subtype can be observed once on TextSource vtable");
    Check(SerializeEvent(enter).find("\"reference_source_218\":\"18446744073709551614\"") != std::string::npos,
          "reference scalar preserves unsigned 64-bit precision in JSON");
    Call(callable, completion); NoEvent();
    const std::string automaticQuote = "BOT-QUOTE-002 引用修复验证🙂";
    Text(source, 0x758, automaticQuote);
    Call(callable, completion);
    Check(PopEvent(enter) && PopEvent(returned) && Submit(enter).sourceType == 49,
          "one automatic quote is captured independently of the manual quote sample");
    Call(callable, completion); NoEvent();
    Put(source, 0x118, uint32_t{1}); Put(source, 0x11C, uint32_t{0});
    const std::string automaticMention = "BOT-AT-001 @member-a";
    Text(source, 0x758, automaticMention);
    Call(callable, completion);
    Check(PopEvent(enter) && PopEvent(returned) && TextSource(enter).atUserList.view() == mentions,
          "one automatic mention is captured independently of the manual mention sample");
    Call(callable, completion); NoEvent();

    // Media mode is separately gated and has four samples per concrete type.
    constexpr uintptr_t imageVtableRva = 0x08D95A08, voiceVtableRva = 0x09798F28;
    Put(source, 0, base + imageVtableRva);
    Put(source, 0x118, uint32_t{3}); Put(source, 0x11C, uint32_t{0});
    ConfigureSendSubmitTrace(kExpectedImageSize, true, false);
    Call(callable, completion); NoEvent();

    const std::u16string imagePath = u"C:\\媒体\\长路径\\图片样本-发送采样.png";
    const std::vector<uint8_t> imageBytes(33, 0xA5);
    Wide(source, 0x120, imagePath); Binary(source, 0x768, imageBytes);
    Put(source, 0x788, uint32_t{1920}); Put(source, 0x78C, uint32_t{1080});
    ConfigureSendSubmitTrace(kExpectedImageSize, false, true);
    std::string imageJson;
    for (unsigned i = 0; i < 4; ++i) {
        Call(callable, completion);
        Check(PopEvent(enter) && PopEvent(returned) &&
              std::holds_alternative<ImageSubmitSnapshot>(Submit(enter).source) &&
              Submit(enter).sourceType == 3 && Submit(enter).sourceSubtype == 0 &&
              Submit(enter).sourceVtableRva == imageVtableRva &&
              ImageSource(enter).path.read.status == ReadStatus::Ok &&
              ImageSource(enter).dataRead.status == ReadStatus::Ok &&
              ImageSource(enter).dataRead.originalBytes == imageBytes.size() &&
              ImageSource(enter).dataRead.capturedBytes == 0 &&
              ImageSource(enter).widthReadable && ImageSource(enter).width == 1920 &&
              ImageSource(enter).heightReadable && ImageSource(enter).height == 1080 &&
              Submit(enter).target.read.status == ReadStatus::Ok && Submit(enter).target.view() == target &&
              Submit(enter).localUuid.read.status == ReadStatus::Ok && Submit(enter).localUuid.view() == uuid,
              "image source captures confirmed base identity, path, layout and dimensions");
        if (!i) imageJson = SerializeEvent(enter);
    }
    Call(callable, completion); NoEvent();
    Check(imageJson.find("\"media_kind\":\"image\"") != std::string::npos &&
          imageJson.find("\"image_path\":\"C:\\\\媒体") != std::string::npos &&
          imageJson.find("\"image_data_layout\":{\"status\":\"ok\",\"original_bytes\":33,\"captured_bytes\":0}") != std::string::npos &&
          imageJson.find("\"image_width\":1920") != std::string::npos &&
          imageJson.find("\"vtable_rva\":\"0x123456\"") != std::string::npos &&
          imageJson.find("\"target\":\"a_long_private_target_id\"") != std::string::npos &&
          imageJson.find("\"local_uuid\":\"uuid-local-only\"") != std::string::npos &&
          imageJson.find("\"content\"") == std::string::npos,
          "image JSON uses the actual source vtable base and only confirmed base identity fields");

    Put(source, 0, base + voiceVtableRva);
    Put(source, 0x118, uint32_t{34}); Put(source, 0x11C, uint32_t{0});
    std::vector<uint8_t> voiceBytes(300, 0x5A);
    voiceBytes[0] = 0x00; voiceBytes[1] = 0xFF; voiceBytes[2] = 0xC0;
    voiceBytes[3] = 0x80; voiceBytes[4] = 0x41;
    Binary(source, 0x758, voiceBytes);
    Put(source, 0x778, uint32_t{2468}); Put(source, 0x780, uint32_t{4});
    std::string voiceJson;
    for (unsigned i = 0; i < 4; ++i) {
        Call(callable, completion);
        Check(PopEvent(enter) && PopEvent(returned) &&
              std::holds_alternative<VoiceSubmitSnapshot>(Submit(enter).source) &&
              Submit(enter).sourceType == 34 && Submit(enter).sourceSubtype == 0 &&
              Submit(enter).sourceVtableRva == voiceVtableRva &&
              VoiceSource(enter).dataRead.status == ReadStatus::Truncated &&
              VoiceSource(enter).dataRead.originalBytes == voiceBytes.size() &&
              VoiceSource(enter).dataRead.capturedBytes == 256 &&
              VoiceSource(enter).lengthReadable && VoiceSource(enter).length == 2468 &&
              VoiceSource(enter).formatReadable && VoiceSource(enter).format == 4 &&
              Submit(enter).target.read.status == ReadStatus::Ok && Submit(enter).target.view() == target &&
              Submit(enter).localUuid.read.status == ReadStatus::Ok && Submit(enter).localUuid.view() == uuid,
              "voice source captures base identity, bounded binary prefix and confirmed XML source scalars");
        if (!i) voiceJson = SerializeEvent(enter);
    }
    Call(callable, completion); NoEvent();
    Check(voiceJson.find("\"media_kind\":\"voice\"") != std::string::npos &&
          voiceJson.find("\"voice_prefix_hex\":\"00ffc08041") != std::string::npos &&
          voiceJson.find("\"voice_data_read\":{\"status\":\"truncated\",\"original_bytes\":300,\"captured_bytes\":256}") != std::string::npos &&
          voiceJson.find("\"source_voicelength\":2468") != std::string::npos &&
          voiceJson.find("\"source_voiceformat\":4") != std::string::npos &&
          voiceJson.find("\"content\"") == std::string::npos,
          "voice binary is hex serialized and never passed through JSON text escaping");

    Put(source, 0x118, uint32_t{1}); Put(source, 0x11C, uint32_t{0}); Text(source, 0x758, text);
    Put(source, 0, base + kSendContextSourceVtableRva);
    ConfigureSendSubmitTrace(kExpectedImageSize, true, false);
    throwFromOriginal = true;
    bool caught = false;
    try { Call(callable, completion); } catch (int value) { caught = value == 42; }
    throwFromOriginal = false;
    Check(caught && PopEvent(enter) && enter.Kind() == EventKind::SendSubmitEnter,
          "original exception propagates without a false normal-return event");
    NoEvent();
    ConfigureSendSubmitTrace(0, false, false);
    g_originalSendSubmit = nullptr;
    g_weixin = previousBase;
    VirtualFree(image, 0, MEM_RELEASE);
    return json;
}
