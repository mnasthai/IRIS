#pragma once
#include <string>

namespace wechatbot::monitor {

// Shared by text and media. These values describe one local native attempt,
// not server acknowledgement or recipient delivery.
enum class NativeAttemptStatus {
    invalid_argument = 0,
    ui_thread_required = 1,
    api_unavailable = 2,
    preparation_failed = 3,
    start_returned = 4,
    unknown_after_start = 5
};

// Numeric values are emitted as native_stage in JSONL. Keep them stable even
// when preparation stages are reorganized or new source types are added.
enum class NativeSubmitStage {
    validate = 0,
    source_factory = 1,
    local_uuid = 2,
    target_assign = 3,
    text_assign = 4,
    vector_allocate = 5,
    delayed_factory = 6,
    metadata = 7,
    before_start = 8,
    start = 9,
    quote_prepare = 10,
    media_factory = 11,
    media_prepare = 12
};

struct NativeAttemptResult {
    NativeAttemptStatus status = NativeAttemptStatus::invalid_argument;
    NativeSubmitStage stage = NativeSubmitStage::validate;
    // Local correlation only; not a submission or delivery identifier.
    std::string localUuid;
    bool startEntered = false;
    bool startReturned = false;
    bool cleanupComplete = true;
};

} // namespace wechatbot::monitor
