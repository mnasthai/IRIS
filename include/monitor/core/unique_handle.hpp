#pragma once
#include "monitor/core/platform.hpp"
#include <utility>

namespace wechatbot::monitor {

// Owns only a Win32 CloseHandle resource. Native reference-counted objects,
// LocalFree allocations and COM interfaces have different ownership rules.
class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(Normalize(handle)) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        handle = Normalize(handle);
        if (handle_ == handle) return;
        const DWORD error = GetLastError();
        if (handle_) CloseHandle(handle_);
        handle_ = handle;
        SetLastError(error);
    }
private:
    static HANDLE Normalize(HANDLE handle) noexcept {
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }
    HANDLE handle_ = nullptr;
};

} // namespace wechatbot::monitor
