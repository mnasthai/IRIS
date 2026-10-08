// Synthetic native objects and isolated temporary files only. No WeChat code,
// injection, hooked process, message directory or network operation is used.
#include "monitor/media/media_receive_native.hpp"
#include "monitor/media/media_file.hpp"
#include "monitor/config/version_config.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

using namespace wechatbot::monitor;
using namespace wechatbot::monitor::media_receive_detail;
void Check(bool condition, const char* message);
namespace {
static_assert(!std::is_copy_constructible_v<ImageCandidateRecordBuilder>);
static_assert(!std::is_move_constructible_v<ImageCandidateRecordBuilder>);
template<class T> void Put(void* object, size_t offset, const T& value) {
    memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}
void NativeText(void* object, size_t offset, const std::string& value) {
    auto* at = static_cast<uint8_t*>(object) + offset;
    memset(at, 0, 0x20);
    if (value.size() < 16) memcpy(at, value.data(), value.size());
    else { const auto* data = value.data(); Put(at, 0, data); }
    Put(at, 0x10, static_cast<uint64_t>(value.size()));
    Put(at, 0x18, static_cast<uint64_t>(value.size() < 16 ? 15 : value.size()));
}
void NativeWide(void* object, size_t offset, const std::wstring& value) {
    auto* at = static_cast<uint8_t*>(object) + offset;
    memset(at, 0, 0x20);
    if (value.size() < 8) memcpy(at, value.data(), value.size() * sizeof(wchar_t));
    else { const auto* data = value.data(); Put(at, 0, data); }
    Put(at, 0x10, static_cast<uint64_t>(value.size()));
    Put(at, 0x18, static_cast<uint64_t>(value.size() < 8 ? 7 : value.size()));
}
void WriteFixture(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(stream.good(), "write only the exact isolated media receive fixture");
}
std::string ReadFixture(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string FromHex(std::string_view hex) {
    std::string bytes;
    const auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes.push_back(static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    return bytes;
}
unsigned voiceCalls = 0, imageCalls = 0;
bool originalResult = true, throwOriginal = false;
void* expectedHandler = nullptr;
const void* expectedPair = nullptr;
bool __fastcall FakeVoice(void* handler, const void* pair, const void* buffer) {
    ++voiceCalls;
    Check(handler == expectedHandler && pair == expectedPair && buffer == nullptr,
          "voice hook forwards original arguments exactly");
    Check(GetLastError() == 3141, "voice original receives caller LastError");
    SetLastError(5926);
    if (throwOriginal) throw 73;
    return originalResult;
}
void __fastcall FakeImage(void* handler, const void* pair, const void* resource, const void* result) {
    ++imageCalls;
    Check(handler == expectedHandler && pair == expectedPair && !resource && !result,
          "image hook forwards original arguments exactly");
    Check(GetLastError() == 3141, "image original receives caller LastError");
    SetLastError(5926);
    if (throwOriginal) throw 73;
}
}

void TestNativeMediaReceive() {
    Identity boundaryIdentity;
    boundaryIdentity.sequence = UINT64_MAX;
    boundaryIdentity.observedUnixMs = 17;
    boundaryIdentity.messageId = UINT64_MAX;
    boundaryIdentity.type = 3;
    memcpy(boundaryIdentity.from, "a\"\\\n", 5);
    const std::string identityFields =
        "\"schema_version\":2,\"seq\":18446744073709551615,\"observed_unix_ms\":17,"
        "\"msg_type\":3,\"message_id\":\"18446744073709551615\",\"from\":\"a\\\"\\\\\\n\"";
    ImageCandidate unopened;
    unopened.openError = ERROR_FILE_NOT_FOUND;
    const auto unread = ReadCandidate(unopened);
    Check(!unread && unread.status == CandidateStatus::PathUnreadable && unread.bytes.empty(),
          "unopened candidate returns owned empty bytes and a typed failure");
    Check(EncodeImageCandidate(boundaryIdentity, 2, 0x88, unopened, unread, nullptr, {}) ==
          "{\"kind\":\"media_image_candidate\"," + identityFields +
          ",\"status\":\"pending\",\"resource_kind\":2,\"resource_path_offset\":136,"
          "\"capture_status\":\"path_unreadable\",\"file_size\":0,\"win32_error\":2,"
          "\"byte_length\":0,\"plaintext_format\":null,\"detected_format\":null,\"asset_published\":false}",
          "candidate encoding closes its object, preserves field order and explicit nulls, and omits absent fields");
    ImageCandidateRecordBuilder preparedCandidate(boundaryIdentity, 2, 0x88, unopened, unread, nullptr);
    unopened.path = L"path-added-after-publication";
    unopened.openError = ERROR_ACCESS_DENIED;
    Check(std::move(preparedCandidate).Finish({}) ==
          "{\"kind\":\"media_image_candidate\"," + identityFields +
          ",\"status\":\"pending\",\"resource_kind\":2,\"resource_path_offset\":136,"
          "\"capture_status\":\"path_unreadable\",\"file_size\":0,\"win32_error\":2,"
          "\"byte_length\":0,\"plaintext_format\":null,\"detected_format\":null,\"asset_published\":false}",
          "builder owns already-encoded candidate fields before publication and exposes only its finished object");
    unopened.path.clear();
    unopened.openError = ERROR_FILE_NOT_FOUND;
    Check(EncodeMediaAssetError(boundaryIdentity, PublishStatus::InvalidImageIdentity) ==
          "{\"kind\":\"media_asset_error\"," + identityFields +
          ",\"status\":\"pending\",\"reason\":\"invalid_image_identity\"}",
          "failure encoding retains decimal-string identifiers and escapes identity text");
    const PublishedAsset boundaryAsset{boundaryIdentity, std::string(64, 'a'), std::string(64, 'a') + ".wxgf",
                                       3, true, 2, 596, 380};
    Check(EncodePublishedAsset(boundaryAsset) ==
          "{\"kind\":\"media_encoded_asset\"," + identityFields +
          ",\"status\":\"encoded\",\"sha256\":\"" + std::string(64, 'a') +
          "\",\"byte_length\":3,\"asset_name\":\"" + std::string(64, 'a') +
          ".wxgf\",\"image_variant\":\"unknown\",\"resource_kind\":2,\"resource_path_offset\":104,\"width\":596,\"height\":380}",
          "encoded asset preserves the complete schema and decoded input offset without private source paths");
    Check(EncodeMediaDropped(UINT64_MAX) ==
          "{\"kind\":\"media_asset_dropped\",\"total\":18446744073709551615}",
          "dropped totals remain JSON numbers with exact uint64 precision");
    Check(EncodeMediaPipelineError(PipelineFailure::WriterException) ==
          "{\"kind\":\"media_asset_error\",\"reason\":\"writer_exception\",\"status\":\"pending\"}" &&
          EncodeMediaPipelineError(PipelineFailure::InvalidInboundRoot) ==
          "{\"kind\":\"media_asset_error\",\"reason\":\"invalid_inbound_root\"}",
          "writer and startup errors retain their distinct optional status field");
    unopened.path = L"E:\\fixture\\测试.dat";
    unopened.initial.nFileSizeLow = 2;
    const CandidateReadResult binaryRead{CandidateStatus::StableFileRead, std::string("\0\xff", 2)};
    const PublishOutcome failedPublication{{}, PublishStatus::WxgfHeaderOrBoundsInvalid};
    Check(EncodeImageCandidate(boundaryIdentity, 2, 0x68, unopened, binaryRead, ".wxgf", failedPublication) ==
          "{\"kind\":\"media_image_candidate\"," + identityFields +
          ",\"status\":\"pending\",\"resource_kind\":2,\"resource_path_offset\":104,"
          "\"capture_status\":\"stable_file_read\",\"file_size\":2,\"win32_error\":2,"
          "\"byte_length\":2,\"plaintext_format\":null,\"detected_format\":\"wxgf\","
          "\"source_path\":\"E:\\\\fixture\\\\测试.dat\",\"sha256\":\"" + MediaSha256(binaryRead.bytes) +
          "\",\"prefix_hex\":\"00ff\",\"asset_published\":false,\"asset_error\":\"wxgf_header_or_bounds_invalid\"}",
          "diagnostic candidates retain the explicit local path, binary prefix and encoded-format rejection");
    constexpr uintptr_t base = 0x180000000ULL;
    alignas(8) uint8_t message[active_profile::reference_message::kObjectSize]{};
    uintptr_t voiceHandler = base + kVoiceHandlerVtableRva;
    uintptr_t imageHandler = base + kImageHandlerVtableRva;
    const void* messagePair[]{message, nullptr};
    Put(message, 0, base + kMessageVtableRva);
    Put(message, 0x0C, uint32_t{34});
    constexpr uint64_t messageId = 18446744073709550001ULL;
    Put(message, 0x148, messageId);
    const std::string from = "synthetic_from", to = "synthetic_to";
    NativeText(message, 0x18, from); NativeText(message, 0x38, to);
    std::string silk = std::string("\x02#!SILK_V3\x02\x00", 12) + std::string("a\0", 2);
    struct Buffer { const void* bytes; uint64_t cursor; uint64_t length; uint64_t capacity; }
        buffer{silk.data(), 0, silk.size(), silk.size()};
    const void* bufferPair[]{&buffer, nullptr};
    Snapshot captured;
    Check(CaptureVoice(base, base + kVoiceDownloadedReturnRva, &voiceHandler, messagePair, bufferPair, captured),
          "download-success ABI captures a full binary voice buffer");
    Check(captured.bytes == silk && captured.identity.messageId == messageId &&
          std::string(captured.identity.from) == from && std::string(captured.identity.to) == to,
          "voice snapshot preserves binary NUL and complete message identity");
    silk.back() = 'z';
    Check(captured.bytes.back() == '\0', "voice snapshot owns bytes independently of native buffer lifetime");
    Snapshot rejected;
    Check(!CaptureVoice(base, base + 0x39DC85C, &voiceHandler, messagePair, bufferPair, rejected),
          "send-side caller cannot be reported as received voice");
    Check(!CaptureVoice(base, base + kVoiceDownloadedReturnRva, &imageHandler, messagePair, bufferPair, rejected),
          "wrong handler vtable rejected");
    buffer.length = kMaxVoiceBytes + 1; buffer.capacity = buffer.length;
    Check(!CaptureVoice(base, base + kVoiceDownloadedReturnRva, &voiceHandler, messagePair, bufferPair, rejected),
          "voice size limit checked before allocation or copying");
    buffer.length = silk.size(); buffer.capacity = silk.size() - 1;
    Check(!CaptureVoice(base, base + kVoiceDownloadedReturnRva, &voiceHandler, messagePair, bufferPair, rejected),
          "AutoBuffer capacity mismatch rejected");
    buffer.capacity = silk.size(); buffer.bytes = reinterpret_cast<void*>(1);
    Check(!CaptureVoice(base, base + kVoiceDownloadedReturnRva, &voiceHandler, messagePair, bufferPair, rejected),
          "unreadable native bytes rejected without altering target memory");
    buffer.bytes = silk.data();
    Put(message, 0x148, uint64_t{0});
    Check(!CaptureVoice(base, base + kVoiceDownloadedReturnRva, &voiceHandler, messagePair, bufferPair, rejected),
          "unidentified message cannot generate an asset");
    Put(message, 0x148, messageId);

    Check(VoiceExtension(captured.bytes) && std::string(VoiceExtension(captured.bytes)) == ".silk",
          "SILK packet framing is accepted");
    Check(!VoiceExtension(captured.bytes.substr(0, captured.bytes.size() - 1)),
          "truncated SILK packet is never published");
    const auto amr = std::string("#!AMR\n") + std::string("\x04", 1) + std::string(12, '\0');
    Check(VoiceExtension(amr) && std::string(VoiceExtension(amr)) == ".amr" &&
          !VoiceExtension(amr.substr(0, amr.size() - 1)) && !VoiceExtension("#!AMR\n"),
          "AMR checks frame type and exact frame length");

    const auto root = std::filesystem::temp_directory_path() /
        (L"wechatbot-receive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(std::filesystem::create_directory(root), "create isolated receive fixture directory");
    const auto hash = MediaSha256(captured.bytes);
    const auto asset = root / (hash + ".silk");
    std::string error;
    auto record = SaveVoiceAsset(captured, root.wstring(), error);
    Check(error.empty() && record.find("\"status\":\"available\"") != std::string::npos &&
          record.find("\"message_id\":\"18446744073709550001\"") != std::string::npos &&
          record.find("\"byte_length\":14") != std::string::npos && ReadFixture(asset) == captured.bytes,
          "asset is published atomically with exact bytes, hash name and decimal-string server id");
    record = SaveVoiceAsset(captured, root.wstring(), error);
    Check(!record.empty() && error.empty(), "identical existing asset is verified and reused");
    const auto typedVoice = PublishVoiceAsset(captured, root.wstring());
    Check(typedVoice && typedVoice.status == PublishStatus::Published && typedVoice.asset->identity.messageId == messageId && typedVoice.asset->sha256 == hash &&
          typedVoice.asset->byteLength == captured.bytes.size() && !typedVoice.asset->encoded &&
          EncodePublishedAsset(*typedVoice.asset) == record,
          "typed publication separates file ownership from the unchanged voice event encoding");
    const auto staging = root / (hash + ".silk." + std::to_string(GetCurrentProcessId()) + "." +
        std::to_string(captured.identity.sequence) + ".part");
    WriteFixture(staging, "already-owned-staging");
    Check(SaveVoiceAsset(captured, root.wstring(), error).empty() && error == "staging_create_failed" &&
          ReadFixture(staging) == "already-owned-staging" && ReadFixture(asset) == captured.bytes,
          "failed staging creation preserves the existing file and published asset");
    Check(std::filesystem::remove(staging), "remove only the known pre-existing staging fixture");
    WriteFixture(asset, std::string(captured.bytes.size(), 'x'));
    Check(SaveVoiceAsset(captured, root.wstring(), error).empty() && error == "asset_name_conflict" &&
          ReadFixture(asset) == std::string(captured.bytes.size(), 'x') && !std::filesystem::exists(staging),
          "conflicting hash-named file is preserved and its staging file is removed");
    Check(std::filesystem::remove(asset), "remove only the known synthetic asset");

    const auto png = FromHex(
        "89504e470d0a1a0a0000000d4948445200000002000000020802000000fdd49a73"
        "0000001049444154789c63904b3901440c100a00217e05299b21fd860000000049454e44ae426082");
    auto inspection = InspectImage(png);
    Check(inspection && inspection->width == 2 && inspection->height == 2 &&
          std::string(inspection->extension) == ".png",
          "Windows image decoder consumes every pixel of the complete synthetic PNG");
    auto brokenPng = png; brokenPng[45] ^= 1;
    Check(!InspectImage(brokenPng) && !InspectImage(png.substr(0, png.size() - 1)),
          "corrupt PNG CRC and truncated image are not published");
    Check(!InspectImage(std::string("\xff\xd8\xff\xd9", 4)),
          "JPEG header and EOI without a decodable image do not imply a valid asset");
    const auto wxgf = std::string("wxgf") + std::string(20, '\0');
    Check(ImageExtension(wxgf) && std::string(ImageExtension(wxgf)) == ".wxgf" && !InspectImage(wxgf),
          "WXGF is diagnosed distinctly and never advertised as JPEG");
    auto imageIdentity = captured.identity; imageIdentity.type = 3;
    const auto imageHash = MediaSha256(png);
    const auto imageAsset = root / (imageHash + ".png");
    const auto imageRecord = SaveImageAsset(imageIdentity, 2, 0x68, png, root.wstring(), error);
    Check(!imageRecord.empty() && error.empty() && ReadFixture(imageAsset) == png &&
          imageRecord.find("\"resource_kind\":2") != std::string::npos &&
          imageRecord.find("\"width\":2,\"height\":2") != std::string::npos,
          "complete decoded image preserves original bytes and native variant metadata");
    const auto typedImage = PublishImageAsset(imageIdentity, 2, 0x68, png, root.wstring());
    Check(typedImage && typedImage.status == PublishStatus::Published && typedImage.asset->resourceKind == 2 && typedImage.asset->width == 2 && typedImage.asset->height == 2 &&
          !typedImage.asset->encoded && EncodePublishedAsset(*typedImage.asset) == imageRecord,
          "typed image result preserves schema fields, order, dimensions and publication status");
    Check(SaveImageAsset(imageIdentity, 2, 0x88, png, root.wstring(), error).empty() &&
          error == "invalid_image_identity", "cache output path cannot be substituted for proven download input");
    const auto rejectedImage = PublishImageAsset(imageIdentity, 2, 0x88, png, root.wstring());
    Check(!rejectedImage && !rejectedImage.asset && rejectedImage.status == PublishStatus::InvalidImageIdentity,
          "typed image failure owns its reason and contains no asset");
    Snapshot invalidVoice;
    const auto rejectedVoice = PublishVoiceAsset(invalidVoice, root.wstring());
    Check(!rejectedVoice && rejectedVoice.status == PublishStatus::InvalidVoiceIdentity,
          "typed voice identity rejection returns its narrow status");
    Check(std::filesystem::remove(imageAsset), "remove only the known synthetic image asset");
    const auto encoded = FromHex("777867661300020254017c000000000000000000") + "encoded-fixture";
    inspection = InspectWxgfHeader(encoded);
    Check(inspection && inspection->width == 596 && inspection->height == 380,
          "WXGF bounded header has big-endian dimensions");
    auto badEncoded = encoded; badEncoded[4] = '\x7f';
    Check(!InspectWxgfHeader(badEncoded) &&
          !InspectWxgfHeader(encoded.substr(0, 19)) &&
          !InspectWxgfHeader(std::string("wxgf") + std::string(24, '\0')),
          "WXGF rejects out-of-bounds header, absent payload and invalid dimensions/version");
    const auto encodedRecord = SaveEncodedImageAsset(imageIdentity, 2, 0x68, encoded, root.wstring(), error);
    const auto encodedAsset = root / (MediaSha256(encoded) + ".wxgf");
    Check(!encodedRecord.empty() && error.empty() && ReadFixture(encodedAsset) == encoded &&
          encodedRecord.find("\"kind\":\"media_encoded_asset\"") != std::string::npos &&
          encodedRecord.find("\"status\":\"encoded\"") != std::string::npos &&
          encodedRecord.find("\"status\":\"available\"") == std::string::npos,
          "complete captured WXGF bytes are retained as encoded, never mislabelled available JPEG");
    Check(std::filesystem::remove(encodedAsset), "remove only known synthetic encoded asset");

    // The path is opaque to capture: this fixture represents a .dat resource.
    const auto imagePath = root / L"native-resource.dat";
    const auto pathString = imagePath.wstring();
    const auto missingString = (root / L"missing-resource.dat").wstring();
    const std::string encrypted = "opaque_encrypted_fixture";
    WriteFixture(imagePath, encrypted);
    alignas(8) uint8_t resource[0x150]{}, result[0xA0]{};
    Put(resource, 0x50, uint32_t{2}); Put(result, 0x20, uint32_t{2});
    Put(result, 0x24, uint32_t{2}); Put(result, 0x28, uint32_t{1}); Put(result, 0x74, uint32_t{2});
    NativeWide(resource, 0x68, pathString); NativeWide(resource, 0x88, missingString);
    Put(message, 0x0C, uint32_t{3});
    {
        Snapshot image;
        Check(CaptureImage(base, base + 0x17E5ECE, &imageHandler, messagePair, resource, result, image),
              "image completion links verified message identity and explicit native paths");
        Check(image.image[0].handle && !image.image[1].handle &&
              image.image[1].status == CandidateStatus::OpenFailed,
              "only resource paths are opened; missing path is not replaced with a directory search");
        Check(DeleteFileW(imagePath.c_str()) != FALSE, "candidate read handle preserves native deletion permissions");
        const auto read = ReadCandidate(image.image[0]);
        Check(read && read.status == CandidateStatus::StableFileRead && read.bytes == encrypted && !ImageExtension(read.bytes),
              "held handle survives native temporary-file deletion and opaque data remains unrecognized");
    }
    WriteFixture(imagePath, encrypted);
    {
        Snapshot image;
        Check(CaptureImage(base, base + 0x17E62B1, &imageHandler, messagePair, resource, result, image),
              "second verified manager callback site is accepted");
        WriteFixture(imagePath, "changed");
        const auto read = ReadCandidate(image.image[0]);
        Check(!read && read.status == CandidateStatus::FileChanged && read.bytes.empty(),
              "resource mutation after callback snapshot is detected");
    }
    Put(result, 0x74, uint32_t{3});
    Check(!CaptureImage(base, base + 0x17E5ECE, &imageHandler, messagePair, resource, result, rejected),
          "failed download cannot produce a completed image candidate");
    Put(result, 0x74, uint32_t{2}); Put(result, 0x20, uint32_t{3});
    Check(!CaptureImage(base, base + 0x17E5ECE, &imageHandler, messagePair, resource, result, rejected),
          "resource/result kind disagreement is rejected");

    Check(!StartMediaReceivePipeline(L"relative-inbound"), "pipeline rejects nonabsolute roots without a worker");
    Check(StartMediaReceivePipeline(root.wstring()), "ordinary pipeline starts without installing any hook");
    std::vector<std::unique_ptr<Snapshot>> held;
    for (size_t i = 0; i < kMaxPending; ++i) {
        held.push_back(ReserveMediaSnapshot());
        Check(static_cast<bool>(held.back()), "each in-flight capture reserves one pending permit");
    }
    Check(OutstandingMediaSnapshots() == kMaxPending, "all held captures count even while the queue is empty");
    const auto droppedBefore = DroppedMediaSnapshots();
    Check(!ReserveMediaSnapshot() && DroppedMediaSnapshots() == droppedBefore + 1,
          "seventeenth outstanding snapshot is dropped without allocation");
    {
        Snapshot moved = std::move(*held.front());
        held.front().reset();
        Check(OutstandingMediaSnapshots() == kMaxPending,
              "moving snapshot ownership does not prematurely return its permit");
    }
    Check(OutstandingMediaSnapshots() == kMaxPending - 1, "final moved snapshot destruction returns capacity");
    held.front() = ReserveMediaSnapshot();
    Check(held.front() && OutstandingMediaSnapshots() == kMaxPending, "returned capacity can be reserved again");
    Check(!StopNativeMediaReceive(0) && MediaReceivePipelineStarted() &&
          !ReserveMediaSnapshot() && OutstandingMediaSnapshots() == kMaxPending,
          "stop timeout closes acceptance but retains the worker, wake and live snapshots");
    Check(!StartMediaReceivePipeline(root.wstring()), "timed-out worker cannot be replaced by a second worker");
    Check(!EnqueueMediaSnapshot(std::move(held.back())) && OutstandingMediaSnapshots() == kMaxPending - 1,
          "late enqueue after stop is rejected and destroys its owned snapshot once");
    held.clear();
    Check(OutstandingMediaSnapshots() == 0 && StopNativeMediaReceive(1500) && !MediaReceivePipelineStarted(),
          "a later stop joins and closes resources after the held captures are released");

    Check(StartMediaReceivePipeline(root.wstring()), "pipeline can restart after a successful drain and join");
    Check(!EnqueueMediaSnapshot(std::make_unique<Snapshot>()) && OutstandingMediaSnapshots() == 0,
          "unreserved test values cannot bypass the total in-flight limit");
    auto delayedCapture = ReserveMediaSnapshot();
    bool queued = false;
    for (size_t attempt = 0; attempt < 32 && !queued; ++attempt) {
        auto voice = ReserveMediaSnapshot();
        Check(static_cast<bool>(voice), "ordinary voice fixture reserves bounded capacity");
        voice->identity = captured.identity;
        voice->bytes = captured.bytes;
        queued = EnqueueMediaSnapshot(std::move(voice));
        if (!queued) Sleep(1); // Try-enqueue may legitimately lose to the worker.
    }
    Check(queued && !StopNativeMediaReceive(0), "stop retains an outstanding capture while draining queued work");
    delayedCapture.reset();
    Check(StopNativeMediaReceive(1500) && OutstandingMediaSnapshots() == 0 &&
          ReadFixture(asset) == captured.bytes,
          "successful stop drains the queue and publishes the exact owned bytes before closing");
    Check(StopNativeMediaReceive(0), "stopping an already joined pipeline is idempotent");
    Check(std::filesystem::remove(asset), "remove only the queued synthetic voice asset");
    Check(std::filesystem::remove(imagePath) && std::filesystem::remove(root),
          "remove only known fixture files and the empty fixture directory");

    const auto savedVoice = originalVoice; const auto savedImage = originalImage;
    originalVoice = FakeVoice; originalImage = FakeImage;
    expectedHandler = &voiceHandler; expectedPair = messagePair;
    const unsigned voiceBefore = voiceCalls, imageBefore = imageCalls;
    SetLastError(3141); originalResult = true;
    Check(HookVoice(expectedHandler, expectedPair, nullptr) && GetLastError() == 5926 &&
          voiceCalls == voiceBefore + 1, "voice call-through is exact once and preserves returned LastError");
    SetLastError(3141); originalResult = false;
    Check(!HookVoice(expectedHandler, expectedPair, nullptr) && GetLastError() == 5926 &&
          voiceCalls == voiceBefore + 2, "voice false return is unchanged");
    SetLastError(3141); HookImage(expectedHandler, expectedPair, nullptr, nullptr);
    Check(GetLastError() == 5926 && imageCalls == imageBefore + 1,
          "image call-through is exact once and preserves returned LastError");
    throwOriginal = true;
    bool caught = false;
    SetLastError(3141);
    try { HookVoice(expectedHandler, expectedPair, nullptr); } catch (int value) { caught = value == 73; }
    Check(caught && voiceCalls == voiceBefore + 3 && GetLastError() == 5926,
          "voice original exception propagates without retry");
    caught = false; SetLastError(3141);
    try { HookImage(expectedHandler, expectedPair, nullptr, nullptr); } catch (int value) { caught = value == 73; }
    Check(caught && imageCalls == imageBefore + 2 && GetLastError() == 5926,
          "image original exception propagates without retry");
    throwOriginal = false; originalVoice = savedVoice; originalImage = savedImage;
}
