#include "monitor/media/media_receive_native.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/diagnostics/event.hpp"
#include "monitor/native/target.hpp"
#include <MinHook.h>
#include <intrin.h>
#include <utility>

namespace wechatbot::monitor {
namespace media_receive_detail {

VoiceFn originalVoice = nullptr;
ImageFn originalImage = nullptr;

/**
 * @brief 微信语音接收 MinHook 拦截回调
 * @details 
 *   调用约定：__fastcall
 *   流程：
 *   1. 保存 entryError，获取调用者返回地址 `_ReturnAddress()`；
 *   2. 预留 Snapshot 配额，调用 CaptureVoice 校验虚表并拷贝内存中的语音音频二进制数据；
 *   3. 调用微信原始 originalVoice 处理函数，保持微信底层内部状态同步；
 *   4. 若原始函数返回 true，将包含语音数据的快照入队 EnqueueMediaSnapshot 供后台流水线消费；
 *   5. 恢复 LastError 错误码。
 */
bool __fastcall HookVoice(void* handler, const void* messagePair, const void* bufferPair) {
    const DWORD entryError = GetLastError();
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::unique_ptr<Snapshot> snapshot;
    try {
        snapshot = ReserveMediaSnapshot();
        if (snapshot && !CaptureVoice(reinterpret_cast<uintptr_t>(g_weixin), caller, handler, messagePair, bufferPair, *snapshot))
            snapshot.reset();
    } catch (...) {
        snapshot.reset();
        RecordMediaSnapshotDrop();
    }
    SetLastError(entryError);
    const bool result = originalVoice(handler, messagePair, bufferPair);
    const DWORD returnedError = GetLastError();
    if (result) EnqueueMediaSnapshot(std::move(snapshot));
    else snapshot.reset();
    SetLastError(returnedError);
    return result;
}

/**
 * @brief 微信图片下载就绪 MinHook 拦截回调
 * @details 
 *   调用约定：__fastcall
 *   流程：
 *   1. 预留 Snapshot 配额并调用 CaptureImage；
 *   2. 在 CaptureImage 内部立即解析微信的图片缓存磁盘路径，并**当场调用 CreateFileW 打开并锁住文件句柄**，
 *      杜绝在回调返回到异步线程消费之间的竞态窗口中文件被微信删除或覆盖；
 *   3. 执行原始微信 originalImage；
 *   4. 将快照推入流水线 EnqueueMediaSnapshot。
 */
void __fastcall HookImage(void* handler, const void* messagePair, const void* resource, const void* result) {
    const DWORD entryError = GetLastError();
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::unique_ptr<Snapshot> snapshot;
    try {
        snapshot = ReserveMediaSnapshot();
        if (snapshot && !CaptureImage(reinterpret_cast<uintptr_t>(g_weixin), caller, handler, messagePair, resource, result, *snapshot))
            snapshot.reset();
    } catch (...) {
        snapshot.reset();
        RecordMediaSnapshotDrop();
    }
    SetLastError(entryError);
    originalImage(handler, messagePair, resource, result);
    const DWORD returnedError = GetLastError();
    EnqueueMediaSnapshot(std::move(snapshot));
    SetLastError(returnedError);
}
} // namespace media_receive_detail

void EnableNativeMediaReceive(bool enabled, const std::wstring& inboundRoot) {
    using namespace media_receive_detail;
    namespace profile = active_profile::media_receive;
    if (!enabled || MediaReceivePipelineStarted()) return;
    
    // 1. 定位目标函数地址并校验函数入口特征码
    auto* voice = reinterpret_cast<uint8_t*>(g_weixin) + profile::kVoiceRva;
    auto* image = reinterpret_cast<uint8_t*>(g_weixin) + profile::kImageRva;
    const bool validVoice = VerifyEntry(voice, profile::kVoiceEntry, sizeof(profile::kVoiceEntry));
    const bool validImage = VerifyEntry(image, profile::kImageEntry, sizeof(profile::kImageEntry));
    if (!validVoice && !validImage) {
        AppendDirect("{\"kind\":\"media_receive_disabled\",\"reason\":\"entry_signature_mismatch\"}");
        return;
    }
    
    // 2. 启动后台消费者工作线程流水线
    if (!StartMediaReceivePipeline(inboundRoot)) return;
    
    // 3. 使用 MinHook 创建并启用钩子
    const auto hook = [](void* target, void* detour, void** original, bool valid) {
        if (!valid || MH_CreateHook(target, detour, original) != MH_OK) return false;
        if (MH_EnableHook(target) == MH_OK) return true;
        MH_RemoveHook(target); return false;
    };
    const bool voiceEnabled = hook(voice, reinterpret_cast<void*>(&HookVoice), reinterpret_cast<void**>(&originalVoice), validVoice);
    const bool imageEnabled = hook(image, reinterpret_cast<void*>(&HookImage), reinterpret_cast<void**>(&originalImage), validImage);
    if (!voiceEnabled && !imageEnabled) StopNativeMediaReceive(1000);
    
    // 4. 记录初始化就绪日志
    const auto record = std::string("{\"kind\":\"media_receive_enabled\",\"voice\":") + (voiceEnabled ? "true" : "false") +
        ",\"image_candidates\":" + (imageEnabled ? "true" : "false") +
        ",\"max_pending\":16,\"voice_limit_bytes\":1048576,\"image_limit_bytes\":33554432}";
    AppendDirect(record.c_str());
}

} // namespace wechatbot::monitor
