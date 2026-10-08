#pragma once
#include "monitor/config/version_profile.hpp"
#include <iterator>

namespace wechatbot::monitor {
inline constexpr char kMonitorBuild[] = "monitor-refactor-cpp";
inline constexpr size_t kMaxBatchItems = 1024;
inline constexpr size_t kMaxLoggedItemsPerBatch = 32;
inline constexpr size_t kIdCapacity = 256;
inline constexpr size_t kTextCapacity = 4096;
inline constexpr size_t kSourceCapacity = 8192;
inline constexpr size_t kQueueCapacity = 256;
inline constexpr wchar_t kObserverLogFilename[] = L"observer-4.1.13.12.jsonl";
} // namespace wechatbot::monitor
