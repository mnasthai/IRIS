#include "monitor/core/worker_thread.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/transport/command_pipe.hpp"
#include "monitor/runtime/runtime.hpp"
#include "monitor/media/media_receive_pipeline.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

void Check(bool condition, const char* message);
using namespace wechatbot::monitor;

namespace {
DWORD WINAPI WaitOnSignal(void* context) {
    WaitForSingleObject(static_cast<HANDLE>(context), INFINITE);
    return 0;
}
}

void TestWorkerLifecycle() {
    UniqueHandle gate(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(static_cast<bool>(gate), "create lifecycle test event");
    const HANDLE rawGate = gate.get();
    UniqueHandle moved(std::move(gate));
    Check(!gate && moved.get() == rawGate, "move transfers exclusive Win32 ownership");
    SetLastError(1234);
    { UniqueHandle temporary(CreateEventW(nullptr, FALSE, FALSE, nullptr)); SetLastError(1234); }
    Check(GetLastError() == 1234, "handle destruction preserves caller LastError");

    WorkerThread worker;
    Check(worker.Start(WaitOnSignal, moved.get()), "start worker with caller-owned context");
    Check(worker.Join(0) == WAIT_TIMEOUT && worker.HasThread(),
          "join timeout retains the live worker and its caller-owned context");
    Check(!worker.Start(WaitOnSignal, moved.get()), "worker cannot be replaced before join");
    SetEvent(moved.get());
    Check(worker.Join(2000) == WAIT_OBJECT_0 && !worker.HasThread(),
          "successful join releases only the finished worker handle");
    Check(worker.Join(0) == WAIT_OBJECT_0, "repeated join is idempotent");

    wchar_t temp[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "locate temporary logger test directory");
    const auto directory = std::filesystem::path(temp) /
        (L"wechatbot-logger-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto path = directory / L"events.jsonl";
    Event absent;
    while (PopEvent(absent)) {} // Ordinary capture tests may leave diagnostic events.
    Check(OpenLog(directory.wstring(), path.wstring()), "open isolated logger fixture");
    const Event event = MakeCallEvent(EventKind::Batch);
    for (size_t i = 0; i < kQueueCapacity; ++i)
        Check(QueueEvent(event), "bounded queue accepts exactly its available capacity");
    Check(!QueueEvent(event), "full observation queue rejects without blocking");
    Check(StartLogger(), "start logger after filling queue");
    Check(!CloseLog(), "a registered worker prevents premature log closure");
    Check(StartCommandPipe(true), "start isolated managed command pipe worker");
    SignalStop();
    Check(StopCommandPipe(2000) && StopCommandPipe(0),
          "command pipe cancels pending I/O and joins before log closure");
    Check(StopLogger(2000), "stop drains all accepted observations before joining");
    Check(!QueueEvent(event), "stop closes new queue admission");
    AppendDirect("{\"kind\":\"shutdown_tail\"}");
    Check(CloseLog(), "close file only after worker joined");
    AppendDirect("{\"kind\":\"after_close\"}");
    Check(StopLogger(0) && CloseLog(), "repeated worker/file cleanup is safe");

    std::ifstream file(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t batches = 0, position = 0;
    while ((position = text.find("\"kind\":\"batch\"", position)) != std::string::npos) { ++batches; ++position; }
    Check(batches == kQueueCapacity && text.find("\"kind\":\"dropped\"") != std::string::npos &&
          text.find("shutdown_tail") != std::string::npos && text.find("after_close") == std::string::npos,
          "shutdown preserves queued data, drop accounting and final direct evidence");
    file.close();

    // A held capture keeps the full runtime stop incomplete. The log must stay
    // open until that capture releases and a later shutdown attempt succeeds.
    InterlockedExchange(&g_stop, 0);
    Check(OpenLog(directory.wstring(), path.wstring()) && StartLogger(),
          "restart logger for supervised shutdown fixture");
    const auto mediaRoot = directory / L"media";
    Check(StartMediaReceivePipeline(mediaRoot.wstring()), "start media worker without native hooks");
    auto held = media_receive_detail::ReserveMediaSnapshot();
    Check(held && StartCommandPipe(true), "hold accepted capture while starting pipe worker");
    Check(!StopObserverWorkers(500), "runtime stop times out while accepted capture remains held");
    Check(!CloseLog() && !media_receive_detail::ReserveMediaSnapshot(),
          "incomplete runtime stop retains logger and closes media admission");
    AppendDirect("{\"kind\":\"stop_retry\"}");
    held.reset();
    Check(StopObserverWorkers(2000) && StopObserverWorkers(0),
          "retry finishes ordered runtime shutdown and repeated stop is safe");
    Check(!MediaReceivePipelineStarted() && !QueueEvent(event), "successful stop leaves workers closed");
    AppendDirect("{\"kind\":\"after_runtime_close\"}");
    std::ifstream finalFile(path, std::ios::binary);
    const std::string finalText((std::istreambuf_iterator<char>(finalFile)), std::istreambuf_iterator<char>());
    Check(finalText.find("stop_retry") != std::string::npos &&
          finalText.find("after_runtime_close") == std::string::npos,
          "timeout preserves final evidence until complete supervised closure");
    finalFile.close();
    std::filesystem::remove(mediaRoot);
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}
