<div align="center">

# 🧯 疑难排查

### 按症状定位问题

<p align="center">
  <b>IRIS</b> · 面向所有使用者<br>
  构建、注入、观测、发信与管道的常见故障
</p>

</div>

---

按症状组织。每条给出**判断依据**与**处理方式**。

> [!TIP]
> 排查时优先看事件日志里的 `observer_disabled` / `hook_error` / `native_sender_*` 事件，它们通常直接给出原因。

---

## 🏗️ A. 构建

### `No CMAKE_C_COMPILER could be found`，或日志里出现 MSB6001「已添加项」

**症状**

```
error MSB6001: “CL.exe”的命令行开关无效。
System.ArgumentException: 已添加项。字典中的关键字:"HTTP_PROXY"所添加的关键字:"http_proxy"
```

**原因**

这是 MSBuild 的缺陷，不是项目问题。它通过 `ProcessStartInfo.EnvironmentVariables`（.NET 的**大小写不敏感**哈希表）复制环境变量；一旦环境里同时存在仅大小写不同的同名变量，插入就抛异常，编译器探测随之失败，最终只报「找不到编译器」。

代理工具常同时设置 `HTTP_PROXY` 与 `http_proxy`（还有 `HTTPS_PROXY`/`https_proxy`、`NO_PROXY`/`no_proxy`），会触发此问题。

**处理**

`build.ps1` 会自动检测并折叠这类重复变量，并在输出里列出被折叠的名称。若你直接用 `cmake` 构建，需先清理环境。

> [!WARNING]
> `cmd` 的 `set X=` 只把值清空、**保留变量槽**，无法解决此问题。

### `D8050`

临时目录路径包含非 ASCII 字符时，MSVC 可能报 `D8050`。

**处理**：使用 `CMakePresets.json` 中的预设，它会把 `TEMP`/`TMP` 重定向到仓库内的纯 ASCII 路径。手动 configure 时自行设置：

```powershell
$env:TEMP = "$PWD\build\tmp"; $env:TMP = $env:TEMP
```

### 中文注释引发诡异的编译错误

**原因**：源码是无 BOM 的 UTF-8，而简体中文 Windows 的 ANSI 代码页是 936(GBK)。缺少 `/utf-8` 时 MSVC 按 GBK 解析源文件，中文字符串字面量被损坏，甚至因误判引号边界而直接失败。

**处理**：构建系统已全局启用 `/utf-8`。若你新增源文件，请保存为 **UTF-8 无 BOM**。

### 配置阶段报「仅支持 x64」或「要求 MSVC」

> [!IMPORTANT]
> 这不是可绕过的检查。所有 RVA 按 64 位逆向得出；接收侧用 `__try / __except` 做内存探针，GCC / Clang 不支持该语法。

### `CMakePresets.json` 版本不支持

预设需要 CMake ≥ 3.23。升级 CMake，或改用[不使用预设](building.md#-不使用预设)的手动 configure。

### Debug 与 Release 混用导致崩溃

`build/release` 与 `build/debug` 是独立目录，产物分别在各自的 `bin/Release` 与 `bin/Debug`。

> [!WARNING]
> **不要手工把不同配置的 DLL 与 EXE 放进同一目录**——CRT 不匹配会产生难以定位的崩溃。

---

## 🚀 B. 注入与启动

### 启动器报「已经在运行」

启动器拒绝在目标微信已运行时继续，因为它需要捕获微信**完整的初始化阶段**。

**处理**：完全退出微信后重试。

### 启动器报 `Weixin.exe` 不存在

默认路径为 `C:\Program Files (x86)\Tencent\WeChat\Weixin.exe`，未必与你的安装位置一致。

```powershell
$env:WECHATBOT_WEIXIN_EXE = 'C:\Program Files\Tencent\Weixin\Weixin.exe'
```

> [!IMPORTANT]
> 变量需在**启动启动器的那个 shell** 里设置。

### 启动器报 `IRIS DLL 不存在`

`IRIS.dll` 必须与 `IRIS-launcher.exe` **位于同一目录**。构建产物默认满足这一点；手工搬动时容易拆散。

### `IRIS load failed: ... probe=module_absent`

注入线程执行完毕，但目标进程的模块表里始终没有这个 DLL。通常意味着 `LoadLibraryW` 在本进程内失败，常见原因：

- DLL 依赖缺失（本项目的唯一依赖 MinHook 已静态链接，一般不会）；
- 安全软件拦截了模块加载。

错误信息里会带 `thread_exit32`（远程线程返回码）、`process_exit`、`attempts`，以及若存在同名但路径不同的模块时的 `same_name_path`。

### 事件日志里出现 `observer_disabled`

后端主动拒绝启动。看 `reason` 字段：

| `reason` 内容 | 含义 |
| :--- | :--- |
| `unexpected Weixin.dll version: x.y.z.w` | 微信版本与 Profile 不符 |
| `PE identity mismatch` | 镜像大小不符，可能是同版本号的改版 |
| `receive entry signature mismatch` | 入口机器码指纹不符，目标二进制被改过 |
| `*_invalid`（路径类） | 某项配置非法，见下节 |
| `startup_exception` | 初始化期间抛出异常 |

> [!IMPORTANT]
> **这些都意味着当前微信版本不受支持**，而不是配置问题。需要适配新版本 Profile。

### 日志文件根本不存在

三种可能，按顺序排查：

1. **配置被拒绝**——此时后端不会打开日志。查 `OutputDebugStringA` 输出（用 DebugView 之类的工具），或先排查环境变量；
2. **运行数据根目录推导不符预期**——默认是仓库根目录下的 `runtime/`（因为向上能命中 `CMakeLists.txt`）。释放到别处部署时会变成 `<IRIS.dll 所在目录>/runtime/`；
3. **日志目录不可写**。

> [!TIP]
> 用 `WECHATBOT_RUNTIME_ROOT` 显式指定可以排除第 2 种。

---

## 👀 C. 观测不到消息

### 日志有内容，但一条 `item` 都没有

先确认日志里有没有 `hook_enabled`：

```
{"kind":"hook_enabled","source":"receive_batch","rva":"0x17955e0"}
```

- **没有这一行**：接收挂钩失败。往前找 `hook_error`，它会带 `stage`（`create` / `enable`）。
- **有这一行**：挂钩成功，那么问题在别处——例如微信当时确实没有消息，或消息类型不是当前解析器覆盖的范围。

### 有 `item` 事件，但 `vtable_match` 为 `false`

消息对象的类型指纹不匹配，该条的字段**不可信**，应整条忽略。

> [!WARNING]
> 持续出现说明内存布局与 Profile 不符——即使版本号相同，也可能是被修改过的客户端。

### 出现 `dropped` 事件

> [!CAUTION]
> 事件队列溢出，**期间的消息事件已永久丢失**。队列容量固定为 256 槽，无法通过配置扩大。

**处理**：降低事件产生速率——关闭不必要的追踪开关（`WECHATBOT_SEND_TRACE` 等），或提高消费者读取频率。这类事件在开启追踪时更容易触发。

### 收到的消息字段大量为 `not_read` 或 `missing`

> [!NOTE]
> `missing` 表示微信没有发送该字段，通常是正常的（如文本消息没有 `field11`）。`not_read` 表示该字段不适用于此事件类型。

判断字段是否真的有问题，看 `status` 是否为 `invalid_object` / `unreadable` / `invalid_layout`——这些才是内存读取失败。详见[读取诊断对象与状态枚举](events.md#-7-读取诊断对象与状态枚举)。

---

## 📤 D. 发不出消息

发信被拒时，管道响应里的 `error_code` 与事件日志里的 `native_send_result` 事件都会给出原因。完整错误码语义见[命令管道协议](protocol.md#-9-错误码全表)。以下是几个最常见的：

### `hello` 一直返回 `mode: "read_only"`

说明发信能力不可用。可能原因：

1. `WECHATBOT_NATIVE_SEND` 未设为精确的 `1`；
2. `WECHATBOT_SEND_ACCOUNT` 未设置，或与实际登录账号不符；
3. 原生发信入口的特征码不匹配（日志中有 `native_sender_disabled`）。

### `native_sender_unavailable`

未开启原生发信、开关值不精确、签名不匹配，或后端尚未初始化完成。

### `native_media_sender_unavailable`

媒体发送要求**持续模式**（`WECHATBOT_SEND_MODE=continuous`）**且** `WECHATBOT_MEDIA_SEND=1`，两者缺一不可。

### `target_not_allowed`

接收人不在白名单。持续模式看 `WECHATBOT_SEND_TARGETS`（**分号**分隔）；单次模式看 `WECHATBOT_SEND_TARGET`，且单次模式**不允许群聊**。

### `session_mismatch`

`observer_session_id` 与当前会话不符。通常是复用了上一次运行抓到的旧会话 ID——每次启动后端都会生成新的。

### `rate_limited`

持续模式的最小发送间隔为 1000 ms。频繁自动回复很容易撞上。

### `unknown`

> [!WARNING]
> 已进入原生调用或 UI 执行，结果不可知。**这不是失败，是"不知道"**，重试可能导致重复发送。

---

## 🔌 E. 管道连不上

### 找不到管道名

管道名含会话 ID，`\\.\pipe\wechatbot-<session_id>`。不要猜——读事件日志里的 `command_pipe_ready`：

```json
{"kind":"command_pipe_ready","protocol_version":1,"pipe":"\\\\.\\pipe\\wechatbot-12345-133...","session_id":"12345-133..."}
```

> [!TIP]
> 其中的 `pipe` 字段**已经是完整路径**，直接用它打开即可，不要再拼 `\\.\pipe`。

### 日志里没有 `command_pipe_ready`

`WECHATBOT_COMMAND_PIPE` 未开启，或管道创建失败。后者会在日志里留下 `command_pipe_error`，带 `stage` 与 `win32_error`。

### 连接被拒 / 读不到响应

管道是**单实例短连接**：一次连接只处理一帧请求，服务端应答后等待客户端断开。

- 不要复用连接发多条请求；
- 客户端必须在读完响应后**主动关闭**连接，否则会占用这唯一的实例；
- 请求与响应各有 1000 ms 预算，超时即断开。

### 一帧就被判 `invalid_request`

协议比通用 JSON 严格得多：**只接受扁平对象**（不支持嵌套）、最多 32 个字段、拒绝重复键与未在白名单内的键。详见[请求编码约束](protocol.md#-2-请求编码约束)。

---

## 🧰 F. 排查手段

| 手段 | 用途 |
| :--- | :--- |
| 事件日志 `observer_disabled` / `hook_error` / `native_sender_*` | 后端自身直接给出的失败原因 |
| `OutputDebugStringA` 输出 | 配置被拒时**不写日志**，原因只在这里 |
| `IRIS-tests.exe` | 离线验证字符串/UTF-8/指针边界与协议解析，不需要微信 |
| `IRIS-launcher-tests.exe` | 验证路径解析、模块枚举与 DLL 可加载性 |
| `WECHATBOT_*_TRACE` 追踪开关 | 输出原生调用的完整内存视图，用于定位布局不符 |

---

## 🔗 相关文档

| 文档 | 内容 |
| :--- | :--- |
| 🛠️ **[构建与编译](building.md)** | 工具链要求、CMake 预设与构建选项、工程结构、测试 |
| 🔌 **[命令管道协议](protocol.md)** | 发信接口：分帧、5 个操作、字段约束、状态语义、错误码全表 |
| 📜 **[事件日志协议](events.md)** | 收信接口：JSONL 事件种类、字段含义、读取诊断、丢事件告警 |
| ⚙️ **[运行配置](configuration.md)** | 全部环境变量、目录布局、单次与持续模式、启动与停机行为 |
| 💡 **[内部架构](architecture.md)** | 注入与 Hook 链、内存安全模型、UI 线程调度、发信分层、停机顺序 |
