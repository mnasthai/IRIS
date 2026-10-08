#pragma once
#include "monitor/core/unique_handle.hpp"
#include "monitor/send/send_gate.hpp"
#include <memory>
#include <string_view>

namespace wechatbot::monitor {

/**
 * @brief 已就绪的待发送多媒体文件结构 (PreparedMediaFile)
 * @details 
 *   当发送图片或语音消息时，在进入微信 UI 线程与原生底层之前，必须在后台工作线程先将文件准备就绪。
 *   - 对于图片：微信原生底层仅接收宽字符磁盘路径，并通过后台异步线程去读取上传。
 *     因此内部的 `handle` 必须以 `GENERIC_READ | FILE_SHARE_READ` 独占锁定文件（禁止外部写入或删除），
 *     防止在微信异步上传读取期间文件被篡改或删除造成访问越界崩溃。
 *   - 对于语音：解析出的 SILK 二进制数据缓存在 `audio` 中，毫秒级音频时长保存在 `durationMs`。
 */
struct PreparedMediaFile {
    std::wstring path;          ///< 图片文件在磁盘上的完整规范化路径 (UTF-16)
    std::string audio;          ///< 语音文件的原始二进制数据 (SILK 编码)
    uint32_t durationMs = 0;    ///< 语音时长 (毫秒，必须为 20ms 的整数倍)
    UniqueHandle handle;        ///< 保持打开的 Win32 只读文件句柄 (用于锁住文件防写防删)

    PreparedMediaFile() = default;
    PreparedMediaFile(const PreparedMediaFile&) = delete;
    PreparedMediaFile& operator=(const PreparedMediaFile&) = delete;
};

/**
 * @brief 媒体文件预处理结果
 */
struct MediaFileResult {
    std::shared_ptr<PreparedMediaFile> file; ///< 准备好的文件对象，失败时为空
    std::string error;                       ///< 错误原因码 (如 "media_hash_mismatch", "media_path_not_allowed")
};

/**
 * @brief 计算微信 SILK 语音音频的时长（毫秒）
 * @param bytes SILK 格式二进制数据（可带或不带微信专有 '\x02' 头）
 * @return 语音时长毫秒数；若格式不符合 SILK_V3 规范或超过 60 秒上限则返回 0
 */
uint32_t SilkDurationMs(std::string_view bytes);

/**
 * @brief 使用 Windows BCrypt CNG 加密库计算二进制数据的 SHA-256 哈希值
 * @param bytes 待计算哈希的数据切片
 * @return 64 字符的小写十六进制哈希字符串；失败返回空字符串
 */
std::string MediaSha256(std::string_view bytes);

/**
 * @brief 校验并准备待发送的媒体文件
 * @details 
 *   运行环境说明：在命名管道通信工作线程执行，严禁在微信 Qt UI 线程执行。
 *   该函数执行极其严格的安全核验：
 *   1. 检查文件路径是否为绝对路径，且必须直接位于配置的 root 暂存根目录内（防止路径穿越攻击）；
 *   2. 检查文件名必须与给定的 sha256 严格一致；
 *   3. 检查文件后缀名合法性（图片限 .png/.jpg/.jpeg，语音限 .silk）；
 *   4. 解析并核验文件真实的规范化物理路径（解析符号链接与目录联接点，防止 Junction 劫持）；
 *   5. 校验文件大小并在内存中分块计算实际 SHA-256，与 command.sha256 比对；
 *   6. 对于图片校验其魔数文件头尾完整性；对于语音校验 SILK 帧头与时长一致性。
 * @param command 发送指令中的媒体描述
 * @param root 本地配置的媒体暂存根目录路径
 * @return MediaFileResult 包含已锁定的 PreparedMediaFile 句柄或错误码
 */
MediaFileResult PrepareMediaFile(const MediaCommand& command, const std::wstring& root);

} // namespace wechatbot::monitor
