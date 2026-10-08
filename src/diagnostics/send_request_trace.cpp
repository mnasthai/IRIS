#include "monitor/diagnostics/send_trace.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/native/target.hpp"
#include "monitor/diagnostics/logging.hpp"
#include <MinHook.h>
#include <cstring>

namespace wechatbot::monitor {

SendRequestFn g_originalSendRequest = nullptr;

namespace {
volatile LONG g_traceBudget = 0;
uintptr_t g_imageSize = 0;

void CaptureSendStack(Event& event) {
    if (InterlockedCompareExchange(&g_traceBudget, 0, 0) <= 0) return;
    if (InterlockedDecrement(&g_traceBudget) < 0) return;
    auto& stack = event.header.stack;
    stack.attempted = true;
    void* frames[24]{};
    stack.frameCount = CaptureStackBackTrace(2, 24, frames, nullptr);
    const auto base = reinterpret_cast<uintptr_t>(g_weixin);
    for (uint16_t i = 0; i < stack.frameCount; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        if (address >= base && address - base < g_imageSize)
            stack.rvas[i] = static_cast<uint32_t>(address - base);
    }
}
} // namespace

void ConfigureSendTrace(uintptr_t imageSize, bool enabled) {
    g_imageSize = imageSize;
    InterlockedExchange(&g_traceBudget, enabled ? 8 : 0);
}

Event ObserveSendRequest(const void* request) {
    Event batch = MakeCallEvent(EventKind::OutboundRequest);
    CaptureSendStack(batch);
    auto& message = batch.Get<CallSnapshot>().message;
    message.context = reinterpret_cast<uint64_t>(request);
    __try {
        if (!IsReadableRange(request, 0x30)) {
            QueueEvent(batch);
            return batch;
        }
        const auto* outer = static_cast<const uint8_t*>(request);
        message.vtableMatch = *reinterpret_cast<const uintptr_t*>(outer) ==
                            reinterpret_cast<uintptr_t>(g_weixin) + kSendOuterVtableRva;
        if (!message.vtableMatch) {
            QueueEvent(batch);
            return batch;
        }
        const int32_t count = *reinterpret_cast<const int32_t*>(outer + 0x10);
        const int32_t allocated = *reinterpret_cast<const int32_t*>(outer + 0x14);
        const int32_t capacity = *reinterpret_cast<const int32_t*>(outer + 0x18);
        const auto* entries = *reinterpret_cast<const uint8_t* const* const*>(outer + 8);
        if (count < 0 || allocated < count || capacity < allocated || count > kMaxBatchItems ||
            (count && !IsReadableRange(entries, static_cast<size_t>(count) * sizeof(void*)))) {
            QueueEvent(batch);
            return batch;
        }
        message.count = static_cast<uint32_t>(count);
        message.begin = reinterpret_cast<uint64_t>(entries);
        message.end = message.begin + static_cast<uint64_t>(count) * sizeof(void*);
        message.validVector = 1;
        QueueEvent(batch);
        for (uint32_t i = 0; i < message.count && i < kMaxLoggedItemsPerBatch; ++i) {
            Event event = batch;
            event.SetKind(EventKind::OutboundItem);
            event.header.sequence = NextSequence();
            auto& snapshot = event.Get<MessageSnapshot>();
            auto& itemMessage = snapshot.message;
            itemMessage.index = i;
            const auto* item = entries[i];
            itemMessage.item = reinterpret_cast<uint64_t>(item);
            itemMessage.vtableMatch = IsReadableRange(item, kSendLayout.size) &&
                *reinterpret_cast<const uintptr_t*>(item) ==
                    reinterpret_cast<uintptr_t>(g_weixin) + kSendItemVtableRva;
            if (itemMessage.vtableMatch) {
                itemMessage.hasBits = *reinterpret_cast<const uint32_t*>(item + kSendLayout.hasBits);
                if (itemMessage.hasBits & 4) itemMessage.msgType = *reinterpret_cast<const uint32_t*>(item + 0x18);
                snapshot.to.read = CopyField(item, kSendLayout, kSendTo, snapshot.to.data(), snapshot.to.capacity());
                snapshot.content.read = CopyField(item, kSendLayout, kSendContent, snapshot.content.data(), snapshot.content.capacity());
                snapshot.msgSource.read = CopyField(item, kSendLayout, kSendSource, snapshot.msgSource.data(), snapshot.msgSource.capacity());
                CopyScalars(item, itemMessage.hasBits, kSendScalars, std::size(kSendScalars), snapshot.rawFields.data());
            }
            QueueEvent(event);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Do not enqueue a second batch summary if memory changed mid-snapshot.
    }
    return batch;
}

void CaptureRequestResult(Event& event, const void* result) {
    auto& message = event.Get<CallSnapshot>().message;
    __try {
        if (IsReadableRange(result, sizeof(message.resultStatus))) {
            memcpy(message.resultStatus.data(), result, sizeof(message.resultStatus));
            message.resultReadable = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        message.resultReadable = false;
    }
}

void* __fastcall HookSendRequest(void* resultOut, void* unusedInput, const void* request,
                                 const void* extra, const void* options) {
    const DWORD entryError = GetLastError();
    Event event = ObserveSendRequest(request);
    SetLastError(entryError);
    void* result = g_originalSendRequest(resultOut, unusedInput, request, extra, options);
    const DWORD resultError = GetLastError();
    event.SetKind(EventKind::OutboundReturn);
    event.header.sequence = NextSequence();
    event.Get<CallSnapshot>().message.resultPointerMatches = result == resultOut;
    CaptureRequestResult(event, resultOut);
    QueueEvent(event);
    SetLastError(resultError);
    return result;
}

void EnableSendObservation(bool traceEnabled) {
    // Diagnostic mode uses the already verified observation point only.
    // No extra hook or native send invocation is installed.
    if (traceEnabled) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_weixin);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            reinterpret_cast<const uint8_t*>(g_weixin) + dos->e_lfanew);
        ConfigureSendTrace(nt->OptionalHeader.SizeOfImage, true);
        AppendDirect("{\"kind\":\"send_trace_enabled\",\"max_requests\":8,\"max_frames\":24,\"scope\":\"weixin_rva\"}");
    }
    void* target = reinterpret_cast<uint8_t*>(g_weixin) + kSendRequestRva;
    if (!VerifyEntry(target, kSendEntry, sizeof(kSendEntry))) {
        AppendDirect("{\"kind\":\"send_hook_disabled\",\"reason\":\"entry_signature_mismatch\"}");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookSendRequest),
                      reinterpret_cast<void**>(&g_originalSendRequest)) != MH_OK) {
        AppendDirect("{\"kind\":\"hook_error\",\"source\":\"send_request\",\"stage\":\"create\"}");
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        AppendDirect("{\"kind\":\"hook_error\",\"source\":\"send_request\",\"stage\":\"enable\"}");
        return;
    }
    AppendDirect("{\"kind\":\"hook_enabled\",\"source\":\"send_request\",\"rva\":\"0x39e7da0\"}");
}

} // namespace wechatbot::monitor
