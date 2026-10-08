#pragma once
#include "monitor/native/native_submit.hpp"
#include "monitor/send/send_gate.hpp"
#include "monitor/transport/ui_dispatch.hpp"
#include <optional>

namespace wechatbot::monitor {

// A normal native Start return is accepted locally; downstream submission and
// delivery remain unconfirmed. Any uncertainty after Start must not be retried.
SendResult MapNativeAttemptResult(const NativeAttemptResult& result);

// nullopt means the action completed and its owned result may now be read.
// For every other status, the action's result must not be read: it may still be
// executing. The returned rejection/uncertainty preserves the wire contract.
std::optional<SendResult> MapUiDispatchFailure(UiDispatchStatus status);

} // namespace wechatbot::monitor
