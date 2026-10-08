#include "monitor/receive/receive.hpp"
#include "monitor/diagnostics/logging.hpp"
#include <stdexcept>

void Check(bool condition, const char* message);
using namespace wechatbot::monitor;

namespace {
unsigned calls = 0;
unsigned char __fastcall FakeReceive(void* context, void* vector,
                                    unsigned char flag3, unsigned char flag4) {
    ++calls;
    Check(context == reinterpret_cast<void*>(42) && vector == nullptr &&
          flag3 == 3 && flag4 == 4, "receive forwards every native argument unchanged");
    Check(GetLastError() == 1234, "receive observation preserves entry LastError");
    SetLastError(5678);
    return 19;
}
unsigned char __fastcall ThrowingReceive(void*, void*, unsigned char, unsigned char) {
    ++calls;
    throw std::runtime_error("native receive failure");
}
}

void TestReceiveHook() {
    const auto previous = g_originalReceiveBatch;
    g_originalReceiveBatch = FakeReceive;
    calls = 0;
    SetLastError(1234);
    Check(HookReceiveBatch(reinterpret_cast<void*>(42), nullptr, 3, 4) == 19 && calls == 1,
          "receive calls original exactly once and returns original result");
    Check(GetLastError() == 5678, "receive preserves original result LastError");
    g_originalReceiveBatch = ThrowingReceive;
    bool propagated = false;
    try { HookReceiveBatch(nullptr, nullptr, 0, 0); }
    catch (const std::runtime_error&) { propagated = true; }
    Check(propagated && calls == 2, "receive original exception propagates without retry");
    g_originalReceiveBatch = previous;
    Event event;
    unsigned observations = 0;
    unsigned returns = 0;
    while (PopEvent(event)) {
        ++observations;
        if (event.Kind() == EventKind::ReturnValue) {
            ++returns;
            Check(event.Get<CallSnapshot>().message.returnValue == 19,
                  "receive return keeps the original native result in call snapshot");
        }
    }
    Check(observations == 3, "receive emits two snapshots and one successful return observation");
    Check(returns == 1, "receive emits one typed return observation");
}
