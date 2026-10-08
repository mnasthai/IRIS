#include "monitor/send/native_send_result.hpp"

namespace wechatbot::monitor {

SendResult MapNativeAttemptResult(const NativeAttemptResult& result) {
    if (result.status == NativeAttemptStatus::start_returned)
        return {"accepted", "", "Native Start returned; downstream submission and delivery are unconfirmed"};
    if (result.startEntered)
        return {"unknown", "native_start_uncertain", "Native Start was entered; do not retry"};
    return {"rejected", "native_preparation_failed", "Native preparation stopped before Start"};
}

std::optional<SendResult> MapUiDispatchFailure(UiDispatchStatus status) {
    switch (status) {
    case UiDispatchStatus::completed:
        return std::nullopt;
    case UiDispatchStatus::timeout_after_start:
    case UiDispatchStatus::exception:
        return SendResult{"unknown", "ui_execution_uncertain", "UI action began; native completion is unknown; do not retry"};
    case UiDispatchStatus::cancelled_before_start:
    case UiDispatchStatus::busy:
    case UiDispatchStatus::unavailable:
        return SendResult{"rejected", "ui_dispatch_not_started", "UI task cancelled or unavailable before action began"};
    }
    // Defensive handling must not turn an unrecognized scheduler result into
    // permission to read an action's possibly unfinished output or to retry.
    return SendResult{"unknown", "ui_execution_uncertain", "UI action began; native completion is unknown; do not retry"};
}

} // namespace wechatbot::monitor
