#include "monitor/send/send_gate.hpp"
#include "monitor/send/recipient.hpp"
#include <stdexcept>
using namespace wechatbot::monitor;
void Check(bool, const char*);
void TestSendGate() {
    for (const auto* id : {"wxid_friend", "filehelper", "123456@chatroom", "test_room@chatroom"})
        Check(IsValidRecipient(id), "private and chatroom IDs validate");
    for (const auto* id : {"", "@chatroom", "group@@chatroom", "friend@other", "group@Chatroom",
                          "group@chatroom;wxid_other", "group name", "group\n@chatroom"})
        Check(!IsValidRecipient(id), "invalid recipient shape rejected");
    Check(!IsValidRecipient(std::string(129, 'x') + "@chatroom") &&
          IsValidRecipient(std::string(128, 'x') + "@chatroom"), "recipient prefix length bounded");
    uint64_t now = 0, alternate = 0;
    Check(ParseCommandTime("2026-09-17T12:00:01.123456Z", now) &&
          ParseCommandTime("2026-09-17T12:00:01.123456+00:00", alternate) && now == alternate,
          "UTC timestamps agree including microseconds");
    for (const auto* value : {"2026-02-29T12:00:00Z", "2026-09-17T24:00:00Z",
            "2026-09-17T12:00:00.Z", "2026-09-17T12:00:00.1234567Z", "2026-09-17T12:00:00+08:00"})
        Check(!ParseCommandTime(value, alternate), "invalid or non-UTC command timestamp rejected");
    SendCommand command{"r", "a", "s", "wxid_self", "wxid_friend", "你好🙂",
        "2026-09-17T12:00:00.000000Z", "2026-09-17T12:05:00.000000Z", "manual", ""};
    SendPolicy policy{true, command.accountId, command.targetId, command.text};
    SendGate gate(policy);
    unsigned calls = 0;
    const auto invoke = [&] { ++calls; return SendResult{"accepted", "", ""}; };
    auto bad = command; bad.targetId = "group@chatroom";
    Check(gate.Run(bad, now, invoke).code == "target_not_allowed" && gate.Available(),
          "wrong target does not consume reservation");
    bad = command; bad.expiresAt = bad.createdAt;
    Check(gate.Run(bad, now, invoke).code == "invalid_timestamp", "invalid lifetime rejected");
    bad = command; bad.expiresAt = "2026-09-17T12:00:01Z";
    Check(gate.Run(bad, now, invoke).code == "command_expired", "expired text never reaches backend");
    bad = command; bad.expiresAt = "2026-09-17T12:10:01Z";
    Check(gate.Run(bad, now, invoke).code == "invalid_timestamp",
          "command lifetime over 600 seconds is rejected before reservation");
    bad = command; bad.text += "x";
    Check(gate.Run(bad, now, invoke).code == "text_not_allowed" && calls == 0,
          "configured exact text enforced before side effect");
    Check(gate.Run(command, now, invoke).status == "accepted" && calls == 1 && !gate.Available(),
          "one valid request reserves the only native attempt");
    Check(gate.Run(command, now, invoke).status == "accepted" && calls == 1,
          "exact duplicate returns prior result without native re-entry");
    bad = command; bad.attemptId = "second";
    Check(gate.Run(bad, now, invoke).code == "request_conflict", "different attempt cannot retry reserved request");
    bad = command; bad.requestId = "second";
    Check(gate.Run(bad, now, invoke).code == "single_send_limit", "second command cannot start");
    SendGate exceptional(policy);
    Check(exceptional.Run(command, now, []() -> SendResult { throw std::runtime_error("test"); }).status == "unknown" &&
          exceptional.Run(command, now, invoke).status == "unknown" && calls == 1,
          "exception keeps reservation and repeated command unknown without retry");

    SendPolicy continuous;
    continuous.enabled = true;
    continuous.accountId = command.accountId;
    continuous.continuous = true;
    continuous.targetIds = {command.targetId, "wxid_second"};
    continuous.minIntervalMs = 1000;
    continuous.maxRecords = 2;
    SendGate stream(continuous);
    auto first = command;
    first.requestId = "continuous-1"; first.attemptId = "attempt-1"; first.origin = "ai";
    first.text = std::string(1100, 'x');
    auto second = first;
    second.requestId = "continuous-2"; second.attemptId = "attempt-2";
    second.targetId = "wxid_second"; second.origin = "game"; second.text = "second";
    unsigned continuousCalls = 0;
    const auto continuousSubmit = [&] { ++continuousCalls; return SendResult{"accepted", "", ""}; };
    Check(stream.Available(now, 100) &&
          stream.Run(first, now, 100, continuousSubmit).status == "accepted" &&
          continuousCalls == 1 && !stream.Available(now, 1099),
          "continuous mode accepts non-exact AI text and enters monotonic cooldown");
    Check(stream.Run(second, now, 1099, continuousSubmit).code == "rate_limited" &&
          continuousCalls == 1 &&
          stream.Run(second, now, 1100, continuousSubmit).status == "accepted" &&
          continuousCalls == 2,
          "rate-limited request is not recorded and succeeds after cooldown");
    Check(stream.Run(second, now, 1100, continuousSubmit).status == "accepted" &&
          continuousCalls == 2,
          "exact continuous duplicate returns its stored result without re-entry");
    auto conflict = second; conflict.text = "different";
    Check(stream.Run(conflict, now, 2100, continuousSubmit).code == "request_conflict" &&
          continuousCalls == 2,
          "same continuous request ID with different content is rejected");
    auto capacity = second; capacity.requestId = "continuous-capacity";
    Check(stream.Run(capacity, now, 2100, continuousSubmit).code == "capacity_unavailable" &&
          continuousCalls == 2,
          "unexpired deduplication records are never evicted at capacity");

    uint64_t cleanupNow = 0;
    Check(ParseCommandTime("2026-09-17T12:06:01Z", cleanupNow), "cleanup time parses");
    Check(stream.Run(first, cleanupNow, 3000, continuousSubmit).code == "command_expired" &&
          continuousCalls == 2,
          "expired original request is rejected instead of replaying its stored result");
    auto third = command;
    third.requestId = "continuous-3"; third.attemptId = "attempt-3";
    third.createdAt = "2026-09-17T12:06:00Z";
    third.expiresAt = "2026-09-17T12:07:00Z";
    Check(stream.Available(cleanupNow, 3000) &&
          stream.Run(third, cleanupNow, 3000, continuousSubmit).status == "accepted" &&
          continuousCalls == 3,
          "records retire after expiry plus 60 seconds and continuous sending resumes");

    SendGate serialized(continuous);
    auto nested = second;
    nested.requestId = "nested"; nested.attemptId = "nested-attempt";
    SendResult nestedResult;
    Check(serialized.Run(first, now, 100, [&] {
              nestedResult = serialized.Run(nested, now, 1100, continuousSubmit);
              return SendResult{"accepted", "", ""};
          }).status == "accepted" && nestedResult.code == "busy",
          "continuous gate permits only one native submit at a time");

    auto groupPolicy = continuous;
    groupPolicy.targetIds = {"123456@chatroom", "wxid_friend"};
    SendGate groupGate(groupPolicy);
    auto group = command;
    group.targetId = "123456@chatroom";
    group.requestId = "group-1";
    group.text = "群聊文本\n你好🙂";
    unsigned groupCalls = 0;
    const auto groupSubmit = [&] { ++groupCalls; return SendResult{"accepted", "", ""}; };
    Check(groupGate.Available(now, 100) &&
          groupGate.Run(group, now, 100, groupSubmit).status == "accepted" && groupCalls == 1,
          "allowlisted chatroom uses continuous text path");
    Check(groupGate.Run(group, now, 100, groupSubmit).status == "accepted" && groupCalls == 1,
          "group duplicate does not re-enter native send");
    auto wrongGroup = group;
    wrongGroup.requestId = "group-2"; wrongGroup.targetId = "654321@chatroom";
    Check(groupGate.Run(wrongGroup, now, 1100, groupSubmit).code == "target_not_allowed" && groupCalls == 1,
          "other chatrooms are not implicitly allowed");
    auto singleGroup = policy;
    Check(IsValidMentionList("wxid_one,wxid_two") && !IsValidMentionList("wxid_one,") &&
          !IsValidMentionList("wxid_one,wxid_one") && !IsValidMentionList("@all") &&
          !IsValidMentionList("filehelper") && !IsValidMentionList("123@chatroom"),
          "mentions accept distinct member IDs only");
    auto mention = group;
    mention.requestId = "mention-1"; mention.atUserList = "wxid_one,wxid_two";
    Check(groupGate.Run(mention, now, 1100, groupSubmit).status == "accepted" && groupCalls == 2,
          "continuous group can carry structured mention metadata");
    mention.atUserList = "wxid_one";
    Check(groupGate.Run(mention, now, 2100, groupSubmit).code == "request_conflict",
          "changed mention metadata cannot reuse a request ID");
    mention.requestId = "invalid-mention"; mention.targetId = "wxid_friend";
    Check(groupGate.Run(mention, now, 2100, groupSubmit).code == "invalid_mentions",
          "mentions cannot silently become a private send");
    singleGroup.targetId = group.targetId; singleGroup.text = group.text;
    SendGate quoteDisabled(groupPolicy);
    auto quotePolicy = groupPolicy;
    quotePolicy.experimentalQuote = true;
    SendGate quoteGate(quotePolicy);
    auto quoteCommand = group;
    quoteCommand.requestId = "quote-1";
    quoteCommand.quote = QuoteText{18446744073709551614ULL, "123456@chatroom", "wxid_self",
        "wxid_member", "123456@chatroom", "原文🙂", "", 1789702480, 1};
    const auto callsBeforeQuote = groupCalls;
    Check(quoteDisabled.Run(quoteCommand, now, 100, groupSubmit).code == "native_quote_sender_unavailable" &&
          groupCalls == callsBeforeQuote && quoteDisabled.Available(now, 100),
          "disabled quote never enters native code or consumes the plain-text send slot");
    Check(quoteGate.Run(quoteCommand, now, 100, groupSubmit).status == "accepted",
          "reference metadata follows the existing serialized send path");
    quoteCommand.quote->text = "另一条原文";
    Check(quoteGate.Run(quoteCommand, now, 1100, groupSubmit).code == "request_conflict",
          "changing only the quote cannot replay a prior request ID");
    quoteCommand.requestId = "quote-2";
    quoteCommand.quote->conversationId = "other@chatroom";
    Check(quoteGate.Run(quoteCommand, now, 1100, groupSubmit).code == "invalid_quote",
          "quote must belong to its command conversation");
    quoteCommand.quote->conversationId = group.targetId;
    quoteCommand.quote->messageId = 0;
    Check(quoteGate.Run(quoteCommand, now, 1100, groupSubmit).code == "invalid_quote",
          "zero reference ID never reaches the native sender");
    SendGate singleGroupGate(singleGroup);
    Check(!singleGroupGate.Available(now, 100) &&
          singleGroupGate.Run(group, now, 100, groupSubmit).code == "target_not_allowed",
          "legacy single-private-test mode does not acquire group capability");
    auto mediaCommand = group;
    mediaCommand.requestId = "media-1"; mediaCommand.text.clear();
    mediaCommand.media = MediaCommand{"image", "E:\\spool\\image.png", std::string(64, 'a'), 123, 0};
    SendGate mediaDisabled(groupPolicy);
    Check(mediaDisabled.Run(mediaCommand, now, 100, groupSubmit).code == "native_media_sender_unavailable",
          "media requires explicit native enablement before reservation");
    auto mediaPolicy = groupPolicy; mediaPolicy.mediaEnabled = true;
    SendGate mediaGate(mediaPolicy);
    const auto beforeMedia = groupCalls;
    Check(mediaGate.Run(mediaCommand, now, 100, groupSubmit).status == "accepted" &&
          mediaGate.Run(mediaCommand, now, 100, groupSubmit).status == "accepted" && groupCalls == beforeMedia + 1,
          "media shares strict request identity and single native attempt");
    mediaCommand.media->sha256[0] = 'b';
    Check(mediaGate.Run(mediaCommand, now, 1100, groupSubmit).code == "request_conflict",
          "same request cannot silently replace media bytes");
    mediaCommand.requestId = "media-2"; mediaCommand.text = "hidden";
    Check(mediaGate.Run(mediaCommand, now, 1100, groupSubmit).code == "invalid_media",
          "mixed media and text rejected before native code");
}
