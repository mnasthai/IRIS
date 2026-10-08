# ============================================================================
#  IRIS Profile 代码生成器
#
#  从 profile JSON 生成 C++ 头文件。用法：
#
#    cmake -DPROFILE_JSON=<abs .json> -DOUTPUT_FILE=<abs .hpp> -P GenerateProfile.cmake
#
#  设计要点：
#    * 纯 CMake 实现（string(JSON)），不引入 Python / PowerShell 等额外构建依赖。
#    * 生成结果与手写头文件语义完全一致：全部常量仍是 inline constexpr，
#      因此 native_text.hpp 里的 static_assert 等编译期校验全部继续生效。
#      写错尺寸会直接编译失败，而不是运行到一半越界。
#    * 任何缺失字段 / 类型不符都会 FATAL_ERROR 并在消息中点名具体路径。
# ============================================================================

cmake_minimum_required(VERSION 3.23)

# 统一的失败出口。
#
# execute_process 捕获子进程输出时会按控制台 ANSI 代码页解码，中文会变成乱码；
# 而 file(READ) 按原始字节读入、CMake 内部按 UTF-8 处理。因此诊断文本同时写入
# ERROR_FILE，由父进程读取后再用自身编码输出。
function(iris_fail text)
    if(DEFINED ERROR_FILE AND NOT ERROR_FILE STREQUAL "")
        file(WRITE "${ERROR_FILE}" "${text}")
    endif()
    message(FATAL_ERROR "${text}")
endfunction()

if(NOT DEFINED PROFILE_JSON OR NOT DEFINED OUTPUT_FILE)
    iris_fail("必须同时传入 -DPROFILE_JSON=<路径> 与 -DOUTPUT_FILE=<路径>")
endif()

if(NOT EXISTS "${PROFILE_JSON}")
    iris_fail("找不到 Profile 文件：${PROFILE_JSON} 。该文件不随源码仓库分发，请先从 README 的「微信版本 Profile」一节中的网盘链接下载，放入仓库的 profile/ 目录，或用 -DIRIS_PROFILE_JSON=<路径> 指定。")
endif()

file(READ "${PROFILE_JSON}" JSON)

# ---------------------------------------------------------------------------
# 取值助手：失败时给出可定位的错误
# ---------------------------------------------------------------------------
function(json_get out)
    string(JSON value ERROR_VARIABLE error GET "${JSON}" ${ARGN})
    if(error)
        iris_fail("Profile JSON 缺少字段或类型不符：${ARGN}\n  ${error}")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# 取数组长度（对对象同样可用）
function(json_length out)
    string(JSON value ERROR_VARIABLE error LENGTH "${JSON}" ${ARGN})
    if(error)
        iris_fail("Profile JSON 缺少数组或类型不符：${ARGN}\n  ${error}")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 校验 schema 版本
# ---------------------------------------------------------------------------
json_get(SCHEMA schema_version)
if(NOT SCHEMA EQUAL 1)
    iris_fail("不支持的 profile schema_version：${SCHEMA}（本生成器只支持 1）")
endif()
json_get(PROFILE_NAME profile_name)
json_get(TARGET_VERSION target_version)

# ---------------------------------------------------------------------------
# 可选的键集合校验
#
#   生成器是「遍历 JSON 的键」来产出常量，因此把 kTargetOffset 拼成
#   kTargetOffsetTYPO 并不会报错——它只会生成一个拼错的常量名，直到编译期
#   才因为别处引用不到而失败。以结构模板 example.json 为键清单做一次比对，
#   可以把这类问题提前到配置阶段，并精确定位到出错的键路径。
# ---------------------------------------------------------------------------
function(collect_key_paths out json prefix)
    string(JSON node_type ERROR_VARIABLE _ TYPE "${json}")
    if(NOT node_type STREQUAL "OBJECT")
        set(${out} "${prefix}" PARENT_SCOPE)
        return()
    endif()
    string(JSON member_count LENGTH "${json}")
    # {"ref": "kXxx"} 是符号引用的叶子形式
    if(member_count EQUAL 1)
        string(JSON only_key MEMBER "${json}" 0)
        if(only_key STREQUAL "ref")
            set(${out} "${prefix}" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(acc "")
    math(EXPR last "${member_count} - 1")
    foreach(i RANGE ${last})
        string(JSON key MEMBER "${json}" ${i})
        string(JSON child GET "${json}" ${key})
        collect_key_paths(sub "${child}" "${prefix}${key}.")
        list(APPEND acc ${sub})
    endforeach()
    set(${out} "${acc}" PARENT_SCOPE)
endfunction()

if(DEFINED SCHEMA_FILE AND EXISTS "${SCHEMA_FILE}")
    file(READ "${SCHEMA_FILE}" SCHEMA_JSON)
    collect_key_paths(PROFILE_PATHS "${JSON}" "")
    collect_key_paths(SCHEMA_PATHS "${SCHEMA_JSON}" "")

    set(MISSING_KEYS "")
    foreach(path IN LISTS SCHEMA_PATHS)
        if(NOT path IN_LIST PROFILE_PATHS)
            list(APPEND MISSING_KEYS "${path}")
        endif()
    endforeach()
    set(EXTRA_KEYS "")
    foreach(path IN LISTS PROFILE_PATHS)
        if(NOT path IN_LIST SCHEMA_PATHS)
            list(APPEND EXTRA_KEYS "${path}")
        endif()
    endforeach()

    if(MISSING_KEYS OR EXTRA_KEYS)
        set(detail "")
        foreach(path IN LISTS MISSING_KEYS)
            string(APPEND detail "\n  缺少：${path}")
        endforeach()
        foreach(path IN LISTS EXTRA_KEYS)
            string(APPEND detail "\n  多余或拼写有误：${path}")
        endforeach()
        iris_fail("Profile 的键与结构模板不一致（模板：${SCHEMA_FILE}）。键名拼错会导致生成出错误的常量名，因此在此拦截。${detail}")
    endif()
endif()

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------
# "5541ec18" -> "0x55, 0x41, 0xec, 0x18"
function(hex_to_bytes out hex)
    string(LENGTH "${hex}" length)
    math(EXPR count "${length} / 2")
    if(count LESS 1)
        iris_fail("特征码十六进制串为空或长度非法：'${hex}'")
    endif()
    set(acc "")
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        math(EXPR offset "${i} * 2")
        string(SUBSTRING "${hex}" ${offset} 2 byte)
        if(acc)
            string(APPEND acc ", ")
        endif()
        string(APPEND acc "0x${byte}")
    endforeach()
    set(${out} "${acc}" PARENT_SCOPE)
endfunction()

function(bool_literal out value)
    if(value)
        set(${out} "true" PARENT_SCOPE)
    else()
        set(${out} "false" PARENT_SCOPE)
    endif()
endfunction()

set(BODY "")

# ---------------------------------------------------------------------------
# 逐 scope 生成
#   root 直接落在 profiles::active 下，其余各自开一个同名 namespace
# ---------------------------------------------------------------------------
set(SCOPES root session text reference_message sender media_send media_receive)
set(BUCKETS uptr size uint32 uint64 uint char_arr u16_arr uptr_arr size_arr u8arr wchar_arr
            layout strfield scalar_arr sig_arr)

foreach(scope IN LISTS SCOPES)
    if(scope STREQUAL "root")
        # root 的常量直接落在 profiles::active 下，不额外开 namespace；
        # 但 JSON 路径仍要指向 "root" 节点。
        set(path "root")
    else()
        set(path "${scope}")
        string(APPEND BODY "\nnamespace ${scope} {\n")
        # Signature 定义在 sender 内；media_send 沿用 sender::Signature，
        # 与手写头文件的原有 API 保持一致（native_bindings.cpp 依赖 sender::Signature）。
        if(scope STREQUAL "sender")
            string(APPEND BODY "struct Signature { uintptr_t rva; uint8_t bytes[16]; };\n")
        elseif(scope STREQUAL "media_send")
            string(APPEND BODY "using sender::Signature;\n")
        endif()
    endif()

    set(scope_emitted FALSE)

    foreach(bucket IN LISTS BUCKETS)
        string(JSON member_count ERROR_VARIABLE missing LENGTH "${JSON}" ${path} ${bucket})
        if(missing)
            continue()   # 该 scope 不含此类型桶
        endif()
        if(member_count LESS 1)
            continue()
        endif()

        set(scope_emitted TRUE)
        math(EXPR bucket_last "${member_count} - 1")

        foreach(i RANGE ${bucket_last})
            string(JSON name MEMBER "${JSON}" ${path} ${bucket} ${i})

            if(bucket STREQUAL "layout")
                json_get(size_v   ${path} ${bucket} ${name} size)
                json_get(has_bits ${path} ${bucket} ${name} has_bits)
                string(APPEND BODY "inline constexpr MessageLayout ${name}{${size_v}, ${has_bits}};\n")

            elseif(bucket STREQUAL "strfield")
                json_get(offset  ${path} ${bucket} ${name} offset)
                json_get(bit     ${path} ${bucket} ${name} bit)
                json_get(wrapped ${path} ${bucket} ${name} wrapped)
                bool_literal(wrapped_lit "${wrapped}")
                string(APPEND BODY "inline constexpr StringField ${name}{${offset}, ${bit}, ${wrapped_lit}};\n")

            elseif(bucket STREQUAL "scalar_arr")
                json_length(item_count ${path} ${bucket} ${name})
                unset(entries)
                math(EXPR item_last "${item_count} - 1")
                foreach(j RANGE ${item_last})
                    json_get(number    ${path} ${bucket} ${name} ${j} number)
                    json_get(offset    ${path} ${bucket} ${name} ${j} offset)
                    json_get(bit       ${path} ${bucket} ${name} ${j} bit)
                    json_get(wide      ${path} ${bucket} ${name} ${j} wide)
                    json_get(signed32  ${path} ${bucket} ${name} ${j} signed32)
                    bool_literal(wide_lit "${wide}")
                    bool_literal(signed_lit "${signed32}")
                    string(APPEND entries "{${number}, ${offset}, ${bit}, ${wide_lit}, ${signed_lit}}")
                    if(NOT j EQUAL item_last)
                        string(APPEND entries ", ")
                    endif()
                endforeach()
                string(APPEND BODY "inline constexpr ScalarField ${name}[] = {${entries}};\n")

            elseif(bucket STREQUAL "sig_arr")
                json_length(item_count ${path} ${bucket} ${name})
                unset(entries)
                math(EXPR item_last "${item_count} - 1")
                foreach(j RANGE ${item_last})
                    json_get(rva   ${path} ${bucket} ${name} ${j} rva)
                    json_get(bytes ${path} ${bucket} ${name} ${j} bytes)
                    hex_to_bytes(byte_list "${bytes}")
                    string(APPEND entries "{${rva}, {${byte_list}}}")
                    if(NOT j EQUAL item_last)
                        string(APPEND entries ", ")
                    endif()
                endforeach()
                string(APPEND BODY "inline constexpr Signature ${name}[] = {${entries}};\n")

            elseif(bucket STREQUAL "char_arr")
                json_get(value ${path} ${bucket} ${name})
                string(APPEND BODY "inline constexpr char ${name}[] = \"${value}\";\n")

            elseif(bucket STREQUAL "wchar_arr")
                json_get(value ${path} ${bucket} ${name})
                string(APPEND BODY "inline constexpr wchar_t ${name}[] = L\"${value}\";\n")

            elseif(bucket STREQUAL "u8arr")
                json_get(value ${path} ${bucket} ${name})
                hex_to_bytes(byte_list "${value}")
                string(APPEND BODY "inline constexpr uint8_t ${name}[] = {${byte_list}};\n")

            elseif(bucket STREQUAL "u16_arr" OR bucket STREQUAL "uptr_arr" OR bucket STREQUAL "size_arr")
                if(bucket STREQUAL "u16_arr")
                    set(cpp_type "uint16_t")
                elseif(bucket STREQUAL "uptr_arr")
                    set(cpp_type "uintptr_t")
                else()
                    set(cpp_type "size_t")
                endif()
                json_length(item_count ${path} ${bucket} ${name})
                unset(entries)
                math(EXPR item_last "${item_count} - 1")
                foreach(j RANGE ${item_last})
                    json_get(value ${path} ${bucket} ${name} ${j})
                    string(APPEND entries "${value}")
                    if(NOT j EQUAL item_last)
                        string(APPEND entries ", ")
                    endif()
                endforeach()
                string(APPEND BODY "inline constexpr ${cpp_type} ${name}[] = {${entries}};\n")

            else()
                # 标量类型桶
                if(bucket STREQUAL "uptr")
                    set(cpp_type "uintptr_t")
                elseif(bucket STREQUAL "size")
                    set(cpp_type "size_t")
                elseif(bucket STREQUAL "uint32")
                    set(cpp_type "uint32_t")
                elseif(bucket STREQUAL "uint64")
                    set(cpp_type "uint64_t")
                elseif(bucket STREQUAL "uint")
                    set(cpp_type "unsigned")
                else()
                    iris_fail("生成器不支持的类型桶：${bucket}")
                endif()
                # 标量值支持两种写法：
                #   "0x1234"                       —— 直接写字面量
                #   { "ref": "kSomeOtherConst" }   —— 输出符号引用，避免两处值不同步
                string(JSON value_type ERROR_VARIABLE _ TYPE "${JSON}" ${path} ${bucket} ${name})
                if(value_type STREQUAL "OBJECT")
                    json_get(value ${path} ${bucket} ${name} ref)
                else()
                    json_get(value ${path} ${bucket} ${name})
                endif()
                string(APPEND BODY "inline constexpr ${cpp_type} ${name} = ${value};\n")
            endif()
        endforeach()
    endforeach()

    if(NOT scope STREQUAL "root")
        if(NOT scope_emitted)
            iris_fail("Profile JSON 中 scope '${scope}' 为空或字段名拼写有误")
        endif()
        string(APPEND BODY "} // namespace ${scope}\n")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# 组装头文件
# ---------------------------------------------------------------------------
string(REPLACE "\\" "/" PROFILE_JSON_DISPLAY "${PROFILE_JSON}")

set(HEADER "// ============================================================================
//  本文件由 cmake/GenerateProfile.cmake 自动生成，请勿手工编辑。
//
//  源文件 : ${PROFILE_JSON_DISPLAY}
//  Profile: ${PROFILE_NAME}
//  目标版本: ${TARGET_VERSION}
//
//  重新生成方式：重新运行 CMake 配置（修改 JSON 后 CMake 会自动重跑）。
//  字段含义见 docs/profile.md。
// ============================================================================
#pragma once
#include <cstddef>
#include <cstdint>

namespace wechatbot::monitor {

// 共享描述符类型，布局必须与消费方一致。
struct MessageLayout { size_t size; size_t hasBits; };
struct StringField { size_t offset; uint32_t bit; bool wrapped; };
struct ScalarField { unsigned number; size_t offset; uint32_t bit; bool wide; bool signed32; };

namespace profiles::active {
")

string(APPEND HEADER "${BODY}")
string(APPEND HEADER "
} // namespace profiles::active
} // namespace wechatbot::monitor
")

file(WRITE "${OUTPUT_FILE}" "${HEADER}")
message(STATUS "Profile 已生成: ${OUTPUT_FILE}  (源: ${PROFILE_NAME})")
