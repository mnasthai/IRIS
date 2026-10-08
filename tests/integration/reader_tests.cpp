// Tests run in an ordinary local process; no Weixin process or hook is started.
#include "monitor/receive/receive.hpp"
#include "monitor/diagnostics/send_trace.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/native/target.hpp"
#include "monitor/transport/command_pipe.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
using namespace wechatbot::monitor;

void Check(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        exit(1);
    }
}

struct NativeFixture {
    alignas(8) unsigned char bytes[0x20]{};
    std::string storage;
    explicit NativeFixture(std::string text) : storage(std::move(text)) {
        const uint64_t length = storage.size();
        const uint64_t capacity = length < 16 ? 15 : length;
        if (length < 16) memcpy(bytes, storage.data(), static_cast<size_t>(length));
        else { const char* data = storage.data(); memcpy(bytes, &data, sizeof(data)); }
        memcpy(bytes + 0x10, &length, 8);
        memcpy(bytes + 0x18, &capacity, 8);
    }
};

unsigned sendCalls = 0;
const void* expectedRequest = nullptr;
void* __fastcall FakeSend(void* out, void* unused, const void* request,
                         const void* extra, const void* options) {
    ++sendCalls;
    Check(unused == reinterpret_cast<void*>(2) && request == expectedRequest &&
          extra == reinterpret_cast<void*>(4) && options == reinterpret_cast<void*>(5),
          "all five send arguments forwarded unchanged");
    Check(GetLastError() == 1234, "caller last error preserved before original send");
    static_cast<uint32_t*>(out)[0] = 17;
    static_cast<uint32_t*>(out)[1] = 23;
    SetLastError(5678);
    return out;
}

unsigned contextCalls = 0;
const void* expectedSourcePair = nullptr;
unsigned char expectedContextFlag = 0;
void* __fastcall FakeContextConstructor(void* context, const void* sourcePair, unsigned char flag) {
    ++contextCalls;
    Check(sourcePair == expectedSourcePair && flag == expectedContextFlag,
          "all context constructor arguments forwarded unchanged");
    Check(GetLastError() == 2468, "caller last error preserved before original context constructor");
    auto* bytes = static_cast<uint8_t*>(context);
    *reinterpret_cast<uintptr_t*>(bytes) = reinterpret_cast<uintptr_t>(g_weixin) +
                                           kSendContextBaseVtableRva;
    if (IsReadableRange(sourcePair, 2 * sizeof(void*))) {
        memcpy(bytes + 0x08, sourcePair, 2 * sizeof(void*));
        const auto* source = *static_cast<const uint8_t* const*>(sourcePair);
        if (IsReadableRange(source, 0xD0))
            memcpy(bytes + 0x38, source + 0xB0, 0x20);
    }
    bytes[0xB8] = flag;
    *reinterpret_cast<uint32_t*>(bytes + 0xBC) = 77;
    SetLastError(8642);
    return context;
}

void TestCommandProtocol();
void TestUiDispatch();
void TestNativeSession();
void TestNativeBindings();
void TestNativeSendResult();
void TestNativeText();
void TestNativeMedia();
void TestSendGate();
void TestSendService();
void TestObserverConfig();
void TestReceiveHook();
void TestWorkerLifecycle();
void TestMediaCapture();
void TestMediaMemory();
void TestMediaFile();
void TestNativeMediaReceive();
void TestTypedEvents();
std::string TestSendSubmit();

int wmain(int argc, wchar_t** argv) {
    // Diagnostics print wide paths; keep the console able to render them even
    // when the active code page is a legacy one.
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 3 && wcscmp(argv[1], L"--serve-command-pipe") == 0) {
        volatile LONG stop = 0;
        return RunCommandPipe(argv[2], "test-session", stop, 15000) ? 0 : 1;
    }
    TestObserverConfig();
    TestReceiveHook();
    TestCommandProtocol();
    TestUiDispatch();
    TestNativeSession();
    TestNativeBindings();
    TestNativeSendResult();
    TestNativeText();
    TestNativeMedia();
    TestSendGate();
    TestSendService();
    TestMediaCapture();
    TestMediaMemory();
    TestMediaFile();
    TestNativeMediaReceive();
    TestTypedEvents();
    char output[32]{};
    NativeFixture empty("");
    auto result = CopyNativeString(empty.bytes, output, sizeof(output));
    Check(result.status == ReadStatus::Empty && result.lengthKnown && !result.originalBytes,
          "empty is distinct from missing/failure");
    NativeFixture text("中文😀");
    result = CopyNativeString(text.bytes, output, sizeof(output));
    Check(result.status == ReadStatus::Ok && result.originalBytes == 10 &&
          result.capturedBytes == 10 && std::string(output) == text.storage, "UTF-8 inline string");
    NativeFixture heap(std::string(1200, 'x'));
    char longOutput[1500]{};
    result = CopyNativeString(heap.bytes, longOutput, sizeof(longOutput));
    Check(result.status == ReadStatus::Ok && result.capturedBytes == 1200,
          "content beyond old 768-byte limit is captured");
    NativeFixture longText(std::string(6000, 'x'));
    char bounded[kTextCapacity]{};
    result = CopyNativeString(longText.bytes, bounded, sizeof(bounded));
    Check(result.status == ReadStatus::Truncated && result.originalBytes == 6000 &&
          result.capturedBytes == 4095, "long content is a bounded snapshot with original length");
    result = CopyNativeString(text.bytes, output, 9);
    Check(result.status == ReadStatus::Truncated && result.capturedBytes == 6 &&
          std::string(output) == "中文", "UTF-8 truncation preserves code point boundary");
    NativeFixture nul(std::string("a\0b", 3));
    result = CopyNativeString(nul.bytes, output, sizeof(output));
    std::string json;
    AppendJsonBytes(json, output, result.capturedBytes);
    Check(result.status == ReadStatus::Ok && json == "\"a\\u0000b\"", "embedded NUL survives JSON");
    NativeFixture invalid(std::string("\xC0\xAF", 2));
    result = CopyNativeString(invalid.bytes, output, sizeof(output));
    Check(result.status == ReadStatus::InvalidUtf8 && !result.capturedBytes, "invalid UTF-8 labeled");
    result = CopyNativeString(nullptr, output, sizeof(output));
    Check(result.status == ReadStatus::InvalidObject && !result.lengthKnown, "invalid pointer");
    uint64_t badLength = 17;
    memcpy(empty.bytes + 0x10, &badLength, 8);
    result = CopyNativeString(empty.bytes, output, sizeof(output));
    Check(result.status == ReadStatus::InvalidLayout && result.originalBytes == 17, "invalid layout");
    void* unreadable = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    Check(unreadable != nullptr, "allocate unreadable fixture");
    memcpy(heap.bytes, &unreadable, 8);
    result = CopyNativeString(heap.bytes, output, sizeof(output));
    Check(result.status == ReadStatus::Unreadable && result.originalBytes == 1200, "unreadable payload");
    VirtualFree(unreadable, 0, MEM_RELEASE);

    g_weixin = reinterpret_cast<HMODULE>(0x180000000ULL);
    alignas(8) uint8_t item[0x78]{};
    alignas(8) uint8_t wrapper[0x18]{};
    result = CopyMessageString(item, 0x70, 0x6C, 0x20, 0x10, true, output, sizeof(output));
    Check(result.status == ReadStatus::Missing, "has-bit absent");
    *reinterpret_cast<uint32_t*>(item + 0x6C) = 0x10 | 0x200;
    *reinterpret_cast<void**>(item + 0x20) = wrapper;
    result = CopyMessageString(item, 0x70, 0x6C, 0x20, 0x10, true, output, sizeof(output));
    Check(result.status == ReadStatus::InvalidWrapper, "wrapper identity mismatch");
    *reinterpret_cast<uintptr_t*>(wrapper) = reinterpret_cast<uintptr_t>(g_weixin) + kBuiltinStringVtableRva;
    result = CopyMessageString(item, 0x70, 0x6C, 0x20, 0x10, true, output, sizeof(output));
    Check(result.status == ReadStatus::InnerMissing, "inner has-bit absent");
    *reinterpret_cast<uint32_t*>(wrapper + 0x14) = 1;
    *reinterpret_cast<void**>(wrapper + 8) = text.bytes;
    result = CopyMessageString(item, 0x70, 0x6C, 0x20, 0x10, true, output, sizeof(output));
    Check(result.status == ReadStatus::Ok && result.originalBytes == 10, "nested string layout");
    *reinterpret_cast<void**>(item + 0x38) = text.bytes;
    result = CopyMessageString(item, 0x70, 0x6C, 0x38, 0x200, false, output, sizeof(output));
    Check(result.status == ReadStatus::Ok, "direct metadata string layout");

    *reinterpret_cast<uintptr_t*>(item) = reinterpret_cast<uintptr_t>(g_weixin) + kAddMsgVtableRva;
    *reinterpret_cast<uint32_t*>(item + 0x6C) |= 0x800;
    *reinterpret_cast<uint64_t*>(item + 0x50) = 18446744073709551614ULL;
    Event batch{};
    batch.header.callId = 42;
    ObserveItem(batch, item, 0);
    Event captured{};
    Check(PopEvent(captured) && captured.header.callId == 42 &&
           captured.Kind() == EventKind::Item && captured.Get<MessageSnapshot>().message.vtableMatch &&
           captured.Get<MessageSnapshot>().rawFields[4] == 18446744073709551614ULL,
           "raw uint64 preserved and call correlated");
    BeginSession();
    std::string logged = SerializeEvent(captured);
    Check(logged.find("\"12\":\"18446744073709551614\"") != std::string::npos, "uint64 serialized as string");
    Check(logged.find("\"content_read\":{\"status\":\"ok\"") != std::string::npos, "read diagnostics serialized");
    Check(logged.find(CurrentSessionId()) != std::string::npos, "session identity serialized");
    alignas(8) uint8_t sendItem[0x38]{};
    alignas(8) uint8_t outer[0x30]{};
    *reinterpret_cast<uintptr_t*>(sendItem) = reinterpret_cast<uintptr_t>(g_weixin) + kSendItemVtableRva;
    *reinterpret_cast<uint32_t*>(sendItem + 0x34) = 0x7F;
    *reinterpret_cast<void**>(sendItem + 8) = wrapper;
    *reinterpret_cast<void**>(sendItem + 0x10) = text.bytes;
    *reinterpret_cast<void**>(sendItem + 0x20) = text.bytes;
    *reinterpret_cast<uint32_t*>(sendItem + 0x18) = 1;
    *reinterpret_cast<uint32_t*>(sendItem + 0x1C) = 42;
    *reinterpret_cast<uint32_t*>(sendItem + 0x28) = 99;
    *reinterpret_cast<uint32_t*>(sendItem + 0x2C) = 100;
    const uint8_t* entries[] = {sendItem};
    *reinterpret_cast<uintptr_t*>(outer) = reinterpret_cast<uintptr_t>(g_weixin) + kSendOuterVtableRva;
    *reinterpret_cast<const uint8_t***>(outer + 8) = entries;
    *reinterpret_cast<int32_t*>(outer + 0x10) = 1;
    *reinterpret_cast<int32_t*>(outer + 0x14) = 1;
    *reinterpret_cast<int32_t*>(outer + 0x18) = 1;
    expectedRequest = outer;
    g_originalSendRequest = FakeSend;
    ConfigureSendTrace(0x1000, true);
    uint32_t statuses[2]{};
    SetLastError(1234);
    void* sent = HookSendRequest(statuses, reinterpret_cast<void*>(2), outer,
                                 reinterpret_cast<void*>(4), reinterpret_cast<void*>(5));
    Check(GetLastError() == 5678, "original send last error preserved on return");
    Check(sendCalls == 1 && sent == statuses, "original send called exactly once, pointer unchanged");
    Event sendBatch{}, sendEvent{}, sendReturn{};
    Check(PopEvent(sendBatch) && PopEvent(sendEvent) && PopEvent(sendReturn), "send observation events queued");
    Check(sendBatch.Kind() == EventKind::OutboundRequest && sendBatch.Get<CallSnapshot>().message.validVector &&
           sendEvent.Kind() == EventKind::OutboundItem && sendEvent.Get<MessageSnapshot>().message.vtableMatch &&
           sendEvent.Get<MessageSnapshot>().content.read.status == ReadStatus::Ok &&
           sendEvent.Get<MessageSnapshot>().to.read.status == ReadStatus::Ok &&
           sendEvent.Get<MessageSnapshot>().message.msgType == 1 &&
           sendEvent.Get<MessageSnapshot>().msgSource.read.status == ReadStatus::Ok,
          "outbound request layout read without modifying it");
    Check(sendReturn.Kind() == EventKind::OutboundReturn &&
           sendReturn.Get<CallSnapshot>().message.resultPointerMatches &&
           sendReturn.Get<CallSnapshot>().message.resultReadable &&
           sendReturn.Get<CallSnapshot>().message.resultStatus[0] == 17 &&
           sendReturn.Get<CallSnapshot>().message.resultStatus[1] == 23 &&
           sendBatch.header.callId == sendEvent.header.callId &&
           sendEvent.header.callId == sendReturn.header.callId, "request return correlated");
    Check(sendBatch.header.stack.attempted && sendBatch.header.stack.frameCount <= 24,
          "optional bounded stack trace captured");
    for (int i = 0; i < 8; ++i) {
        ObserveSendRequest(nullptr);
        Event trace{};
        Check(PopEvent(trace), "trace probe queued");
        Check(trace.header.stack.attempted == (i < 7), "trace stops after eight requests");
    }
    ConfigureSendTrace(0, false);
    sendBatch.header.stack.attempted = true;
    sendBatch.header.stack.frameCount = 2;
    sendBatch.header.stack.rvas[0] = 0x1234;
    sendBatch.header.stack.rvas[1] = 0;
    logged += SerializeEvent(sendBatch) + SerializeEvent(sendEvent) + SerializeEvent(sendReturn);
    Check(logged.find("\"rvas\":[\"0x1234\",null]") != std::string::npos,
          "trace uses module RVAs and null for other modules");
    Check(logged.find("\"kind\":\"outbound_item\"") != std::string::npos &&
          logged.find("\"source\":\"send_request\"") != std::string::npos &&
          logged.find("\"raw_fields\":{\"3\":\"1\",\"4\":\"42\",\"5\":\"99\",\"7\":\"100\"}") != std::string::npos,
          "outbound source and scalar mapping serialized");
    Check(logged.find("\"result_pointer_matches\":true,\"raw_result_status\":[17,23]") != std::string::npos,
          "outbound raw result serialized without delivery claim");

    alignas(8) uint8_t contextSource[0x798]{};
    alignas(8) uint8_t context[0xC0]{};
    NativeFixture contextTarget("wxid_context_target_123");
    NativeFixture contextContent("中文\nnext");
    NativeFixture contextUuid("uuid-local");
    memcpy(contextSource + 0xB0, contextTarget.bytes, sizeof(contextTarget.bytes));
    memcpy(contextSource + 0x758, contextContent.bytes, sizeof(contextContent.bytes));
    memcpy(contextSource + 0x6F8, contextUuid.bytes, sizeof(contextUuid.bytes));
    *reinterpret_cast<uintptr_t*>(contextSource) = reinterpret_cast<uintptr_t>(g_weixin) +
                                                   kSendContextSourceVtableRva;
    *reinterpret_cast<uint32_t*>(contextSource + 0x118) = 1;
    *reinterpret_cast<uint32_t*>(contextSource + 0x11C) = 0;
    const void* sourcePair[] = {contextSource, reinterpret_cast<void*>(0x12345678)};
    expectedSourcePair = sourcePair;
    expectedContextFlag = 5;
    g_originalSendContextConstructor = FakeContextConstructor;
    ConfigureSendContextTrace(0x1000, true);

    alignas(8) uint8_t nonText[0x798]{};
    const void* nonTextPair[] = {nonText, reinterpret_cast<void*>(0x42)};
    expectedSourcePair = nonTextPair;
    SetLastError(2468);
    Check(HookSendContextConstructor(context, nonTextPair, expectedContextFlag) == context &&
          GetLastError() == 8642, "non-text context call-through and last error preserved");
    Event absent{};
    Check(!PopEvent(absent), "non-text context does not consume or emit trace budget");

    void* unreadablePair = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    Check(unreadablePair != nullptr, "allocate unreadable source pair fixture");
    expectedSourcePair = unreadablePair;
    SetLastError(2468);
    Check(HookSendContextConstructor(context, unreadablePair, expectedContextFlag) == context &&
          GetLastError() == 8642 && !PopEvent(absent),
          "invalid context source pointer is safe and still calls original once");
    VirtualFree(unreadablePair, 0, MEM_RELEASE);

    expectedSourcePair = sourcePair;
    Event firstContextEnter{}, firstContextReturn{};
    const unsigned callsBeforeText = contextCalls;
    for (int i = 0; i < 9; ++i) {
        memset(context, 0, sizeof(context));
        SetLastError(2468);
        Check(HookSendContextConstructor(context, sourcePair, expectedContextFlag) == context &&
              GetLastError() == 8642, "text context call-through and last error preserved");
        if (i < 8) {
            Event enter{}, returned{};
            Check(PopEvent(enter) && PopEvent(returned), "matched context emits enter and return pair");
            Check(enter.Kind() == EventKind::SendContextEnter &&
                  returned.Kind() == EventKind::SendContextReturn &&
                  enter.header.callId == returned.header.callId,
                  "context diagnostic pair has distinct correlated kinds");
            if (i == 0) {
                firstContextEnter = enter;
                firstContextReturn = returned;
            }
        } else {
            Check(!PopEvent(absent), "context trace stops after exactly eight matched texts");
        }
    }
    Check(contextCalls == callsBeforeText + 9, "context original called exactly once per invocation");
    const auto& contextEnter = firstContextEnter.Get<ContextSnapshot>();
    const auto& contextReturn = firstContextReturn.Get<ContextSnapshot>();
    Check(contextEnter.target.read.status == ReadStatus::Ok &&
          std::string(contextEnter.target.data(), contextEnter.target.read.capturedBytes) == contextTarget.storage &&
          contextEnter.content.read.status == ReadStatus::Ok &&
          std::string(contextEnter.content.data(), contextEnter.content.read.capturedBytes) ==
              contextContent.storage && contextEnter.localUuid.read.status == ReadStatus::Ok,
          "context captures heap target plus SSO UTF-8 newline content and UUID");
    Check(firstContextEnter.header.stack.attempted && firstContextEnter.header.stack.frameCount <= 24,
          "context entry captures bounded caller stack");
    Check(contextReturn.returnedContextMatches && contextReturn.postContextReadable &&
          contextReturn.postVtableIsBase && contextReturn.postSourceIdentityMatches &&
          contextReturn.postTarget.read.status == ReadStatus::Ok &&
          contextReturn.postFlag == expectedContextFlag && contextReturn.postState == 77,
          "context return captures correlated post-construction state");
    const std::string contextJson = SerializeEvent(firstContextEnter) +
                                    SerializeEvent(firstContextReturn);
    Check(contextJson.find("\"kind\":\"send_context_enter\"") != std::string::npos &&
          contextJson.find("\"kind\":\"send_context_return\"") != std::string::npos &&
          contextJson.find("\"source\":\"send_context_constructor\"") != std::string::npos &&
          contextJson.find("\"post_source_identity_matches\":true") != std::string::npos &&
          contextJson.find("\"kind\":\"outbound_item\"") == std::string::npos &&
          contextJson.find("\"msg_type\"") == std::string::npos,
          "context diagnostics serialize with dedicated schema and never as messages");
    ConfigureSendContextTrace(0, false);

    const auto submitJson = TestSendSubmit();
    if (argc == 3 && wcscmp(argv[1], L"--json-output") == 0) {
        FILE* file = nullptr;
        Check(_wfopen_s(&file, argv[2], L"wb") == 0 && file, "open emitted JSON fixture");
        Check(fwrite(logged.data(), 1, logged.size(), file) == logged.size(), "write emitted JSON fixture");
        fclose(file);
        const std::wstring submitPath = std::wstring(argv[2]) + L".submit.jsonl";
        Check(_wfopen_s(&file, submitPath.c_str(), L"wb") == 0 && file,
              "open emitted submit JSON fixture");
        Check(fwrite(submitJson.data(), 1, submitJson.size(), file) == submitJson.size(),
              "write emitted submit JSON fixture");
        fclose(file);
        const std::wstring contextPath = std::wstring(argv[2]) + L".context.jsonl";
        Check(_wfopen_s(&file, contextPath.c_str(), L"wb") == 0 && file,
              "open emitted context JSON fixture");
        Check(fwrite(contextJson.data(), 1, contextJson.size(), file) == contextJson.size(),
              "write emitted context JSON fixture");
        fclose(file);
    }
    *reinterpret_cast<int32_t*>(outer + 0x10) = 2;
    auto rejected = ObserveSendRequest(outer);
    Check(!rejected.Get<CallSnapshot>().message.validVector, "invalid repeated-field counts rejected");
    TestWorkerLifecycle(); // Last: verifies closed queue admission after shutdown.
    puts("Observer reader tests passed (strings, UTF-8, pointers, raw IDs, send and context call-through).");
    return 0;
}
