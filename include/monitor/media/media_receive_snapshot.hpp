#pragma once
#include "monitor/media/media_candidate_file.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace wechatbot::monitor::media_receive_detail {

/// 语音消息最大允许的内存字节大小 (1MB)
inline constexpr size_t kMaxVoiceBytes = 1024 * 1024;

/**
 * @brief 接收多媒体消息的基础身份标识
 */
struct Identity {
    uint64_t sequence = 0;        ///< 递增全局流水号
    uint64_t observedUnixMs = 0;  ///< 观测到的 Unix 时间戳 (毫秒)
    uint64_t messageId = 0;       ///< 微信服务端消息唯一 ID (NewMsgId)
    uint32_t type = 0;            ///< 消息主类型 (3=图片, 34=语音)
    char from[256]{};             ///< 会话 ID (聊天室 wxid 或好友 wxid)
    char to[256]{};               ///< 接收者 ID (当前登录微信号)
    char sender[256]{};           ///< 群内发言人 wxid (私聊时与 from 相同)
};

struct Snapshot;

/**
 * @brief 在途多媒体处理门票 (PendingPermit)
 * @details 
 *   采用 RAII 模式管理流水线的并发容量配额：
 *   - 在 MinHook 回调中通过 ReserveMediaSnapshot() 获取门票；
 *   - 门票跨越：现场捕获 -> 环形队列排队 -> 工作线程处理完毕 的完整生命周期；
 *   - 只有在 Snapshot 彻底析构时，门票析构函数才会递减原子计数释放容量配额；
 *   - 移动构造/赋值转移所有权，析构自动归还配额，杜绝泄漏。
 */
class PendingPermit {
public:
    PendingPermit() = default;
    ~PendingPermit();
    PendingPermit(PendingPermit&& other) noexcept;
    PendingPermit& operator=(PendingPermit&& other) noexcept;
    explicit operator bool() const noexcept { return state_ != nullptr; }
    PendingPermit(const PendingPermit&) = delete;
    PendingPermit& operator=(const PendingPermit&) = delete;
private:
    explicit PendingPermit(std::atomic<size_t>& state) noexcept : state_(&state) {}
    void Release() noexcept;
    std::atomic<size_t>* state_ = nullptr;
    friend std::unique_ptr<Snapshot> ReserveMediaSnapshot();
};

/**
 * @brief 多媒体接收原始快照 (Snapshot)
 */
struct Snapshot {
    PendingPermit permit;       ///< 关联的并发配额门票 (首字段声明：保证在句柄释放后才归还配额)
    Identity identity;          ///< 消息身份标识
    std::string bytes;          ///< 语音数据的内存拷贝 (SILK/AMR 二进制)
    uint32_t resourceKind = 0;  ///< 资源类别
    ImageCandidate image[2];    ///< 候选图片文件上下文 (+0x68 与 +0x88 偏移)
};

/**
 * @brief 在 MinHook 回调中安全捕获接收到的语音消息
 * @param base WeChatWin.dll 模块基地址
 * @param caller 调用方返回地址 (_ReturnAddress)
 * @param handler 微信内部 VoiceHandler 指针
 * @param messagePair 原始消息 NativeSharedPair
 * @param bufferPair 包含语音二进制数据的缓冲对
 * @param[out] out 输出填充好的快照对象
 * @return true 捕获成功；false 校验不符或内存不可读
 */
bool CaptureVoice(uintptr_t base, uintptr_t caller, const void* handler,
                  const void* messagePair, const void* bufferPair, Snapshot& out);

/**
 * @brief 在 MinHook 回调中安全捕获接收到的图片候选文件
 * @param base WeChatWin.dll 模块基地址
 * @param caller 调用方返回地址 (_ReturnAddress)
 * @param handler 微信内部 ImageHandler 指针
 * @param messagePair 原始消息 NativeSharedPair
 * @param resource 图片资源对象指针 (包含缓存文件路径)
 * @param result 微信下载结果状态指针
 * @param[out] out 输出填充好的快照对象 (并立即锁定候选文件句柄)
 * @return true 捕获成功；false 校验不符或下载未就绪
 */
bool CaptureImage(uintptr_t base, uintptr_t caller, const void* handler,
                  const void* messagePair, const void* resource, const void* result, Snapshot& out);

} // namespace wechatbot::monitor::media_receive_detail
