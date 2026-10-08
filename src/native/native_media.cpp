#include "monitor/native/native_media.hpp"
#include "monitor/config/version_profile.hpp"
#include <cstring>
#include <string>
#include <utility>

namespace wechatbot::monitor {
namespace {
namespace profile = active_profile::text;

constexpr size_t kPathOffset = 0x120;           ///< 图片原生对象内部宽字符路径成员的偏移量
constexpr uint64_t kImageTypeAndSubtype = 3;    ///< 微信图片消息类型码
constexpr uint64_t kVoiceTypeAndSubtype = 34;   ///< 微信语音消息类型码

/**
 * @brief 微信原生字符串 (NativeString) 输入结构体
 * @details 微信底层标准字符串采用类似 std::string 的 SSO (Small String Optimization) 内存布局：
 *          - 大小为 0x20 字节，8 字节对齐；
 *          - +0x00 ~ +0x0F: 短字符串内联存储缓冲区 (长度 < 16 字节 / 宽字符 < 8 字符)；
 *            若超出短字符串阈值，此处存放指向堆上动态分配缓冲区的真实指针；
 *          - +0x10: 字符串实际长度 (字节数或宽字符数)；
 *          - +0x18: 字符串缓冲区容量 (capacity)。
 */
struct alignas(8) NativeStringInput {
    unsigned char bytes[0x20]{};
};

/**
 * @brief 微信原生语音发信所需的元数据结构体
 * @details 大小为 0x38 字节，包含语音发信参数：
 *          +0x00: uint32_t = 4
 *          +0x04: uint32_t = 3
 *          +0x08: uint64_t = 30000
 *          +0x10: uint8_t = 0
 *          +0x18 ~ +0x37: 内嵌一个空的 NativeString 结构体 (+0x30 处 capacity = 15)
 */
struct alignas(8) NativeVoiceMetadata {
    unsigned char bytes[0x38]{};
};

template<class T>
void Store(void* object, size_t offset, const T& value) noexcept {
    memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}

/**
 * @brief 构造 UTF-8 窄字符 NativeString 结构
 */
NativeStringInput NarrowInput(const std::string& storage) noexcept {
    NativeStringInput input{};
    const uint64_t length = storage.size();
    const uint64_t capacity = length < 16 ? 15 : length;
    if (length < 16) memcpy(input.bytes, storage.data(), static_cast<size_t>(length));
    else {
        const char* pointer = storage.data();
        Store(input.bytes, 0, pointer);
    }
    Store(input.bytes, 0x10, length);
    Store(input.bytes, 0x18, capacity);
    return input;
}

/**
 * @brief 构造 UTF-16 宽字符 NativeString 结构
 */
NativeStringInput WideInput(std::wstring_view value) noexcept {
    static_assert(sizeof(wchar_t) == 2);
    NativeStringInput input{};
    const uint64_t length = value.size();
    const uint64_t capacity = length < 8 ? 7 : length;
    if (length < 8) memcpy(input.bytes, value.data(), static_cast<size_t>(length) * sizeof(wchar_t));
    else {
        const wchar_t* pointer = value.data();
        Store(input.bytes, 0, pointer);
    }
    Store(input.bytes, 0x10, length);
    Store(input.bytes, 0x18, capacity);
    return input;
}

/**
 * @brief 填充原生语音发信所需的元数据
 */
NativeVoiceMetadata VoiceMetadata() noexcept {
    NativeVoiceMetadata metadata{};
    Store(metadata.bytes, 0, uint32_t{4});
    Store(metadata.bytes, 4, uint32_t{3});
    Store(metadata.bytes, 8, uint64_t{30000});
    Store(metadata.bytes, 0x10, uint8_t{0});
    Store(metadata.bytes, 0x30, uint64_t{15}); // 对应 +0x18 处空 NativeString 的 capacity
    return metadata;
}

bool ValidImageRequest(const NativeMediaRequest& request) noexcept {
    if (request.imagePath.empty() || request.imagePath.size() > kNativeImagePathMaxUnits ||
        !request.audioBytes.empty() || request.durationMs != 0) return false;
    for (wchar_t value : request.imagePath) if (value == L'\0') return false;
    return true;
}

bool ValidVoiceRequest(const NativeMediaRequest& request) noexcept {
    return request.imagePath.empty() && !request.audioBytes.empty() &&
           request.audioBytes.size() <= kNativeVoiceMaxBytes && request.durationMs > 0 &&
           request.durationMs <= kNativeVoiceMaxDurationMs;
}
} // namespace

NativeAttemptResult SubmitNativeMediaOnce(const NativeMediaApi& api,
                                       const NativeMediaRequest& request) noexcept {
    NativeAttemptResult result{};
    
    // 1. 请求合法性审查
    if ((request.kind != NativeMediaKind::image && request.kind != NativeMediaKind::voice) ||
        !native_detail::IsValidNativeUtf8NoNul(request.targetUtf8) ||
        (request.kind == NativeMediaKind::image ? !ValidImageRequest(request)
                                                : !ValidVoiceRequest(request))) return result;
    
    // 2. 线程亲和性校验：必须在绑定的微信 Qt UI 线程执行
    if (!request.authorizedUiThreadId || GetCurrentThreadId() != request.authorizedUiThreadId) {
        result.status = NativeAttemptStatus::ui_thread_required;
        return result;
    }
    
    // 3. 原生接口函数指针完备性校验
    if (!native_detail::CompleteNativeCommonApi(api.common) ||
        (request.kind == NativeMediaKind::image
             ? (!api.imageFactory || !api.wideAssign)
             : !api.voiceFactory)) {
        result.status = NativeAttemptStatus::api_unavailable;
        return result;
    }

    NativeSharedPair source{};
    native_detail::NativeCallPhase factoryPhase = native_detail::NativeCallPhase::NotEntered;
    bool prepared = false;
    result.status = NativeAttemptStatus::preparation_failed;
    
    try {
        result.stage = NativeSubmitStage::media_factory;
        
        if (request.kind == NativeMediaKind::image) {
            // 4.1 图片分支构造：
            // 调用原生 imageFactory 工厂构造图片消息控制块与原生对象
            factoryPhase = native_detail::NativeCallPhase::EnteredWithoutReturn;
            NativeSharedPair* returned = api.imageFactory(&source);
            factoryPhase = native_detail::NativeCallPhase::Returned;
            if (returned != &source || !source.raw || !source.control) throw false;

            // 提取微信分配的 Local UUID
            result.stage = NativeSubmitStage::local_uuid;
            if (!native_detail::ReadNativeSourceUuid(source, result.localUuid)) throw false;
            
            // 写入接收目标 wxid
            result.stage = NativeSubmitStage::target_assign;
            auto* target = static_cast<unsigned char*>(source.raw) + profile::kTargetOffset;
            if (api.common.assign(target, request.targetUtf8.data(), request.targetUtf8.size()) != target)
                throw false;
            
            // 写入宽字符图片物理文件路径至偏移 +0x120 处
            result.stage = NativeSubmitStage::media_prepare;
            auto path = WideInput(request.imagePath);
            auto* destination = static_cast<unsigned char*>(source.raw) + kPathOffset;
            if (api.wideAssign(destination, &path) != destination) throw false;
            
            // 设置消息类型码为 3 (图片)
            Store(source.raw, profile::kTypeOffset, kImageTypeAndSubtype);
        } else {
            // 4.2 语音分支构造：
            // 将目标 wxid 与音频二进制数据包装为具备尾零保护的 NativeString 结构
            std::string targetStorage(request.targetUtf8);
            std::string audioStorage(request.audioBytes);
            auto target = NarrowInput(targetStorage);
            auto audio = NarrowInput(audioStorage);
            auto metadata = VoiceMetadata();
            const uint32_t duration = request.durationMs;
            
            // 调用原生 voiceFactory 工厂直接完成语音对象构造与数据注入
            factoryPhase = native_detail::NativeCallPhase::EnteredWithoutReturn;
            NativeSharedPair* returned = api.voiceFactory(
                &source, &target, &audio, &duration, &metadata);
            factoryPhase = native_detail::NativeCallPhase::Returned;
            if (returned != &source || !source.raw || !source.control) throw false;

            // 提取 Local UUID
            result.stage = NativeSubmitStage::local_uuid;
            if (!native_detail::ReadNativeSourceUuid(source, result.localUuid)) throw false;
            
            // 设置消息类型码为 34 (语音)
            result.stage = NativeSubmitStage::media_prepare;
            Store(source.raw, profile::kTypeOffset, kVoiceTypeAndSubtype);
        }
        prepared = true;
    } catch (bool) {
        // 构造返回验证失败
    } catch (...) {
        // 原生调用发生未捕获异常
    }

    // 5. 若前期构造成功，转交 SubmitPreparedNativeSourceOnce 组装 Vector/Delayed/Metadata 并触发 start()
    if (prepared) {
        return native_detail::SubmitPreparedNativeSourceOnce(
            api.common, source, std::move(result.localUuid), request.beforeStart);
    }
    
    // 6. 构造失败安全清理
    bool cleanup = factoryPhase != native_detail::NativeCallPhase::EnteredWithoutReturn;
    if (factoryPhase == native_detail::NativeCallPhase::Returned &&
        !native_detail::ReleaseNativeSource(source)) cleanup = false;
    result.cleanupComplete = cleanup;
    return result;
}

} // namespace wechatbot::monitor
