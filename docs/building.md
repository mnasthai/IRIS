<div align="center">

# 🛠️ 构建与编译

### 从源码构建 IRIS

<p align="center">
  <b>IRIS</b> · 面向 C++ / 逆向开发者<br>
  工具链要求、CMake 预设、工程结构与故障排查
</p>

</div>

---

本文档说明如何在 Windows 上从源码构建 IRIS。

> [!TIP]
> **只想使用、不想构建？** 直接下载 [Releases](https://github.com/mnasthai/IRIS/releases) 中的预编译版，解压即用——适配数据已经编译在 `IRIS.dll` 内，**不需要本文所述的任何工具链，也不需要版本 Profile**。

---

## ⚠️ 1. 前置条件

| 组件 | 要求 |
| :--- | :--- |
| 操作系统 | Windows 10 / 11 x64 |
| 编译器 | MSVC v143（Visual Studio 2022 或 Build Tools 2022），需包含「使用 C++ 的桌面开发」工作负载 |
| Windows SDK | 10.0.19041 或更高 |
| CMake | ≥ 3.23 |
| PowerShell | ≥ 7.0（仅 `build.ps1` 需要） |
| 微信版本 Profile | **必须单独下载**，见下节 |

### 🔐 版本 Profile（构建前必须准备）

针对特定微信版本逆向得到的 RVA、虚表地址与结构体偏移维护在 `profile/*.json` 中，**不随源码仓库分发**。

> **网盘链接**：https://pan.baidu.com/s/1EYrobx6mqF8CsZrSXPYe_A
>
> **提取码**：`2e5s`

下载后放入仓库的 `profile/` 目录：

```
profile/weixin-4.1.13.12.json
```

也可以用 `-DIRIS_PROFILE_JSON=<路径>` 指定其它位置。

> [!WARNING]
> 下载 Profile 后**必须重新运行 CMake 配置**——CMake 不会自动发现新出现的文件。最省事的做法是 `.\build.ps1 -Clean`。

配置阶段会把它转成 `<build>/generated/iris_profile_generated.hpp`，全部常量仍是 `inline constexpr`，因此布局写错会**直接编译失败**。完整说明见[微信版本 Profile](profile.md)。

### 🔒 工具链约束

这三条不是偏好，是项目的硬性约束，CMake 配置阶段会强制检查前两条：

- **必须 MSVC。** 接收侧每一步对微信内存的访问都用 `__try / __except` 做硬件异常隔离，这是避免读取内部结构时崩溃的最后一道防线。GCC / Clang / MinGW 不支持该语法。
- **必须 x64。** 所有函数入口 RVA、虚表地址、结构体字段偏移都是针对微信 Windows x64 版本逆向得到的常量，32 位构建在语义上不成立。
- **源码是无 BOM 的 UTF-8。** 简体中文 Windows 的 ANSI 代码页为 936(GBK)，因此 `CMakeLists.txt` 全局启用 `/utf-8`。新增源文件请保持 UTF-8 无 BOM。

### 📥 安装 CMake

```powershell
winget install Kitware.CMake
```

或从 <https://cmake.org/download/> 下载 `cmake-<版本>-windows-x86_64.zip` 便携版，解压后将 `bin` 目录加入 PATH。若不希望修改 PATH，也可以：

- 设置环境变量 `IRIS_CMAKE` 指向 `cmake.exe`；
- 或使用 `build.ps1 -CMakePath <路径>`。

---

## ⚡ 2. 快速开始

```powershell
git clone https://github.com/mnasthai/IRIS.git
cd IRIS
.\build.ps1
```

脚本依次完成：定位 CMake → 环境预处理 → 配置 → 构建 → 打印产物路径。首次全新构建约 35 秒（4 核并行），增量构建约 1 秒。

若 PowerShell 拒绝执行脚本：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

### 🎛️ 参数

| 参数 | 说明 |
| :--- | :--- |
| `-Config <Release\|Debug>` | 构建配置，默认 `Release` |
| `-Clean` | 构建前删除构建目录，做一次全新配置 |
| `-Test` | 构建后运行 ctest |
| `-Target <名称>` | 只构建指定目标 |
| `-Jobs <n>` | 并行编译进程数 |
| `-CMakePath <路径>` | 显式指定 `cmake.exe` |

```powershell
.\build.ps1                      # Release，构建全部目标
.\build.ps1 -Test                # 构建并运行测试
.\build.ps1 -Config Debug        # Debug 配置
.\build.ps1 -Clean -Target IRIS  # 清理后只构建 IRIS.dll
```

---

## 📦 3. 构建产物

产物位于 `build/<预设名>/`：

```
build/release/
├── bin/Release/
│   ├── IRIS.dll                  ← 注入目标
│   ├── IRIS-launcher.exe         ← 拉起微信并注入 IRIS.dll
│   ├── IRIS-tests.exe            ← 聚合单元测试
│   └── IRIS-launcher-tests.exe   ← 启动器测试
├── lib/Release/
│   ├── monitor_core.lib          ← 观察者内核静态库
│   ├── launcher_core.lib
│   └── minhook.lib
└── tmp/                          ← 预设重定向的 TEMP/TMP
```

> [!WARNING]
> **`IRIS-launcher-tests.exe` 与 `IRIS.dll` 必须位于同一目录。** 该测试会 `LoadLibrary` 同目录下的 IRIS.dll，以验证它能在普通宿主进程中安全加载。上述布局已保证这一点，手工搬动产物时请保持两者相邻。

---

## 🧰 4. 手动构建

### 🧩 使用预设

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

可用预设为 `release` 与 `debug`，每套同时提供 configure / build / test 三种预设。

`CMakePresets.json` 承担两件事：

1. **把 `TEMP` / `TMP` 重定向到 `<仓库>/build/tmp`。** MSVC 在临时目录路径包含非 ASCII 字符时可能报 `D8050`（例如 `C:\Users\张三\AppData\Local\Temp`），重定向到纯 ASCII 路径可规避。
2. 固定生成器为 Visual Studio 2022 x64，保证本地与 CI 的一致性。

### 🧱 不使用预设

```powershell
cmake -S . -B build\manual -G "Visual Studio 17 2022" -A x64
cmake --build build\manual --config Release
```

这种方式不会重定向 TEMP。若临时目录含非 ASCII 字符并触发 `D8050`：

```powershell
$env:TEMP = "$PWD\build\tmp"; $env:TMP = $env:TEMP
```

---

## ⚙️ 5. 构建选项

| 选项 | 默认 | 说明 |
| :--- | :--- | :--- |
| `IRIS_BUILD_TESTS` | `ON` | 构建测试与测试 runner |
| `IRIS_ENABLE_WARNINGS` | `ON` | 启用 `/W4` |
| `IRIS_WARNINGS_AS_ERRORS` | `OFF` | 把警告当作错误（`/WX`） |

```powershell
cmake --preset release -DIRIS_BUILD_TESTS=OFF
```

### 🔧 固定的编译设置

| 设置 | 原因 |
| :--- | :--- |
| C++20 | 源码使用 `std::string::starts_with`、`std::unordered_map::contains`、`std::variant` |
| 动态 CRT（`/MD`、`/MDd`） | `IRIS.dll` 注入进微信进程，而微信自身使用动态 CRT，必须保持一致。**不可改为 `/MT`** |
| `/utf-8` | 源码为无 BOM 的 UTF-8，系统 ANSI 代码页为 936 |
| `/EHsc` | 逆向调用外层大量使用 `try / catch (...)` 隔离异常 |
| `/MP` | 多进程编译 |

### 🔔 编译警告

在当前工具链（MSVC 14.44）下，Release 构建在 `/W4` 下**没有任何警告**。若你引入了新的警告，可用 `-DIRIS_WARNINGS_AS_ERRORS=ON` 让构建直接失败以便拦截。

---

## 🗂️ 6. 工程结构

CMake 目标与职责：

| 目标 | 类型 | 产物 / 内容 |
| :--- | :--- | :--- |
| `minhook` | 静态库 | MinHook，第三方 Hook 引擎（C 源码，BSD-2-Clause） |
| `monitor_core` | 静态库 | `src/` 下的内核实现，不含 `src/runtime/dllmain.cpp` |
| `launcher_core` | 静态库 | `launcher/launcher.cpp`，独立成库以便测试复用 |
| `IRIS` | 动态库 | **`IRIS.dll`** ← `monitor_core` + `src/runtime/dllmain.cpp` |
| `IRIS-launcher` | 可执行 | **`IRIS-launcher.exe`** ← `launcher/main.cpp` + `launcher_core` |
| `IRIS-tests` | 可执行 | **`IRIS-tests.exe`** ← `tests/` 下除 `launcher_tests.cpp` 外的全部测试 |
| `IRIS-launcher-tests` | 可执行 | **`IRIS-launcher-tests.exe`** ← `tests/launcher/launcher_tests.cpp` |

目标名与产物名一一对应，`cmake --build --preset release --target <目标名>` 即可单独构建。

> [!NOTE]
> `src/` 与 `tests/` 的源文件列表由 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 收集。新增 `.cpp` 后无需修改 `CMakeLists.txt`，CMake 会在下一次构建时自动重新扫描。

模块职责与运行时架构参见 `include/monitor/` 下各模块头文件的文件头注释。

---

## 🧪 7. 测试

```powershell
ctest --preset release --output-on-failure
```

### ▶️ `IRIS-tests.exe`

聚合 runner。`tests/integration/reader_tests.cpp` 提供 `Check()` 与 `wmain()`，并依次调用其余测试文件导出的 `TestXxx()` 函数。

- 全部测试在**普通本地进程**中运行，不启动微信、不安装任何 Hook。
- `TestWorkerLifecycle()` 被安排在**最后**执行，因为它会关闭日志队列并验证关闭后的准入行为。

额外提供两个运行模式：

```powershell
# 把本次运行观测到的事件写出为 JSONL 快照，便于人工查看或做黄金文件比对。
# 主文件包含 4 行（item + outbound_request / outbound_item / outbound_return），
# 同时写出两个兄弟文件：<名字>.submit.jsonl 与 <名字>.context.jsonl
build\release\bin\Release\IRIS-tests.exe --json-output out\events.jsonl

# 作为命令管道服务端运行 15 秒，供外部客户端联调协议
build\release\bin\Release\IRIS-tests.exe --serve-command-pipe \\.\pipe\iris-manual-test
```

### 🧾 `IRIS-launcher-tests.exe`

验证启动器的路径解析、模块枚举与远程模块探测，最后 `LoadLibrary` 同目录下的 `IRIS.dll`，确认该 DLL 能在普通宿主进程中安全加载与卸载。DllMain 会检测宿主不是 `Weixin.exe` 并直接返回，不会启动任何工作线程。

---

## 🧯 8. 故障排查

### 🧨 `No CMAKE_C_COMPILER could be found`，或日志中出现 MSB6001「已添加项」

**症状**

```
error MSB6001: “CL.exe”的命令行开关无效。
System.ArgumentException: 已添加项。字典中的关键字:"HTTP_PROXY"所添加的关键字:"http_proxy"
```

**原因**

MSBuild 通过 `ProcessStartInfo.EnvironmentVariables`（.NET 的**大小写不敏感**哈希表）复制环境变量。当环境里同时存在仅大小写不同的同名变量时，插入会抛异常，编译器探测随之失败，最终只报「找不到编译器」，难以定位。

代理工具经常同时设置 `HTTP_PROXY` 与 `http_proxy`（以及 `HTTPS_PROXY`/`https_proxy`、`NO_PROXY`/`no_proxy`），会触发该问题。

**解决**

`build.ps1` 会自动检测并折叠这类重复变量，并在输出中列出被折叠的变量名。使用 `cmake` 手动构建时请先清理环境。

> [!WARNING]
> 注意：`cmd` 的 `set X=` 只会把值清空、保留变量槽，**无法**解决此问题。

### 🌡️ `D8050`

临时目录路径包含非 ASCII 字符时，MSVC 可能报 `D8050`。使用[预设](#-使用预设)即可规避，或在手动构建前重定向 `TEMP` / `TMP`。

### 🈶 中文注释引发奇怪的编译错误

确认 `/utf-8` 生效，并确认源文件保存为 UTF-8 无 BOM。

### 📐 `CMakePresets.json` 版本不支持

预设需要 CMake ≥ 3.23（schema 版本 4）。请升级 CMake，或改用[不使用预设](#-不使用预设)的方式。

### 🧬 Debug 与 Release 混用

`build/release` 与 `build/debug` 是完全独立的构建目录，产物分别落在各自的 `bin/Release` 与 `bin/Debug`。不要手工把不同配置的 DLL 与 EXE 放进同一目录——CRT 不匹配会导致难以定位的崩溃。

---

## 🔗 相关文档

| 文档 | 内容 |
| :--- | :--- |
| 💡 **[内部架构](architecture.md)** | 注入与 Hook 链、内存安全模型、UI 线程调度 |
| 📜 **[事件日志协议](events.md)** | JSONL 事件种类、字段含义与读取诊断 |
| 🔌 **[命令管道协议](protocol.md)** | 发信接口分帧、操作与字段约束、错误码 |
| ⚙️ **[运行配置](configuration.md)** | 全部环境变量、目录布局与启动停机行为 |
| 🧯 **[疑难排查](troubleshooting.md)** | 构建、注入与观测链路的问题定位 |
