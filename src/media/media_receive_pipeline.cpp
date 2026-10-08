#include "monitor/media/media_receive_pipeline.hpp"
#include "monitor/media/media_asset_publisher.hpp"
#include "monitor/media/media_inspection.hpp"
#include "monitor/diagnostics/event.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/runtime/runtime.hpp"
#include "monitor/core/worker_thread.hpp"
#include "monitor/config/version_profile.hpp"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <utility>

namespace wechatbot::monitor {
namespace media_receive_detail {
namespace profile = active_profile::media_receive;

namespace {
/// 使用最高有效位 (MSB) 标记流水线处于关闭状态 (Closed Bit)
constexpr size_t kClosed = size_t{1} << (sizeof(size_t) * 8 - 1);

/**
 * @brief 流水线全局状态管理单例
 */
struct PipelineState {
    std::atomic<size_t> pendingState{kClosed};   ///< 原子门票状态：低位为在途门票计数，高位为 kClosed 标记
    std::atomic<uint64_t> dropped{0};            ///< 因队列超限或关闭而丢弃的快照总数
    std::atomic<bool> started{false};            ///< 流水线是否已处于运行中
    SRWLOCK lifecycleLock = SRWLOCK_INIT;        ///< 保护启动/停止生命周期的锁
    SRWLOCK queueLock = SRWLOCK_INIT;            ///< 保护环形队列的锁
    std::unique_ptr<Snapshot> queue[kMaxPending];///< 容量为 16 的环形队列
    size_t head = 0, count = 0;                  ///< 队列头与元素计数
    UniqueHandle wake;                           ///< 唤醒后台工作线程的 Win32 事件
    WorkerThread worker;                         ///< 后台处理工作线程
    std::wstring inboundRoot;                    ///< 资产落盘根目录
};

PipelineState& Pipeline() {
    // 采用静态指针堆分配，严禁在 DllMain 卸载时通过 CRT 析构，防止 Loader Lock 死锁
    static auto* state = new PipelineState;
    return *state;
}

struct LifecycleGuard {
    LifecycleGuard() { AcquireSRWLockExclusive(&Pipeline().lifecycleLock); }
    ~LifecycleGuard() { ReleaseSRWLockExclusive(&Pipeline().lifecycleLock); }
};

bool Accepting() { return !(Pipeline().pendingState.load(std::memory_order_acquire) & kClosed); }
void CloseAcceptance() { Pipeline().pendingState.fetch_or(kClosed, std::memory_order_acq_rel); }

/**
 * @brief 从环形队列中弹出一个待处理的快照
 */
std::unique_ptr<Snapshot> Pop() {
    auto& state = Pipeline();
    AcquireSRWLockExclusive(&state.queueLock);
    std::unique_ptr<Snapshot> value;
    if (state.count) {
        value = std::move(state.queue[state.head]);
        state.head = (state.head + 1) % kMaxPending;
        --state.count;
    }
    ReleaseSRWLockExclusive(&state.queueLock);
    return value;
}

/**
 * @brief 工作线程处理图片快照
 * @details 
 *   微信的资源对象包含两个候选路径：
 *   - +0x68: 原始下载的图片输入；
 *   - +0x88: 微信后续封装的缓存表示。
 *   代码先对两者都生成 ImageCandidateRecord 诊断日志；
 *   对于 +0x68 的原始文件，若可读且格式正确，则调用 PublishImageAsset 进行原子落盘发布。
 */
void ProcessImage(Snapshot& value) {
    auto& state = Pipeline();
    for (size_t i = 0; i < 2; ++i) {
        auto& candidate = value.image[i];
        const auto read = ReadCandidate(candidate);
        const char* extension = read ? ImageExtension(read.bytes) : nullptr;
        auto identity = value.identity; identity.sequence = NextSequence();
        ImageCandidateRecordBuilder recordBuilder(identity, value.resourceKind, profile::kImagePathOffsets[i],
            candidate, read, extension);
        PublishOutcome publication;
        
        // 仅对 +0x68 原图执行发布
        if (read && i == 0 && extension) {
            auto assetIdentity = identity; assetIdentity.sequence = NextSequence();
            publication = PublishImageAsset(assetIdentity, value.resourceKind, profile::kImagePathOffsets[0],
                read.bytes, state.inboundRoot,
                strcmp(extension, ".wxgf") == 0 ? ImagePublication::Encoded : ImagePublication::Decoded);
        }
        const auto record = std::move(recordBuilder).Finish(publication);
        AppendDirect(record.c_str());
        if (publication) AppendDirect(EncodePublishedAsset(*publication.asset).c_str());
    }
}

/**
 * @brief 后台 Worker 工作线程主循环 (Writer)
 * @details 
 *   1. 循环从队列中 Pop 快照进行处理；
 *   2. 若为图片调用 ProcessImage，若为语音调用 PublishVoiceAsset；
 *   3. 检查并输出丢弃统计日志；
 *   4. 当收到全局停止信号且在途任务已排空时退出循环；
 *   5. 阻塞等待 wake 事件（带 250ms 超时保底轮询）。
 */
DWORD WINAPI Writer(void*) {
    auto& state = Pipeline();
    uint64_t lastDropped = 0;
    for (;;) {
        while (auto value = Pop()) {
            try {
                if (value->identity.type == 3) ProcessImage(*value);
                else {
                    const auto publication = PublishVoiceAsset(*value, state.inboundRoot);
                    if (publication) AppendDirect(EncodePublishedAsset(*publication.asset).c_str());
                    else AppendDirect(EncodeMediaAssetError(value->identity, publication.status).c_str());
                }
            } catch (...) {
                AppendDirect(EncodeMediaPipelineError(PipelineFailure::WriterException).data());
            }
        }
        const uint64_t failures = state.dropped.load();
        if (failures != lastDropped) {
            const auto record = EncodeMediaDropped(failures);
            AppendDirect(record.c_str()); lastDropped = failures;
        }
        if (!Accepting() || InterlockedCompareExchange(&g_stop, 0, 0)) {
            CloseAcceptance();
            if (OutstandingMediaSnapshots() == 0) break;
        }
        WaitForSingleObject(state.wake.get(), 250);
    }
    return 0;
}
} // namespace

std::unique_ptr<Snapshot> ReserveMediaSnapshot() {
    auto& state = Pipeline();
    if (!state.started.load(std::memory_order_acquire)) return {};
    size_t old = state.pendingState.load(std::memory_order_acquire);
    for (;;) {
        if (old & kClosed) return {};
        if (old >= kMaxPending) { ++state.dropped; return {}; }
        if (state.pendingState.compare_exchange_weak(old, old + 1, std::memory_order_acq_rel)) break;
    }
    PendingPermit permit(state.pendingState);
    try {
        auto value = std::make_unique<Snapshot>();
        value->permit = std::move(permit);
        return value;
    } catch (...) { ++state.dropped; return {}; }
}

bool EnqueueMediaSnapshot(std::unique_ptr<Snapshot> value) {
    auto& state = Pipeline();
    if (!value) return false;
    if (!value->permit) { ++state.dropped; return false; }
    if (!Accepting() || !TryAcquireSRWLockExclusive(&state.queueLock)) { ++state.dropped; return false; }
    if (state.count == kMaxPending) { ReleaseSRWLockExclusive(&state.queueLock); ++state.dropped; return false; }
    state.queue[(state.head + state.count) % kMaxPending] = std::move(value); ++state.count;
    SetEvent(state.wake.get());
    ReleaseSRWLockExclusive(&state.queueLock);
    return true;
}

void RecordMediaSnapshotDrop() noexcept { ++Pipeline().dropped; }
size_t OutstandingMediaSnapshots() noexcept {
    return Pipeline().pendingState.load(std::memory_order_acquire) & ~kClosed;
}
uint64_t DroppedMediaSnapshots() noexcept { return Pipeline().dropped.load(); }
} // namespace media_receive_detail

bool MediaReceivePipelineStarted() noexcept {
    return media_receive_detail::Pipeline().started.load(std::memory_order_acquire);
}

bool StartMediaReceivePipeline(const std::wstring& root) {
    using namespace media_receive_detail;
    auto& state = Pipeline();
    LifecycleGuard guard;
    if (state.worker.HasThread() || OutstandingMediaSnapshots() != 0) return false;
    if (!std::filesystem::path(root).is_absolute()) {
        AppendDirect(EncodeMediaPipelineError(PipelineFailure::InvalidInboundRoot).data()); return false;
    }
    state.inboundRoot = root;
    state.wake.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!state.wake) {
        AppendDirect(EncodeMediaPipelineError(PipelineFailure::WriterEventFailed).data()); return false;
    }
    state.head = state.count = 0;
    state.pendingState.store(0, std::memory_order_release);
    if (!state.worker.Start(Writer)) {
        CloseAcceptance(); state.wake.reset();
        AppendDirect(EncodeMediaPipelineError(PipelineFailure::WriterThreadFailed).data()); return false;
    }
    state.started.store(true, std::memory_order_release);
    return true;
}

bool StopNativeMediaReceive(DWORD timeoutMs) noexcept {
    using namespace media_receive_detail;
    auto& state = Pipeline();
    LifecycleGuard guard;
    CloseAcceptance();
    if (!state.worker.HasThread()) return true;
    SetEvent(state.wake.get());
    if (state.worker.Join(timeoutMs) != WAIT_OBJECT_0) return false;
    state.wake.reset();
    state.started.store(false, std::memory_order_release);
    return true;
}

} // namespace wechatbot::monitor
