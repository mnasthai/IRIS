// Installs thread-specific hooks only on this test process's own message loops.
// No Weixin process, native sender, window interaction, or injection is involved.
#include "monitor/transport/ui_dispatch.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <utility>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);

namespace {
constexpr DWORD kTestWait = 5000;
constexpr UINT kPause = WM_APP + 91;
constexpr UINT kBarrier = WM_APP + 92;
constexpr UINT kInline = WM_APP + 93;

struct Event {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { Check(value != nullptr, "create local dispatcher test event"); }
    ~Event() { CloseHandle(value); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    void Set() const { Check(SetEvent(value) != FALSE, "signal dispatcher test event"); }
    void Reset() const { Check(ResetEvent(value) != FALSE, "reset dispatcher test event"); }
    void Wait() const { Check(WaitForSingleObject(value, kTestWait) == WAIT_OBJECT_0,
                              "dispatcher test event arrives within deadline"); }
};

class MessageThread {
public:
    MessageThread() {
        thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, &id_);
        Check(thread_ != nullptr, "create ordinary local message-loop thread");
        ready_.Wait();
    }
    ~MessageThread() {
        resume_.Set();
        Check(PostThreadMessageW(id_, WM_QUIT, 0, 0) != FALSE, "stop local message-loop thread");
        Check(WaitForSingleObject(thread_, kTestWait) == WAIT_OBJECT_0, "local message loop stops");
        CloseHandle(thread_);
    }
    DWORD Id() const { return id_; }
    void Pause() {
        resume_.Reset(); paused_.Reset();
        Check(PostThreadMessageW(id_, kPause, 0, 0) != FALSE, "pause local message retrieval");
        paused_.Wait();
    }
    void Resume() { resume_.Set(); }
    void Barrier() {
        barrier_.Reset();
        Check(PostThreadMessageW(id_, kBarrier, 0, 0) != FALSE, "queue local message barrier");
        barrier_.Wait();
    }
    MSG LastWake() {
        Barrier(); // Publishes lastWake_ after previously posted messages.
        AcquireSRWLockExclusive(&lock_);
        const MSG copied = lastWake_;
        ReleaseSRWLockExclusive(&lock_);
        return copied;
    }
    void OnThread(std::function<void()> action) {
        inlineDone_.Reset();
        AcquireSRWLockExclusive(&lock_);
        inlineAction_ = std::move(action);
        ReleaseSRWLockExclusive(&lock_);
        Check(PostThreadMessageW(id_, kInline, 0, 0) != FALSE, "queue local inline-call test");
        inlineDone_.Wait();
    }

private:
    static DWORD WINAPI ThreadMain(void* pointer) {
        auto& self = *static_cast<MessageThread*>(pointer);
        try {
            MSG msg{};
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // Create this thread's queue.
            self.ready_.Set();
            for (;;) {
                const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                Check(got != -1, "local GetMessage succeeds");
                if (!got) break;
                if (!msg.hwnd && msg.message >= 0xC000) {
                    AcquireSRWLockExclusive(&self.lock_);
                    self.lastWake_ = msg;
                    ReleaseSRWLockExclusive(&self.lock_);
                }
                if (msg.message == kPause) { self.paused_.Set(); self.resume_.Wait(); }
                if (msg.message == kBarrier) self.barrier_.Set();
                if (msg.message == kInline) {
                    std::function<void()> action;
                    AcquireSRWLockExclusive(&self.lock_);
                    action = std::move(self.inlineAction_);
                    ReleaseSRWLockExclusive(&self.lock_);
                    action();
                    self.inlineDone_.Set();
                }
            }
        } catch (...) { Check(false, "no exception crosses local Win32 test thread entry"); }
        return 0;
    }
    HANDLE thread_ = nullptr;
    DWORD id_ = 0;
    Event ready_, paused_, resume_, barrier_, inlineDone_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    MSG lastWake_{};
    std::function<void()> inlineAction_;
};

struct Lifetime {
    std::shared_ptr<Event> destroyed;
    explicit Lifetime(std::shared_ptr<Event> signal) : destroyed(std::move(signal)) {}
    ~Lifetime() { destroyed->Set(); }
};
} // namespace

void TestUiDispatch() {
    MessageThread ui;
    UiDispatcher dispatcher;
    Check(!dispatcher.Bind(0) && GetLastError() == ERROR_INVALID_PARAMETER,
          "zero/global hook thread id is rejected");
    Check(dispatcher.Bind(ui.Id()) && dispatcher.BoundThreadId() == ui.Id(),
          "bind only this process's prepared message-loop thread");
    UiDispatcher duplicate;
    Check(!duplicate.Bind(ui.Id()) && GetLastError() == ERROR_BUSY,
          "a target thread has only one dispatcher");

    const auto calls = std::make_shared<std::atomic<unsigned>>(0);
    const auto observedThread = std::make_shared<std::atomic<DWORD>>(0);
    SetLastError(7788);
    const auto completed = dispatcher.Execute([calls, observedThread] {
        ++*calls;
        observedThread->store(GetCurrentThreadId());
        SetLastError(9988);
    }, kTestWait);
    Check(completed.status == UiDispatchStatus::completed && completed.executionThreadId == ui.Id() &&
          observedThread->load() == ui.Id() && calls->load() == 1 && GetLastError() == 7788,
          "action runs once on target thread and caller LastError is preserved");

    const auto originalWake = ui.LastWake();
    Check(originalWake.message >= 0xC000 && originalWake.wParam != 0,
          "dispatcher uses a registered message and nonzero task token");
    for (int i = 0; i < 3; ++i)
        Check(PostThreadMessageW(ui.Id(), originalWake.message, originalWake.wParam,
                                 originalWake.lParam) != FALSE, "replay completed local wake token");
    ui.Barrier();
    Check(calls->load() == 1, "duplicate wake tokens never execute completed task again");

    ui.Pause();
    const auto cancelled = dispatcher.Execute([calls] { ++*calls; }, 20);
    Check(cancelled.status == UiDispatchStatus::cancelled_before_start &&
          cancelled.executionThreadId == 0 && calls->load() == 1,
          "timeout cancels a task whose message has not been removed");
    ui.Resume(); ui.Barrier();
    Check(calls->load() == 1, "cancelled pending task stays cancelled when its wake is later removed");

    // Block the action itself to distinguish an executing timeout from pending
    // cancellation. All task captures are owned values, including its events.
    const auto started = std::make_shared<Event>();
    const auto release = std::make_shared<Event>();
    const auto destroyed = std::make_shared<Event>();
    auto lifetime = std::make_shared<Lifetime>(destroyed);
    const std::weak_ptr<Lifetime> weakLifetime = lifetime;
    std::function<void()> runningAction = [lifetime, started, release, calls] {
        ++*calls;
        started->Set();
        release->Wait();
        Check(lifetime != nullptr, "running action retains owned capture");
    };
    lifetime.reset();
    UiDispatchResult runningResult;
    std::thread caller([&dispatcher, &runningResult, action = std::move(runningAction)]() mutable {
        runningResult = dispatcher.Execute(std::move(action), 1000);
    });
    started->Wait();
    Check(dispatcher.Execute([] {}, 0).status == UiDispatchStatus::busy,
          "second action is rejected while first one is running");
    Check(!dispatcher.Unbind() && GetLastError() == ERROR_BUSY,
          "running action prevents normal unbind");
    caller.join();
    Check(runningResult.status == UiDispatchStatus::timeout_after_start &&
          runningResult.executionThreadId == ui.Id() && !weakLifetime.expired(),
          "running timeout returns actual thread and keeps captured data alive");
    Check(dispatcher.Execute([] {}, 0).status == UiDispatchStatus::busy,
          "running timeout retains the single busy slot");
    release->Set(); destroyed->Wait(); ui.Barrier();
    Check(weakLifetime.expired() && calls->load() == 2,
          "capture is released only after action returns and action was not retried");

    const auto exceptional = dispatcher.Execute([] { throw 123; }, kTestWait);
    Check(exceptional.status == UiDispatchStatus::exception && exceptional.executionThreadId == ui.Id(),
          "action exception is reported without escaping Windows hook");
    Check(dispatcher.Execute([] {}, kTestWait).status == UiDispatchStatus::completed,
          "exception does not strand the busy slot");

    // This call originates on the target thread outside a dispatcher task. It
    // must execute inline; posting and synchronously waiting would deadlock.
    auto* const bridge = &dispatcher;
    const DWORD uiId = ui.Id();
    ui.OnThread([bridge, uiId] {
        const auto inlineResult = bridge->Execute([] {}, 0);
        Check(inlineResult.status == UiDispatchStatus::completed &&
              inlineResult.executionThreadId == uiId, "target-thread caller runs inline");
    });
    const auto reentrant = std::make_shared<std::atomic<UiDispatchStatus>>(UiDispatchStatus::unavailable);
    dispatcher.Execute([bridge, reentrant] {
        reentrant->store(bridge->Execute([] {}, 0).status);
    }, kTestWait);
    Check(reentrant->load() == UiDispatchStatus::busy, "reentrant dispatch does not wait on itself");

    ui.Pause();
    UiDispatchResult pendingResult;
    std::atomic<bool> submitting{false};
    std::thread pending([&dispatcher, &pendingResult, &submitting, calls] {
        submitting.store(true);
        const ULONGLONG admissionDeadline = GetTickCount64() + kTestWait;
        do {
            pendingResult = dispatcher.Execute([calls] { ++*calls; }, kTestWait);
            if (pendingResult.status != UiDispatchStatus::busy) break;
            // Only busy is retried here: no task was admitted. The no-op probe
            // below may briefly win admission; no timeout/result is retried.
            Sleep(1);
        } while (GetTickCount64() < admissionDeadline);
    });
    // Bind/Execute's public contract has no queue-inspection API. A zero-timeout
    // local no-op can only cancel while retrieval is paused; busy proves that
    // the other caller has acquired the slot. All probe tokens remain harmless.
    const ULONGLONG deadline = GetTickCount64() + kTestWait;
    bool sawPending = false;
    while (GetTickCount64() < deadline) {
        if (submitting.load() && dispatcher.Execute([] {}, 0).status == UiDispatchStatus::busy) {
            sawPending = true; break;
        }
        Sleep(1); // Bound the no-op probes below the thread-message queue limit.
    }
    Check(sawPending, "pending task occupies slot before cleanup");
    Check(dispatcher.Unbind(), "unbind cancels pending task and removes Windows hook");
    pending.join();
    Check(pendingResult.status == UiDispatchStatus::cancelled_before_start &&
          pendingResult.executionThreadId == 0, "unbind wakes pending waiter without executing action");
    ui.Resume(); ui.Barrier();
    Check(calls->load() == 2 && dispatcher.BoundThreadId() == 0 &&
          dispatcher.Execute([] {}, 0).status == UiDispatchStatus::unavailable,
          "unbound dispatcher executes nothing and releases thread identity");
    Check(duplicate.Bind(ui.Id()), "successful unbind releases registry slot for another instance");
    Check(duplicate.Execute([] {}, kTestWait).status == UiDispatchStatus::completed,
          "fresh binding ignores stale tokens and accepts fresh work");
    Check(duplicate.Unbind() && dispatcher.Unbind(), "all test hooks are explicitly removed");
}
