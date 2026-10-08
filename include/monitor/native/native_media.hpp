#pragma once
#include "monitor/native/native_text.hpp"
#include <string_view>

namespace wechatbot::monitor {

inline constexpr size_t kNativeVoiceMaxBytes = 4 * 1024 * 1024;      ///< 原生语音最大字节上限 (4MB)
inline constexpr uint32_t kNativeVoiceMaxDurationMs = 60 * 1000;    ///< 原生语音最大时长上限 (60秒)
inline constexpr size_t kNativeImagePathMaxUnits = 32767;           ///< 宽字符路径长度上限

/// 原生多媒体发送类别
enum class NativeMediaKind {
    image,  ///< 图片消息 (类型码 3)
    voice,  ///< 语音消息 (类型码 34)
};

/// 微信原生图片构造工厂函数原型
using NativeImageFactoryFn = NativeSharedPair* (__fastcall*)(NativeSharedPair* out);

/// 微信原生语音构造工厂函数原型
using NativeVoiceFactoryFn = NativeSharedPair* (__fastcall*)(
    NativeSharedPair* out, const void* targetNativeString,
    const void* audioNativeString, const uint32_t* durationMs,
    const void* metadata);

/// 微信原生宽字符对象拷贝赋值函数原型 (RVA 0x999A0: 从完整 UTF-16 NativeString 拷贝赋值)
using NativeWideAssignFn = void* (__fastcall*)(void* destination,
                                                const void* sourceNativeWideString);

/**
 * @brief 微信原生多媒体逆向 API 函数指针表
 */
struct NativeMediaApi {
    NativeTextApi common;                         ///< 基础公共 API (allocate, free, makeDelayed, initMetadata, start)
    NativeImageFactoryFn imageFactory = nullptr;  ///< 原生图片工厂函数
    NativeVoiceFactoryFn voiceFactory = nullptr;  ///< 原生语音工厂函数
    NativeWideAssignFn wideAssign = nullptr;      ///< 宽字符路径赋值函数
};

/**
 * @brief 原生多媒体发信请求体
 */
struct NativeMediaRequest {
    std::string_view targetUtf8;                 ///< 发送目标 (wxid / 聊天室 ID)
    NativeMediaKind kind = NativeMediaKind::image;///< 媒体类型 (图片或语音)
    std::wstring_view imagePath;                 ///< 图片在本地的宽字符路径 (kind == image 时有效)
    std::string_view audioBytes;                 ///< 语音 SILK 二进制数据 (kind == voice 时有效)
    uint32_t durationMs = 0;                     ///< 语音时长毫秒数 (kind == voice 时有效)
    DWORD authorizedUiThreadId = 0;              ///< 授权的微信 UI 线程 ID
    std::function<bool()> beforeStart;           ///< 调用 start() 前一刻的回调 (用于 RetainImage 锁定)
};

/**
 * @brief 执行单次原生多媒体消息构造与发送
 * @details 
 *   运行环境要求：必须在微信授权的 Qt UI 线程执行。
 *   执行步骤：
 *   1. 校验请求参数有效性（UTF-8 目标 ID、图片路径/语音时长）；
 *   2. 校验当前线程亲和性（必须等于 authorizedUiThreadId）；
 *   3. 根据媒体种类调用原生构造：
 *      - 图片：调用 imageFactory(&source)，提取 localUuid，assign 目标，wideAssign 写入宽字符图片路径；
 *      - 语音：组装 target 与 audio 的 NativeString，构建 VoiceMetadata，调用 voiceFactory(&source, ...)；
 *   4. 调用 SubmitPreparedNativeSourceOnce 完成公共生命周期 (Vector -> Delayed -> Metadata -> Start) 并逆序清理。
 * @param api 原生函数指针绑定表
 * @param request 发送请求参数
 * @return NativeAttemptResult 执行结果与底层阶段状态
 */
NativeAttemptResult SubmitNativeMediaOnce(const NativeMediaApi& api,
                                       const NativeMediaRequest& request) noexcept;

} // namespace wechatbot::monitor
