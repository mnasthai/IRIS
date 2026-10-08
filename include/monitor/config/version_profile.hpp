#pragma once
// Profile 常量由 cmake/GenerateProfile.cmake 从 JSON 生成到构建目录，
// 该 JSON 不随源码仓库分发，获取方式见 docs/profile.md。
#include "iris_profile_generated.hpp"

namespace wechatbot::monitor {

// 唯一的 Profile 命名空间。切换微信版本只需替换 JSON，无需改动源码。
namespace active_profile = profiles::active;
using active_profile::kTargetVersion;
using active_profile::kVersionParts;
using active_profile::kReceiveBatchRva;
using active_profile::kSendRequestRva;
using active_profile::kSendContextConstructorRva;
using active_profile::kSendSubmitRva;
using active_profile::kSendSubmitCallableVtableRva;
using active_profile::kRootObjectPointerRva;
using active_profile::kSendOuterVtableRva;
using active_profile::kSendItemVtableRva;
using active_profile::kSendContextSourceVtableRva;
using active_profile::kSendContextBaseVtableRva;
using active_profile::kAddMsgVtableRva;
using active_profile::kBuiltinStringVtableRva;
using active_profile::kExpectedImageSize;
using active_profile::kBatchStride;
using active_profile::kReceiveEntry;
using active_profile::kSendEntry;
using active_profile::kSendContextConstructorEntry;
using active_profile::kSendSubmitEntry;
using active_profile::kReceiveLayout;
using active_profile::kSendLayout;
using active_profile::kReceiveFrom;
using active_profile::kReceiveTo;
using active_profile::kReceiveContent;
using active_profile::kReceiveSource;
using active_profile::kReceiveAux;
using active_profile::kSendTo;
using active_profile::kSendContent;
using active_profile::kSendSource;
using active_profile::kReceiveScalars;
using active_profile::kSendScalars;

// Observation marker is a trace convention, not a native image offset.
inline constexpr char kSendSubmitMarker[] = "SEND-SUBMIT-001";

} // namespace wechatbot::monitor
