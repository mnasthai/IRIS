#pragma once
#include "monitor/media/media_receive_snapshot.hpp"
#include "monitor/media/media_candidate_file.hpp"
#include "monitor/media/media_inspection.hpp"
#include "monitor/media/media_asset_publisher.hpp"
#include "monitor/media/media_receive_pipeline.hpp"
#include "monitor/config/version_profile.hpp"

namespace wechatbot::monitor {

/**
 * @brief 启用微信原生多媒体接收监听 (Hook)
 * @details 
 *   在完成微信基址与特征码验证、且 MinHook 初始化完毕后由 Worker 初始化流程调用。
 *   主要完成：
 *   1. 校验 VoiceRva 与 ImageRva 处的函数入口特征指令 (VerifyEntry)；
 *   2. 启动多媒体后台流水线工作线程 (StartMediaReceivePipeline)；
 *   3. 使用 MinHook 为微信底层的语音接收与图片落盘回调函数安装 Detour 拦截点。
 * @param enabled 是否启用多媒体接收监听
 * @param inboundRoot 本地多媒体文件存储根目录
 */
void EnableNativeMediaReceive(bool enabled, const std::wstring& inboundRoot);

namespace media_receive_detail {
using active_profile::media_receive::kVoiceRva;
using active_profile::media_receive::kVoiceDownloadedReturnRva;
using active_profile::media_receive::kVoiceHandlerVtableRva;
using active_profile::media_receive::kImageRva;
using active_profile::media_receive::kImageHandlerVtableRva;
using active_profile::media_receive::kMessageVtableRva;

using VoiceFn = bool(__fastcall*)(void*, const void*, const void*);
using ImageFn = void(__fastcall*)(void*, const void*, const void*, const void*);

extern VoiceFn originalVoice; ///< 原始微信语音下载完成处理函数
extern ImageFn originalImage; ///< 原始微信图片下载就绪处理函数

/**
 * @brief 微信语音接收拦截回调 (__fastcall)
 */
bool __fastcall HookVoice(void* handler, const void* messagePair, const void* bufferPair);

/**
 * @brief 微信图片接收拦截回调 (__fastcall)
 */
void __fastcall HookImage(void* handler, const void* messagePair, const void* resource, const void* result);

} // namespace media_receive_detail
} // namespace wechatbot::monitor
