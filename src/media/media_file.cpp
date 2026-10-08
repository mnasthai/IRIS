#include "monitor/media/media_file.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace wechatbot::monitor {
namespace {

/**
 * @brief Windows CNG (Cryptography Next Generation) 算法提供者句柄释放器
 */
struct AlgorithmDeleter {
    void operator()(BCRYPT_ALG_HANDLE handle) const noexcept {
        BCryptCloseAlgorithmProvider(handle, 0);
    }
};

/**
 * @brief Windows CNG 哈希计算句柄释放器
 */
struct HashDeleter {
    void operator()(BCRYPT_HASH_HANDLE handle) const noexcept { BCryptDestroyHash(handle); }
};

using UniqueAlgorithm = std::unique_ptr<void, AlgorithmDeleter>;
using UniqueHash = std::unique_ptr<void, HashDeleter>;

/**
 * @brief 将 UTF-8 字符串转换为 Windows 宽字符 (UTF-16) std::wstring
 */
std::wstring Wide(std::string_view value) {
    if (value.empty() || value.size() > 4096) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                             result.data(), size)) return {};
    return result;
}

/**
 * @brief 通过打开的文件句柄解析其真实的规范化最终物理路径
 * @details 
 *   调用 Win32 GetFinalPathNameByHandleW(..., FILE_NAME_NORMALIZED)。
 *   此函数能自动穿透并解析任何符号链接 (Symlink)、NTFS 目录联接点 (Junction Point) 以及相对路径，
 *   获取文件在物理磁盘上的真实唯一样貌，是抵御目录穿越与软链接劫持攻击的黄金标准。
 */
std::wstring FinalPath(HANDLE handle) {
    const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!size || size > 32768) return {};
    std::wstring result(size, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(handle, result.data(), size, FILE_NAME_NORMALIZED);
    if (!count || count >= size) return {};
    result.resize(count);
    return result;
}

/**
 * @brief 忽略大小写的 Windows 路径序数比较
 */
bool EqualPath(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() && CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

/**
 * @brief 快速检查图片二进制数据的文件头尾魔数与基本结构
 * @details 
 *   - PNG: 开头为 8 字节签名 "\x89PNG\r\n\x1a\n"，偏移 12 处必须为 "IHDR" 块头，结尾必须为 12 字节 "IEND" 块；
 *   - JPEG: 开头为 3 字节 SOI 标记 "\xff\xd8\xff"，结尾为 2 字节 EOI 标记 "\xff\xd9"。
 */
bool ImageShape(std::string_view data, const std::wstring& extension) {
    if (extension == L".png") {
        constexpr std::string_view signature("\x89PNG\r\n\x1a\n", 8);
        constexpr std::string_view ending("\0\0\0\0IEND\xae\x42\x60\x82", 12);
        return data.size() >= 45 && data.starts_with(signature) && data.substr(12, 4) == "IHDR" && data.ends_with(ending);
    }
    return (extension == L".jpg" || extension == L".jpeg") && data.size() >= 4 &&
        data.starts_with(std::string_view("\xff\xd8\xff", 3)) && data.ends_with(std::string_view("\xff\xd9", 2));
}
} // namespace

/**
 * @brief 解析并计算微信 SILK 语音音频的时长（毫秒）
 * @details 
 *   微信的语音编码采用 Skype 开发的 SILK 格式：
 *   1. 识别并剥离微信私有的单字节 '\x02' 前缀；
 *   2. 验证 "#!SILK_V3" 文件头；
 *   3. 逐帧解析：每帧前 2 字节为该帧大小（小端 uint16_t）；
 *   4. 当帧长度为 0xffff 时表示结束，返回 frames * 20 毫秒；
 *   5. 严格防御畸变数据：单帧上限 32KB，总帧数上限 3000 帧（即 60 秒上限）。
 */
uint32_t SilkDurationMs(std::string_view bytes) {
    if (bytes.size() > 1024 * 1024) return 0;
    // 微信私有导出的 SILK 数据常带有前导 0x02 字节
    if (bytes.starts_with('\x02')) bytes.remove_prefix(1);
    constexpr std::string_view header = "#!SILK_V3";
    if (!bytes.starts_with(header)) return 0;
    bytes.remove_prefix(header.size());
    uint32_t frames = 0;
    while (!bytes.empty()) {
        if (bytes.size() < 2) return 0;
        const auto count = static_cast<uint16_t>(static_cast<uint8_t>(bytes[0]) |
                                                 (static_cast<uint8_t>(bytes[1]) << 8));
        bytes.remove_prefix(2);
        // 0xffff 为 SILK 语音流结束标志
        if (count == 0xffff) return bytes.empty() && frames ? frames * 20 : 0;
        if (count > 0x7fff || count > bytes.size() || ++frames > 3000) return 0;
        // 步进跳过当前帧载荷（静音 DTX 帧长度可为 0，但仍计为 1 个 20ms 帧）
        bytes.remove_prefix(count);
    }
    return frames * 20;
}

/**
 * @brief 使用 Windows BCrypt 原生 API 计算数据的 SHA-256
 */
std::string MediaSha256(std::string_view bytes) {
    if (bytes.size() > (std::numeric_limits<ULONG>::max)()) return {};
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    const UniqueAlgorithm algorithm(rawAlgorithm);
    DWORD size = 0, count = 0;
    if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                          sizeof(size), &count, 0) < 0) return {};
    std::vector<unsigned char> object(size);
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    const auto createStatus = BCryptCreateHash(algorithm.get(), &rawHash, object.data(), size, nullptr, 0, 0);
    const UniqueHash hash(rawHash);
    if (createStatus < 0) return {};
    if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                       static_cast<ULONG>(bytes.size()), 0) < 0) return {};
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (auto byte : digest) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}

/**
 * @brief 校验待发送多媒体文件并锁定只读句柄
 * @details 
 *   执行完整 8 步安全准入检验：
 *   1. 基础入参格式审查；
 *   2. 路径层级限制：检查目标文件父目录是否严格等于 root（杜绝任何 `../` 目录穿越）；
 *   3. 文件名一致性：主文件名（stem）必须与 SHA-256 字符串一致；
 *   4. 打开文件并核验文件属性：必须为普通文件，禁止目录或重解析点 (Reparse Point/Junction)；
 *   5. 真实物理路径一致性核验：通过 FinalPath 解析出句柄的物理路径，再次确认其位于 root 之下；
 *   6. 文件大小一致性核验；
 *   7. 分块读取数据并用 BCrypt 重新计算 SHA-256，核对一致性；
 *   8. 语音/图片格式专项核验：语音校验 SILK 帧头与时长，图片校验魔数头尾。
 */
MediaFileResult PrepareMediaFile(const MediaCommand& command, const std::wstring& root) {
    try {
        if (!IsValidMediaCommand(command)) return {{}, "invalid_media"};
        const auto path = Wide(command.path);
        const std::filesystem::path input(path), directory(root);
        if (path.empty() || !input.is_absolute() || !directory.is_absolute() ||
            !EqualPath(input.parent_path().wstring(), directory.lexically_normal().wstring()))
            return {{}, "media_path_not_allowed"};
        
        // 校验文件扩展名与哈希命名
        const auto extension = input.extension().wstring();
        if (input.stem().wstring() != Wide(command.sha256) ||
            (command.kind == "voice" ? extension != L".silk" :
                (extension != L".png" && extension != L".jpg" && extension != L".jpeg")))
            return {{}, "media_path_not_allowed"};
        
        // 打开根目录句柄用于真实物理路径比对
        UniqueHandle parent{CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        if (!parent) return {{}, "media_root_unavailable"};
        
        // 打开目标文件：以 GENERIC_READ 配合 FILE_SHARE_READ 打开，锁定文件写与删权限
        auto file = std::make_shared<PreparedMediaFile>();
        file->handle.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file->handle) return {{}, "media_file_unavailable"};
        
        // 拒绝重解析点（符号链接/Junction）与目录
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file->handle.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return {{}, "media_path_not_allowed"};
        
        // 规范化物理路径核查：确保实际物理文件父路径与根目录一致
        file->path = FinalPath(file->handle.get());
        const auto parentFinal = FinalPath(parent.get());
        if (!EqualPath(std::filesystem::path(file->path).parent_path().wstring(), parentFinal))
            return {{}, "media_path_not_allowed"};
        
        // 大小核验
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file->handle.get(), &size) || size.QuadPart <= 0 ||
            static_cast<uint64_t>(size.QuadPart) != command.bytes) return {{}, "media_size_mismatch"};
        
        // 分块读取内容至内存
        std::string data(static_cast<size_t>(size.QuadPart), '\0');
        size_t offset = 0;
        while (offset < data.size()) {
            DWORD read = 0;
            const auto chunk = static_cast<DWORD>((std::min)(data.size() - offset, size_t{1024 * 1024}));
            if (!ReadFile(file->handle.get(), data.data() + offset, chunk, &read, nullptr) || !read)
                return {{}, "media_read_failed"};
            offset += read;
        }
        
        // SHA-256 完整性哈希比对
        if (MediaSha256(data) != command.sha256) return {{}, "media_hash_mismatch"};
        
        // 格式专项校验
        if (command.kind == "voice") {
            file->durationMs = SilkDurationMs(data);
            if (!file->durationMs || file->durationMs != command.durationMs) return {{}, "media_format_invalid"};
            file->audio = std::move(data);
        } else if (!ImageShape(data, extension)) return {{}, "media_format_invalid"};
        
        // 保留原有的规范路径拼写供微信底层使用
        file->path = path;
        return {std::move(file), {}};
    } catch (...) { return {{}, "media_preparation_failed"}; }
}

} // namespace wechatbot::monitor
