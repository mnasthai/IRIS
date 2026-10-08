#pragma once
#include "monitor/core/platform.hpp"
#include "monitor/core/read_result.hpp"
#include "monitor/config/version_profile.hpp"

namespace wechatbot::monitor {
bool IsReadableRange(const void* address, size_t length);
TextRead CopyNativeString(const void* object, char* destination, size_t capacity);
// Binary data is never UTF-8 validated; capacity includes every usable byte.
TextRead CopyNativeBinary(const void* object, uint8_t* destination, size_t capacity);
// UTF-16 native string (SSO < 8 code units) converted to bounded UTF-8.
// originalBytes/capturedBytes both refer to UTF-8 output, not UTF-16 units.
TextRead CopyNativeWideString(const void* object, char* destination, size_t capacity);
TextRead CopyMessageString(const uint8_t* item, size_t objectSize, size_t bitsOffset,
                          size_t offset, uint32_t bit, bool wrapped, char* destination, size_t capacity);
TextRead CopyField(const uint8_t* item, MessageLayout layout, StringField field,
                   char* destination, size_t capacity);
void CopyScalars(const uint8_t* item, uint32_t hasBits, const ScalarField* fields,
                 size_t count, uint64_t* output);
} // namespace wechatbot::monitor
