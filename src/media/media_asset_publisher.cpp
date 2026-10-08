#include "monitor/media/media_asset_publisher.hpp"
#include "monitor/media/media_inspection.hpp"
#include "monitor/media/media_file.hpp"
#include "monitor/core/json_writer.hpp"
#include "monitor/config/version_profile.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <span>
#include <utility>

namespace wechatbot::monitor::media_receive_detail {
namespace profile = active_profile::media_receive;

namespace {

/**
 * @brief 分块将全部二进制数据写入 Win32 文件句柄
 */
bool WriteAll(HANDLE file, std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size(), size_t{1024 * 1024}));
        if (!WriteFile(file, bytes.data(), chunk, &written, nullptr) || !written) return false;
        bytes.remove_prefix(written);
    }
    return true;
}

/**
 * @brief 比对已存在的磁盘文件内容是否与预期二进制完全一致
 */
bool ExistingMatches(const std::wstring& path, std::string_view expected) {
    UniqueHandle file;
    file.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        CandidateFileLength(info) != expected.size()) return false;
    char buffer[8192];
    while (!expected.empty()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(expected.size(), sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer, chunk, &read, nullptr) || read != chunk ||
            memcmp(buffer, expected.data(), chunk) != 0) return false;
        expected.remove_prefix(chunk);
    }
    return true;
}

/**
 * @brief 暂存文件 (.part) 的 RAII 自动清理守卫
 * @details 只有在成功执行 MoveFileExW 原子移动之后调用 Dismiss() 释放所有权；
 *          若中途任何写入失败或异常退出，析构函数自动执行 DeleteFileW 清理残缺暂存文件。
 */
class StagingFile final {
public:
    explicit StagingFile(const std::filesystem::path& path) noexcept : path_(path) {}
    ~StagingFile() { if (owned_) DeleteFileW(path_.c_str()); }
    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    void Own() noexcept { owned_ = true; }
    void Dismiss() noexcept { owned_ = false; }
private:
    const std::filesystem::path& path_;
    bool owned_ = false;
};

/**
 * @brief 原子化落盘发布文件至目标资产目录
 * @details 
 *   采用标准的写暂存再原子移动（Write-to-temp-then-rename）模式：
 *   1. 确保目标目录存在；
 *   2. 创建临时文件：`<filename>.<PID>.<seq>.part`；
 *   3. 写入数据并通过 FlushFileBuffers 强制刷入物理磁盘扇区；
 *   4. 关闭文件句柄，调用 MoveFileExW(..., ..., MOVEFILE_WRITE_THROUGH) 原子更名；
 *   5. 若目标文件已存在，逐字节校验其内容：若内容一致视为发布成功，若不同则报 AssetNameConflict。
 */
PublishStatus Publish(const std::wstring& root, const std::string& name, std::string_view bytes,
                      uint64_t sequence) {
    const std::filesystem::path directory(root);
    if (!directory.is_absolute()) { return PublishStatus::InvalidInboundRoot; }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { return PublishStatus::InboundDirectoryFailed; }
    const auto destination = directory / name;
    const auto staging = directory / (name + "." + std::to_string(GetCurrentProcessId()) + "." +
                                       std::to_string(sequence) + ".part");
    StagingFile cleanup(staging);
    UniqueHandle file;
    file.reset(CreateFileW(staging.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) { return PublishStatus::StagingCreateFailed; }
    cleanup.Own();
    const bool written = WriteAll(file.get(), bytes) && FlushFileBuffers(file.get());
    file.reset();
    if (!written) { return PublishStatus::AssetWriteFailed; }
    if (MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        cleanup.Dismiss();
        return PublishStatus::Published;
    }
    const DWORD moveError = GetLastError();
    const bool exists = moveError == ERROR_ALREADY_EXISTS || moveError == ERROR_FILE_EXISTS;
    const bool same = exists && ExistingMatches(destination.wstring(), bytes);
    if (same) return PublishStatus::Published;
    return exists ? PublishStatus::AssetNameConflict : PublishStatus::AssetPublishFailed;
}

const char* PublishStatusText(PublishStatus status) {
    switch (status) {
    case PublishStatus::NotAttempted:
    case PublishStatus::Published: return "";
    case PublishStatus::InvalidInboundRoot: return "invalid_inbound_root";
    case PublishStatus::InboundDirectoryFailed: return "inbound_directory_failed";
    case PublishStatus::StagingCreateFailed: return "staging_create_failed";
    case PublishStatus::AssetWriteFailed: return "asset_write_failed";
    case PublishStatus::AssetNameConflict: return "asset_name_conflict";
    case PublishStatus::AssetPublishFailed: return "asset_publish_failed";
    case PublishStatus::InvalidImageIdentity: return "invalid_image_identity";
    case PublishStatus::WxgfHeaderOrBoundsInvalid: return "wxgf_header_or_bounds_invalid";
    case PublishStatus::ImageFormatOrDecodeInvalid: return "image_format_or_decode_invalid";
    case PublishStatus::Sha256Failed: return "sha256_failed";
    case PublishStatus::InvalidVoiceIdentity: return "invalid_voice_identity";
    case PublishStatus::VoiceFormatOrFramingInvalid: return "voice_format_or_framing_invalid";
    }
    return "";
}

const char* CandidateStatusText(CandidateStatus status) {
    switch (status) {
    case CandidateStatus::PathUnreadable: return "path_unreadable";
    case CandidateStatus::OpenFailed: return "open_failed";
    case CandidateStatus::NotRegularFile: return "not_regular_file";
    case CandidateStatus::SizeRejected: return "size_rejected";
    case CandidateStatus::Opened: return "opened";
    case CandidateStatus::FileChanged: return "file_changed";
    case CandidateStatus::SeekFailed: return "seek_failed";
    case CandidateStatus::ReadFailed: return "read_failed";
    case CandidateStatus::StableFileRead: return "stable_file_read";
    }
    return "path_unreadable";
}

void IdentityFields(json::ObjectWriter& writer, const Identity& value) {
    writer.Number("schema_version", 2);
    writer.Number("seq", value.sequence);
    writer.Number("observed_unix_ms", value.observedUnixMs);
    writer.Number("msg_type", value.type);
    writer.NumberString("message_id", value.messageId);
    if (*value.from) writer.String("from", value.from);
    if (*value.to) writer.String("to", value.to);
    if (*value.sender) writer.String("sender", value.sender);
}

std::string Utf8Path(const std::wstring& path) {
    if (path.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
                            result.data(), length, nullptr, nullptr)) return {};
    return result;
}
} // namespace

PublishOutcome PublishImageAsset(const Identity& identity, uint32_t resourceKind,
    size_t pathOffset, std::string_view bytes, const std::wstring& root, ImagePublication publication) {
    if (identity.type != 3 || !identity.messageId || resourceKind < 1 || resourceKind > 3 ||
        pathOffset != profile::kImagePathOffsets[0])
        return {{}, PublishStatus::InvalidImageIdentity};
    const bool encoded = publication == ImagePublication::Encoded;
    const auto image = encoded ? InspectWxgfHeader(bytes) : InspectImage(bytes);
    if (!image) return {{}, encoded ? PublishStatus::WxgfHeaderOrBoundsInvalid : PublishStatus::ImageFormatOrDecodeInvalid};
    const auto hash = MediaSha256(bytes);
    if (hash.size() != 64) return {{}, PublishStatus::Sha256Failed};
    const auto name = hash + image->extension;
    const auto status = Publish(root, name, bytes, identity.sequence);
    if (status != PublishStatus::Published) return {{}, status};
    return {PublishedAsset{identity, hash, name, bytes.size(), encoded, resourceKind, image->width, image->height},
            PublishStatus::Published};
}

PublishOutcome PublishVoiceAsset(const Snapshot& snapshot, const std::wstring& root) {
    if (snapshot.identity.type != 34 || !snapshot.identity.messageId || snapshot.bytes.empty())
        return {{}, PublishStatus::InvalidVoiceIdentity};
    const char* extension = VoiceExtension(snapshot.bytes);
    if (!extension) return {{}, PublishStatus::VoiceFormatOrFramingInvalid};
    const auto hash = MediaSha256(snapshot.bytes);
    if (hash.size() != 64) return {{}, PublishStatus::Sha256Failed};
    const auto name = hash + extension;
    const auto status = Publish(root, name, snapshot.bytes, snapshot.identity.sequence);
    if (status != PublishStatus::Published) return {{}, status};
    return {PublishedAsset{snapshot.identity, hash, name, snapshot.bytes.size()}, PublishStatus::Published};
}

std::string EncodePublishedAsset(const PublishedAsset& asset) {
    std::string record;
    json::ObjectWriter writer(record);
    writer.String("kind", asset.encoded ? "media_encoded_asset" : "media_asset");
    IdentityFields(writer, asset.identity);
    writer.String("status", asset.encoded ? "encoded" : "available");
    writer.String("sha256", asset.sha256);
    writer.Number("byte_length", asset.byteLength);
    writer.String("asset_name", asset.name);
    if (asset.identity.type == 3) {
        writer.String("image_variant", "unknown");
        writer.Number("resource_kind", asset.resourceKind);
        writer.Number("resource_path_offset", profile::kImagePathOffsets[0]);
        writer.Number("width", asset.width);
        writer.Number("height", asset.height);
    }
    writer.Close();
    return record;
}

ImageCandidateRecordBuilder::ImageCandidateRecordBuilder(const Identity& identity,
    uint32_t resourceKind, size_t pathOffset, const ImageCandidate& candidate,
    const CandidateReadResult& read, const char* extension) : writer_(record_) {
    writer_.String("kind", "media_image_candidate");
    IdentityFields(writer_, identity);
    writer_.String("status", "pending");
    writer_.Number("resource_kind", resourceKind);
    writer_.Number("resource_path_offset", pathOffset);
    writer_.String("capture_status", CandidateStatusText(read.status));
    writer_.Number("file_size", CandidateFileLength(candidate.initial));
    writer_.Number("win32_error", candidate.openError);
    writer_.Number("byte_length", read.bytes.size());
    if (extension && strcmp(extension, ".wxgf") != 0) writer_.String("plaintext_format", extension + 1);
    else writer_.Null("plaintext_format");
    if (extension) writer_.String("detected_format", extension + 1);
    else writer_.Null("detected_format");
    const auto sourcePath = Utf8Path(candidate.path);
    if (!sourcePath.empty()) writer_.String("source_path", sourcePath);
    if (read) {
        writer_.String("sha256", MediaSha256(read.bytes));
        writer_.HexBytes("prefix_hex", std::span{reinterpret_cast<const uint8_t*>(read.bytes.data()),
                                               (std::min)(read.bytes.size(), size_t{16})});
    }
}

std::string ImageCandidateRecordBuilder::Finish(const PublishOutcome& publication) && {
    writer_.Boolean("asset_published", static_cast<bool>(publication));
    const char* error = PublishStatusText(publication.status);
    if (*error) writer_.String("asset_error", error);
    writer_.Close();
    return std::move(record_);
}

std::string EncodeImageCandidate(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
    const ImageCandidate& candidate, const CandidateReadResult& read, const char* extension,
    const PublishOutcome& publication) {
    return ImageCandidateRecordBuilder(identity, resourceKind, pathOffset, candidate, read, extension).Finish(publication);
}

std::string EncodeMediaAssetError(const Identity& identity, PublishStatus status) {
    std::string record;
    json::ObjectWriter writer(record);
    writer.String("kind", "media_asset_error");
    IdentityFields(writer, identity);
    writer.String("status", "pending");
    writer.String("reason", PublishStatusText(status));
    writer.Close();
    return record;
}

std::string_view EncodeMediaPipelineError(PipelineFailure failure) noexcept {
    switch (failure) {
    case PipelineFailure::WriterException:
        return "{\"kind\":\"media_asset_error\",\"reason\":\"writer_exception\",\"status\":\"pending\"}";
    case PipelineFailure::InvalidInboundRoot:
        return "{\"kind\":\"media_asset_error\",\"reason\":\"invalid_inbound_root\"}";
    case PipelineFailure::WriterEventFailed:
        return "{\"kind\":\"media_asset_error\",\"reason\":\"writer_event_failed\"}";
    case PipelineFailure::WriterThreadFailed:
        return "{\"kind\":\"media_asset_error\",\"reason\":\"writer_thread_failed\"}";
    }
    return {};
}

std::string EncodeMediaDropped(uint64_t total) {
    std::string record;
    json::ObjectWriter writer(record);
    writer.String("kind", "media_asset_dropped");
    writer.Number("total", total);
    writer.Close();
    return record;
}

std::string SaveImageAsset(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
    std::string_view bytes, const std::wstring& root, std::string& error) {
    const auto result = PublishImageAsset(identity, resourceKind, pathOffset, bytes, root);
    error = PublishStatusText(result.status);
    return result ? EncodePublishedAsset(*result.asset) : std::string{};
}

std::string SaveEncodedImageAsset(const Identity& identity, uint32_t resourceKind, size_t pathOffset,
    std::string_view bytes, const std::wstring& root, std::string& error) {
    const auto result = PublishImageAsset(identity, resourceKind, pathOffset, bytes, root, ImagePublication::Encoded);
    error = PublishStatusText(result.status);
    return result ? EncodePublishedAsset(*result.asset) : std::string{};
}

std::string SaveVoiceAsset(const Snapshot& snapshot, const std::wstring& root, std::string& error) {
    const auto result = PublishVoiceAsset(snapshot, root);
    error = PublishStatusText(result.status);
    return result ? EncodePublishedAsset(*result.asset) : std::string{};
}

} // namespace wechatbot::monitor::media_receive_detail
