#pragma once
#include "monitor/diagnostics/event.hpp"

namespace wechatbot::monitor {
// Observation of client-originated calls; these declarations do not submit
// bot commands. Active bot sending belongs to the send/native modules.
using SendRequestFn = void*(__fastcall*)(void*, void*, const void*, const void*, const void*);
using SendContextConstructorFn = void*(__fastcall*)(void*, const void*, unsigned char);
using SendSubmitFn = void(__fastcall*)(const void*, const void*);
extern SendRequestFn g_originalSendRequest;
extern SendContextConstructorFn g_originalSendContextConstructor;
extern SendSubmitFn g_originalSendSubmit;
Event ObserveSendRequest(const void* request);
void* __fastcall HookSendRequest(void* resultOut, void* unusedInput, const void* request,
                                 const void* extra, const void* options);
void EnableSendObservation(bool traceEnabled);
void ConfigureSendTrace(uintptr_t imageSize, bool enabled);
void ConfigureSendContextTrace(uintptr_t imageSize, bool enabled);
void* __fastcall HookSendContextConstructor(void* context, const void* sourcePair, unsigned char flag);
void EnableSendContextObservation(bool traceEnabled);
void ConfigureSendSubmitTrace(uintptr_t imageSize, bool enabled, bool mediaEnabled);
void __fastcall HookSendSubmit(const void* callable, const void* completion);
void EnableSendSubmitObservation(bool textEnabled, bool mediaEnabled);
} // namespace wechatbot::monitor
