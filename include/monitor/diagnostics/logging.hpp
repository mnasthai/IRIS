#pragma once
#include "monitor/diagnostics/event.hpp"

namespace wechatbot::monitor {
// Hook producers only enqueue bounded snapshots. The logger owns all file I/O.
bool QueueEvent(const Event& event);
bool PopEvent(Event& event);
bool OpenLog(const std::wstring& directory, const std::wstring& path);
bool StartLogger();
// Stop rejects new queued events, drains existing events and waits. A timeout
// retains live thread/event/file resources; retry Join outside DllMain.
bool StopLogger(DWORD timeoutMs) noexcept;
// Only legal after StopLogger succeeds and other output producers are joined.
bool CloseLog() noexcept;
void AppendDirect(const char* jsonRecord);
} // namespace wechatbot::monitor
