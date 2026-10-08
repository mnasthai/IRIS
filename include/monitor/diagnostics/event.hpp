#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/core/read_result.hpp"
#include "monitor/config/version_config.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace wechatbot::monitor {

enum class EventKind : uint32_t {
    Batch, Item, ReturnValue, OutboundRequest, OutboundItem, OutboundReturn,
    SendContextEnter, SendContextReturn, SendSubmitEnter, SendSubmitReturn, MediaReceive
};

// Fixed storage keeps capture bounded. Status and byte length travel with the
// buffer, including embedded NUL and truncated UTF-8.
template<size_t Capacity>
struct CapturedText {
    std::array<char, Capacity> bytes{};
    TextRead read{};
    char* data() noexcept { return bytes.data(); }
    const char* data() const noexcept { return bytes.data(); }
    static constexpr size_t capacity() noexcept { return Capacity; }
    std::string_view view() const noexcept {
        return {data(), (std::min)(size_t{read.capturedBytes}, Capacity)};
    }
};

struct StackSnapshot {
    bool attempted{};
    uint16_t frameCount{};
    std::array<uint32_t, 24> rvas{}; // zero means outside Weixin.dll
};

struct EventHeader {
    uint64_t sequence{}, callId{}, observedUnixMs{}, tick{};
    uint32_t threadId{};
    StackSnapshot stack{};
};

// The schema emits these fields for summaries and items. Separate metadata
// avoids carrying message buffers into return snapshots.
struct MessageMetadata {
    uint64_t context{}, vector{}, begin{}, end{}, item{};
    uint32_t count{}, index{}, msgType{}, hasBits{};
    uint8_t flag3{}, flag4{}, validVector{}, vtableMatch{}, returnValue{};
    bool resultReadable{}, resultPointerMatches{};
    std::array<uint32_t, 2> resultStatus{};
};
struct CallSnapshot { MessageMetadata message{}; };
struct MessageSnapshot {
    MessageMetadata message{};
    CapturedText<kIdCapacity> from{}, to{};
    CapturedText<kTextCapacity> content{}, field11{};
    CapturedText<kSourceCapacity> msgSource{};
    std::array<uint64_t, 7> rawFields{};
};

struct ContextSnapshot {
    uint64_t context{}, sourcePairAddress{}, sourceObjectAddress{}, sourceControlAddress{}, sourceVtable{};
    uint32_t sourceType{}, sourceSubtype{};
    uint8_t constructorFlag{};
    CapturedText<kIdCapacity> target{}, localUuid{}, postTarget{};
    CapturedText<kTextCapacity> content{};
    uint64_t returnedContextAddress{};
    bool returnedContextMatches{}, postContextReadable{};
    uint64_t postVtable{}, postSourceObjectAddress{}, postSourceControlAddress{};
    bool postVtableIsBase{}, postSourceIdentityMatches{};
    uint8_t postFlag{};
    uint32_t postState{};
};

struct ReferenceSnapshot {
    uint8_t idPresent{}, messagePresent{};
    bool idFlagReadable{}, messageFlagReadable{}, messageVtableReadable{}, messageVtableMatches{};
    uint64_t messageVtable{};
    std::array<uint32_t, 2> idScalars{};
    uint64_t idWide{};
    bool idScalarsReadable{};
    std::array<CapturedText<512>, 2> idStrings{};
    std::array<CapturedText<512>, 9> messageStrings{};
    std::array<CapturedText<512>, 3> partialStrings{};
    std::array<uint64_t, 3> messageWide{};
    std::array<uint32_t, 3> messageScalars{};
    std::array<bool, 3> messageWideReadable{}, messageScalarsReadable{};
    uint64_t partialWide{};
    bool partialWideReadable{};
};

struct TextSubmitSnapshot {
    CapturedText<kTextCapacity> content{}, optionalText{};
    CapturedText<kSourceCapacity> atUserList{};
    CapturedText<kIdCapacity> copyFromUuid{};
    uint64_t sourceTextBytes{};
    bool sourceTextBytesReadable{};
    uint8_t sourceStateFlag{};
    bool sourceStateFlagReadable{};
    ReferenceSnapshot reference{};
};
struct ImageSubmitSnapshot {
    CapturedText<kTextCapacity> path{};
    TextRead dataRead{};
    uint32_t width{}, height{};
    bool widthReadable{}, heightReadable{};
};
struct VoiceSubmitSnapshot {
    std::array<uint8_t, 256> prefix{};
    TextRead dataRead{};
    uint32_t length{}, format{};
    bool lengthReadable{}, formatReadable{};
};

struct SubmitSnapshot {
    uint64_t context{}, begin{}, end{};
    uint32_t count{}, index{};
    uint64_t sourcePairAddress{}, sourceObjectAddress{}, sourceControlAddress{}, sourceVtable{};
    uint32_t sourceType{}, sourceSubtype{};
    CapturedText<kIdCapacity> target{}, localUuid{};
    uint64_t callableVtable{}, vectorCapacity{}, completionAddress{};
    uint32_t sourceVtableRva{};
    bool completionReadable{};
    std::array<uint64_t, 2> sharedPair{}, weakPair{}, executorPair{};
    std::array<uint64_t, 3> callbackPayloads{}, callbackVtables{};
    std::array<ReadStatus, 3> callbackReads{};
    uint64_t completionValue{};
    bool rootPointerReadable{}, executorPairReadable{}, executorInnerReadable{}, executorFlagsReadable{};
    uint64_t rootAddress{}, executorInner{};
    uint8_t executorFlags{};
    uint32_t directReturnRva{};
    uint64_t returnTick{};
    std::variant<TextSubmitSnapshot, ImageSubmitSnapshot, VoiceSubmitSnapshot> source{};
};

// Diagnostic samples only; a prefix is never a complete media asset.
struct BinarySnapshot {
    TextRead read{};
    uint32_t declaredBytes{};
    bool declaredLengthKnown{};
    std::array<uint8_t, 8192> bytes{};
};
struct MediaReceiveSnapshot {
    uint64_t sourceEventSequence{};
    uint32_t msgType{};
    BinarySnapshot field8{}, field14{};
};

class Event {
public:
    EventHeader header{};
    Event() noexcept = default;
    explicit Event(EventKind kind) noexcept { SetKind(kind); }
    EventKind Kind() const noexcept { return kind_; }
    template<class T> T& Get() { return std::get<T>(payload_); }
    template<class T> const T& Get() const { return std::get<T>(payload_); }
    template<class T> const T* TryGet() const noexcept { return std::get_if<T>(&payload_); }
    template<class Visitor> decltype(auto) Visit(Visitor&& visitor) const {
        return std::visit(std::forward<Visitor>(visitor), payload_);
    }

    // A kind change selects its matching payload. Message transitions carry
    // metadata only; enter/return traces retain their own snapshot.
    void SetKind(EventKind kind) noexcept {
        const size_t index = PayloadIndex(kind);
        if (payload_.index() != index) {
            MessageMetadata metadata{};
            if (const auto* call = std::get_if<CallSnapshot>(&payload_)) metadata = call->message;
            if (const auto* item = std::get_if<MessageSnapshot>(&payload_)) metadata = item->message;
            switch (index) {
            case 0: payload_.emplace<CallSnapshot>().message = metadata; break;
            case 1: payload_.emplace<MessageSnapshot>().message = metadata; break;
            case 2: payload_.emplace<ContextSnapshot>(); break;
            case 3: payload_.emplace<SubmitSnapshot>(); break;
            case 4: payload_.emplace<MediaReceiveSnapshot>(); break;
            }
        }
        kind_ = kind;
    }
private:
    static constexpr size_t PayloadIndex(EventKind kind) noexcept {
        switch (kind) {
        case EventKind::Item: case EventKind::OutboundItem: return 1;
        case EventKind::SendContextEnter: case EventKind::SendContextReturn: return 2;
        case EventKind::SendSubmitEnter: case EventKind::SendSubmitReturn: return 3;
        case EventKind::MediaReceive: return 4;
        default: return 0;
        }
    }
    EventKind kind_ = EventKind::Batch;
    std::variant<CallSnapshot, MessageSnapshot, ContextSnapshot, SubmitSnapshot, MediaReceiveSnapshot> payload_{};
};

// SEH capture functions must not acquire C++ unwinding obligations, heap
// ownership or borrowed native pointers through their snapshot values.
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_trivially_destructible_v<Event>);

const char* StatusName(ReadStatus status);
void AppendJsonBytes(std::string& output, const char* value, size_t length);
void AppendJsonString(std::string& output, const char* value);
std::string SerializeEvent(const Event& event);
std::string SerializeSendSubmitEvent(const Event& event);
std::string AddSessionId(std::string record);
const std::string& CurrentSessionId();
void BeginSession();
uint64_t NextSequence();
Event MakeCallEvent(EventKind kind);

} // namespace wechatbot::monitor
