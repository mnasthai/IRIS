#include "monitor/media/media_receive_snapshot.hpp"
#include "monitor/diagnostics/event.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/config/version_profile.hpp"
#include <cstring>
#include <utility>

namespace wechatbot::monitor::media_receive_detail {
namespace profile = active_profile::media_receive;

namespace {
namespace reference = active_profile::reference_message;

/**
 * @brief 带 SEH (Structured Exception Handling) 结构化异常保护的内存安全拷贝
 */
bool CopyMemoryChecked(const void* source, void* destination, size_t size) {
    __try {
        if (!IsReadableRange(source, size)) return false;
        memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template<class T> bool Read(const void* object, size_t offset, T& value) {
    if (!object || reinterpret_cast<uintptr_t>(object) > UINTPTR_MAX - offset) return false;
    return CopyMemoryChecked(static_cast<const uint8_t*>(object) + offset, &value, sizeof(value));
}

uint64_t ObservedUnixMs() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    return ((uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime) / 10000 - 11644473600000ULL;
}

/**
 * @brief 从原始消息对象中提取 Identity 身份元数据
 * @details 严格核验虚表指针 (vtable) 必须匹配预期的 kMessageVtableRva，提取 NewMsgId、会话和发送者 wxid
 */
bool IdentityFromPair(uintptr_t base, const void* pair, uint32_t type, Identity& out) {
    const void* pointers[2]{};
    alignas(8) uint8_t message[reference::kObjectSize]{};
    if (!base || !CopyMemoryChecked(pair, pointers, sizeof(pointers)) ||
        !CopyMemoryChecked(pointers[0], message, sizeof(message))) return false;
    uintptr_t vtable = 0; uint32_t actualType = 0, subtype = 0;
    memcpy(&vtable, message, sizeof(vtable));
    memcpy(&actualType, message + reference::kTypeOffset, sizeof(actualType));
    memcpy(&subtype, message + reference::kSubtypeOffset, sizeof(subtype));
    memcpy(&out.messageId, message + reference::kServerIdOffset, sizeof(out.messageId));
    if (vtable != base + profile::kMessageVtableRva || actualType != type || subtype != 0 || !out.messageId) return false;
    out.type = actualType;
    const auto from = CopyNativeString(message + reference::kFromOffset, out.from, sizeof(out.from));
    const auto to = CopyNativeString(message + reference::kToOffset, out.to, sizeof(out.to));
    const auto sender = CopyNativeString(message + reference::kSenderOffset, out.sender, sizeof(out.sender));
    if (from.status != ReadStatus::Ok) out.from[0] = 0;
    if (to.status != ReadStatus::Ok) out.to[0] = 0;
    if (sender.status != ReadStatus::Ok) out.sender[0] = 0;
    out.sequence = NextSequence();
    out.observedUnixMs = ObservedUnixMs();
    return true;
}

/**
 * @brief 解析微信原生字符串中的磁盘物理路径
 * @details 
 *   安全防护：
 *   - 限制路径长度在 4096 字符内，且不可包含 '\0'；
 *   - 必须是本地绝对盘符路径（如 "C:\..." 或 "\\?\C:\..."）；
 *   - 严格禁止 UNC 网络共享路径（如 "\\server\share"）或设备路径，杜绝网络请求阻塞；
 *   - 禁止包含通配符 '*' 或 '?'。
 */
bool NativePath(const void* object, std::wstring& out) {
    alignas(8) uint8_t native[0x20]{};
    if (!CopyMemoryChecked(object, native, sizeof(native))) return false;
    uint64_t length = 0, capacity = 0;
    memcpy(&length, native + 0x10, 8); memcpy(&capacity, native + 0x18, 8);
    if (!length || length > 4096 || capacity < length || capacity > 0x4000000 ||
        (capacity < 8 && length > 7)) return false;
    const void* text = native;
    if (capacity >= 8) memcpy(&text, native, sizeof(text));
    out.resize(static_cast<size_t>(length));
    if (!CopyMemoryChecked(text, out.data(), out.size() * sizeof(wchar_t)) ||
        out.find(L'\0') != std::wstring::npos) { out.clear(); return false; }
    
    // 盘符格式审查
    const size_t start = out.starts_with(L"\\\\?\\") ? 4 : 0;
    if (out.size() < start + 3 ||
        !((out[start] >= L'A' && out[start] <= L'Z') || (out[start] >= L'a' && out[start] <= L'z')) ||
        out[start + 1] != L':' || out[start + 2] != L'\\' ||
        out.find(L':', start + 2) != std::wstring::npos ||
        out.find_first_of(L"*?", start) != std::wstring::npos) { out.clear(); return false; }
    return true;
}
} // namespace

PendingPermit::~PendingPermit() { Release(); }
PendingPermit::PendingPermit(PendingPermit&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
PendingPermit& PendingPermit::operator=(PendingPermit&& other) noexcept {
    if (this != &other) { Release(); state_ = std::exchange(other.state_, nullptr); }
    return *this;
}
void PendingPermit::Release() noexcept {
    if (state_) { state_->fetch_sub(1, std::memory_order_acq_rel); state_ = nullptr; }
}

/**
 * @brief 在语音下载完成拦截点捕获语音数据
 * @details 
 *   1. 校验调用方返回地址 caller 是否严格匹配预期微信 RVA；
 *   2. 校验 VoiceHandler 虚表指针；
 *   3. 提取消息身份元数据；
 *   4. 从 bufferPair 中读取音频长度与内存地址，安全拷贝至 out.bytes。
 */
bool CaptureVoice(uintptr_t base, uintptr_t caller, const void* handler,
                  const void* messagePair, const void* bufferPair, Snapshot& out) {
    if (!base || caller != base + profile::kVoiceDownloadedReturnRva) return false;
    uintptr_t vtable = 0;
    if (!Read(handler, 0, vtable) || vtable != base + profile::kVoiceHandlerVtableRva ||
        !IdentityFromPair(base, messagePair, 34, out.identity)) return false;
    const void* pair[2]{};
    struct Buffer { const void* bytes; uint64_t cursorAndPadding; uint64_t length; uint64_t capacity; } buffer{};
    if (!CopyMemoryChecked(bufferPair, pair, sizeof(pair)) ||
        !CopyMemoryChecked(pair[0], &buffer, sizeof(buffer)) || !buffer.length ||
        buffer.length > kMaxVoiceBytes || buffer.capacity < buffer.length) return false;
    out.bytes.resize(static_cast<size_t>(buffer.length));
    if (!CopyMemoryChecked(buffer.bytes, out.bytes.data(), out.bytes.size())) { out.bytes.clear(); return false; }
    return true;
}

/**
 * @brief 在图片就绪拦截点捕获图片候选文件
 * @details 
 *   1. 校验调用方返回地址与 ImageHandler 虚表；
 *   2. 检查资源下载状态 state == 2 (下载完成就绪)；
 *   3. 提取消息身份；
 *   4. 从 resource 对象提取两个候选路径（偏移 +0x68 原图下载与 +0x88 缓存封装），
 *      并当场调用 OpenImageCandidate 打开并锁定其 Win32 文件句柄！
 */
bool CaptureImage(uintptr_t base, uintptr_t caller, const void* handler,
                  const void* messagePair, const void* resource, const void* result, Snapshot& out) {
    if (!base || (caller != base + profile::kImageCompletedReturnRvas[0] &&
                  caller != base + profile::kImageCompletedReturnRvas[1])) return false;
    uintptr_t vtable = 0; uint32_t kind = 0, resultKind = 0, source = 0, state = 0, destinationKind = 0;
    if (!Read(handler, 0, vtable) || vtable != base + profile::kImageHandlerVtableRva ||
        !Read(resource, profile::kResourceKindOffset, kind) || kind < 1 || kind > 3 ||
        !Read(result, profile::kResultKindOffset, resultKind) || resultKind != kind ||
        !Read(result, profile::kResultSourceOffset, source) || source != 2 ||
        !Read(result, profile::kDestinationKindOffset, destinationKind) || destinationKind < 1 || destinationKind > 2 ||
        !Read(result, profile::kCompletionStateOffset, state) || state != 2 ||
        !IdentityFromPair(base, messagePair, 3, out.identity)) return false;
    out.resourceKind = kind;
    for (size_t i = 0; i < 2; ++i) {
        std::wstring path;
        if (NativePath(static_cast<const uint8_t*>(resource) + profile::kImagePathOffsets[i], path))
            OpenImageCandidate(path, out.image[i]);
    }
    return true;
}

} // namespace wechatbot::monitor::media_receive_detail
