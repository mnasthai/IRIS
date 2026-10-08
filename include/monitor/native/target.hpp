#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/config/version_config.hpp"
#include <string>

namespace wechatbot::monitor {
extern HMODULE g_weixin;
bool VerifyTarget(HMODULE module, std::string& reason);
bool VerifyEntry(const void* address, const uint8_t* expected, size_t length);
} // namespace wechatbot::monitor
