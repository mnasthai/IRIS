#include "monitor/core/memory.hpp"
#include <cstring>
#include <string>
using namespace wechatbot::monitor;
void Check(bool condition, const char* message);
namespace {
template<class T> void Put(void* memory, size_t offset, const T& value) {
    memcpy(static_cast<uint8_t*>(memory) + offset, &value, sizeof(value));
}
void Wide(void* native, const std::wstring& value) {
    memset(native, 0, 0x20);
    const uint64_t size = value.size(), capacity = size < 8 ? 7 : size;
    if (size < 8) memcpy(native, value.data(), static_cast<size_t>(size) * 2);
    else { const auto* pointer = value.data(); Put(native, 0, pointer); }
    Put(native, 0x10, size); Put(native, 0x18, capacity);
}
}
void TestMediaMemory() {
    alignas(8) uint8_t native[0x20]{};
    char output[256]{};
    const std::wstring shortPath = L"图😀";
    Wide(native, shortPath);
    auto read = CopyNativeWideString(native, output, sizeof(output));
    Check(read.status == ReadStatus::Ok && read.originalBytes == 7 && std::string(output) == "图😀",
          "native wide inline Unicode and surrogate pair");
    read = CopyNativeWideString(native, output, 7);
    Check(read.status == ReadStatus::Truncated && read.originalBytes == 7 && read.capturedBytes == 3 &&
          std::string(output) == "图", "wide snapshot never cuts a UTF-8 code point");
    const std::wstring heapPath = L"E:\\图片目录\\测试照片.png";
    Wide(native, heapPath);
    read = CopyNativeWideString(native, output, sizeof(output));
    Check(read.status == ReadStatus::Ok && std::string(output) == "E:\\图片目录\\测试照片.png",
          "native wide heap path converted using UTF-16 size");
    Put(native, 0, uintptr_t{1});
    Check(CopyNativeWideString(native, output, sizeof(output)).status == ReadStatus::Unreadable,
          "unreadable wide pointer rejected");
    const std::wstring invalid(1, wchar_t{0xD800});
    Wide(native, invalid);
    read = CopyNativeWideString(native, output, sizeof(output));
    Check(read.status == ReadStatus::InvalidLayout && !read.capturedBytes && !output[0],
          "unpaired UTF-16 surrogate rejected");
    Put(native, 0x10, uint64_t{32769}); Put(native, 0x18, uint64_t{32769});
    Check(CopyNativeWideString(native, output, sizeof(output)).status == ReadStatus::InvalidLayout,
          "wide reader limits conversion work");
    memset(native, 0, sizeof(native));
    native[0] = 0; native[1] = 0xFF; native[2] = 0xC0;
    Put(native, 0x10, uint64_t{3}); Put(native, 0x18, uint64_t{15});
    uint8_t binary[3]{};
    read = CopyNativeBinary(native, binary, sizeof(binary));
    Check(read.status == ReadStatus::Ok && read.capturedBytes == 3 && binary[1] == 0xFF && binary[2] == 0xC0,
          "binary reader preserves NUL and invalid UTF-8 without reserving terminator");
    read = CopyNativeBinary(native, binary, 2);
    Check(read.status == ReadStatus::Truncated && read.originalBytes == 3 && read.capturedBytes == 2,
          "binary reader distinguishes prefix from full buffer");
}
