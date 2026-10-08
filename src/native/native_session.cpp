#include "monitor/native/native_session.hpp"
#include "monitor/config/version_profile.hpp"
#include "monitor/core/memory.hpp"
#include <cstring>

namespace wechatbot::monitor {
namespace {
namespace profile = active_profile::session;
constexpr size_t kAccountIdCapacity = 512;

struct Pair {
    uintptr_t raw = 0;
    uintptr_t control = 0;
};

struct LastErrorGuard {
    DWORD value = GetLastError();
    ~LastErrorGuard() { SetLastError(value); }
};

bool Add(uintptr_t base, size_t offset, uintptr_t& result) {
    if (!base || offset > UINTPTR_MAX - base) return false;
    result = base + offset;
    return true;
}

template<class T> bool Read(uintptr_t base, size_t offset, T& result) {
    uintptr_t address = 0;
    if (!Add(base, offset, address) || !IsReadableRange(reinterpret_cast<const void*>(address), sizeof(T)))
        return false;
    __try {
        memcpy(&result, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ExactImagePointer(uintptr_t value, uintptr_t imageBase, uintptr_t rva) {
    uintptr_t expected = 0;
    return Add(imageBase, rva, expected) && value == expected;
}

bool ReadExactSlot(uintptr_t vtable, size_t slot, uintptr_t imageBase, uintptr_t expectedRva) {
    uintptr_t target = 0;
    return Read(vtable, slot, target) && ExactImagePointer(target, imageBase, expectedRva);
}

NativeSessionSnapshot Failure(const char* reason) {
    NativeSessionSnapshot result;
    result.reason = reason;
    return result;
}

const char* AccountReadReason(ReadStatus status) {
    switch (status) {
    case ReadStatus::Empty: return "account_id_empty";
    case ReadStatus::Truncated: return "account_id_too_long";
    case ReadStatus::InvalidUtf8: return "account_id_invalid_utf8";
    case ReadStatus::InvalidLayout: return "account_id_invalid_layout";
    case ReadStatus::Unreadable: return "account_id_unreadable";
    case ReadStatus::Exception: return "account_id_read_exception";
    default: return "account_id_unavailable";
    }
}
} // namespace

NativeSessionSnapshot ReadNativeSession(uintptr_t imageBase, DWORD expectedUiThreadId) {
    LastErrorGuard preserveLastError;
    if (!imageBase) return Failure("image_base_missing");
    if (!expectedUiThreadId) return Failure("ui_thread_unknown");
    if (GetCurrentThreadId() != expectedUiThreadId) return Failure("wrong_ui_thread");

    uintptr_t root = 0;
    if (!Read(imageBase, profile::kRootGlobalRva, root)) return Failure("root_global_unreadable");
    if (!root) return Failure("root_missing");

    uintptr_t rootVtable = 0;
    if (!Read(root, 0, rootVtable)) return Failure("root_unreadable");
    if (!ExactImagePointer(rootVtable, imageBase, profile::kRootVtableRva) ||
        !ReadExactSlot(rootVtable, profile::kRootIdentityGetterSlot, imageBase, profile::kRootIdentityGetterRva) ||
        !ReadExactSlot(rootVtable, profile::kRootServicePairGetterSlot, imageBase, profile::kRootServicePairGetterRva) ||
        !ReadExactSlot(rootVtable, profile::kRootServiceReadySlot, imageBase, profile::kRootServiceReadyRva))
        return Failure("root_profile_mismatch");

    Pair service{};
    if (!Read(root, profile::kRootServicePairOffset, service)) return Failure("identity_service_pair_unreadable");
    if (!service.raw || !service.control) return Failure("identity_service_missing");

    uintptr_t serviceVtable = 0;
    if (!Read(service.raw, 0, serviceVtable)) return Failure("identity_service_unreadable");
    if (!ExactImagePointer(serviceVtable, imageBase, profile::kIdentityServiceVtableRva) ||
        !ReadExactSlot(serviceVtable, profile::kIdentityReadyGetterSlot, imageBase, profile::kIdentityReadyGetterRva) ||
        !ReadExactSlot(serviceVtable, profile::kIdentityStringGetterSlot, imageBase, profile::kIdentityStringGetterRva))
        return Failure("identity_service_profile_mismatch");

    uint8_t readyBefore = 0;
    if (!Read(service.raw, profile::kIdentityReadyOffset, readyBefore))
        return Failure("account_ready_unreadable");
    if ((readyBefore & profile::kAccountReadyMask) == 0) return Failure("account_not_ready");

    char accountId[kAccountIdCapacity]{};
    uintptr_t identityAddress = 0;
    if (!Add(service.raw, profile::kIdentityStringOffset, identityAddress))
        return Failure("account_id_address_overflow");
    const auto text = CopyNativeString(reinterpret_cast<const void*>(identityAddress),
                                       accountId, sizeof(accountId));
    if (text.status != ReadStatus::Ok) return Failure(AccountReadReason(text.status));
    if (!text.capturedBytes || memchr(accountId, 0, text.capturedBytes))
        return Failure("account_id_invalid_value");

    NativeSessionSnapshot result;
    result.accountId.assign(accountId, text.capturedBytes);
    result.accountReady = true;

    // Re-read the root and borrowed identity owner immediately after copying.
    // This detects ordinary logout/account-switch replacement without calling
    // native retain methods. UI-thread serialization remains a prerequisite.
    uintptr_t freshRoot = 0, freshRootVtable = 0, freshServiceVtable = 0;
    Pair freshService{};
    uint8_t freshReady = 0;
    if (!Read(imageBase, profile::kRootGlobalRva, freshRoot) || freshRoot != root ||
        !Read(root, 0, freshRootVtable) || freshRootVtable != rootVtable ||
        !Read(root, profile::kRootServicePairOffset, freshService) ||
        freshService.raw != service.raw || freshService.control != service.control ||
        !Read(service.raw, 0, freshServiceVtable) || freshServiceVtable != serviceVtable ||
        !Read(service.raw, profile::kIdentityReadyOffset, freshReady) || (freshReady & profile::kAccountReadyMask) == 0)
        return Failure("identity_session_changed");

    const auto executorFailure = [&result](const char* reason) {
        result.executorReady = false;
        result.reason = reason;
        return result;
    };

    Pair executor{};
    if (!Read(root, profile::kRootExecutorPairOffset, executor))
        return executorFailure("executor_pair_unreadable");
    if (!executor.raw || !executor.control) return executorFailure("executor_missing");
    uintptr_t inner = 0;
    if (!Read(executor.raw, profile::kExecutorInnerOffset, inner))
        return executorFailure("executor_unreadable");
    if (!inner) return executorFailure("executor_inner_missing");
    uint8_t executorFlags = 0;
    if (!Read(inner, profile::kExecutorFlagsOffset, executorFlags))
        return executorFailure("executor_flags_unreadable");

    // The executor is another borrowed chain; reject a change between its two
    // reads instead of treating a stale normal-branch bit as session state.
    Pair freshExecutor{};
    uintptr_t freshInner = 0;
    uint8_t freshExecutorFlags = 0;
    if (!Read(root, profile::kRootExecutorPairOffset, freshExecutor) ||
        freshExecutor.raw != executor.raw || freshExecutor.control != executor.control ||
        !Read(executor.raw, profile::kExecutorInnerOffset, freshInner) || freshInner != inner ||
        !Read(inner, profile::kExecutorFlagsOffset, freshExecutorFlags) || freshExecutorFlags != executorFlags)
        return executorFailure("executor_session_changed");

    result.executorReady = (executorFlags & profile::kExecutorDummyMask) == 0;
    result.reason = result.executorReady ? "ok" : "executor_dummy_branch";
    return result;
}

} // namespace wechatbot::monitor
