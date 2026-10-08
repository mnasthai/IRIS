#pragma once
#include "monitor/media/media_receive_snapshot.hpp"
#include "monitor/core/json_writer.hpp"
#include <optional>
#include <string_view>

namespace wechatbot::monitor::media_receive_detail {

/**
 * @brief 已发布到接收暂存目录的多媒体资产元数据
 */
struct PublishedAsset {
    Identity identity;          ///< 关联的微信消息身份标识 (seq, messageId, from, to, sender)
    std::string sha256;         ///< 文件的 SHA-256 哈希值
    std::string name;           ///< 目标磁盘文件名 (如 "<sha256>.png", "<sha256>.silk")
    uint64_t byteLength = 0;    ///< 文件字节总数
    bool encoded = false;       ///< 是否为编码/专有格式 (如微信专有 .wxgf 动图表情)
    uint32_t resourceKind = 0;  ///< 资源类型 (微信内部分类：原图/缩略图/高清等)
    uint32_t width = 0;         ///< 图片宽度 (像素)
    uint32_t height = 0;        ///< 图片高度 (像素)
};

/// 资产发布状态码
enum class PublishStatus : uint8_t {
    NotAttempted,                 ///< 未尝试发布 (仅诊断候选者)
    Published,                    ///< 发布成功
    InvalidInboundRoot,           ///< 接收根目录不是绝对路径
    InboundDirectoryFailed,       ///< 创建接收根目录失败
    StagingCreateFailed,          ///< 创建暂存 .part 文件失败
    AssetWriteFailed,             ///< 写入暂存文件数据失败
    AssetNameConflict,            ///< 同名文件已存在且内容不一致 (哈希冲突)
    AssetPublishFailed,           ///< 原子移动暂存文件至最终文件名失败
    InvalidImageIdentity,         ///< 图片消息元数据不合法
    WxgfHeaderOrBoundsInvalid,    ///< WXGF 动图表情头或尺寸不合法
    ImageFormatOrDecodeInvalid,   ///< 图片格式错误或 WIC 解码失败
    Sha256Failed,                 ///< SHA-256 计算失败
    InvalidVoiceIdentity,         ///< 语音消息元数据不合法
    VoiceFormatOrFramingInvalid   ///< 语音格式非法或帧结构损坏
};

enum class ImagePublication : uint8_t { Decoded, Encoded };

/**
 * @brief 资产发布结果
 */
struct PublishOutcome {
    std::optional<PublishedAsset> asset;
    PublishStatus status = PublishStatus::NotAttempted;
    explicit operator bool() const noexcept { return asset.has_value(); }
};

/**
 * @brief 将接收到的图片发布到本地资产目录
 * @param identity 消息身份
 * @param resourceKind 资源种类
 * @param pathOffset 候选路径在微信资源结构中的偏移
 * @param bytes 图片原始数据
 * @param root 本地接收资产根目录
 * @param publication 发布模式（Decoded 普通解码 或 Encoded 专有编码如 WXGF）
 */
PublishOutcome PublishImageAsset(const Identity& identity, uint32_t resourceKind,
    size_t pathOffset, std::string_view bytes, const std::wstring& root,
    ImagePublication publication = ImagePublication::Decoded);

/**
 * @brief 将接收到的语音发布到本地资产目录
 * @param snapshot 语音接收快照（包含 SILK 或 AMR 数据）
 * @param root 本地接收资产根目录
 */
PublishOutcome PublishVoiceAsset(const Snapshot& snapshot, const std::wstring& root);

/// 将已发布的资产序列化为 JSON 字符串
std::string EncodePublishedAsset(const PublishedAsset& asset);

/**
 * @brief 候选图片接收记录的 JSON 构造器
 * @details 采用流式 ObjectWriter 构建，在发布完成后输出 candidate 审查日志
 */
class ImageCandidateRecordBuilder final {
public:
    ImageCandidateRecordBuilder(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
        const ImageCandidate& candidate, const CandidateReadResult& read, const char* extension);
    ImageCandidateRecordBuilder(const ImageCandidateRecordBuilder&) = delete;
    ImageCandidateRecordBuilder& operator=(const ImageCandidateRecordBuilder&) = delete;
    ImageCandidateRecordBuilder(ImageCandidateRecordBuilder&&) = delete;
    ImageCandidateRecordBuilder& operator=(ImageCandidateRecordBuilder&&) = delete;
    std::string Finish(const PublishOutcome& publication) &&;
private:
    std::string record_;
    json::ObjectWriter writer_;
};

std::string EncodeImageCandidate(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
    const ImageCandidate& candidate, const CandidateReadResult& read, const char* extension,
    const PublishOutcome& publication);

std::string EncodeMediaAssetError(const Identity& identity, PublishStatus status);

enum class PipelineFailure : uint8_t {
    WriterException, InvalidInboundRoot, WriterEventFailed, WriterThreadFailed
};
std::string_view EncodeMediaPipelineError(PipelineFailure failure) noexcept;
std::string EncodeMediaDropped(uint64_t total);

// 测试兼容接口
std::string SaveImageAsset(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
    std::string_view bytes, const std::wstring& root, std::string& error);
std::string SaveEncodedImageAsset(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
    std::string_view bytes, const std::wstring& root, std::string& error);
std::string SaveVoiceAsset(const Snapshot& snapshot, const std::wstring& root, std::string& error);

} // namespace wechatbot::monitor::media_receive_detail
