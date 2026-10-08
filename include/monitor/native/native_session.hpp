#pragma once
#include "monitor/core/platform.hpp"
#include <cstdint>
#include <string>

namespace wechatbot::monitor {

// A bounded, read-only view of the current native session. The ready fields
// describe only the account identity service and the executor's normal-branch
// predicate; neither field is evidence of network connectivity or delivery.
struct NativeSessionSnapshot {
    std::string accountId;
    bool accountReady = false;
    bool executorReady = false;
    std::string reason;
};

// Must run on the currently verified UI thread. Reads memory directly and
// never invokes a Weixin function or retains a borrowed native pointer.
NativeSessionSnapshot ReadNativeSession(uintptr_t imageBase, DWORD expectedUiThreadId);

} // namespace wechatbot::monitor
