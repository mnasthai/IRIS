// Deterministic service ports only: no hooks, native addresses, Weixin process,
// real media files, network, or database. Deferred actions are finished before
// their stack-owned test service and port state are destroyed.
#include "monitor/send/send_service.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

using namespace wechatbot::monitor;
void Check(bool, const char*);

namespace {
enum class DispatchMode { complete, cancel, startedTimeout };
struct Record { std::string kind, status, reason; bool native; };

SendCommand Command() {
    return {"service-r", "service-a", "service-s", "wxid_self", "wxid_friend", "owned text",
            "2026-09-17T12:00:00Z", "2026-09-17T12:05:00Z", "manual", ""};
}
SendPolicy Policy() {
    SendPolicy policy;
    policy.enabled = true;
    policy.accountId = "wxid_self";
    policy.continuous = true;
    policy.targetIds = {"wxid_friend", "123456@chatroom"};
    policy.minIntervalMs = 0;
    policy.experimentalQuote = true;
    policy.mediaEnabled = true;
    return policy;
}
SendCommand ImageCommand() {
    auto command = Command();
    command.text.clear();
    command.media = MediaCommand{"image", "E:\\fixture\\image.png", std::string(64, 'a'), 123, 0};
    return command;
}

struct FakePorts {
    uint64_t now = 0, monotonic = 100;
    bool stopped = false, senderMatches = true, mediaMatches = true, throwSubmit = false;
    DWORD thread = 17;
    NativeSessionSnapshot session{"wxid_self", true, true, "ok"};
    DispatchMode mode = DispatchMode::complete;
    unsigned dispatches = 0, sessions = 0, preparations = 0, submits = 0, starts = 0;
    std::string prepareError, observedText;
    NativeAttemptResult native{NativeAttemptStatus::start_returned, NativeSubmitStage::start,
                               "fake-uuid", true, true, true};
    std::function<void()> beforeGuard, deferred;
    std::weak_ptr<PreparedMediaFile> preparedLifetime;
    std::vector<Record> records;

    FakePorts() {
        Check(ParseCommandTime("2026-09-17T12:00:01Z", now), "service fixture clock is valid UTC");
    }
    SendServicePorts Ports() {
        SendServicePorts ports;
        ports.now = [this] { return now; };
        ports.monotonicMs = [this] { return monotonic; };
        ports.stopped = [this] { return stopped; };
        ports.senderMatches = [this] { return senderMatches; };
        ports.mediaMatches = [this] { return mediaMatches; };
        ports.bindUi = [this] { return thread; };
        ports.dispatch = [this](std::function<void()> action, DWORD timeout) {
            ++dispatches;
            Check(timeout == 1000 || timeout == 1500, "service preserves probe and attempt wait limits");
            if (deferred) return UiDispatchResult{UiDispatchStatus::busy, 0, ERROR_BUSY};
            if (mode == DispatchMode::cancel)
                return UiDispatchResult{UiDispatchStatus::cancelled_before_start, 0, ERROR_TIMEOUT};
            if (mode == DispatchMode::startedTimeout) {
                // Model an admitted action paused on its UI thread at action
                // entry. The port owns it until CompleteDeferred is called.
                deferred = std::move(action);
                return UiDispatchResult{UiDispatchStatus::timeout_after_start, thread, ERROR_TIMEOUT};
            }
            try { action(); }
            catch (...) { return UiDispatchResult{UiDispatchStatus::exception, thread, ERROR_UNHANDLED_EXCEPTION}; }
            return UiDispatchResult{UiDispatchStatus::completed, thread, ERROR_SUCCESS};
        };
        ports.readSession = [this](DWORD ui) {
            Check(ui == thread, "session reads use only the currently authorized UI thread");
            ++sessions;
            return session; // Value snapshot, never a borrowed native pointer.
        };
        ports.prepareMedia = [this](const MediaCommand& command) {
            ++preparations;
            if (!prepareError.empty()) return MediaFileResult{nullptr, prepareError};
            auto file = std::make_shared<PreparedMediaFile>();
            file->path.assign(command.path.begin(), command.path.end());
            file->audio = "binary fixture";
            file->durationMs = command.durationMs;
            preparedLifetime = file;
            return MediaFileResult{std::move(file), ""};
        };
        ports.submit = [this](const SendCommand& command, DWORD ui,
                               const std::shared_ptr<PreparedMediaFile>& media,
                               const std::function<bool()>& guard) {
            Check(ui == thread && (command.media.has_value() == static_cast<bool>(media)),
                  "one native attempt receives owned command and prepared input");
            ++submits;
            observedText = command.text;
            if (beforeGuard) beforeGuard();
            if (!guard()) return NativeAttemptResult{NativeAttemptStatus::preparation_failed,
                NativeSubmitStage::before_start, "fake-uuid", false, false, true};
            ++starts;
            if (throwSubmit) throw std::runtime_error("fake action failure");
            return native;
        };
        ports.log = [this](const char* kind, const std::string&, const std::string&,
                           const SendResult& result, const NativeAttemptResult* attempt) {
            records.push_back({kind, result.status, result.code, attempt != nullptr});
        };
        return ports;
    }
    void CompleteDeferred() {
        auto action = std::move(deferred);
        deferred = {};
        Check(static_cast<bool>(action), "there is one deferred action to finish");
        action();
    }
};
} // namespace

void TestSendService() {
    {
        FakePorts fake;
        SendService service(Policy(), fake.Ports());
        const auto capability = service.Probe();
        Check(capability.accountVerified && capability.sendText && capability.sendGroupText &&
              capability.sendMention && capability.sendQuote && capability.sendImage && capability.sendVoice,
              "verified session and explicit policy form capabilities through UI dispatch");
        const auto command = Command();
        const auto result = service.Run(command);
        Check(result.status == "accepted" && fake.submits == 1 && fake.starts == 1 &&
              fake.records.size() == 2 && fake.records[0].kind == "native_send_result" &&
              fake.records[0].native && fake.records[1].kind == "native_send_request",
              "one accepted native attempt preserves result-before-request evidence order");
        Check(service.Run(command).status == "accepted" && fake.submits == 1,
              "exact service duplicate returns cached result without native re-entry");
        auto conflict = command; conflict.attemptId = "changed";
        Check(service.Run(conflict).code == "request_conflict" && fake.submits == 1,
              "changed attempt identity never retries the accepted command");
    }
    {
        FakePorts fake;
        fake.mode = DispatchMode::cancel;
        SendService service(Policy(), fake.Ports());
        Check(service.Run(Command()).code == "ui_dispatch_not_started" && !fake.submits &&
              fake.records.size() == 1 && service.WaitIdle(0),
              "pending timeout starts nothing, releases its lease and reaches idle");
    }
    {
        FakePorts fake;
        fake.mode = DispatchMode::startedTimeout;
        SendService service(Policy(), fake.Ports());
        auto command = Command();
        Check(service.Run(command).code == "ui_execution_uncertain" && !fake.submits,
              "started timeout returns unknown without reading uncompleted action output");
        command.text = "caller storage changed after return";
        Check(!service.WaitIdle(0), "started timeout retains outstanding action until actual completion");
        fake.CompleteDeferred();
        Check(fake.observedText == "owned text" && fake.submits == 1 && fake.starts == 1 && service.WaitIdle(0),
              "timed-out action owns its original input and releases idle lease only after completion");
        Check(service.Run(Command()).status == "unknown" && fake.submits == 1,
              "late completion does not upgrade cached unknown or trigger duplicate retry");
        auto next = Command(); next.requestId = "after-drain";
        Check(service.Run(next).code == "ui_dispatch_not_started" && fake.submits == 1,
              "successful idle barrier closes admission to later actions");
    }
    {
        FakePorts fake;
        fake.mode = DispatchMode::startedTimeout;
        SendService service(Policy(), fake.Ports());
        const auto capability = service.Probe();
        Check(!capability.accountVerified && !capability.sendText && !fake.sessions && !service.WaitIdle(0),
              "probe timeout does not inspect or publish a still-pending snapshot");
        fake.CompleteDeferred();
        Check(service.WaitIdle(0), "probe capture also participates in the idle barrier");
    }
    {
        FakePorts fake;
        fake.mode = DispatchMode::startedTimeout;
        SendService service(Policy(), fake.Ports());
        Check(service.Run(Command()).status == "unknown", "shutdown fixture owns a started action");
        fake.stopped = true;
        Check(!service.WaitIdle(0), "stop does not discard an outstanding action before it finishes");
        fake.CompleteDeferred();
        Check(!fake.submits && service.WaitIdle(0),
              "late UI action sees stop before preparation and then releases the shutdown barrier");
    }
    {
        FakePorts fake;
        fake.session.accountId = "wxid_other";
        SendService service(Policy(), fake.Ports());
        Check(service.Run(Command()).code == "account_not_verified" && !fake.submits,
              "changed account is rejected on UI thread before any source preparation");
    }
    for (int change = 0; change != 3; ++change) {
        FakePorts fake;
        fake.beforeGuard = [&fake, change] {
            if (change == 0) fake.session.accountId = "wxid_other";
            else if (change == 1) Check(ParseCommandTime("2026-09-17T12:05:00Z", fake.now), "expire before Start");
            else fake.stopped = true;
        };
        SendService service(Policy(), fake.Ports());
        Check(service.Run(Command()).code == "native_preparation_failed" && fake.submits == 1 &&
              !fake.starts && fake.sessions == 2,
              "fresh beforeStart account, expiry and stop checks prevent native Start");
    }
    {
        FakePorts fake;
        fake.native = {NativeAttemptStatus::unknown_after_start, NativeSubmitStage::start,
                       "fake-uuid", true, false, false};
        SendService service(Policy(), fake.Ports());
        Check(service.Run(Command()).code == "native_start_uncertain" &&
              service.Run(Command()).status == "unknown" && fake.submits == 1 && fake.starts == 1,
              "uncertainty after native Start remains cached and never retries");
    }
    {
        FakePorts fake;
        fake.throwSubmit = true;
        SendService service(Policy(), fake.Ports());
        Check(service.Run(Command()).code == "ui_execution_uncertain" &&
              service.Run(Command()).status == "unknown" && fake.submits == 1 && service.WaitIdle(0),
              "UI exception remains unknown, releases lease and never retries");
    }
    {
        FakePorts fake;
        fake.mediaMatches = false;
        SendService service(Policy(), fake.Ports());
        const auto capability = service.Probe();
        Check(capability.sendText && !capability.sendImage &&
              service.Run(ImageCommand()).code == "native_media_sender_unavailable" &&
              !fake.preparations && !fake.submits,
              "media signature mismatch leaves text capability and blocks media before preparation");
    }
    {
        FakePorts fake;
        fake.prepareError = "media_hash_mismatch";
        SendService service(Policy(), fake.Ports());
        Check(service.Run(ImageCommand()).code == fake.prepareError && fake.preparations == 1 && !fake.submits,
              "media preparation error preserves wire reason before UI native execution");
    }
    {
        FakePorts fake;
        SendService service(Policy(), fake.Ports());
        Check(service.Run(ImageCommand()).status == "accepted" && !fake.preparedLifetime.expired() &&
              service.WaitIdle(0) && !fake.preparedLifetime.expired(),
              "image read handle remains owned after Start, action completion and idle barrier");
    }
    {
        FakePorts fake;
        auto policy = Policy(); policy.maxRecords = 5000;
        SendService service(policy, fake.Ports());
        auto command = ImageCommand();
        Check(service.Run(command).status == "accepted", "retain first distinct image path");
        const auto firstLifetime = fake.preparedLifetime;
        for (unsigned i = 1; i < 4096; ++i) {
            command.requestId = "image-" + std::to_string(i);
            command.media->path = "E:\\fixture\\image-" + std::to_string(i) + ".png";
            Check(service.Run(command).status == "accepted", "retain image paths up to existing process limit");
        }
        command.requestId = "image-over-limit";
        command.media->path = "E:\\fixture\\image-over-limit.png";
        Check(service.Run(command).code == "native_preparation_failed" && fake.starts == 4096 &&
              !firstLifetime.expired() && fake.preparedLifetime.expired(),
              "4097th image path is rejected before Start without evicting or closing retained paths");
    }
}
