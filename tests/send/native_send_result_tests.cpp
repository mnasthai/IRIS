#include "monitor/send/native_send_result.hpp"

void Check(bool condition, const char* message);
using namespace wechatbot::monitor;

void TestNativeSendResult() {
    NativeAttemptResult native;
    native.status = NativeAttemptStatus::start_returned;
    native.startEntered = native.startReturned = true;
    const auto accepted = MapNativeAttemptResult(native);
    Check(accepted.status == "accepted" && accepted.code.empty() &&
          accepted.detail.find("delivery are unconfirmed") != std::string::npos,
          "normal native Start return is local acceptance, not delivery acknowledgement");

    native.status = NativeAttemptStatus::unknown_after_start;
    native.startReturned = false;
    native.cleanupComplete = false;
    const auto uncertain = MapNativeAttemptResult(native);
    Check(uncertain.status == "unknown" && uncertain.code == "native_start_uncertain",
          "entered Start remains uncertain even when cleanup fails");

    native.status = NativeAttemptStatus::preparation_failed;
    Check(MapNativeAttemptResult(native).status == "unknown",
          "an entered Start cannot be downgraded to a retryable preparation rejection");
    native.startEntered = false;
    const auto rejected = MapNativeAttemptResult(native);
    Check(rejected.status == "rejected" && rejected.code == "native_preparation_failed",
          "failed preparation before Start is rejected");

    Check(!MapUiDispatchFailure(UiDispatchStatus::completed),
          "completed UI action permits reading its owned output");
    for (const auto status : {UiDispatchStatus::cancelled_before_start,
                             UiDispatchStatus::busy, UiDispatchStatus::unavailable}) {
        const auto failure = MapUiDispatchFailure(status);
        Check(failure && failure->status == "rejected" && failure->code == "ui_dispatch_not_started",
              "UI work that did not start is rejected");
    }
    for (const auto status : {UiDispatchStatus::timeout_after_start, UiDispatchStatus::exception}) {
        const auto failure = MapUiDispatchFailure(status);
        Check(failure && failure->status == "unknown" && failure->code == "ui_execution_uncertain",
              "UI timeout or exception after beginning an action must not allow automatic retry");
    }
}
