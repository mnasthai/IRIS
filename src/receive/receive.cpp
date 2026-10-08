/**
 * @file receive.cpp
 * @brief 微信批量接收消息处理函数的 MinHook 拦截与透传实现
 */

#include "monitor/receive/receive.hpp"
#include "monitor/native/target.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/media/media_capture.hpp"
#include <MinHook.h>

namespace wechatbot::monitor {

/// @brief 原函数跳板指针
ReceiveBatchFn g_originalReceiveBatch = nullptr;

/**
 * @brief 挂钩后的批量消息接收处理函数
 * @details
 *  微信原函数的返回值是一个状态字节 非 0 通常代表处理成功
 *  HOOK 容易闪退 往往是 污染了寄存器或系统错误码
 */
unsigned char __fastcall HookReceiveBatch(void* context, void* vector,
                                          unsigned char flag3, unsigned char flag4) {
    // 步骤 1: 暂存调用进入时的系统错误码 保存的是全局的 errno 备份
    const DWORD entryError = GetLastError();

    // 步骤 2: 把 vector 里的消息全读出来
    const Event batch = ObserveBatch(context, vector, flag3, flag4);

    // 步骤 3: 恢复进入时的错误码 避免解包逻辑干扰微信
    SetLastError(entryError);

    // 步骤 4: 跳板
    const unsigned char result = g_originalReceiveBatch
        ? g_originalReceiveBatch(context, vector, flag3, flag4) : 0;

    // 步骤 5: 暂存并记录原函数执行结果
    const DWORD resultError = GetLastError();

    // 排队 ReturnValue 事件
    Event returned = batch;
    returned.SetKind(EventKind::ReturnValue);
    returned.header.sequence = NextSequence();
    returned.Get<CallSnapshot>().message.returnValue = result;
    QueueEvent(returned);

    // 步骤 6: 恢复原函数的错误码 并把原函数的返回值返回给微信调用者
    SetLastError(resultError);
    return result;
}

/**
 * @brief 在 Weixin.dll 的批量接收函数入口处安装并启用 MinHook
 * @param mediaEnabled 是否开启多媒体接收追踪
 * @return true 挂钩成功；false 失败
 */
bool EnableReceiveObservation(bool mediaEnabled) {
    // 配置多媒体追踪参数 (图片上限8张，语音上限8条，缓冲区上限8KB)
    ConfigureMediaReceiveTrace(mediaEnabled);
    if (mediaEnabled) {
        AppendDirect("{\"kind\":\"media_receive_trace_enabled\",\"max_images\":8,\"max_voices\":8,\"buffer_limit_bytes\":8192}");
    }

    // 计算挂钩目标虚拟内存地址: Weixin.dll基址 + kReceiveBatchRva (0x017955E0)
    void* target = reinterpret_cast<uint8_t*>(g_weixin) + kReceiveBatchRva;

    // 创建 Hook：挂在 target 上，跳转到 HookReceiveBatch，将原逻辑指令存入 g_originalReceiveBatch
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookReceiveBatch),
                      reinterpret_cast<void**>(&g_originalReceiveBatch)) != MH_OK) {
        AppendDirect("{\"kind\":\"hook_error\",\"stage\":\"create\"}");
        return false;
    }

    // 激活 Hook
    if (MH_EnableHook(target) != MH_OK) {
        AppendDirect("{\"kind\":\"hook_error\",\"stage\":\"enable\"}");
        return false;
    }

    AppendDirect("{\"kind\":\"hook_enabled\",\"source\":\"receive_batch\",\"rva\":\"0x17955e0\"}");
    return true;
}

} // namespace wechatbot::monitor
