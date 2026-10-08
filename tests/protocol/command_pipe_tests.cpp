#include "monitor/transport/command_pipe.hpp"
#include <string>
#include <utility>

void Check(bool condition, const char* message);
using namespace wechatbot::monitor;

void TestCommandProtocol() {
    const auto hello = HandleCommand(R"({"op":"hello","protocol_version":1,"request_id":"中文\ud83d\ude00"})", "test-session");
    Check(hello.find("\"send_text\":false") != std::string::npos &&
          hello.find("\"send_group_text\":false") != std::string::npos &&
          hello.find("\"send_mention\":false") != std::string::npos &&
          hello.find("\"send_quote\":false") != std::string::npos &&
          hello.find("\"account_verified\":false") != std::string::npos &&
          hello.find("中文😀") != std::string::npos, "hello is explicitly unavailable with UTF-8 request echo");
    const std::string send = R"({"op":"send_text","protocol_version":1,"request_id":"req","attempt_id":"try","observer_session_id":"test-session","expected_account_id":"wxid_test","target_id":"test@chatroom","text":"中文\n😀","created_at":"2026-09-17T12:00:00+00:00","expires_at":"2026-09-17T12:05:00+00:00","origin":"manual","source_event_key":null})";
    const auto mediaHello = HandleCommand(R"({"op":"hello_media","protocol_version":1,"request_id":"media-hello"})", "test-session");
    Check(mediaHello.find("\"op\":\"hello_media\"") != std::string::npos &&
          mediaHello.find("\"send_image\":false") != std::string::npos &&
          mediaHello.find("\"send_voice\":false") != std::string::npos &&
          hello.find("send_image") == std::string::npos,
          "opt-in media hello does not alter the legacy strict response");
    const std::string media = R"({"op":"send_media","protocol_version":1,"request_id":"media","attempt_id":"try","observer_session_id":"test-session","expected_account_id":"wxid_test","target_id":"test@chatroom","created_at":"2026-09-17T12:00:00+00:00","expires_at":"2026-09-17T12:05:00+00:00","origin":"manual","source_event_key":null,"media_kind":"voice","media_path":"E:\\spool\\file.silk","media_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","media_bytes":123,"duration_ms":2000})";
    Check(HandleCommand(media, "test-session").find("native_sender_unavailable") != std::string::npos,
          "media wire parsed but unavailable host never simulates sending");
    for (const auto* extra : {R"(,"text":"hidden")", R"(,"at_user_list":"wxid_one")"}) {
        auto mixed = media; mixed.insert(mixed.size() - 1, extra);
        Check(HandleCommand(mixed, "test-session").find("invalid_request") != std::string::npos,
              "media cannot smuggle text or mention metadata");
    }
    auto fractionalDuration = media;
    fractionalDuration.replace(fractionalDuration.find("2000"), 4, "2001");
    Check(HandleCommand(fractionalDuration, "test-session").find("invalid_request") != std::string::npos,
          "voice duration must use supported twenty-millisecond units");
    Check(HandleCommand(send, "test-session").find("native_sender_unavailable") != std::string::npos,
          "native send is not simulated even for valid commands");
    Check(HandleCommand(send, "other-session").find("session_mismatch") != std::string::npos,
          "send bound to different session rejected");
    auto rich = send;
    rich.replace(rich.find("send_text"), std::string("send_text").size(), "send_rich_text");
    rich.insert(rich.size() - 1, R"(,"at_user_list":"wxid_one,wxid_two")");
    Check(HandleCommand(rich, "test-session").find("native_sender_unavailable") != std::string::npos,
          "rich command parses without pretending a native test backend exists");
    auto accidental = send;
    accidental.insert(accidental.size() - 1, R"(,"at_user_list":"wxid_one")");
    Check(HandleCommand(accidental, "test-session").find("invalid_request") != std::string::npos,
          "plain operation cannot silently ignore mention metadata");
    const std::string quoteFields = R"(,"quote_message_id":"18446744073709551614","quote_from_id":"test@chatroom","quote_to_id":"wxid_test","quote_sender_id":"wxid_member","quote_conversation_id":"test@chatroom","quote_text":"被引用的完整正文🙂","quote_timestamp":1789702480,"quote_msg_source":"","quote_message_type":1)";
    auto quoted = send;
    quoted.replace(quoted.find("send_text"), std::string("send_text").size(), "send_rich_text");
    quoted.insert(quoted.size() - 1, quoteFields);
    Check(HandleCommand(quoted, "test-session").find("native_sender_unavailable") != std::string::npos,
          "quote-only command accepts exact uint64 string and empty msgsource");
    auto combined = rich;
    combined.insert(combined.size() - 1, quoteFields);
    Check(HandleCommand(combined, "test-session").find("native_sender_unavailable") != std::string::npos,
          "quote and member metadata coexist on rich commands");
    auto misspelled = combined;
    misspelled.replace(misspelled.find("at_user_list"), 12, "at_user_lsit");
    Check(HandleCommand(misspelled, "test-session").find("invalid_request") != std::string::npos,
          "unknown metadata cannot silently turn a combined request into quote-only");
    for (const auto* members : {"wxid_one,wxid_one", "filehelper", "group@chatroom", "wxid_one,"}) {
        auto invalidMembers = rich;
        invalidMembers.replace(invalidMembers.find("wxid_one,wxid_two"), 17, members);
        Check(HandleCommand(invalidMembers, "test-session").find("invalid_request") != std::string::npos,
              "invalid members rejected at the protocol boundary before native dispatch");
    }
    auto plainQuote = send;
    plainQuote.insert(plainQuote.size() - 1, quoteFields);
    Check(HandleCommand(plainQuote, "test-session").find("invalid_request") != std::string::npos,
          "plain operation cannot silently discard a quote");
    for (const auto* replacement : {"0", "018446744073709551614", "18446744073709551616", "+123"}) {
        auto invalidQuote = quoted;
        invalidQuote.replace(invalidQuote.find("18446744073709551614"), 20, replacement);
        Check(HandleCommand(invalidQuote, "test-session").find("invalid_request") != std::string::npos,
              "quote ID is a nonzero canonical uint64 string");
    }
    for (const auto& [field, replacement] : {
             std::pair{std::string("\"quote_message_id\":\"18446744073709551614\""), std::string("\"quote_message_id\":18446744073709551614")},
             std::pair{std::string("\"quote_timestamp\":1789702480"), std::string("\"quote_timestamp\":true")},
             std::pair{std::string("\"quote_timestamp\":1789702480"), std::string("\"quote_timestamp\":4294967296")},
             std::pair{std::string("\"quote_message_type\":1"), std::string("\"quote_message_type\":49")},
             std::pair{std::string("\"quote_to_id\""), std::string("\"quote_typo\"")},
             std::pair{std::string("\"quote_conversation_id\":\"test@chatroom\""), std::string("\"quote_conversation_id\":\"other@chatroom\"")}}) {
        auto invalidQuote = quoted;
        invalidQuote.replace(invalidQuote.find(field), field.size(), replacement);
        Check(HandleCommand(invalidQuote, "test-session").find("invalid_request") != std::string::npos,
              "quote wrong types, unknown or missing fields and cross-conversation references are rejected");
    }
    for (const auto* invalid : {
        R"({"op":"hello","op":"send_text","protocol_version":1,"request_id":"x"})",
        R"({"op":"hello","protocol_version":1,"request_id":"\ud800"})",
        R"({"op":"hello","protocol_version":1,"request_id":"\udc00"})",
        R"({"op":"hello","protocol_version":1,"request_id":"x\u0000"})",
        R"({"op":"hello","protocol_version":1,"request_id":"x","extra":[]})",
        R"({"op":"hello","protocol_version":1,"request_id":"x","extra":"unknown"})",
        R"({"op":"hello","protocol_version":1,"request_id":"x"} junk)",
        R"({"op":"hello","protocol_version":01,"request_id":"x"})",
        R"({"op":"hello","protocol_version":1.0,"request_id":"x"})"}) {
        Check(HandleCommand(invalid, "test-session").find("invalid_request") != std::string::npos,
              "ambiguous JSON or invalid Unicode rejected");
    }
    Check(HandleCommand(R"({"op":"hello","protocol_version":true,"request_id":"x"})", "test-session")
          .find("unsupported_protocol") != std::string::npos, "boolean is not protocol integer");
    Check(HandleCommand(std::string(65537, 'x'), "test-session").find("invalid_frame") != std::string::npos,
          "oversize payload rejected");
    Check(HandleCommand(std::string("\xc0\xaf", 2), "test-session").find("invalid_utf8") != std::string::npos,
          "noncanonical UTF-8 rejected");

    // Exercise the protocol without the process-wide native sender. Callback
    // counts and captured commands verify the boundary before any side effect.
    int probes = 0, sends = 0;
    NativeSendCapabilities capabilities;
    capabilities.accountId = "wxid_fake";
    capabilities.accountVerified = capabilities.sendText = capabilities.sendGroupText = true;
    capabilities.sendMention = capabilities.sendQuote = capabilities.sendImage = capabilities.sendVoice = true;
    capabilities.maxTextBytes = 1024;
    SendCommand captured;
    SendResult backendResult{"accepted", "", ""};
    const CommandHandlers handlers{
        [&] { ++probes; return capabilities; },
        [&](const SendCommand& command) { ++sends; captured = command; return backendResult; }};
    const auto enabledHello = HandleCommand(
        R"({"op":"hello","protocol_version":1,"request_id":"fake-hello"})", "test-session", handlers);
    Check(enabledHello ==
          R"({"op":"hello","protocol_version":1,"request_id":"fake-hello","observer_session_id":"test-session","target_version":"4.1.13.12","mode":"send_enabled","account_id":"wxid_fake","account_verified":true,"send_text":true,"send_group_text":true,"send_mention":true,"send_quote":true,"max_text_bytes":1024})" &&
          probes == 1 && sends == 0, "injected hello preserves fields and order and probes exactly once");
    const auto enabledMediaHello = HandleCommand(
        R"({"op":"hello_media","protocol_version":1,"request_id":"fake-hello"})", "test-session", handlers);
    auto expectedMediaHello = enabledHello;
    expectedMediaHello.replace(expectedMediaHello.find("\"hello\""), 7, "\"hello_media\"");
    expectedMediaHello.insert(expectedMediaHello.size() - 1, R"(,"send_image":true,"send_voice":true)");
    Check(enabledMediaHello == expectedMediaHello && probes == 2 && sends == 0,
          "injected media hello only appends the optional media capabilities");

    SendCommand expected;
    expected.requestId = "req"; expected.attemptId = "try"; expected.sessionId = "test-session";
    expected.accountId = "wxid_test"; expected.targetId = "test@chatroom"; expected.text = "中文\n😀";
    expected.createdAt = "2026-09-17T12:00:00+00:00"; expected.expiresAt = "2026-09-17T12:05:00+00:00";
    expected.origin = "manual";
    const auto accepted = HandleCommand(send, "test-session", handlers);
    Check(accepted ==
          R"({"op":"send_result","protocol_version":1,"request_id":"req","attempt_id":"try","observer_session_id":"test-session","status":"accepted","error_code":null,"error_detail":null})" &&
          captured == expected && sends == 1 && probes == 2,
          "text is completely decoded and sent once without probing or inventing a result");
    expected.atUserList = "wxid_one,wxid_two";
    HandleCommand(rich, "test-session", handlers);
    Check(captured == expected && sends == 2, "rich mention metadata reaches the handler exactly once");
    expected.atUserList.clear();
    expected.quote = QuoteText{18446744073709551614ULL, "test@chatroom", "wxid_test", "wxid_member",
        "test@chatroom", "被引用的完整正文🙂", "", 1789702480, 1};
    HandleCommand(quoted, "test-session", handlers);
    Check(captured == expected && sends == 3, "quote-only decoding preserves uint64, authorship and empty msgsource");
    expected.atUserList = "wxid_one,wxid_two";
    HandleCommand(combined, "test-session", handlers);
    Check(captured == expected && sends == 4, "combined quote and mentions remain one backend execution");
    expected.requestId = "media"; expected.text.clear(); expected.atUserList.clear(); expected.quote.reset();
    expected.media = MediaCommand{"voice", R"(E:\spool\file.silk)",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 123, 2000};
    HandleCommand(media, "test-session", handlers);
    Check(captured == expected && sends == 5, "media decoder preserves path, digest, bytes and duration without text");
    auto image = media;
    image.replace(image.find("\"voice\""), 7, "\"image\"");
    image.replace(image.find("file.silk"), 9, "file.png");
    image.replace(image.find("\"duration_ms\":2000"), 18, "\"duration_ms\":0");
    expected.media->kind = "image"; expected.media->path = R"(E:\spool\file.png)"; expected.media->durationMs = 0;
    HandleCommand(image, "test-session", handlers);
    Check(captured == expected && sends == 6, "image and voice share the same single execution boundary");

    backendResult = {"rejected", "fake_rejection", "No native send was attempted"};
    Check(HandleCommand(send, "test-session", handlers) ==
          R"({"op":"send_result","protocol_version":1,"request_id":"req","attempt_id":"try","observer_session_id":"test-session","status":"rejected","error_code":"fake_rejection","error_detail":"No native send was attempted"})" && sends == 7,
          "backend rejection is echoed without retry or status translation");
    backendResult = {"unknown", "fake_uncertain", "line\n\"quoted\"\\path"};
    Check(HandleCommand(send, "test-session", handlers) ==
          R"({"op":"send_result","protocol_version":1,"request_id":"req","attempt_id":"try","observer_session_id":"test-session","status":"unknown","error_code":"fake_uncertain","error_detail":"line\n\"quoted\"\\path"})" && sends == 8,
          "unknown and escaped backend detail are preserved without retry");
    Check(HandleCommand(send, "other-session", handlers) ==
          R"({"op":"send_result","protocol_version":1,"request_id":"req","attempt_id":"try","observer_session_id":"other-session","status":"rejected","error_code":"session_mismatch","error_detail":"No native send was attempted"})" && sends == 8,
          "session mismatch never invokes the injected backend");
    auto badOrigin = send;
    badOrigin.replace(badOrigin.find("\"manual\""), 8, "\"unsupported\"");
    Check(HandleCommand(badOrigin, "other-session", handlers) ==
          R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"invalid_request","error_detail":"Unsupported command origin"})" && sends == 8,
          "full decoding still precedes the session check");
    for (const auto& invalid : {misspelled, accidental, plainQuote, fractionalDuration,
            std::string(R"({"op":"hello","protocol_version":1,"request_id":"bad","extra":true})"),
            std::string(R"({"op":"send_text","protocol_version":1,"request_id":"bad"})"),
            std::string(R"({"op":"hello","op":"hello_media","protocol_version":1,"request_id":"bad"})"),
            std::string(R"({"op":"hello","protocol_version":2,"request_id":"bad"})"),
            std::string(R"({"op":"unknown","protocol_version":1,"request_id":"bad"})"),
            std::string("\xc0\xaf", 2), std::string{}, std::string(kCommandFrameLimit + 1, 'x')}) {
        Check(HandleCommand(invalid, "test-session", handlers).find("\"op\":\"error\"") != std::string::npos &&
              probes == 2 && sends == 8, "invalid requests invoke neither probe nor send");
    }
    // Keep precedence visible when multiple validation layers could reject the
    // same frame. Exact responses also pin request-ID availability and order.
    for (const auto& [payload, response] : {
        std::pair{R"({"op":"unknown","protocol_version":2,"request_id":"req","extra":[]})",
                  R"({"op":"error","protocol_version":1,"request_id":null,"error_code":"invalid_request","error_detail":"invalid flat JSON request"})"},
        std::pair{R"({"op":"unknown","protocol_version":2,"request_id":"","extra":true})",
                  R"({"op":"error","protocol_version":1,"request_id":null,"error_code":"invalid_request","error_detail":"invalid request_id"})"},
        std::pair{R"({"op":false,"protocol_version":2,"request_id":"req","extra":true})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"unsupported_protocol","error_detail":"Only command protocol 1 is supported"})"},
        std::pair{R"({"op":"hello","protocol_version":"1","request_id":"req"})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"unsupported_protocol","error_detail":"Only command protocol 1 is supported"})"},
        std::pair{R"({"op":false,"protocol_version":1,"request_id":"req","extra":true})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"invalid_request","error_detail":"invalid op"})"},
        std::pair{R"({"op":"unknown","protocol_version":1,"request_id":"req","extra":true})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"unsupported_operation","error_detail":"Unknown command operation"})"},
        std::pair{R"({"op":"send_rich_text","protocol_version":1,"request_id":"req","extra":true})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"invalid_request","error_detail":"unknown or unsupported request field"})"},
        std::pair{R"({"op":"send_rich_text","protocol_version":1,"request_id":"req","quote_message_id":1})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"invalid_request","error_detail":"invalid quote_message_id"})"},
        std::pair{R"({"op":"send_rich_text","protocol_version":1,"request_id":"req"})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"invalid_request","error_detail":"Rich text requires mention or quote metadata"})"},
        std::pair{R"({"op":"send_text","protocol_version":1,"request_id":"req"})",
                  R"({"op":"error","protocol_version":1,"request_id":"req","error_code":"invalid_request","error_detail":"invalid attempt_id"})"},
        std::pair{R"({"op":"hello","protocol_version":1e0,"request_id":"req"})",
                  R"({"op":"error","protocol_version":1,"request_id":null,"error_code":"invalid_request","error_detail":"invalid flat JSON request"})"},
        std::pair{R"({"op":"hello","protocol_version":-1,"request_id":"req"})",
                  R"({"op":"error","protocol_version":1,"request_id":null,"error_code":"invalid_request","error_detail":"invalid flat JSON request"})"}}) {
        Check(HandleCommand(payload, "other-session", handlers) == response && probes == 2 && sends == 8,
              "error precedence, exact diagnostics and absent side effects remain stable");
    }
    std::string bounded = R"({"op":"hello","protocol_version":1,"request_id":"bound")";
    for (int index = 0; index < 29; ++index)
        bounded += ",\"extra" + std::to_string(index) + "\":null";
    bounded += "}";
    Check(HandleCommand(bounded, "test-session", handlers) ==
          R"({"op":"error","protocol_version":1,"request_id":"bound","error_code":"invalid_request","error_detail":"unknown or unsupported request field"})",
          "thirty-two flat fields reach semantic validation");
    bounded.insert(bounded.size() - 1, R"(,"overflow":null)");
    Check(HandleCommand(bounded, "test-session", handlers) ==
          R"({"op":"error","protocol_version":1,"request_id":null,"error_code":"invalid_request","error_detail":"invalid flat JSON request"})" &&
          probes == 2 && sends == 8, "the thirty-third field is rejected by the bounded parser");
    Check(HandleCommand(R"({"op":"hello","protocol_version":1,"request_id":"empty"})",
                        "test-session", CommandHandlers{}) ==
          R"({"op":"hello","protocol_version":1,"request_id":"empty","observer_session_id":"test-session","target_version":"4.1.13.12","mode":"read_only","account_id":null,"account_verified":false,"send_text":false,"send_group_text":false,"send_mention":false,"send_quote":false,"max_text_bytes":16384})",
          "read-only hello preserves null account, numeric limit and complete field order");
    Check(HandleCommand(send, "test-session", CommandHandlers{}).find("native_sender_unavailable") != std::string::npos &&
          HandleCommand(R"({"op":"hello","protocol_version":1,"request_id":"empty"})", "test-session", CommandHandlers{})
              .find("\"send_text\":false") != std::string::npos,
          "missing handlers preserve explicit read-only behavior");
}
