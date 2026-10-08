#include "monitor/media/media_file.hpp"
#include <filesystem>
#include <fstream>
using namespace wechatbot::monitor;
void Check(bool condition, const char* message);
void TestMediaFile() {
    const std::string silk = std::string("\x02#!SILK_V3\x02\x00", 12) + "ab";
    Check(SilkDurationMs(silk) == 20 && SilkDurationMs(silk + "\xff\xff") == 20 &&
          !SilkDurationMs(silk.substr(0, silk.size() - 1)) && !SilkDurationMs("#!SILK_V3"),
          "SILK container validates packet boundaries and optional terminal marker");
    Check(MediaSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "media SHA-256 agrees with standard vector");
    Check(MediaSha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "media SHA-256 accepts an empty byte view");
    const auto root = std::filesystem::temp_directory_path() /
        (L"wechatbot-media-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(std::filesystem::create_directory(root), "create isolated media fixture directory");
    const auto hash = MediaSha256(silk);
    const auto path = root / (hash + ".silk");
    { std::ofstream file(path, std::ios::binary); file.write(silk.data(), silk.size()); Check(file.good(), "write SILK fixture"); }
    const auto utf8 = path.u8string();
    MediaCommand command{"voice", std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()), hash, silk.size(), 20};
    auto result = PrepareMediaFile(command, root.wstring());
    Check(result.file && result.file->audio == silk && result.file->durationMs == 20,
          "staged media is content-checked and retained with its native input");
    const auto writeAttempt = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writeAttempt == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
          "prepared file cannot be rewritten before native asynchronous input consumption");
    Check(!DeleteFileW(path.c_str()), "prepared file cannot be removed while the handle is held");
    auto retained = result.file;
    result.file.reset();
    Check(!DeleteFileW(path.c_str()), "another shared owner retains the prepared file lock");
    retained.reset();
    {
        UniqueHandle writable(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, 0, nullptr));
        Check(static_cast<bool>(writable), "last prepared owner releases its file handle");
    }
    auto invalid = command; invalid.durationMs = 40;
    Check(PrepareMediaFile(invalid, root.wstring()).error == "media_format_invalid", "packet duration mismatch rejected");
    {
        UniqueHandle writable(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, 0, nullptr));
        Check(static_cast<bool>(writable), "failed preparation releases its file handle");
    }
    invalid = command; invalid.bytes += 1;
    Check(PrepareMediaFile(invalid, root.wstring()).error == "media_size_mismatch", "size mismatch rejected before native calls");
    Check(PrepareMediaFile(command, root.parent_path().wstring()).error == "media_path_not_allowed",
          "only direct children of configured spool accepted");
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << std::string(silk.size(), 'x'); }
    Check(PrepareMediaFile(command, root.wstring()).error == "media_hash_mismatch", "content replacement detected independently of size");
    Check(std::filesystem::remove(path) && std::filesystem::remove(root), "remove only known fixture file and empty directory");
}
