#include "monitor/diagnostics/send_trace.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/native/target.hpp"
#include <MinHook.h>

namespace wechatbot::monitor {

SendContextConstructorFn g_originalSendContextConstructor = nullptr;

namespace {
volatile LONG g_contextTraceBudget = 0;
uintptr_t g_contextImageSize = 0;

bool ClaimContextTraceBudget() {
    LONG remaining = InterlockedCompareExchange(&g_contextTraceBudget, 0, 0);
    while (remaining > 0) {
        const LONG observed = InterlockedCompareExchange(
            &g_contextTraceBudget, remaining - 1, remaining);
        if (observed == remaining)
            return true;
        remaining = observed;
    }
    return false;
}

bool CaptureContextEnter(Event& event, void* context, const void* sourcePair,
                         unsigned char flag) {
    bool captured = false;
    __try {
        if (InterlockedCompareExchange(&g_contextTraceBudget, 0, 0) <= 0)
            return false;
        if (!g_weixin || !IsReadableRange(sourcePair, 2 * sizeof(void*)))
            return false;
        const auto* pair = static_cast<const uint8_t*>(sourcePair);
        const auto* source = *reinterpret_cast<const uint8_t* const*>(pair);
        const auto* control = *reinterpret_cast<const void* const*>(pair + sizeof(void*));
        // Read only the identity fields until the object is known to be the text source type.
        if (!IsReadableRange(source, 0x120))
            return false;
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(source);
        const uint32_t type = *reinterpret_cast<const uint32_t*>(source + 0x118);
        const uint32_t subtype = *reinterpret_cast<const uint32_t*>(source + 0x11C);
        if (vtable != reinterpret_cast<uintptr_t>(g_weixin) + kSendContextSourceVtableRva ||
            type != 1 || subtype != 0 || !IsReadableRange(source, 0x798))
            return false;
        if (!ClaimContextTraceBudget())
            return false;

        event = MakeCallEvent(EventKind::SendContextEnter);
        auto& snapshot = event.Get<ContextSnapshot>();
        snapshot.context = reinterpret_cast<uint64_t>(context);
        snapshot.sourcePairAddress = reinterpret_cast<uint64_t>(sourcePair);
        snapshot.sourceObjectAddress = reinterpret_cast<uint64_t>(source);
        snapshot.sourceControlAddress = reinterpret_cast<uint64_t>(control);
        snapshot.sourceVtable = vtable;
        snapshot.sourceType = type;
        snapshot.sourceSubtype = subtype;
        snapshot.constructorFlag = flag;
        snapshot.target.read = CopyNativeString(source + 0xB0, snapshot.target.data(), snapshot.target.capacity());
        snapshot.content.read = CopyNativeString(source + 0x758, snapshot.content.data(), snapshot.content.capacity());
        snapshot.localUuid.read = CopyNativeString(source + 0x6F8, snapshot.localUuid.data(), snapshot.localUuid.capacity());
        captured = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        captured = false;
    }
    return captured;
}

void CaptureContextReturn(Event& event, void* context, void* returned) {
    event.SetKind(EventKind::SendContextReturn);
    event.header.sequence = NextSequence();
    auto& snapshot = event.Get<ContextSnapshot>();
    snapshot.returnedContextAddress = reinterpret_cast<uint64_t>(returned);
    snapshot.returnedContextMatches = returned == context;
    __try {
        if (!IsReadableRange(context, 0xC0)) {
            snapshot.postTarget.read.status = context ? ReadStatus::Unreadable
                                                      : ReadStatus::InvalidObject;
            return;
        }
        const auto* bytes = static_cast<const uint8_t*>(context);
        snapshot.postContextReadable = true;
        snapshot.postVtable = *reinterpret_cast<const uintptr_t*>(bytes);
        snapshot.postVtableIsBase = snapshot.postVtable ==
            reinterpret_cast<uintptr_t>(g_weixin) + kSendContextBaseVtableRva;
        snapshot.postSourceObjectAddress = *reinterpret_cast<const uint64_t*>(bytes + 0x08);
        snapshot.postSourceControlAddress = *reinterpret_cast<const uint64_t*>(bytes + 0x10);
        snapshot.postSourceIdentityMatches =
            snapshot.postSourceObjectAddress == snapshot.sourceObjectAddress &&
            snapshot.postSourceControlAddress == snapshot.sourceControlAddress;
        snapshot.postTarget.read = CopyNativeString(bytes + 0x38, snapshot.postTarget.data(),
                                                    snapshot.postTarget.capacity());
        snapshot.postFlag = *(bytes + 0xB8);
        snapshot.postState = *reinterpret_cast<const uint32_t*>(bytes + 0xBC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snapshot.postContextReadable = false;
        snapshot.postTarget.read = {};
        snapshot.postTarget.read.status = ReadStatus::Exception;
    }
}
} // namespace

void ConfigureSendContextTrace(uintptr_t imageSize, bool enabled) {
    g_contextImageSize = imageSize;
    InterlockedExchange(&g_contextTraceBudget, enabled ? 8 : 0);
}

void* __fastcall HookSendContextConstructor(void* context, const void* sourcePair,
                                            unsigned char flag) {
    const DWORD entryError = GetLastError();
    Event event{};
    const bool traced = CaptureContextEnter(event, context, sourcePair, flag);
    if (traced) {
        event.header.stack.attempted = true;
        void* frames[24]{};
        // Capture is performed directly in the hook: skip the hook frame and retain its caller.
        event.header.stack.frameCount = CaptureStackBackTrace(1, 24, frames, nullptr);
        const auto base = reinterpret_cast<uintptr_t>(g_weixin);
        for (uint16_t i = 0; i < event.header.stack.frameCount; ++i) {
            const auto address = reinterpret_cast<uintptr_t>(frames[i]);
            if (address >= base && address - base < g_contextImageSize)
                event.header.stack.rvas[i] = static_cast<uint32_t>(address - base);
        }
        QueueEvent(event);
    }

    SetLastError(entryError);
    void* returned = g_originalSendContextConstructor(context, sourcePair, flag);
    const DWORD resultError = GetLastError();
    if (traced) {
        CaptureContextReturn(event, context, returned);
        QueueEvent(event);
    }
    SetLastError(resultError);
    return returned;
}

void EnableSendContextObservation(bool traceEnabled) {
    if (!traceEnabled) return;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_weixin);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(g_weixin) + dos->e_lfanew);
    ConfigureSendContextTrace(nt->OptionalHeader.SizeOfImage, true);
    AppendDirect("{\"kind\":\"send_context_trace_enabled\",\"max_text_contexts\":8,"
                 "\"max_frames\":24,\"scope\":\"weixin_rva\"}");

    void* target = reinterpret_cast<uint8_t*>(g_weixin) + kSendContextConstructorRva;
    if (!VerifyEntry(target, kSendContextConstructorEntry,
                     sizeof(kSendContextConstructorEntry))) {
        ConfigureSendContextTrace(0, false);
        AppendDirect("{\"kind\":\"send_context_hook_disabled\","
                     "\"reason\":\"entry_signature_mismatch\"}");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookSendContextConstructor),
                      reinterpret_cast<void**>(&g_originalSendContextConstructor)) != MH_OK) {
        ConfigureSendContextTrace(0, false);
        AppendDirect("{\"kind\":\"hook_error\",\"source\":\"send_context_constructor\","
                     "\"stage\":\"create\"}");
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        ConfigureSendContextTrace(0, false);
        AppendDirect("{\"kind\":\"hook_error\",\"source\":\"send_context_constructor\","
                     "\"stage\":\"enable\"}");
        return;
    }
    AppendDirect("{\"kind\":\"hook_enabled\",\"source\":\"send_context_constructor\","
                 "\"rva\":\"0x397c180\"}");
}

} // namespace wechatbot::monitor
