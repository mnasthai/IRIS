#include "monitor/media/media_capture.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/native/target.hpp"

namespace wechatbot::monitor {
namespace {
// Static evidence: analysis/4.1.13.12/media/receive-buffer-contract.md.
constexpr uintptr_t bufferVtableRva = 0x08AF85D8;
volatile LONG imageBudget = 0, voiceBudget = 0;
bool Claim(volatile LONG& budget) {
    LONG remaining = InterlockedCompareExchange(&budget, 0, 0);
    while (remaining > 0) {
        const LONG previous = InterlockedCompareExchange(&budget, remaining - 1, remaining);
        if (previous == remaining) return true;
        remaining = previous;
    }
    return false;
}
} // namespace

BinarySnapshot CopyMediaBuffer(const uint8_t* item, size_t offset, uint32_t bit) {
    BinarySnapshot snapshot{};
    snapshot.read.status = ReadStatus::InvalidObject;
    if (!g_weixin || !IsReadableRange(item, kReceiveLayout.size) ||
        offset > kReceiveLayout.size - sizeof(void*)) return snapshot;
    __try {
        if (*reinterpret_cast<const uintptr_t*>(item) !=
            reinterpret_cast<uintptr_t>(g_weixin) + kAddMsgVtableRva) return snapshot;
        if (!(*reinterpret_cast<const uint32_t*>(item + kReceiveLayout.hasBits) & bit)) {
            snapshot.read.status = ReadStatus::Missing;
            return snapshot;
        }
        const auto* wrapper = *reinterpret_cast<const uint8_t* const*>(item + offset);
        if (!IsReadableRange(wrapper, 0x20)) return snapshot;
        if (*reinterpret_cast<const uintptr_t*>(wrapper) !=
            reinterpret_cast<uintptr_t>(g_weixin) + bufferVtableRva) {
            snapshot.read.status = ReadStatus::InvalidWrapper;
            return snapshot;
        }
        const uint32_t bits = *reinterpret_cast<const uint32_t*>(wrapper + 0x18);
        snapshot.declaredLengthKnown = (bits & 1) != 0;
        if (snapshot.declaredLengthKnown)
            snapshot.declaredBytes = *reinterpret_cast<const uint32_t*>(wrapper + 0x10);
        if (!(bits & 2)) {
            snapshot.read.status = ReadStatus::InnerMissing;
            return snapshot;
        }
        const auto* native = *reinterpret_cast<const uint8_t* const*>(wrapper + 8);
        snapshot.read = CopyNativeBinary(native, snapshot.bytes.data(), snapshot.bytes.size());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snapshot.read.status = ReadStatus::Exception;
        snapshot.read.capturedBytes = 0;
    }
    return snapshot;
}

void ConfigureMediaReceiveTrace(bool enabled) {
    InterlockedExchange(&imageBudget, enabled ? 8 : 0);
    InterlockedExchange(&voiceBudget, enabled ? 8 : 0);
}


void CaptureMediaReceive(const Event& message, const uint8_t* item) {
    const auto* messageSnapshot = message.TryGet<MessageSnapshot>();
    // 只有 msgType == 3 (图片) 或 34 (语音) 才处理
    if (!messageSnapshot || !messageSnapshot->message.vtableMatch ||
        (messageSnapshot->message.msgType != 3 && messageSnapshot->message.msgType != 34)) return;
    if (!Claim(messageSnapshot->message.msgType == 3 ? imageBudget : voiceBudget)) return;
    Event sample = MakeCallEvent(EventKind::MediaReceive);
    sample.header.callId = message.header.callId;
    auto& media = sample.Get<MediaReceiveSnapshot>();
    media.msgType = messageSnapshot->message.msgType;
    media.sourceEventSequence = message.header.sequence;
    media.field8 = CopyMediaBuffer(item, 0x30, 0x80); // // 抓取缩略图/小缓冲
    media.field14 = CopyMediaBuffer(item, 0x58, 0x2000);
    QueueEvent(sample);
}

} // namespace wechatbot::monitor
