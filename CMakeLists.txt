# ============================================================================
#  IRIS —— 面向微信 Windows 桌面端 (x64) 的进程内收发内核
#
#  产物：
#    IRIS.dll                 注入目标（观测 + 受控原生发信）
#    IRIS-launcher.exe        拉起微信并注入 IRIS.dll
#    IRIS-tests.exe           聚合单元测试（reader_tests.cpp 提供 wmain）
#    IRIS-launcher-tests.exe  启动器测试（需要 IRIS.dll 与之同目录）
#
#  要求：
#    Windows x64 + MSVC (Visual Studio 2022 / Build Tools) + CMake >= 3.23
#    （推荐使用 CMakePresets.json 中的预设，见 docs/building.md）
# ============================================================================

cmake_minimum_required(VERSION 3.23)

project(IRIS
    VERSION 0.1.0
    DESCRIPTION "In-process receive/inject observer for WeChat for Windows"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# 平台与架构约束
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "IRIS 仅支持 Windows：代码依赖 Win32 API 与 MSVC 的 SEH (__try/__except)。")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "IRIS 要求 MSVC：接收侧大量使用 SEH (__try/__except) 做内存探针，"
                        "GCC/Clang 不支持该语法。")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "IRIS 仅支持 x64：所有 RVA、虚表地址与结构体布局均按 64 位逆向得出，"
                        "32 位构建必然错误。")
endif()

# ---------------------------------------------------------------------------
# 全局构建默认值
# ---------------------------------------------------------------------------

# 单配置生成器（NMake / Ninja）默认 Release；多配置生成器（VS）此项无意义。
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# 动态 CRT (/MD、/MDd)。observer DLL 要被注入进微信进程，微信自身使用动态 CRT，
# 因此这里必须与宿主保持一致，绝不能改成 /MT。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# 统一输出目录。使用 $<CONFIG> 保证单配置与多配置生成器布局一致，
# 且同一配置下的 EXE 与 DLL 始终相邻 —— IRIS-launcher-tests 会 LoadLibrary 同目录的 IRIS.dll。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(IRIS_BUILD_TESTS          "构建单元测试与测试 runner"        ON)
option(IRIS_WARNINGS_AS_ERRORS   "把编译警告当作错误 (/WX)"          OFF)
option(IRIS_ENABLE_WARNINGS      "启用 /W4 高警告等级"               ON)

# ---------------------------------------------------------------------------
# 产物命名
#   对外产物统一使用 IRIS 前缀。OUTPUT_NAME 不含扩展名，CMake 会自行追加
#   .dll / .exe；静态库属于中间产物，保留按模块划分的命名。
# ---------------------------------------------------------------------------
set(IRIS_OBSERVER_MODULE     "IRIS.dll")             # 注入目标完整文件名，供 MONITOR_OBSERVER_MODULE 使用
set(IRIS_OBSERVER_STEM       "IRIS")                 # -> IRIS.dll
set(IRIS_LAUNCHER_STEM       "IRIS-launcher")        # -> IRIS-launcher.exe
set(IRIS_TESTS_STEM          "IRIS-tests")           # -> IRIS-tests.exe
set(IRIS_LAUNCHER_TESTS_STEM "IRIS-launcher-tests")  # -> IRIS-launcher-tests.exe

# ---------------------------------------------------------------------------
# 微信版本 Profile
#
#   针对特定微信版本逆向得到的 RVA / 虚表地址 / 结构体偏移 / 机器码指纹，
#   全部维护在 JSON 中并由此处生成 C++ 头文件。该 JSON 不随源码仓库分发，
#   获取方式见 README 与 docs/profile.md。
#
#   生成物是 inline constexpr 常量，因此 native_text.hpp 等处的 static_assert
#   编译期校验全部继续生效——JSON 写错尺寸会直接编译失败，而不是运行期越界。
# ---------------------------------------------------------------------------
set(IRIS_PROFILE_JSON "" CACHE FILEPATH
    "微信版本 Profile 的 JSON 路径。留空则自动使用 profile/ 下唯一的 .json（example 除外）。")

if(NOT IRIS_PROFILE_JSON)
    file(GLOB IRIS_PROFILE_CANDIDATES "${CMAKE_CURRENT_SOURCE_DIR}/profile/*.json")
    list(FILTER IRIS_PROFILE_CANDIDATES EXCLUDE REGEX "example\\.json$")
    list(LENGTH IRIS_PROFILE_CANDIDATES IRIS_PROFILE_COUNT)
    if(IRIS_PROFILE_COUNT EQUAL 1)
        list(GET IRIS_PROFILE_CANDIDATES 0 IRIS_PROFILE_JSON)
    elseif(IRIS_PROFILE_COUNT GREATER 1)
        message(FATAL_ERROR
            "profile/ 下存在多个候选 JSON，请用 -DIRIS_PROFILE_JSON=<路径> 明确指定一个：\n"
            "  ${IRIS_PROFILE_CANDIDATES}")
    endif()
endif()

if(NOT IRIS_PROFILE_JSON)
    message(FATAL_ERROR
        "未找到微信版本 Profile 文件。该文件包含针对特定微信版本逆向得到的 RVA、虚表地址与结构体偏移，"
        "出于合规考虑不随源码仓库分发。获取方式见 README 的「微信版本 Profile」一节中的网盘链接；"
        "下载后放入仓库的 profile/ 目录，或用 -DIRIS_PROFILE_JSON=<路径> 指定。"
        "注意：下载后需要重新运行 CMake 配置（CMake 不会自动发现新出现的文件），最省事的做法是 .\\build.ps1 -Clean。"
        "JSON 结构与字段含义见 docs/profile.md。")
endif()

if(NOT EXISTS "${IRIS_PROFILE_JSON}")
    message(FATAL_ERROR "指定的 Profile JSON 不存在：${IRIS_PROFILE_JSON}")
endif()

set(IRIS_PROFILE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(IRIS_PROFILE_HEADER "${IRIS_PROFILE_GENERATED_DIR}/iris_profile_generated.hpp")

# 子进程的输出按原始字节转发会被控制台按 ANSI 代码页误解码（中文变成乱码），
# 因此让它把诊断写入文件，由父进程以自身编码输出。
set(IRIS_PROFILE_ERROR_FILE "${CMAKE_BINARY_DIR}/profile-generate-error.txt")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DPROFILE_JSON=${IRIS_PROFILE_JSON}"
            "-DOUTPUT_FILE=${IRIS_PROFILE_HEADER}"
            "-DSCHEMA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/profile/example.json"
            "-DERROR_FILE=${IRIS_PROFILE_ERROR_FILE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateProfile.cmake"
    RESULT_VARIABLE IRIS_PROFILE_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
)
if(NOT IRIS_PROFILE_RESULT EQUAL 0)
    set(IRIS_PROFILE_DETAIL "")
    if(EXISTS "${IRIS_PROFILE_ERROR_FILE}")
        file(READ "${IRIS_PROFILE_ERROR_FILE}" IRIS_PROFILE_DETAIL)
        file(REMOVE "${IRIS_PROFILE_ERROR_FILE}")
    endif()
    if(IRIS_PROFILE_DETAIL STREQUAL "")
        set(IRIS_PROFILE_DETAIL "生成器未给出诊断信息，请手动运行：cmake -DPROFILE_JSON=<路径> -DOUTPUT_FILE=<输出> -P cmake/GenerateProfile.cmake")
    endif()
    message(FATAL_ERROR "Profile 生成失败：\n${IRIS_PROFILE_DETAIL}")
endif()
message(STATUS "Profile 已生成 -> ${IRIS_PROFILE_HEADER}")

# Profile 变更后自动重新配置
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${IRIS_PROFILE_JSON}")

# 读出目标微信版本，供配置摘要展示
file(READ "${IRIS_PROFILE_JSON}" IRIS_PROFILE_TEXT)
string(JSON IRIS_PROFILE_TARGET_VERSION ERROR_VARIABLE _ GET "${IRIS_PROFILE_TEXT}" target_version)
string(JSON IRIS_PROFILE_ID ERROR_VARIABLE _ GET "${IRIS_PROFILE_TEXT}" profile_name)

# ---------------------------------------------------------------------------
# 本项目 C++ 目标的统一编译设置
# ---------------------------------------------------------------------------
function(iris_configure_cxx_target target)
    # 源码用到 std::string::starts_with / std::unordered_map::contains / std::variant
    target_compile_features(${target} PUBLIC cxx_std_20)

    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(IRIS_ENABLE_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
    endif()

    # /utf-8 是必需的，不是优化项 源码为无 BOM 的 UTF-8,
    # 导致中文字符串字面量损坏，甚至因误判引号边界而编译失败;
    # /MP 启用多进程编译以缩短构建时间.
    target_compile_options(${target} PRIVATE /utf-8 /MP /EHsc)

    if(IRIS_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE /WX)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 第三方：MinHook (BSD-2-Clause)
#   作为静态库编译。MinHook.h 未使用 dllexport/dllimport 宏，
#   因此静态链接无需任何额外定义。
# ---------------------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)

target_include_directories(minhook
    PUBLIC  third_party/minhook/include
    PRIVATE third_party/minhook/src
)

# MinHook 保持上游的默认警告等级与编译行为，不套用本项目的 /W4。
target_compile_definitions(minhook PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(minhook PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED OFF)

# ---------------------------------------------------------------------------
# 观察者内核静态库
#   DllMain (src/runtime/dllmain.cpp) 被排除：它只属于 DLL 目标，
#   放进静态库虽然也能被链接器按需拉取，但意图不清晰。
# ---------------------------------------------------------------------------
file(GLOB_RECURSE IRIS_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER IRIS_CORE_SOURCES EXCLUDE REGEX "src/runtime/dllmain\\.cpp$")

if(NOT IRIS_CORE_SOURCES)
    message(FATAL_ERROR "未在 src/ 下找到任何源文件，检查仓库是否完整。")
endif()
list(LENGTH IRIS_CORE_SOURCES IRIS_CORE_SOURCE_COUNT)

add_library(monitor_core STATIC ${IRIS_CORE_SOURCES})

target_include_directories(monitor_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${IRIS_PROFILE_GENERATED_DIR}"
)

# PUBLIC：minhook 的头文件与实现需要传递给所有消费者
target_link_libraries(monitor_core PUBLIC minhook)

iris_configure_cxx_target(monitor_core)

# ---------------------------------------------------------------------------
# 观察者 DLL（注入目标）
# ---------------------------------------------------------------------------
add_library(IRIS SHARED
    src/runtime/dllmain.cpp
)

target_link_libraries(IRIS PRIVATE monitor_core)
set_target_properties(IRIS PROPERTIES OUTPUT_NAME "${IRIS_OBSERVER_STEM}")
iris_configure_cxx_target(IRIS)

# ---------------------------------------------------------------------------
# 启动器
#   launcher.cpp 单独成库，使 IRIS-launcher-tests 可以只链接实现而排除 main.cpp。
# ---------------------------------------------------------------------------
add_library(launcher_core STATIC
    launcher/launcher.cpp
)

target_include_directories(launcher_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher"
)

target_link_libraries(launcher_core PUBLIC monitor_core)

# launcher.hpp 用该宏定位 DLL；PUBLIC 保证 IRIS-launcher 与 IRIS-launcher-tests 拿到一致的名字。
target_compile_definitions(launcher_core PUBLIC
    "MONITOR_OBSERVER_MODULE=L\"${IRIS_OBSERVER_MODULE}\""
)

iris_configure_cxx_target(launcher_core)

add_executable(IRIS-launcher
    launcher/main.cpp
)

target_link_libraries(IRIS-launcher PRIVATE launcher_core)
set_target_properties(IRIS-launcher PROPERTIES OUTPUT_NAME "${IRIS_LAUNCHER_STEM}")
iris_configure_cxx_target(IRIS-launcher)

# 启动器运行时需要在同目录找到 IRIS.dll
add_dependencies(IRIS-launcher IRIS)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(IRIS_BUILD_TESTS)
    enable_testing()

    # 聚合测试：tests/integration/reader_tests.cpp 提供 Check() 与 wmain()，
    # 并调用其余各 TestXxx()。tests/launcher/launcher_tests.cpp 自带 wmain，
    # 必须排除，否则出现重复入口点。
    file(GLOB_RECURSE IRIS_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )
    list(FILTER IRIS_TEST_SOURCES EXCLUDE REGEX "tests/launcher/launcher_tests\\.cpp$")

    if(NOT IRIS_TEST_SOURCES)
        message(FATAL_ERROR "未在 tests/ 下找到测试源文件。")
    endif()

    add_executable(IRIS-tests ${IRIS_TEST_SOURCES})
    target_link_libraries(IRIS-tests PRIVATE monitor_core)
    set_target_properties(IRIS-tests PROPERTIES OUTPUT_NAME "${IRIS_TESTS_STEM}")
    iris_configure_cxx_target(IRIS-tests)

    # 启动器测试：自验路径解析、模块枚举与远程模块探测，
    # 最后 LoadLibrary 同目录的 IRIS.dll 验证其可在普通宿主进程中加载。
    add_executable(IRIS-launcher-tests
        tests/launcher/launcher_tests.cpp
    )
    target_link_libraries(IRIS-launcher-tests PRIVATE launcher_core)
    set_target_properties(IRIS-launcher-tests PROPERTIES OUTPUT_NAME "${IRIS_LAUNCHER_TESTS_STEM}")
    iris_configure_cxx_target(IRIS-launcher-tests)

    # 必须先构建出 DLL，IRIS-launcher-tests 才能加载它
    add_dependencies(IRIS-launcher-tests IRIS)

    add_test(NAME IRIS-tests          COMMAND IRIS-tests)
    add_test(NAME IRIS-launcher-tests COMMAND IRIS-launcher-tests)

    # 让 CTest 从产物目录运行，保证 DLL 与测试 EXE 的相对位置符合预期
    set_tests_properties(IRIS-tests IRIS-launcher-tests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:IRIS-launcher-tests>"
    )
endif()

# ---------------------------------------------------------------------------
# 配置摘要
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==================== IRIS 配置摘要 ====================")
message(STATUS "  版本              : ${PROJECT_VERSION}")
message(STATUS "  生成器            : ${CMAKE_GENERATOR}")
if(CMAKE_CONFIGURATION_TYPES)
    message(STATUS "  可用配置          : ${CMAKE_CONFIGURATION_TYPES}")
else()
    message(STATUS "  构建类型          : ${CMAKE_BUILD_TYPE}")
endif()
message(STATUS "  C++ 标准          : 20")
message(STATUS "  运行时库          : ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  目标微信版本      : ${IRIS_PROFILE_TARGET_VERSION}  (profile: ${IRIS_PROFILE_ID})")
message(STATUS "  内核源文件数      : ${IRIS_CORE_SOURCE_COUNT}")
message(STATUS "  构建测试          : ${IRIS_BUILD_TESTS}")
message(STATUS "  警告即错误        : ${IRIS_WARNINGS_AS_ERRORS}")
message(STATUS "  产物              : ${IRIS_OBSERVER_MODULE}")
message(STATUS "                      ${IRIS_LAUNCHER_STEM}.exe")
if(IRIS_BUILD_TESTS)
    message(STATUS "                      ${IRIS_TESTS_STEM}.exe")
    message(STATUS "                      ${IRIS_LAUNCHER_TESTS_STEM}.exe")
endif()
message(STATUS "  输出目录          : ${CMAKE_BINARY_DIR}/bin/<配置>")
message(STATUS "======================================================")
message(STATUS "")
