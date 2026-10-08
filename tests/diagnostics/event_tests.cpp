#include "monitor/diagnostics/event.hpp"
#include "monitor/core/json_writer.hpp"
#include "monitor/diagnostics/logging.hpp"
#include <cstring>
#include <string>
#include <variant>

using namespace wechatbot::monitor;

void Check(bool condition, const char* message);

void TestTypedEvents() {
    Event call(EventKind::OutboundRequest);
    call.header.callId = 73;
    call.Get<CallSnapshot>().message.count = 2;
    call.Get<CallSnapshot>().message.validVector = 1;
    call.SetKind(EventKind::OutboundItem);
    Check(call.Kind() == EventKind::OutboundItem && call.TryGet<MessageSnapshot>() &&
          !call.TryGet<CallSnapshot>() && call.header.callId == 73 &&
          call.Get<MessageSnapshot>().message.count == 2 &&
          call.Get<MessageSnapshot>().message.validVector &&
          call.Get<MessageSnapshot>().content.read.status == ReadStatus::NotRead,
          "call-to-item transition preserves metadata and activates empty item buffers");
    auto& item = call.Get<MessageSnapshot>();
    memcpy(item.content.data(), "item", 4);
    item.content.read = {ReadStatus::Ok, true, 4, 4};
    Event copiedItem = call;
    call.SetKind(EventKind::OutboundReturn);
    Check(call.Kind() == EventKind::OutboundReturn && call.TryGet<CallSnapshot>() &&
          !call.TryGet<MessageSnapshot>() && call.Get<CallSnapshot>().message.count == 2 &&
          copiedItem.Get<MessageSnapshot>().content.view() == "item",
          "item-to-return transition keeps metadata without carrying text storage");
    call.SetKind(EventKind::OutboundItem);
    Check(call.Get<MessageSnapshot>().content.read.status == ReadStatus::NotRead &&
          call.Get<MessageSnapshot>().content.data()[0] == '\0',
          "re-entering item kind does not resurrect prior text");

    Event context(EventKind::SendContextEnter);
    context.header.callId = 81;
    auto& contextData = context.Get<ContextSnapshot>();
    contextData.context = 0x1234;
    memcpy(contextData.target.data(), "wxid", 4);
    contextData.target.read = {ReadStatus::Ok, true, 4, 4};
    context.SetKind(EventKind::SendContextReturn);
    Check(context.Kind() == EventKind::SendContextReturn && context.TryGet<ContextSnapshot>() &&
          context.header.callId == 81 && context.Get<ContextSnapshot>().context == 0x1234 &&
          context.Get<ContextSnapshot>().target.view() == "wxid",
          "context enter-to-return retains its typed capture");

    Event submit(EventKind::SendSubmitEnter);
    submit.header.callId = 82;
    auto& submitData = submit.Get<SubmitSnapshot>();
    submitData.context = 0x5678;
    auto& textSource = std::get<TextSubmitSnapshot>(submitData.source);
    memcpy(textSource.content.data(), "text", 4);
    textSource.content.read = {ReadStatus::Ok, true, 4, 4};
    submit.SetKind(EventKind::SendSubmitReturn);
    Check(submit.Kind() == EventKind::SendSubmitReturn && submit.TryGet<SubmitSnapshot>() &&
          submit.Get<SubmitSnapshot>().context == 0x5678 &&
          std::get<TextSubmitSnapshot>(submit.Get<SubmitSnapshot>().source).content.view() == "text",
          "submit enter-to-return retains context and text source");
    auto& imageSource = submit.Get<SubmitSnapshot>().source.emplace<ImageSubmitSnapshot>();
    memcpy(imageSource.path.data(), "image", 5);
    imageSource.path.read = {ReadStatus::Ok, true, 5, 5};
    imageSource.width = 640;
    Check(std::holds_alternative<ImageSubmitSnapshot>(submit.Get<SubmitSnapshot>().source) &&
          !std::holds_alternative<TextSubmitSnapshot>(submit.Get<SubmitSnapshot>().source) &&
          std::get<ImageSubmitSnapshot>(submit.Get<SubmitSnapshot>().source).path.view() == "image",
          "image source excludes text source fields");
    auto& voiceSource = submit.Get<SubmitSnapshot>().source.emplace<VoiceSubmitSnapshot>();
    voiceSource.prefix[0] = 0x02;
    voiceSource.length = 17;
    Check(std::holds_alternative<VoiceSubmitSnapshot>(submit.Get<SubmitSnapshot>().source) &&
          !std::holds_alternative<ImageSubmitSnapshot>(submit.Get<SubmitSnapshot>().source) &&
          std::get<VoiceSubmitSnapshot>(submit.Get<SubmitSnapshot>().source).length == 17 &&
          std::get<VoiceSubmitSnapshot>(submit.Get<SubmitSnapshot>().source).prefix[0] == 0x02,
          "voice source excludes image dimensions and path");
    const auto& newText = submit.Get<SubmitSnapshot>().source.emplace<TextSubmitSnapshot>();
    Check(newText.content.read.status == ReadStatus::NotRead && newText.content.data()[0] == '\0',
          "switching back to text does not expose a stale source buffer");

    Event queued(EventKind::Item);
    queued.header.callId = 91;
    auto& queuedText = queued.Get<MessageSnapshot>().content;
    memcpy(queuedText.data(), "old", 3);
    queuedText.read = {ReadStatus::Ok, true, 3, 3};
    Check(QueueEvent(queued), "typed item enters logger queue");
    memcpy(queuedText.data(), "new", 3);
    queued.SetKind(EventKind::Batch);
    Event popped;
    Check(PopEvent(popped) && popped.Kind() == EventKind::Item &&
          popped.header.callId == 91 && popped.Get<MessageSnapshot>().content.view() == "old",
          "logger queue owns a value snapshot after producer mutation");

    Event embedded(EventKind::Item);
    auto& content = embedded.Get<MessageSnapshot>().content;
    const char bytes[] = {'a', '\0', 'b'};
    memcpy(content.data(), bytes, sizeof(bytes));
    content.read = {ReadStatus::Truncated, true, 8, 3};
    const std::string itemJson = SerializeEvent(embedded);
    Check(itemJson.find("\"content\":\"a\\u0000b\"") != std::string::npos &&
          itemJson.find("\"content_read\":{\"status\":\"truncated\",\"original_bytes\":8,\"captured_bytes\":3}") != std::string::npos,
          "item JSON preserves embedded NUL and declared read state");
    Event reference(EventKind::SendSubmitEnter);
    auto& referenceSnapshot = reference.Get<SubmitSnapshot>();
    referenceSnapshot.sourceType = 49;
    referenceSnapshot.sourceSubtype = 57;
    auto& capturedReference = std::get<TextSubmitSnapshot>(referenceSnapshot.source).reference;
    auto& referenceText = capturedReference.messageStrings[3];
    memcpy(referenceText.data(), bytes, sizeof(bytes));
    referenceText.read = {ReadStatus::Truncated, true, 600, 3};
    const std::string referenceJson = SerializeEvent(reference);
    Check(referenceJson.find("\"value\":\"a\\u0000b\",\"read\":{\"status\":\"truncated\",\"original_bytes\":600,\"captured_bytes\":3}") != std::string::npos &&
          referenceJson.find("\"value\":\"\",\"read\":{\"status\":\"not_read\",\"original_bytes\":null,\"captured_bytes\":0}") != std::string::npos,
          "reference JSON binds text and status while preserving unknown reads");

    std::string wire;
    json::ObjectWriter fields(wire);
    fields.String("control", std::string_view("a\0b\n", 4));
    fields.NumberString("id", UINT64_MAX);
    fields.SignedString("signed", INT64_MIN);
    fields.Object("nested", [](json::ObjectWriter& nested) {
        nested.Null("absent");
        nested.Boolean("present", false);
    });
    fields.Array("empty", [](std::string&) {});
    fields.Close();
    Check(wire == "{\"control\":\"a\\u0000b\\n\",\"id\":\"18446744073709551615\",\"signed\":\"-9223372036854775808\",\"nested\":{\"absent\":null,\"present\":false},\"empty\":[]}",
          "shared JSON encoding preserves binary text, exact 64-bit IDs, nulls and ordered nesting");
}
