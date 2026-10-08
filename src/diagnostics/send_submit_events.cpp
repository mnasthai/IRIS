#include "monitor/diagnostics/event_encoding.hpp"

namespace wechatbot::monitor {
namespace {
using event_encoding::JsonObjectWriter;

void Address(JsonObjectWriter& fields, std::string_view key, uint64_t value, bool readable = true) {
    if (readable) fields.Hex(key, value);
    else fields.Null(key);
}
void NumberString(JsonObjectWriter& fields, std::string_view key, uint64_t value, bool readable) {
    if (readable) fields.NumberString(key, value);
    else fields.Null(key);
}
void AppendReference(JsonObjectWriter& fields, const ReferenceSnapshot& reference) {
    namespace layout = active_profile::text;
    const auto strings = [&](std::string_view key, const auto& offsets,
                             const auto& values) {
        fields.Array(key, [&](std::string& output) {
            for (size_t i = 0; i < std::size(offsets); ++i) {
                if (i) output += ',';
                JsonObjectWriter element(output);
                element.Hex("offset", offsets[i]);
                event_encoding::Text(element, "value", values[i]);
                event_encoding::Read(element, "read", values[i].read);
                element.Close();
            }
        });
    };
    NumberString(fields, "reference_id_present", reference.idPresent, reference.idFlagReadable);
    strings("reference_id_strings_source_offsets", layout::kReferenceIdStringOffsets,
            reference.idStrings);
    NumberString(fields, "reference_source_210", reference.idScalars[0], reference.idScalarsReadable);
    NumberString(fields, "reference_source_214", reference.idScalars[1], reference.idScalarsReadable);
    NumberString(fields, "reference_source_218", reference.idWide, reference.idScalarsReadable);
    NumberString(fields, "reference_message_present", reference.messagePresent, reference.messageFlagReadable);
    Address(fields, "reference_message_vtable", reference.messageVtable, reference.messageVtableReadable);
    fields.Boolean("reference_message_vtable_matches", reference.messageVtableMatches);
    strings("reference_message_strings_object_offsets", layout::kReferenceMessageStringOffsets,
            reference.messageStrings);
    constexpr std::array wideKeys{
        "reference_message_08", "reference_message_b8", "reference_message_c8"};
    constexpr std::array scalarKeys{
        "reference_message_94", "reference_message_c0", "reference_message_d0"};
    for (size_t i = 0; i < wideKeys.size(); ++i) {
        NumberString(fields, wideKeys[i], reference.messageWide[i], reference.messageWideReadable[i]);
        NumberString(fields, scalarKeys[i], reference.messageScalars[i], reference.messageScalarsReadable[i]);
    }
    strings("partial_strings_source_offsets", layout::kPartialStringOffsets,
            reference.partialStrings);
    NumberString(fields, "partial_source_6d0", reference.partialWide, reference.partialWideReadable);
}

void AppendSource(JsonObjectWriter& fields, const SubmitSnapshot& snapshot) {
    std::visit([&](const auto& source) {
        using Source = std::remove_cvref_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, TextSubmitSnapshot>) {
            if (source.sourceTextBytesReadable) fields.Number("source_text_bytes", source.sourceTextBytes);
            else fields.Null("source_text_bytes");
            event_encoding::Text(fields, "target", snapshot.target);
            event_encoding::Text(fields, "content", source.content);
            event_encoding::Text(fields, "local_uuid", snapshot.localUuid);
            event_encoding::Read(fields, "target_read", snapshot.target.read);
            event_encoding::Read(fields, "content_read", source.content.read);
            event_encoding::Read(fields, "local_uuid_read", snapshot.localUuid.read);
            event_encoding::Text(fields, "at_user_list", source.atUserList);
            event_encoding::Text(fields, "copy_from_uuid", source.copyFromUuid);
            event_encoding::Text(fields, "optional_text", source.optionalText);
            event_encoding::Read(fields, "at_user_list_read", source.atUserList.read);
            event_encoding::Read(fields, "copy_from_uuid_read", source.copyFromUuid.read);
            event_encoding::Read(fields, "optional_text_read", source.optionalText.read);
            if (source.sourceStateFlagReadable) fields.Number("source_state_flag", source.sourceStateFlag);
            else fields.Null("source_state_flag");
            if (snapshot.sourceType == 49 && snapshot.sourceSubtype == 57)
                AppendReference(fields, source.reference);
        } else {
            if constexpr (std::is_same_v<Source, ImageSubmitSnapshot>) {
                fields.String("media_kind", "image");
                event_encoding::Text(fields, "image_path", source.path);
                event_encoding::Read(fields, "image_path_read", source.path.read);
                event_encoding::Read(fields, "image_data_layout", source.dataRead);
                if (source.widthReadable) fields.Number("image_width", source.width);
                else fields.Null("image_width");
                if (source.heightReadable) fields.Number("image_height", source.height);
                else fields.Null("image_height");
            } else {
                fields.String("media_kind", "voice");
                fields.HexBytes("voice_prefix_hex", std::span(source.prefix).first(
                    (std::min)(size_t{source.dataRead.capturedBytes}, source.prefix.size())));
                event_encoding::Read(fields, "voice_data_read", source.dataRead);
                if (source.lengthReadable) fields.Number("source_voicelength", source.length);
                else fields.Null("source_voicelength");
                if (source.formatReadable) fields.Number("source_voiceformat", source.format);
                else fields.Null("source_voiceformat");
            }
            event_encoding::Text(fields, "target", snapshot.target);
            event_encoding::Text(fields, "local_uuid", snapshot.localUuid);
            event_encoding::Read(fields, "target_read", snapshot.target.read);
            event_encoding::Read(fields, "local_uuid_read", snapshot.localUuid.read);
        }
    }, snapshot.source);
}
} // namespace

std::string SerializeSendSubmitEvent(const Event& event) {
    const bool enter = event.Kind() == EventKind::SendSubmitEnter;
    const auto& header = event.header;
    const auto& snapshot = event.Get<SubmitSnapshot>();
    std::string line;
    JsonObjectWriter fields(line);
    fields.String("kind", enter ? "send_submit_enter" : "send_submit_return");
    fields.Number("schema_version", 2);
    fields.String("source", "send_submit");
    fields.Number("seq", header.sequence);
    fields.Number("call_id", header.callId);
    fields.Number("tid", header.threadId);
    fields.Number("observed_unix_ms", header.observedUnixMs);
    fields.Number("tick", header.tick);
    fields.Hex("callable_address", snapshot.context);
    fields.Hex("source_object_address", snapshot.sourceObjectAddress);
    fields.Hex("source_control_address", snapshot.sourceControlAddress);
    if (enter) {
        fields.Hex("callable_vtable", snapshot.callableVtable);
        Address(fields, "direct_return_rva", snapshot.directReturnRva, snapshot.directReturnRva != 0);
        fields.Hex("vector_begin", snapshot.begin);
        fields.Hex("vector_end", snapshot.end);
        fields.Hex("vector_capacity", snapshot.vectorCapacity);
        fields.Number("vector_count", snapshot.count);
        fields.Number("source_index", snapshot.index);
        fields.Hex("source_pair_address", snapshot.sourcePairAddress);
        fields.Hex("source_vtable", snapshot.sourceVtable);
        Address(fields, "source_vtable_rva", snapshot.sourceVtableRva, snapshot.sourceVtableRva != 0);
        fields.Number("source_type", snapshot.sourceType);
        fields.Number("source_subtype", snapshot.sourceSubtype);
        AppendSource(fields, snapshot);
        fields.Hex("completion_address", snapshot.completionAddress);
        fields.Boolean("completion_readable", snapshot.completionReadable);
        Address(fields, "completion_shared_raw", snapshot.sharedPair[0], snapshot.completionReadable);
        Address(fields, "completion_shared_control", snapshot.sharedPair[1], snapshot.completionReadable);
        Address(fields, "completion_value_d0", snapshot.completionValue, snapshot.completionReadable);
        Address(fields, "completion_weak_raw", snapshot.weakPair[0], snapshot.completionReadable);
        Address(fields, "completion_weak_control", snapshot.weakPair[1], snapshot.completionReadable);
        fields.Array("completion_callbacks", [&](std::string& output) {
            const bool baseKnown = snapshot.sourceVtableRva != 0 &&
                                   snapshot.sourceVtable >= snapshot.sourceVtableRva;
            const auto base = baseKnown ? snapshot.sourceVtable - snapshot.sourceVtableRva : 0;
            for (size_t i = 0; i < snapshot.callbackReads.size(); ++i) {
                if (i) output += ',';
                JsonObjectWriter callback(output);
                const auto status = snapshot.callbackReads[i];
                callback.String("read_status", StatusName(status));
                const bool readable = status == ReadStatus::Ok;
                Address(callback, "payload", snapshot.callbackPayloads[i], readable || status == ReadStatus::Empty);
                Address(callback, "vtable", snapshot.callbackVtables[i], readable);
                const auto vtable = snapshot.callbackVtables[i];
                Address(callback, "vtable_rva", vtable - base,
                        readable && baseKnown && vtable >= base && vtable - base < kExpectedImageSize);
                callback.Close();
            }
        });
        Address(fields, "root_address", snapshot.rootAddress, snapshot.rootPointerReadable);
        Address(fields, "executor_raw", snapshot.executorPair[0], snapshot.executorPairReadable);
        Address(fields, "executor_control", snapshot.executorPair[1], snapshot.executorPairReadable);
        Address(fields, "executor_inner", snapshot.executorInner, snapshot.executorInnerReadable);
        if (snapshot.executorFlagsReadable) {
            fields.Number("executor_flags", snapshot.executorFlags);
            fields.Boolean("executor_bit0", (snapshot.executorFlags & 1) != 0);
        } else {
            fields.Null("executor_flags");
            fields.Null("executor_bit0");
        }
        if (header.stack.attempted) event_encoding::Stack(fields, header.stack);
    } else {
        fields.Boolean("normal_return", true);
        fields.Number("return_tick", snapshot.returnTick);
    }
    event_encoding::Session(fields);
    fields.Close();
    return line + "\r\n";
}
} // namespace wechatbot::monitor
