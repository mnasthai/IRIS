#pragma once
#include "monitor/media/media_receive_snapshot.hpp"
#include <memory>

namespace wechatbot::monitor {

/**
 * @brief 启动多媒体接收后台消费流水线
 * @details 创建独立的后台工作线程 (Writer)，监听环形队列中的快照并执行 WIC 解码、SILK 校验及资产发布
 * @param inboundRoot 本地资产落盘根目录
 * @return true 启动成功；false 已启动或参数无效
 */
bool StartMediaReceivePipeline(const std::wstring& inboundRoot);

/**
 * @brief 检查多媒体接收流水线是否已启动
 */
bool MediaReceivePipelineStarted() noexcept;

/**
 * @brief 停止多媒体接收流水线并等待在途任务排空
 * @details 
 *   调用安全要求：绝对严禁在 DllMain Loader Lock 下调用！
 *   1. 关闭新快照准入；
 *   2. 阻塞等待所有已入队或处理中的多媒体快照完成；
 *   3. 终止后台 Worker 线程并清理资源。
 * @param timeoutMs 最长等待毫秒数
 * @return true 成功排空停机；false 超时
 */
bool StopNativeMediaReceive(DWORD timeoutMs) noexcept;

namespace media_receive_detail {

/// 队列中允许并存的最大快照容量配额
inline constexpr size_t kMaxPending = 16;

/**
 * @brief 预留一个多媒体快照配额门票并创建 Snapshot 对象
 * @return 成功返回持有 PendingPermit 的唯一指针；若容量超限或流水线已关闭则返回 nullptr
 */
std::unique_ptr<Snapshot> ReserveMediaSnapshot();

/**
 * @brief 将捕获完毕的快照推入环形队列并唤醒 Worker 线程
 * @return true 入队成功；false 队列已满或流水线关闭
 */
bool EnqueueMediaSnapshot(std::unique_ptr<Snapshot> value);

void RecordMediaSnapshotDrop() noexcept;
size_t OutstandingMediaSnapshots() noexcept;
uint64_t DroppedMediaSnapshots() noexcept;

} // namespace media_receive_detail
} // namespace wechatbot::monitor
