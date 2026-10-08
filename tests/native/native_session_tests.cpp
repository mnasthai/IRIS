// Exercises only local virtual memory and fake native objects. No Weixin
// process, native getter, hook, message or account data is used.
#include "monitor/native/native_session.hpp"
#include "monitor/config/version_profile.hpp"
#include <cstring>
#include <string>

using namespace wechatbot::monitor;
void Check(bool condition, const char* message);

namespace {
constexpr uintptr_t kRootVtableRva = 0x08A3C688;
constexpr uintptr_t kRootIdentityGetterRva = 0x0004A470;
constexpr uintptr_t kRootServicePairGetterRva = 0x00041A60;
constexpr uintptr_t kRootServiceReadyRva = 0x0004B080;
constexpr uintptr_t kServiceVtableRva = 0x08AB5948;
constexpr uintptr_t kServiceReadyGetterRva = 0x003442E0;
constexpr uintptr_t kServiceIdentityGetterRva = 0x0033FE50;

template<class T> void Put(void* memory, size_t offset, const T& value) {
    memcpy(static_cast<unsigned char*>(memory) + offset, &value, sizeof(value));
}

void PutNativeString(void* memory, size_t offset, const std::string& value) {
    unsigned char native[0x20]{};
    const uint64_t length = value.size();
    const uint64_t capacity = length < 16 ? 15 : length;
    if (length < 16) memcpy(native, value.data(), static_cast<size_t>(length));
    else {
        const char* data = value.data();
        Put(native, 0, data);
    }
    Put(native, 0x10, length);
    Put(native, 0x18, capacity);
    memcpy(static_cast<unsigned char*>(memory) + offset, native, sizeof(native));
}

class FakeImage {
public:
    FakeImage() {
        bytes = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kExpectedImageSize, MEM_RESERVE, PAGE_READWRITE));
        Check(bytes != nullptr, "reserve fake native-session image address space");
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        pageSize = system.dwPageSize;
        Commit(kRootObjectPointerRva);
        Commit(kRootVtableRva);
        Commit(kServiceVtableRva);
    }
    ~FakeImage() {
        if (bytes) VirtualFree(bytes, 0, MEM_RELEASE);
    }
    uintptr_t Base() const { return reinterpret_cast<uintptr_t>(bytes); }
    unsigned char* At(uintptr_t rva) const { return bytes + rva; }

private:
    void Commit(uintptr_t rva) {
        const uintptr_t page = rva / pageSize * pageSize;
        Check(VirtualAlloc(bytes + page, pageSize, MEM_COMMIT, PAGE_READWRITE) != nullptr,
              "commit fake native-session image page");
    }
    unsigned char* bytes = nullptr;
    DWORD pageSize = 0;
};

struct Fixture {
    FakeImage image;
    alignas(8) unsigned char root[0x330]{};
    alignas(8) unsigned char service[0x88]{};
    alignas(8) unsigned char executor[0x18]{};
    alignas(8) unsigned char inner[0xF0]{};
    uint64_t serviceControl = 1;
    uint64_t executorControl = 1;
    std::string account = "fixture-routing-account";

    Fixture() {
        const auto base = image.Base();
        Put(image.At(kRootObjectPointerRva), 0, reinterpret_cast<uintptr_t>(root));
        Put(root, 0, base + kRootVtableRva);
        Put(image.At(kRootVtableRva), 0x20, base + kRootIdentityGetterRva);
        Put(image.At(kRootVtableRva), 0x60, base + kRootServicePairGetterRva);
        Put(image.At(kRootVtableRva), 0x68, base + kRootServiceReadyRva);

        Put(service, 0, base + kServiceVtableRva);
        Put(image.At(kServiceVtableRva), 0x00, base + kServiceReadyGetterRva);
        Put(image.At(kServiceVtableRva), 0x18, base + kServiceIdentityGetterRva);
        service[0x38] = 1;
        PutNativeString(service, 0x48, account);
        const uintptr_t servicePair[]{reinterpret_cast<uintptr_t>(service),
                                      reinterpret_cast<uintptr_t>(&serviceControl)};
        Put(root, 0x68, servicePair);

        Put(executor, 0x10, reinterpret_cast<uintptr_t>(inner));
        inner[0xE9] = 0;
        const uintptr_t executorPair[]{reinterpret_cast<uintptr_t>(executor),
                                       reinterpret_cast<uintptr_t>(&executorControl)};
        Put(root, 0x310, executorPair);
    }

    NativeSessionSnapshot Read() const {
        return ReadNativeSession(image.Base(), GetCurrentThreadId());
    }
};
} // namespace

void TestNativeSession() {
    Fixture fixture;

    SetLastError(7357);
    auto snapshot = fixture.Read();
    Check(snapshot.accountReady && snapshot.executorReady &&
          snapshot.accountId == fixture.account && snapshot.reason == "ok" &&
          GetLastError() == 7357,
          "coherent local identity and normal executor branch form a ready snapshot");

    snapshot = ReadNativeSession(fixture.image.Base(), GetCurrentThreadId() + 1);
    Check(!snapshot.accountReady && !snapshot.executorReady && snapshot.accountId.empty() &&
          snapshot.reason == "wrong_ui_thread", "snapshot rejects every non-designated thread");

    const auto validRootVtable = fixture.image.Base() + kRootVtableRva;
    Put(fixture.root, 0, fixture.image.Base() + kRootVtableRva + 8);
    snapshot = fixture.Read();
    Check(!snapshot.accountReady && snapshot.reason == "root_profile_mismatch",
          "snapshot rejects an unexpected root type");
    Put(fixture.root, 0, validRootVtable);

    fixture.service[0x38] = 0;
    snapshot = fixture.Read();
    Check(!snapshot.accountReady && !snapshot.executorReady && snapshot.accountId.empty() &&
          snapshot.reason == "account_not_ready",
          "identity ready bit is required but is not treated as network status");
    fixture.service[0x38] = 1;

    const std::string invalidUtf8("\xC0\xAF", 2);
    PutNativeString(fixture.service, 0x48, invalidUtf8);
    snapshot = fixture.Read();
    Check(!snapshot.accountReady && snapshot.reason == "account_id_invalid_utf8",
          "invalid identity UTF-8 fails closed");

    const std::string tooLong(600, 'x');
    PutNativeString(fixture.service, 0x48, tooLong);
    snapshot = fixture.Read();
    Check(!snapshot.accountReady && snapshot.reason == "account_id_too_long",
          "identity copy is bounded and rejects truncation");
    PutNativeString(fixture.service, 0x48, fixture.account);

    fixture.inner[0xE9] = 1;
    snapshot = fixture.Read();
    Check(snapshot.accountReady && !snapshot.executorReady &&
          snapshot.accountId == fixture.account && snapshot.reason == "executor_dummy_branch",
          "executor bit zero is the normal branch and bit one is fail closed");
    fixture.inner[0xE9] = 0;

    Put(fixture.executor, 0x10, uintptr_t{});
    snapshot = fixture.Read();
    Check(snapshot.accountReady && !snapshot.executorReady &&
          snapshot.accountId == fixture.account && snapshot.reason == "executor_inner_missing",
          "valid account identity remains distinguishable from unavailable executor");
}
