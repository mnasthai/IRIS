#pragma once
#include "monitor/core/unique_handle.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace wechatbot::monitor::media_receive_detail {

/// 候选接收图片的最大单文件上限 (32MB)
inline constexpr size_t kMaxImageBytes = 32 * 1024 * 1024;

/// 候选文件状态码
enum class CandidateStatus : uint8_t {
    PathUnreadable,   ///< 路径内存无法安全读取或包含非法字符
    OpenFailed,       ///< 调用 CreateFileW 打开文件失败
    NotRegularFile,   ///< 目标不是普通磁盘文件（如目录、重解析点或设备）
    SizeRejected,     ///< 文件大小为 0 或超出 32MB 上限
    Opened,           ///< 文件句柄成功打开并锁定
    FileChanged,      ///< 读取过程中检测到文件已被外部修改（未通过 SameFile 检验）
    SeekFailed,       ///< 重设文件指针至开头失败
    ReadFailed,       ///< 读取文件数据失败或提前到达 EOF
    StableFileRead    ///< 文件数据完整稳定读取成功
};

/**
 * @brief 候选文件读取结果
 */
struct CandidateReadResult {
    CandidateStatus status = CandidateStatus::PathUnreadable;
    std::string bytes;
    explicit operator bool() const noexcept { return status == CandidateStatus::StableFileRead; }
};

/**
 * @brief 候选图片文件上下文 (ImageCandidate)
 * @details 在 MinHook 回调中即时初始化并打开句柄，记录初始元数据 `initial`，
 *          供后续工作线程在核验 `SameFile` 后安全读取。
 */
struct ImageCandidate {
    UniqueHandle handle;                    ///< 保持打开的文件句柄
    BY_HANDLE_FILE_INFORMATION initial{};   ///< 初始打开时的文件元数据信息
    DWORD openError = ERROR_SUCCESS;        ///< 打开失败时的 Win32 GetLastError
    CandidateStatus status = CandidateStatus::PathUnreadable; ///< 当前状态
    std::wstring path;                      ///< 文件路径
};

/**
 * @brief 在回调现场立即打开候选文件以锁定其句柄
 * @param path 微信内部传递的文件路径
 * @param[out] out 输出填充好的 ImageCandidate 结构
 */
void OpenImageCandidate(const std::wstring& path, ImageCandidate& out);

/**
 * @brief 从 Win32 文件信息结构中组合获取 64 位文件大小
 */
uint64_t CandidateFileLength(const BY_HANDLE_FILE_INFORMATION& info);

/**
 * @brief 在后台工作线程中核验并读取候选文件的全部二进制数据
 * @details 通过比对前后 BY_HANDLE_FILE_INFORMATION 防止并发写入或篡改
 */
CandidateReadResult ReadCandidate(ImageCandidate& candidate);

} // namespace wechatbot::monitor::media_receive_detail
