#include "monitor/diagnostics/logging.hpp"
#include "monitor/core/worker_thread.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace wechatbot::monitor {
namespace {
struct LoggerState {
    UniqueHandle file;
    UniqueHandle wake;
    WorkerThread worker;
    std::mutex lifecycle;
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopping{false};
    SRWLOCK queueLock = SRWLOCK_INIT;
    SRWLOCK fileLock = SRWLOCK_INIT;
    std::array<Event, kQueueCapacity> queue{};
    size_t head = 0, count = 0;
};
LoggerState& Logger() {
    // Explicit shutdown owns cleanup. Never run worker teardown under the DLL
    // loader lock or destroy storage still referenced by a timed-out worker.
    static auto* state = new LoggerState;
    return *state;
}
void WriteAll(HANDLE file, const char* data, size_t length) {
    while (file && length) {
        DWORD written = 0;
        const DWORD chunk = length > MAXDWORD ? MAXDWORD : static_cast<DWORD>(length);
        if (!WriteFile(file, data, chunk, &written, nullptr) || !written) return;
        data += written;
        length -= written;
    }
}
void FlushLog() {
    auto& state = Logger();
    AcquireSRWLockExclusive(&state.fileLock);
    if (state.file) FlushFileBuffers(state.file.get());
    ReleaseSRWLockExclusive(&state.fileLock);
}
void WriteEvent(const Event& event) {
    const auto line = SerializeEvent(event);
    auto& state = Logger();
    AcquireSRWLockExclusive(&state.fileLock);
    WriteAll(state.file.get(), line.data(), line.size());
    ReleaseSRWLockExclusive(&state.fileLock);
}
DWORD WINAPI LoggerThread(void*) {
    auto& state = Logger();
    uint64_t lastDropped = 0;
    for (;;) {
        Event event{};
        bool wrote = false;
        while (PopEvent(event)) {
            WriteEvent(event);
            wrote = true;
        }
        const uint64_t dropped = state.dropped.load();
        if (dropped != lastDropped) {
            char line[160]{};
            sprintf_s(line, "{\"kind\":\"dropped\",\"total\":%llu}",
                      static_cast<unsigned long long>(dropped));
            AppendDirect(line);
            lastDropped = dropped;
        } else if (wrote) FlushLog();
        if (state.stopping.load(std::memory_order_acquire)) {
            // StopLogger closes enqueue admission under queueLock before this
            // flag is published. One final drain handles a concurrent last push.
            while (PopEvent(event)) {
                WriteEvent(event);
            }
            FlushLog();
            return 0;
        }
        WaitForSingleObject(state.wake.get(), 100);
    }
}
} // namespace

void AppendDirect(const char* line) {
    if (!line) return;
    const auto record = AddSessionId(line);
    auto& state = Logger();
    // The validity check and file write share the lock with CloseLog. A late
    // diagnostic after complete shutdown is discarded without using a handle
    // that may have been closed and reused by another component.
    AcquireSRWLockExclusive(&state.fileLock);
    if (state.file) {
        WriteAll(state.file.get(), record.data(), record.size());
        WriteAll(state.file.get(), "\r\n", 2);
        FlushFileBuffers(state.file.get());
    }
    ReleaseSRWLockExclusive(&state.fileLock);
}

bool QueueEvent(const Event& event) {
    auto& state = Logger();
    if (!state.accepting.load(std::memory_order_acquire)) return false;
    if (!TryAcquireSRWLockExclusive(&state.queueLock)) {
        ++state.dropped;
        return false;
    }
    if (!state.accepting.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&state.queueLock);
        return false;
    }
    if (state.count == kQueueCapacity) {
        ++state.dropped;
        ReleaseSRWLockExclusive(&state.queueLock);
        return false;
    }
    state.queue[(state.head + state.count) % kQueueCapacity] = event;
    ++state.count;
    ReleaseSRWLockExclusive(&state.queueLock);
    // Producers remain nonblocking. Polling also permits ordinary test hosts
    // to use the queue before a logger worker is started.
    return true;
}

bool PopEvent(Event& event) {
    auto& state = Logger();
    AcquireSRWLockExclusive(&state.queueLock);
    if (!state.count) {
        ReleaseSRWLockExclusive(&state.queueLock);
        return false;
    }
    event = state.queue[state.head];
    state.head = (state.head + 1) % kQueueCapacity;
    --state.count;
    ReleaseSRWLockExclusive(&state.queueLock);
    return true;
}

bool OpenLog(const std::wstring& directory, const std::wstring& path) {
    auto& state = Logger();
    std::lock_guard lock(state.lifecycle);
    if (state.worker.HasThread() || state.file || directory.empty() || path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    AcquireSRWLockExclusive(&state.fileLock);
    state.file.reset(CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    const bool opened = static_cast<bool>(state.file);
    ReleaseSRWLockExclusive(&state.fileLock);
    return opened;
}

bool StartLogger() {
    auto& state = Logger();
    std::lock_guard lock(state.lifecycle);
    if (!state.file || state.worker.HasThread()) return false;
    state.wake.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!state.wake) return false;
    state.stopping.store(false, std::memory_order_release);
    state.accepting.store(true, std::memory_order_release);
    if (state.worker.Start(LoggerThread)) return true;
    state.wake.reset();
    return false;
}

bool StopLogger(DWORD timeoutMs) noexcept {
    auto& state = Logger();
    std::lock_guard lock(state.lifecycle);
    AcquireSRWLockExclusive(&state.queueLock);
    state.accepting.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&state.queueLock);
    state.stopping.store(true, std::memory_order_release);
    if (state.wake) SetEvent(state.wake.get());
    if (state.worker.Join(timeoutMs) != WAIT_OBJECT_0) return false;
    state.wake.reset();
    return true;
}

bool CloseLog() noexcept {
    auto& state = Logger();
    std::lock_guard lock(state.lifecycle);
    if (state.worker.HasThread()) return false;
    AcquireSRWLockExclusive(&state.fileLock);
    if (state.file) FlushFileBuffers(state.file.get());
    state.file.reset();
    ReleaseSRWLockExclusive(&state.fileLock);
    return true;
}
} // namespace wechatbot::monitor
