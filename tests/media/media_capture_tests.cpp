#include "monitor/media/media_capture.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/native/target.hpp"
#include <cstring>
#include <string>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);
namespace {
template<class T> void Put(void* address, size_t offset, T value) {
    memcpy(static_cast<uint8_t*>(address) + offset, &value, sizeof(value));
}
}
void TestMediaCapture() {
    const auto previous = g_weixin;
    g_weixin = reinterpret_cast<HMODULE>(0x180000000ULL);
    const auto base = reinterpret_cast<uintptr_t>(g_weixin);
    alignas(8) uint8_t item[0x70]{}, wrapper[0x20]{}, native[0x20]{};
    Put(item, 0, base + kAddMsgVtableRva);
    Check(CopyMediaBuffer(item, 0x30, 0x80).read.status == ReadStatus::Missing,
          "media absent outer field is not an empty payload");
    Put(item, 0x6C, uint32_t{0x80}); Put(item, 0x30, wrapper);
    Check(CopyMediaBuffer(item, 0x30, 0x80).read.status == ReadStatus::InvalidWrapper,
          "media wrapper identity must match current version");
    Put(wrapper, 0, base + 0x08AF85D8);
    Check(CopyMediaBuffer(item, 0x30, 0x80).read.status == ReadStatus::InnerMissing,
          "media inner field has-bit required");
    Put(wrapper, 8, native); Put(wrapper, 0x18, uint32_t{3}); Put(wrapper, 0x10, uint32_t{4});
    const uint8_t bytes[]{0, 0xFF, 0xC0, 0x80};
    memcpy(native, bytes, sizeof(bytes)); Put(native, 0x10, uint64_t{4}); Put(native, 0x18, uint64_t{15});
    auto snapshot = CopyMediaBuffer(item, 0x30, 0x80);
    Check(snapshot.read.status == ReadStatus::Ok && snapshot.read.capturedBytes == 4 &&
          snapshot.declaredLengthKnown && snapshot.declaredBytes == 4 && !memcmp(snapshot.bytes.data(), bytes, 4),
          "media payload remains binary including NUL and invalid UTF-8");
    std::string large(9000, '\xFF');
    Put(native, 0, large.data()); Put(native, 0x10, uint64_t{9000}); Put(native, 0x18, uint64_t{9000});
    snapshot = CopyMediaBuffer(item, 0x30, 0x80);
    Check(snapshot.read.status == ReadStatus::Truncated && snapshot.read.originalBytes == 9000 &&
          snapshot.read.capturedBytes == 8192 && snapshot.declaredBytes == 4,
          "media actual length and protobuf declared length remain separate");
    Put(native, 0x18, uint64_t{1});
    Check(CopyMediaBuffer(item, 0x30, 0x80).read.status == ReadStatus::InvalidLayout,
          "media rejects impossible native string capacity");
    Put(item, 0x30, reinterpret_cast<void*>(1));
    Check(CopyMediaBuffer(item, 0x30, 0x80).read.status == ReadStatus::InvalidObject,
          "media invalid pointer is contained");
    Event message{EventKind::Item};
    message.Get<MessageSnapshot>().message.vtableMatch = true;
    message.Get<MessageSnapshot>().message.msgType = 34;
    message.header.sequence = 42;
    ConfigureMediaReceiveTrace(false); CaptureMediaReceive(message, item);
    Event sample{};
    Check(!PopEvent(sample), "media tracing off by default");
    ConfigureMediaReceiveTrace(true);
    message.Get<MessageSnapshot>().message.msgType = 1; CaptureMediaReceive(message, item);
    Check(!PopEvent(sample), "ordinary text does not consume media budget");
    message.Get<MessageSnapshot>().message.msgType = 34;
    for (int i = 0; i < 9; ++i) {
        CaptureMediaReceive(message, item);
        Check(PopEvent(sample) == (i < 8), "voice media budget bounded to eight samples");
    }
    Check(sample.Kind() == EventKind::MediaReceive &&
          sample.Get<MediaReceiveSnapshot>().sourceEventSequence == 42,
          "media sample has its own kind and source-event correlation");
    auto& media = sample.Get<MediaReceiveSnapshot>();
    media.field8 = {};
    media.field8.read = {ReadStatus::Ok, true, 4, 4};
    memcpy(media.field8.bytes.data(), bytes, 4);
    const auto json = SerializeMediaReceiveEvent(sample);
    Check(json.find("\"hex\":\"00ffc080\"") != std::string::npos &&
          json.find("\"source_event_seq\":42") != std::string::npos,
          "media bytes serialize losslessly as bounded hexadecimal");
    ConfigureMediaReceiveTrace(false); g_weixin = previous;
}
