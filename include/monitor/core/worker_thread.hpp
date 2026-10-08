#pragma once
#include "monitor/core/unique_handle.hpp"

namespace wechatbot::monitor {

// Lifecycle calls are serialized by the owning worker. The context must stay
// alive until a successful Join. Destruction never waits or terminates a thread;
// DLL owners remain process-lifetime and join explicitly outside DllMain.
class WorkerThread final {
public:
    bool Start(LPTHREAD_START_ROUTINE routine, void* context = nullptr) noexcept {
        if (!routine || thread_) {
            SetLastError(routine ? ERROR_ALREADY_EXISTS : ERROR_INVALID_PARAMETER);
            return false;
        }
        thread_.reset(CreateThread(nullptr, 0, routine, context, 0, nullptr));
        return static_cast<bool>(thread_);
    }
    DWORD Join(DWORD timeoutMs) noexcept {
        if (!thread_) return WAIT_OBJECT_0;
        const DWORD result = WaitForSingleObject(thread_.get(), timeoutMs);
        if (result == WAIT_OBJECT_0) thread_.reset();
        return result;
    }
    bool HasThread() const noexcept { return static_cast<bool>(thread_); }
private:
    UniqueHandle thread_;
};

} // namespace wechatbot::monitor
