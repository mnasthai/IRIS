#include "monitor/native/native_text.hpp"
#include "monitor/send/recipient.hpp"
#include "monitor/config/version_profile.hpp"
#include "monitor/core/memory.hpp"
#include <cstring>
#include <limits>
#include <utility>

namespace wechatbot::monitor {
namespace native_detail {

/**
 * @brief 严格校验字符串是否为不含空字符 ('\0') 的有效 UTF-8 编码
 * @details 
 *   微信底层 C++ 字符串与网络协议对文本编码有严格要求。本函数按照 RFC 3629 / Unicode 规范，
 *   对 1 至 4 字节的 UTF-8 序列进行逐字节边界、超长编码 (Overlong Encoding) 及非法代理对 (Surrogate) 校验：
 *   - 禁止包含 '\0'（防止 C 风格字符串截断）；
 *   - 限制最大长度不超过 kNativeTextMaxBytes (16KB)；
 *   - 严格拦截 0xED 9F 之后的 Unicode UTF-16 代理项保留区。
 */
bool IsValidNativeUtf8NoNul(std::string_view value) noexcept {
    if (value.empty() || value.size() > kNativeTextMaxBytes ||
        value.size() > static_cast<size_t>((std::numeric_limits<int64_t>::max)())) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char first = bytes[i++];
        if (first == 0) return false;
        if (first < 0x80) continue; // 单字节 ASCII
        if (first >= 0xC2 && first <= 0xDF) { // 双字节序列
            if (i >= value.size() || (bytes[i++] & 0xC0) != 0x80) return false;
            continue;
        }
        if (first >= 0xE0 && first <= 0xEF) { // 三字节序列（涵盖常用汉字）
            if (i + 1 >= value.size()) return false;
            const unsigned char second = bytes[i++], third = bytes[i++];
            if ((third & 0xC0) != 0x80 ||
                (first == 0xE0 ? second < 0xA0 || second > 0xBF :
                 first == 0xED ? second < 0x80 || second > 0x9F :
                                 (second & 0xC0) != 0x80)) return false;
            continue;
        }
        if (first >= 0xF0 && first <= 0xF4) { // 四字节序列（Emoji 表情等）
            if (i + 2 >= value.size()) return false;
            const unsigned char second = bytes[i++], third = bytes[i++], fourth = bytes[i++];
            if ((third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80 ||
                (first == 0xF0 ? second < 0x90 || second > 0xBF :
                 first == 0xF4 ? second < 0x80 || second > 0x8F :
                                 (second & 0xC0) != 0x80)) return false;
            continue;
        }
        return false;
    }
    return true;
}

/**
 * @brief 校验基础文本发送所需的原生 API 函数指针是否完整解析
 */
bool CompleteNativeCommonApi(const NativeTextApi& api) noexcept {
    return api.assign && api.allocate && api.free && api.makeDelayed &&
           api.initMetadata && api.start && api.metadataStaticText1 && api.metadataStaticText2;
}
} // namespace native_detail

namespace {
namespace profile = active_profile::text;
namespace reference = active_profile::reference_message;
constexpr size_t kLocalUuidMaxBytes = 128;

using ControlDestroyFn = void (__fastcall*)(void* control);
using PayloadDestroyFn = void (__fastcall*)(void* payload, bool releaseStorage);

/// 模板辅助函数：从指定对象偏移处按字节拷贝读取标量或结构体
template<class T, class Object>
T LoadAt(const Object& object, size_t offset) noexcept {
    T value{};
    memcpy(&value, reinterpret_cast<const unsigned char*>(&object) + offset, sizeof(value));
    return value;
}

/// 模板辅助函数：将标量或结构体按字节写入目标对象指定偏移处
template<class T, class Object>
void StoreAt(Object& object, size_t offset, const T& value) noexcept {
    memcpy(reinterpret_cast<unsigned char*>(&object) + offset, &value, sizeof(value));
}

/// 引用消息组装结果状态跟踪
struct QuotePreparationOutcome {
    bool ready = false;               ///< 引用卡片是否组装成功
    bool sourceSafeToRelease = true;  ///< 外层 Source 对象是否可以安全执行强引用释放
    bool cleanupComplete = true;      ///< 临时引用对象析构是否完整（无泄漏与悬挂）
};

/// 临时原生消息对象的构造生命周期状态机
enum class TemporaryMessageState {
    NotConstructed,   ///< 尚未构造或初始化失败，无需析构
    Destroyable,      ///< 已成功构造，必须调用原生析构函数
    OwnershipUnknown, ///< 原生函数调用中途抛出异常，所有权与内存状态不明（不可冒然析构）
    DestroyEntered    ///< 已进入析构函数，严禁重复析构
};

/**
 * @brief 单次安全析构临时消息对象
 * @details 严格防止重复析构 (Double Free)。若析构中抛出异常，将状态标记为 DestroyEntered 并标记 cleanupComplete = false。
 */
bool DestroyOnce(void* message, NativeQuoteMessageDestroyFn destroy,
                 TemporaryMessageState& state, QuotePreparationOutcome& outcome) noexcept {
    if (state != TemporaryMessageState::Destroyable) return true;
    state = TemporaryMessageState::DestroyEntered;
    try {
        destroy(message);
        return true;
    } catch (...) {
        // 进入析构函数后即使内部抛出异常，也绝对不能重试，避免破坏堆状态
        outcome.cleanupComplete = false;
        return false;
    }
}

/**
 * @brief 赋值原生引用消息结构中的字符串字段
 */
bool AssignReferenceString(const NativeTextApi& api, void* destination,
                           std::string_view value, TemporaryMessageState& state,
                           QuotePreparationOutcome& outcome) noexcept {
    try {
        if (api.assign(destination, value.data(), value.size()) == destination) return true;
    } catch (...) {
        // 捕获异常并标记状态未知
    }
    state = TemporaryMessageState::OwnershipUnknown;
    outcome.cleanupComplete = false;
    return false;
}

/**
 * @brief 校验已构造的原生引用消息内存镜像与预期 QuoteText 是否完全吻合
 */
bool MatchesReferenceMessage(const unsigned char* message, const QuoteText& quote) {
    NativeSharedPair backing{};
    memcpy(&backing, message + profile::kReferenceBackingPairOffset, sizeof(backing));
    if (!backing.raw || !backing.control) return false;
    char captured[kNativeTextMaxBytes + 1]{};
    const auto matchesString = [&](size_t offset, std::string_view expected) {
        if (expected.size() > kNativeTextMaxBytes) return false;
        const auto read = CopyNativeString(message + offset, captured, sizeof(captured));
        return read.status == (expected.empty() ? ReadStatus::Empty : ReadStatus::Ok) &&
               read.lengthKnown && read.originalBytes == expected.size() &&
               read.capturedBytes == expected.size() &&
               std::string_view(captured, read.capturedBytes) == expected;
    };
    uint64_t type = 0, serverId = 0, milliseconds = 0;
    uint32_t timestamp = 0;
    memcpy(&type, message + profile::kReferenceTypeOffset, sizeof(type));
    memcpy(&serverId, message + profile::kReferenceServerIdOffset, sizeof(serverId));
    memcpy(&milliseconds, message + profile::kReferenceMillisecondsOffset, sizeof(milliseconds));
    memcpy(&timestamp, message + profile::kReferenceTimestampOffset, sizeof(timestamp));
    return type == profile::kTypeAndSubtype && serverId == quote.messageId &&
           milliseconds == static_cast<uint64_t>(quote.timestamp) * 1000 &&
           timestamp == quote.timestamp &&
           matchesString(profile::kReferenceFromOffset, quote.fromId) &&
           matchesString(profile::kReferenceToOffset, quote.toId) &&
           matchesString(profile::kReferenceSenderOffset, quote.senderId) &&
           matchesString(profile::kReferenceConversationOffset, quote.conversationId) &&
           matchesString(profile::kReferenceTextOffset, quote.text) &&
           matchesString(profile::kReferenceMsgSourceOffset, quote.msgSource);
}

/**
 * @brief 构造并挂载引用回复 (Quote) 的原生结构
 * @details 
 *   微信的引用回复底层实现非常精细：
 *   1. 首先在栈上开辟并初始化一个原始接收消息对象 W (original)；
 *   2. 写入原始消息的 senderId、toId、fromId、rawContent、msgSource 以及 serverId/timestamp；
 *   3. 调用微信底层 quoteFromMessage(original, quoted) 将其转换提炼为引用结构体 Q (quoted)；
 *   4. 校验 Q 的内存镜像与预期完全一致后，析构 W；
 *   5. 调用 quoteAttach 将 Q 附加到当前待发送消息 source 的引用数据槽中；
 *   6. 析构临时 Q 对象；
 *   7. 写入微信协议要求的空 MD5 摘要占位符 ("d41d8cd98f00b204e9800998ecf8427e") 及 Quote 消息类型码。
 */
QuotePreparationOutcome PrepareQuoteSource(const NativeTextApi& api, void* source,
                                           const QuoteText& quote) noexcept {
    alignas(8) unsigned char original[reference::kObjectSize]{};
    alignas(8) unsigned char quoted[profile::kReferenceObjectSize]{};
    TemporaryMessageState originalState = TemporaryMessageState::NotConstructed;
    TemporaryMessageState quotedState = TemporaryMessageState::NotConstructed;
    QuotePreparationOutcome outcome{};
    do {
        try {
            // 群聊引用时需拼接 "wxid:\n文本"，私聊直接使用原文本
            std::string rawContent;
            if (quote.fromId == quote.conversationId && IsGroupTarget(quote.conversationId)) {
                rawContent.reserve(quote.senderId.size() + 2 + quote.text.size());
                rawContent.assign(quote.senderId).append(":\n").append(quote.text);
            } else rawContent = quote.text;

            // 1. 初始化原始消息对象 W
            try {
                if (api.referenceMessageInit(original) != original) {
                    originalState = TemporaryMessageState::OwnershipUnknown;
                    outcome.cleanupComplete = false;
                    break;
                }
            } catch (...) {
                originalState = TemporaryMessageState::OwnershipUnknown;
                outcome.cleanupComplete = false;
                break;
            }
            originalState = TemporaryMessageState::Destroyable;
            
            // 2. 写入 W 结构的各字段字符串
            if (!AssignReferenceString(api, original + reference::kFromOffset, quote.fromId,
                                       originalState, outcome) ||
                !AssignReferenceString(api, original + reference::kToOffset, quote.toId,
                                       originalState, outcome) ||
                !AssignReferenceString(api, original + reference::kSenderOffset, quote.senderId,
                                       originalState, outcome) ||
                !AssignReferenceString(api, original + reference::kTextOffset, rawContent,
                                       originalState, outcome) ||
                !AssignReferenceString(api, original + reference::kMsgSourceOffset, quote.msgSource,
                                       originalState, outcome)) break;
            StoreAt(original, reference::kTypeOffset, uint32_t{1});
            StoreAt(original, reference::kSubtypeOffset, uint32_t{0});
            StoreAt(original, reference::kServerIdOffset, quote.messageId);
            StoreAt(original, reference::kMillisecondsOffset,
                    static_cast<uint64_t>(quote.timestamp) * 1000);
            StoreAt(original, reference::kTimestampOffset, quote.timestamp);

            // 3. 转换生成引用结构 Q
            try {
                if (api.quoteFromMessage(original, quoted) != quoted) {
                    quotedState = TemporaryMessageState::OwnershipUnknown;
                    outcome.cleanupComplete = false;
                    break;
                }
            } catch (...) {
                quotedState = TemporaryMessageState::OwnershipUnknown;
                outcome.cleanupComplete = false;
                break;
            }
            quotedState = TemporaryMessageState::Destroyable;
            if (!MatchesReferenceMessage(quoted, quote)) break;
            
            // 4. 析构已无用的原始对象 W
            if (!DestroyOnce(original, api.referenceMessageDestroy,
                             originalState, outcome)) break;
            
            // 5. 将 Q 挂载到 source 上
            try {
                api.quoteAttach(static_cast<unsigned char*>(source) +
                                profile::kReferenceDataOffset, quoted);
            } catch (...) {
                outcome.sourceSafeToRelease = false;
                outcome.cleanupComplete = false;
                break;
            }
            const auto* sourceBytes = static_cast<const unsigned char*>(source);
            if (sourceBytes[profile::kReferenceMessagePresentOffset] != 1 ||
                !MatchesReferenceMessage(sourceBytes + profile::kReferenceMessageOffset, quote)) break;
            
            // 6. 析构临时 Q 对象
            if (!DestroyOnce(quoted, api.quoteDestroy,
                             quotedState, outcome)) break;

            // 7. 写入部分摘要 MD5 占位符与消息类型
            constexpr std::string_view emptyPartialDigest = "d41d8cd98f00b204e9800998ecf8427e";
            auto* digest = static_cast<unsigned char*>(source) + profile::kReferencePartialDigestOffset;
            try {
                if (api.assign(digest, emptyPartialDigest.data(), emptyPartialDigest.size()) != digest) {
                    outcome.sourceSafeToRelease = false;
                    outcome.cleanupComplete = false;
                    break;
                }
            } catch (...) {
                outcome.sourceSafeToRelease = false;
                outcome.cleanupComplete = false;
                break;
            }
            memcpy(static_cast<unsigned char*>(source) + profile::kTypeOffset,
                   &profile::kQuoteTypeAndSubtype, sizeof(profile::kQuoteTypeAndSubtype));
            outcome.ready = true;
        } catch (...) {
            outcome.ready = false;
        }
    } while (false);
    
    // 退出作用域兜底清理
    if (!DestroyOnce(quoted, api.quoteDestroy, quotedState, outcome)) outcome.ready = false;
    if (!DestroyOnce(original, api.referenceMessageDestroy, originalState, outcome)) outcome.ready = false;
    return outcome;
}

/**
 * @brief 释放微信原生控制块的弱引用 (Weak Reference)
 * @details 微信原生对象内部采用类似 std::shared_ptr 的强弱双引用计数模型：
 *          - control + 0x08: 强引用计数 (Strong RefCount)
 *          - control + 0x0C: 弱引用计数 (Weak RefCount)
 *          - control vtable[0]: 托管对象析构函数 (Deleter)
 *          - control vtable[1]: 控制块自身释放函数 (Control Block Deallocator)
 *          当弱引用计数归零时，触发 vtable[1] 回收控制块内存。
 */
bool ReleaseWeak(void* control) noexcept {
    if (!control) return true;
    try {
        auto* weak = reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(control) + 0xC);
        if (InterlockedDecrement(weak) != 0) return true;
        auto** vtable = *static_cast<void***>(control);
        if (!vtable || !vtable[1]) return false;
        reinterpret_cast<ControlDestroyFn>(vtable[1])(control);
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * @brief 释放微信原生控制块的强引用 (Strong Reference)
 * @details 当强引用计数递减为 0 时：
 *          1. 调用虚表第 0 项析构原生对象实体；
 *          2. 递减关联的弱引用计数（强引用持有隐式弱引用）。
 */
bool ReleaseStrong(NativeSharedPair& pair) noexcept {
    void* control = pair.control;
    pair = {};
    if (!control) return true;
    try {
        auto* strong = reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(control) + 8);
        if (InterlockedDecrement(strong) != 0) return true;
        auto** vtable = *static_cast<void***>(control);
        if (!vtable || !vtable[0]) return false;
        reinterpret_cast<ControlDestroyFn>(vtable[0])(control);
        return ReleaseWeak(control);
    } catch (...) {
        return false;
    }
}

// 逆序清理步骤 1: 销毁 Startup 句柄并释放所持有的强引用
bool DestroyStartup(NativeStartupHandle& startup) noexcept {
    NativeSharedPair second = LoadAt<NativeSharedPair>(startup, profile::kStartupWeakLinkPairOffset);
    NativeSharedPair first = LoadAt<NativeSharedPair>(startup, profile::kStartupStatePairOffset);
    StoreAt(startup, profile::kStartupWeakLinkPairOffset, NativeSharedPair{});
    StoreAt(startup, profile::kStartupStatePairOffset, NativeSharedPair{});
    bool ok = ReleaseStrong(second);
    if (!ReleaseStrong(first)) ok = false;
    return ok;
}

// 逆序清理步骤 2: 销毁 Delayed 包装器并释放内部载荷及弱引用
bool DestroyDelayed(NativeDelayedHolder& delayed) noexcept {
    void* weakControl = LoadAt<void*>(delayed, profile::kDelayedWeakControlOffset);
    void* payload = LoadAt<void*>(delayed, profile::kDelayedPayloadOffset);
    StoreAt(delayed, profile::kDelayedWeakControlOffset, static_cast<void*>(nullptr));
    StoreAt(delayed, profile::kDelayedPayloadOffset, static_cast<void*>(nullptr));
    bool ok = ReleaseWeak(weakControl);
    if (!payload) return ok;
    try {
        auto** vtable = *static_cast<void***>(payload);
        if (!vtable || !vtable[4]) return false;
        reinterpret_cast<PayloadDestroyFn>(vtable[4])(
            payload, payload != static_cast<void*>(&delayed));
    } catch (...) {
        ok = false;
    }
    return ok;
}

// 逆序清理步骤 3: 释放由微信 allocate 分配的 Vector 数组槽位及强引用
bool DestroyVector(const NativeTextApi& api, NativeSharedPairVector& vector) noexcept {
    NativeSharedPair* allocation = vector.begin;
    vector = {};
    if (!allocation) return true;
    NativeSharedPair held{};
    memcpy(&held, allocation, sizeof(held));
    memset(allocation, 0, sizeof(held));
    bool ok = ReleaseStrong(held);
    try {
        api.free(allocation, sizeof(NativeSharedPair));
    } catch (...) {
        ok = false;
    }
    return ok;
}
} // namespace

namespace native_detail {

/**
 * @brief 读取原生 Source 消息对象中生成的本地唯一 UUID
 */
bool ReadNativeSourceUuid(const NativeSharedPair& source, std::string& output) noexcept {
    output.clear();
    if (!source.raw || !source.control) return false;
    char localUuid[kLocalUuidMaxBytes + 1]{};
    const auto read = CopyNativeString(
        static_cast<unsigned char*>(source.raw) + profile::kLocalUuidOffset,
        localUuid, sizeof(localUuid));
    if (read.status != ReadStatus::Ok || !read.lengthKnown ||
        read.originalBytes > kLocalUuidMaxBytes || read.capturedBytes != read.originalBytes) return false;
    output.assign(localUuid, read.capturedBytes);
    return true;
}

bool ReleaseNativeSource(NativeSharedPair& source) noexcept {
    return ReleaseStrong(source);
}

/**
 * @brief 提交已预备好的原生 Source 对象至微信发信引擎
 * @details 严格执行以下五阶段流程，并在退出时执行确定性逆序清理：
 *          1. vector_allocate: 调用微信原生分配器为 NativeSharedPair 分配槽位，并递增强引用计数
 *          2. delayed_factory: 调用 makeDelayed 将 vector 包装为延迟可执行体
 *          3. metadata: 初始化元数据对象
 *          4. start: 调用微信底层 start 函数正式投递消息至网络队列
 *          5. reverse cleanup: 无论中途任何阶段抛出异常或失败，严格按照逆序释放已分配资源
 */
NativeAttemptResult SubmitPreparedNativeSourceOnce(
    const NativeTextApi& api, NativeSharedPair& source, std::string localUuid,
    const std::function<bool()>& beforeStart, bool sourceSafeToRelease,
    bool priorCleanupComplete) noexcept {
    NativeAttemptResult result{};
    result.status = NativeAttemptStatus::preparation_failed;
    result.localUuid = std::move(localUuid);
    NativeSharedPairVector vector{};
    NativeDelayedHolder delayed{};
    NativeStartupHandle startup{};
    NativeMetadata metadata{};
    NativeCallableHolder onValue{}, onError{}, onComplete{};
    NativeCallPhase delayedPhase = NativeCallPhase::NotEntered;

    do {
        try {
            // 阶段 1: 分配 vector 存储空间并存入当前 source，递增其强引用
            result.stage = NativeSubmitStage::vector_allocate;
            auto* slot = static_cast<NativeSharedPair*>(api.allocate(sizeof(NativeSharedPair)));
            if (!slot) break;
            memset(slot, 0, sizeof(*slot));
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                static_cast<unsigned char*>(source.control) + 8));
            *slot = source;
            vector.begin = slot;
            vector.end = slot + 1;
            vector.capacityEnd = slot + 1;

            // 阶段 2: 将 vector 封装进 DelayedHolder
            result.stage = NativeSubmitStage::delayed_factory;
            delayedPhase = NativeCallPhase::EnteredWithoutReturn;
            NativeDelayedHolder* delayedResult = api.makeDelayed(nullptr, &delayed, &vector, 0);
            delayedPhase = NativeCallPhase::Returned;
            if (delayedResult != &delayed || !LoadAt<void*>(delayed, profile::kDelayedPayloadOffset)) break;

            // 阶段 3: 初始化发信元数据
            result.stage = NativeSubmitStage::metadata;
            if (api.initMetadata(&metadata, api.metadataStaticText1, api.metadataStaticText2,
                                 api.metadataValue) != &metadata) break;
            static_assert(profile::kCallablePayloadOffset + sizeof(void*) <= sizeof(NativeCallableHolder));
            if (LoadAt<void*>(onValue, profile::kCallablePayloadOffset) ||
                LoadAt<void*>(onError, profile::kCallablePayloadOffset) ||
                LoadAt<void*>(onComplete, profile::kCallablePayloadOffset)) break;

            // 阶段 4: 前置检查钩子 (如超时判定等)
            result.stage = NativeSubmitStage::before_start;
            if (beforeStart && !beforeStart()) break;

            // 阶段 5: 调用微信原生 start 函数启动发信
            result.stage = NativeSubmitStage::start;
            result.startEntered = true;
            NativeStartupHandle* returned = api.start(
                &delayed, &startup, &onValue, &onError, &onComplete, &metadata);
            result.startReturned = true;
            result.status = returned == &startup ? NativeAttemptStatus::start_returned
                                                 : NativeAttemptStatus::unknown_after_start;
        } catch (...) {
            result.status = result.startEntered ? NativeAttemptStatus::unknown_after_start
                                                : NativeAttemptStatus::preparation_failed;
        }
    } while (false);

    // 确定性逆序资源清理: Startup -> Delayed -> Vector -> Source
    // 杜绝任何中间状态导致的句柄或内存泄漏
    bool cleanup = priorCleanupComplete && sourceSafeToRelease &&
                   delayedPhase != NativeCallPhase::EnteredWithoutReturn &&
                   !(result.startEntered && !result.startReturned);
    if (result.startReturned && !DestroyStartup(startup)) cleanup = false;
    if (delayedPhase == NativeCallPhase::Returned && !DestroyDelayed(delayed)) cleanup = false;
    if (!DestroyVector(api, vector)) cleanup = false;
    if (sourceSafeToRelease && !ReleaseStrong(source)) cleanup = false;
    result.cleanupComplete = cleanup;
    if (!cleanup && result.startEntered) result.status = NativeAttemptStatus::unknown_after_start;
    return result;
}
} // namespace native_detail

/**
 * @brief 执行单次原生文本消息构造与发送
 * @details 
 *   运行环境要求：必须运行在授权的微信 UI 线程上。
 *   执行步骤：
 *   1. 【入参合规校验】：目标 wxid、文本内容（UTF-8且无\0）、@群成员列表、引用回复有效性；
 *   2. 【线程亲和性校验】：当前线程 ID 必须等于 authorizedUiThreadId；
 *   3. 【API 完整性校验】：确保 factory、assign、allocate、start 等核心原生接口指针均非空；
 *   4. 【Source 对象构造】：
 *      - 调用微信原生构造工厂 api.factory(&source) 创建消息对象；
 *      - 读取微信分配的本地消息 UUID；
 *      - 填充目标聊天会话 targetId 与文本内容 textUtf8；
 *      - 写入文本消息类型标记 kTypeAndSubtype 与长度；
 *      - 若存在 @成员列表，写入 kAtUserListOffset 槽位；
 *      - 若存在引用回复，调用 PrepareQuoteSource 构造并挂载引用卡片；
 *   5. 【提交与逆序清理】：
 *      - 若构造成功，转交 SubmitPreparedNativeSourceOnce 组装 vector/delayed/metadata 并调用 start；
 *      - 若中途构造失败，安全释放 source 强引用，杜绝内存泄漏。
 */
NativeAttemptResult SubmitNativeTextOnce(const NativeTextApi& api,
                                      const NativeTextRequest& request) noexcept {
    NativeAttemptResult result{};
    
    // 1. 入参合规校验
    if (!native_detail::IsValidNativeUtf8NoNul(request.targetUtf8) ||
        !native_detail::IsValidNativeUtf8NoNul(request.textUtf8))
        return result;
    if (!IsValidMentionList(request.atUserListUtf8) ||
        (!request.atUserListUtf8.empty() && !IsGroupTarget(request.targetUtf8))) return result;
    if (request.quote &&
        (!IsValidQuote(*request.quote, request.targetUtf8) ||
         !native_detail::IsValidNativeUtf8NoNul(request.quote->text) ||
         (!request.quote->msgSource.empty() &&
          !native_detail::IsValidNativeUtf8NoNul(request.quote->msgSource))))
        return result;

    // 2. 线程亲和性校验：必须在 UI 线程执行
    if (!request.authorizedUiThreadId || GetCurrentThreadId() != request.authorizedUiThreadId) {
        result.status = NativeAttemptStatus::ui_thread_required;
        return result;
    }

    // 3. 原生 API 指针完整性校验
    if (!api.factory || !native_detail::CompleteNativeCommonApi(api) ||
        (request.quote && (!api.referenceMessageInit || !api.referenceMessageDestroy ||
                           !api.quoteFromMessage || !api.quoteDestroy || !api.quoteAttach))) {
        result.status = NativeAttemptStatus::api_unavailable;
        return result;
    }

    NativeSharedPair source{};
    native_detail::NativeCallPhase sourcePhase = native_detail::NativeCallPhase::NotEntered;
    QuotePreparationOutcome quoteOutcome{};
    bool prepared = false;

    result.status = NativeAttemptStatus::preparation_failed;
    do {
        try {
            // 4.1 构造消息原生 Source 对象
            result.stage = NativeSubmitStage::source_factory;
            sourcePhase = native_detail::NativeCallPhase::EnteredWithoutReturn;
            NativeSharedPair* sourceResult = api.factory(&source);
            sourcePhase = native_detail::NativeCallPhase::Returned;
            if (sourceResult != &source || !source.raw || !source.control) break;

            // 4.2 提取微信生成的 Local UUID
            result.stage = NativeSubmitStage::local_uuid;
            if (!native_detail::ReadNativeSourceUuid(source, result.localUuid)) break;

            // 4.3 写入接收目标 (wxid / 聊天室 id)
            result.stage = NativeSubmitStage::target_assign;
            auto* target = static_cast<unsigned char*>(source.raw) + profile::kTargetOffset;
            if (api.assign(target, request.targetUtf8.data(), request.targetUtf8.size()) != target) break;

            // 4.4 写入文本内容及类型标记
            result.stage = NativeSubmitStage::text_assign;
            auto* text = static_cast<unsigned char*>(source.raw) + profile::kTextOffset;
            if (api.assign(text, request.textUtf8.data(), request.textUtf8.size()) != text) break;
            const uint64_t typeAndSubtype = profile::kTypeAndSubtype;
            const uint64_t textBytes = request.textUtf8.size();
            memcpy(static_cast<unsigned char*>(source.raw) + profile::kTypeOffset,
                   &typeAndSubtype, sizeof(typeAndSubtype));
            memcpy(static_cast<unsigned char*>(source.raw) + profile::kTextByteLengthOffset,
                   &textBytes, sizeof(textBytes));

            // 校验原生构造默认初始态
            if (*(static_cast<unsigned char*>(source.raw) + profile::kStateFlagOffset) != 0) break;
            uint64_t optionalLength = 0;
            memcpy(&optionalLength, static_cast<unsigned char*>(source.raw) +
                   profile::kOptionalTextLengthOffset, sizeof(optionalLength));
            if (optionalLength != 0) break;

            // 4.5 写入 @群成员列表
            if (!request.atUserListUtf8.empty()) {
                auto* members = static_cast<unsigned char*>(source.raw) + profile::kAtUserListOffset;
                if (api.assign(members, request.atUserListUtf8.data(), request.atUserListUtf8.size()) != members) break;
            }

            // 4.6 组装引用回复卡片
            if (request.quote) {
                result.stage = NativeSubmitStage::quote_prepare;
                quoteOutcome = PrepareQuoteSource(api, source.raw, *request.quote);
                if (!quoteOutcome.ready) break;
            }
            prepared = true;
        } catch (...) {
            result.status = NativeAttemptStatus::preparation_failed;
        }
    } while (false);

    // 5. 若前期组装成功，进入 Submit 投递流程
    if (prepared) {
        return native_detail::SubmitPreparedNativeSourceOnce(
            api, source, std::move(result.localUuid), request.beforeStart,
            quoteOutcome.sourceSafeToRelease, quoteOutcome.cleanupComplete);
    }

    // 6. 前期组装失败时的资源回收
    bool cleanup = sourcePhase != native_detail::NativeCallPhase::EnteredWithoutReturn &&
                   quoteOutcome.cleanupComplete && quoteOutcome.sourceSafeToRelease;
    if (sourcePhase == native_detail::NativeCallPhase::Returned &&
        quoteOutcome.sourceSafeToRelease && !ReleaseStrong(source)) cleanup = false;
    result.cleanupComplete = cleanup;
    return result;
}

} // namespace wechatbot::monitor
