#include "monitor/diagnostics/event.hpp"
#include "monitor/diagnostics/event_encoding.hpp"
#include <atomic>
#include <cstring>

namespace wechatbot::monitor {
namespace {
std::atomic<uint64_t> sequence{0};
std::atomic<uint64_t> callId{0};
std::string sessionId = "uninitialized";
uint64_t FileTimeNow() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
using event_encoding::JsonObjectWriter;

std::string SerializeContext(const Event& event, const ContextSnapshot& snapshot) {
    const auto& header = event.header;
    const bool enter = event.Kind() == EventKind::SendContextEnter;
    std::string line;
    JsonObjectWriter fields(line);
    fields.String("kind", enter ? "send_context_enter" : "send_context_return");
    fields.Number("schema_version", 2);
    fields.String("source", "send_context_constructor");
    fields.Number("seq", header.sequence);
    fields.Number("tick", header.tick);
    fields.Number("tid", header.threadId);
    fields.Number("call_id", header.callId);
    fields.Number("observed_unix_ms", header.observedUnixMs);
    fields.Hex("context_address", snapshot.context);
    if (enter) {
        fields.Hex("source_pair_address", snapshot.sourcePairAddress);
        fields.Hex("source_object_address", snapshot.sourceObjectAddress);
        fields.Hex("source_control_address", snapshot.sourceControlAddress);
        fields.Number("constructor_flag", snapshot.constructorFlag);
        fields.Hex("source_vtable", snapshot.sourceVtable);
        fields.Number("source_type", snapshot.sourceType);
        fields.Number("source_subtype", snapshot.sourceSubtype);
        event_encoding::Text(fields, "target", snapshot.target);
        event_encoding::Text(fields, "content", snapshot.content);
        event_encoding::Text(fields, "local_uuid", snapshot.localUuid);
        event_encoding::Read(fields, "target_read", snapshot.target.read);
        event_encoding::Read(fields, "content_read", snapshot.content.read);
        event_encoding::Read(fields, "local_uuid_read", snapshot.localUuid.read);
        if (header.stack.attempted) event_encoding::Stack(fields, header.stack);
    } else {
        fields.Hex("returned_context_address", snapshot.returnedContextAddress);
        fields.Boolean("returned_context_matches", snapshot.returnedContextMatches);
        fields.Boolean("post_context_readable", snapshot.postContextReadable);
        if (snapshot.postContextReadable) {
            fields.Hex("post_vtable", snapshot.postVtable);
            fields.Boolean("post_vtable_is_base", snapshot.postVtableIsBase);
            fields.Hex("post_source_object_address", snapshot.postSourceObjectAddress);
            fields.Hex("post_source_control_address", snapshot.postSourceControlAddress);
            fields.Boolean("post_source_identity_matches", snapshot.postSourceIdentityMatches);
        } else {
            fields.Null("post_vtable");
            fields.Null("post_vtable_is_base");
            fields.Null("post_source_object_address");
            fields.Null("post_source_control_address");
            fields.Null("post_source_identity_matches");
        }
        event_encoding::Text(fields, "post_target", snapshot.postTarget);
        event_encoding::Read(fields, "post_target_read", snapshot.postTarget.read);
        if (snapshot.postContextReadable) {
            fields.Number("post_flag", snapshot.postFlag);
            fields.Number("post_state", snapshot.postState);
        } else {
            fields.Null("post_flag");
            fields.Null("post_state");
        }
    }
    event_encoding::Session(fields);
    fields.Close();
    return line + "\r\n";
}

std::string SerializeMessage(const Event& event, const MessageMetadata& message,
                             const MessageSnapshot* item) {
    const auto& header = event.header;
    const auto kind = event.Kind();
    const bool outbound = kind == EventKind::OutboundRequest ||
                          kind == EventKind::OutboundItem || kind == EventKind::OutboundReturn;
    const char* name = kind == EventKind::Batch ? "batch" :
                       kind == EventKind::Item ? "item" :
                       kind == EventKind::OutboundRequest ? "outbound_request" :
                       kind == EventKind::OutboundItem ? "outbound_item" :
                       kind == EventKind::OutboundReturn ? "outbound_return" : "return";
    std::string line;
    line.reserve(512 + (item ? item->from.view().size() + item->to.view().size() +
                              item->content.view().size() : 0));
    JsonObjectWriter fields(line);
    fields.String("kind", name);
    fields.Number("seq", header.sequence);
    fields.Number("tick", header.tick);
    fields.Number("tid", header.threadId);
    fields.Hex("context", message.context);
    fields.Hex("vector", message.vector);
    fields.Hex("begin", message.begin);
    fields.Hex("end", message.end);
    fields.Number("count", message.count);
    fields.Number("index", message.index);
    fields.Hex("item", message.item);
    fields.Array("flags", [&](std::string& output) {
        event_encoding::AppendInteger(output, message.flag3);
        output += ',';
        event_encoding::AppendInteger(output, message.flag4);
    });
    fields.Boolean("valid_vector", message.validVector != 0);
    fields.Boolean("vtable_match", message.vtableMatch != 0);
    fields.Hex("has_bits", message.hasBits);
    fields.Number("msg_type", message.msgType);
    fields.Number("return", message.returnValue);
    fields.Number("schema_version", 2);
    fields.String("source", outbound ? "send_request" : "receive_batch");
    fields.Number("call_id", header.callId);
    fields.Number("observed_unix_ms", header.observedUnixMs);
    fields.Number("snapshot_limit", kMaxLoggedItemsPerBatch);
    if (item) {
        event_encoding::Text(fields, "from", item->from);
        event_encoding::Text(fields, "to", item->to);
        event_encoding::Text(fields, "content", item->content);
        event_encoding::Text(fields, "msg_source", item->msgSource);
        event_encoding::Text(fields, "field11", item->field11);
        event_encoding::Read(fields, "from_read", item->from.read);
        event_encoding::Read(fields, "to_read", item->to.read);
        event_encoding::Read(fields, "content_read", item->content.read);
        event_encoding::Read(fields, "source_read", item->msgSource.read);
        event_encoding::Read(fields, "aux_read", item->field11.read);
        const std::span<const ScalarField> scalars = outbound
            ? std::span<const ScalarField>(kSendScalars)
            : std::span<const ScalarField>(kReceiveScalars);
        fields.Object("raw_fields", [&](JsonObjectWriter& raw) {
            for (size_t i = 0; i < scalars.size(); ++i) {
                const auto key = std::to_string(scalars[i].number);
                if (!message.vtableMatch || !(message.hasBits & scalars[i].bit)) raw.Null(key);
                else if (scalars[i].signed32)
                    raw.SignedString(key, static_cast<int32_t>(item->rawFields[i]));
                else raw.NumberString(key, item->rawFields[i]);
            }
        });
    }
    if (kind == EventKind::OutboundRequest && header.stack.attempted) event_encoding::Stack(fields, header.stack);
    if (kind == EventKind::OutboundReturn) {
        fields.Boolean("result_pointer_matches", message.resultPointerMatches);
        if (message.resultReadable) {
            fields.Array("raw_result_status", [&](std::string& output) {
                event_encoding::AppendInteger(output, message.resultStatus[0]);
                output += ',';
                event_encoding::AppendInteger(output, message.resultStatus[1]);
            });
        } else fields.Null("raw_result_status");
    }
    event_encoding::Session(fields);
    fields.Close();
    return line + "\r\n";
}

void AppendBuffer(JsonObjectWriter& fields, std::string_view name, const BinarySnapshot& snapshot) {
    fields.Object(name, [&](JsonObjectWriter& buffer) {
        buffer.String("status", StatusName(snapshot.read.status));
        if (snapshot.declaredLengthKnown) buffer.Number("declared_bytes", snapshot.declaredBytes);
        else buffer.Null("declared_bytes");
        if (snapshot.read.lengthKnown) buffer.Number("original_bytes", snapshot.read.originalBytes);
        else buffer.Null("original_bytes");
        const size_t captured = snapshot.read.capturedBytes <= snapshot.bytes.size()
            ? snapshot.read.capturedBytes : 0;
        buffer.Number("captured_bytes", captured);
        buffer.HexBytes("hex", std::span(snapshot.bytes).first(captured));
    });
}
} // namespace

void BeginSession() {
    sessionId = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(FileTimeNow());
}
const std::string& CurrentSessionId() { return sessionId; }
uint64_t NextSequence() { return ++sequence; }
Event MakeCallEvent(EventKind kind) {
    Event event(kind);
    event.header.sequence = NextSequence();
    event.header.callId = ++callId;
    event.header.tick = GetTickCount64();
    event.header.threadId = GetCurrentThreadId();
    event.header.observedUnixMs = FileTimeNow() / 10000 - 11644473600000ULL;
    return event;
}

const char* StatusName(ReadStatus status) {
    switch (status) {
    case ReadStatus::Ok: return "ok";
    case ReadStatus::Empty: return "empty";
    case ReadStatus::Missing: return "missing";
    case ReadStatus::InnerMissing: return "inner_missing";
    case ReadStatus::InvalidObject: return "invalid_object";
    case ReadStatus::InvalidWrapper: return "invalid_wrapper";
    case ReadStatus::InvalidLayout: return "invalid_layout";
    case ReadStatus::Unreadable: return "unreadable";
    case ReadStatus::Exception: return "exception";
    case ReadStatus::Truncated: return "truncated";
    case ReadStatus::InvalidUtf8: return "invalid_utf8";
    default: return "not_read";
    }
}

void AppendJsonBytes(std::string& output, const char* value, size_t length) {
    json::AppendString(output, value ? std::string_view(value, length) : std::string_view{});
}
void AppendJsonString(std::string& output, const char* value) {
    AppendJsonBytes(output, value, value ? strlen(value) : 0);
}

std::string AddSessionId(std::string record) {
    if (record.empty() || record.back() != '}') return record;
    record.pop_back();
    record += ",\"session_id\":";
    AppendJsonString(record, CurrentSessionId().c_str());
    record += '}';
    return record;
}

std::string SerializeMediaReceiveEvent(const Event& event) {
    const auto& snapshot = event.Get<MediaReceiveSnapshot>();
    std::string line;
    JsonObjectWriter fields(line);
    fields.String("kind", "media_receive_sample");
    fields.Number("schema_version", 2);
    fields.Number("seq", event.header.sequence);
    fields.Number("call_id", event.header.callId);
    fields.Number("observed_unix_ms", event.header.observedUnixMs);
    fields.Number("source_event_seq", snapshot.sourceEventSequence);
    fields.Number("msg_type", snapshot.msgType);
    fields.Number("snapshot_limit_bytes", 8192);
    AppendBuffer(fields, "field8", snapshot.field8);
    AppendBuffer(fields, "field14", snapshot.field14);
    event_encoding::Session(fields);
    fields.Close();
    return line + "\r\n";
}

std::string SerializeEvent(const Event& event) {
    return event.Visit([&](const auto& snapshot) -> std::string {
        using Snapshot = std::remove_cvref_t<decltype(snapshot)>;
        if constexpr (std::is_same_v<Snapshot, CallSnapshot>)
            return SerializeMessage(event, snapshot.message, nullptr);
        else if constexpr (std::is_same_v<Snapshot, MessageSnapshot>)
            return SerializeMessage(event, snapshot.message, &snapshot);
        else if constexpr (std::is_same_v<Snapshot, ContextSnapshot>)
            return SerializeContext(event, snapshot);
        else if constexpr (std::is_same_v<Snapshot, SubmitSnapshot>)
            return SerializeSendSubmitEvent(event);
        else
            return SerializeMediaReceiveEvent(event);
    });
}

} // namespace wechatbot::monitor
