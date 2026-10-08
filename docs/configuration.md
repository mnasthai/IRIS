<div align="center">

# ⚙️ 运行配置

### 环境变量、目录布局与生命周期

<p align="center">
  <b>IRIS</b> · 面向 Bot 开发者<br>
  全部环境变量、单次与持续模式、启动与停机行为
</p>

</div>

---

IRIS 的全部配置来自**环境变量**。启动器在拉起微信时把自身环境块继承给微信子进程，后端在进程内读取这些变量。

因此：**所有变量都必须在启动 `IRIS-launcher.exe` 的那个 shell 里设置**，事后修改无效（除非重启整个流程）。

> [!IMPORTANT]
> 所有配置变量都必须在启动 `IRIS-launcher.exe` 的那个 shell 里设置。事后修改无效，除非重启整个流程。

---

## 📁 1. 目录布局

所有运行数据都落在**运行数据根目录**下：

```
<runtime>/
├── observer-4.1.13.12.jsonl    事件日志（见 事件日志协议）
└── media/
    ├── inbound/                接收的图片与语音
    └── outbound/               待发送的媒体文件
```

`<runtime>` 的确定顺序：

1. 若设置了 `WECHATBOT_RUNTIME_ROOT`，用它；
2. 否则从 `IRIS.dll` 所在目录**逐级向上**查找项目标记——`CMakeLists.txt`、`WeixinMonitor.sln`，或一个名字恰为 `IRIS` 的目录；
3. 找到即取其下的 `runtime/` 子目录；
4. 都没找到，则回退到 `<IRIS.dll 所在目录>/runtime/`。

按源码仓库默认布局（`build/release/bin/Release/IRIS.dll`），向上会在仓库根目录命中 `CMakeLists.txt`，因此运行数据位于 **`<仓库根>/runtime/`**。

> [!NOTE]
> `runtime/` 已在 `.gitignore` 中排除——它会记录聊天正文与媒体文件。

---

## 🛣️ 2. 路径类变量

| 🔧 变量 | 📌 默认值 | 📝 说明 |
| :--- | :--- | :--- |
| `WECHATBOT_WEIXIN_EXE` | `C:\Program Files (x86)\Tencent\WeChat\Weixin.exe` | `Weixin.exe` 绝对路径。**务必按你的实际安装位置覆盖** |
| `WECHATBOT_RUNTIME_ROOT` | 见上节推导 | 运行数据根目录 |
| `WECHATBOT_MEDIA_ROOT` | `<runtime>/media/outbound` | 待发送媒体目录 |
| `WECHATBOT_MEDIA_INBOUND_ROOT` | `<runtime>/media/inbound` | 接收媒体落盘目录 |

`WECHATBOT_WEIXIN_EXE` 只被**启动器**使用，且要求文件名恰为 `Weixin.exe`（大小写不敏感），否则启动器拒绝运行。

### 路径校验规则

路径不是简单接受字符串，而是经过严格规范化。以下情况会**使整个配置被拒绝**：

- 相对路径，或非法盘符
- 含 `< ' '`（控制字符）、`"`、`<`、`>`、`|`、`*`、`?`
- 使用设备命名空间前缀：`\\.\` 或 `\\?\`
- 含备用数据流（第二个 `:`）
- 任一目录段超过 255 字符、以空格或 `.` 结尾，或为保留名（`CON`、`PRN`、`AUX`、`NUL`、`COM1`–`COM9`、`LPT1`–`LPT9`）
- 长度达到 32768 字符上限

正斜杠会被统一为反斜杠，多余的分隔符与 `.`/`..` 会被规范化。UNC 路径（`\\server\share\...`）受支持。

---

## 🎛️ 3. 功能开关

### 指令接口

| 🔧 变量 | 🔢 取值 | 📝 说明 |
| :--- | :--- | :--- |
| `WECHATBOT_COMMAND_PIPE` | `1` | 启用命名管道指令接口。**不开则无法接收任何外部指令**，后端只做只读观测 |

### 发信

| 🔧 变量 | 🔢 取值 | 📝 说明 |
| :--- | :--- | :--- |
| `WECHATBOT_NATIVE_SEND` | `1` | 允许原生发信。**默认关闭**，即纯只读观测 |
| `WECHATBOT_SEND_MODE` | `continuous` | 持续多发模式。**其它任何值都是单次模式** |
| `WECHATBOT_SEND_ACCOUNT` | wxid | 绑定的微信号。指令中的账号必须与它及实际登录账号一致 |
| `WECHATBOT_EXPERIMENTAL_QUOTE` | `1` | 启用引用回复 |
| `WECHATBOT_MEDIA_SEND` | `1` | 启用图片/语音发送 |

### 接收与媒体

| 🔧 变量 | 🔢 取值 | 📝 说明 |
| :--- | :--- | :--- |
| `WECHATBOT_MEDIA_RECEIVE` | `1` | 启用媒体接收落盘管线 |

### 诊断追踪

以下开关默认关闭。它们会让后端额外产生大量诊断事件（见[事件日志协议](events.md#-9-诊断事件需显式开启)），仅排查问题时开启：

| 🔧 变量 | 🔢 取值 | 📝 说明 |
| :--- | :--- | :--- |
| `WECHATBOT_SEND_TRACE` | `1` | 发送信令链路观测 |
| `WECHATBOT_SEND_CONTEXT_TRACE` | `1` | 发送上下文构造观测 |
| `WECHATBOT_SEND_SUBMIT_TRACE` | `1` | 原生提交观测 |
| `WECHATBOT_MEDIA_TRACE` | `1` | 媒体接收追踪 |

> [!WARNING]
> 布尔开关一律要求**精确等于 `1`**。`true`、`yes`、`TRUE` 均视为关闭。

---

## 🔀 4. 单次模式与持续模式

后端有两种发信模式，由 `WECHATBOT_SEND_MODE` 决定。

### 单次模式（默认）

面向**首次联调**的安全上限：

- 仅使用 `WECHATBOT_SEND_TARGET`（唯一接收人）与 `WECHATBOT_SEND_TEXT`（唯一允许的文本，须**逐字节精确匹配**）；
- `origin` 必须为 `manual`；
- 整个进程生命周期内**只允许成功发出一条**消息；
- **不支持**群聊、媒体与引用回复。

### 持续模式

需要 `WECHATBOT_SEND_MODE=continuous`：

| 🔧 变量 | 📝 说明 |
| :--- | :--- |
| `WECHATBOT_SEND_TARGETS` | 允许发送的目标白名单，**分号 `;` 分隔**，自动去重并裁剪空白 |

持续模式的额外约束：

- `origin` 接受 `manual` / `ai` / `game`；
- 文本上限 16384 字节；
- 发送间隔冷却 1000 ms；
- 去重表容量 4096 条；
- 群聊、媒体、引用回复均只在持续模式下可用。

---

## 🛑 5. 配置是快速失败的

这是本项目刻意采用的设计：**任何一项配置非法，整个配置被整体拒绝**。

处理方式是 `RejectConfiguration()`——它会清空全部字段（根目录、日志路径、媒体目录、发信策略、所有开关），后端随即输出 `observer_disabled` 事件并**完全停止初始化**，不挂钩、不建管道、不落盘日志。

> [!WARNING]
> **绝不会出现"某一项非法就悄悄用默认值代替"的情况。** 这一点对发信尤其重要：一个拼错的媒体目录不应该让后端退回到某个仍然可用的默认路径继续发信。

常见的非法情形：

| 场景 | 结果 |
| :--- | :--- |
| 路径不是绝对路径 | `runtime_root_invalid` / `media_root_invalid` / `media_inbound_root_invalid` |
| 日志路径无法构成 | `log_path_invalid` |
| `WECHATBOT_SEND_ACCOUNT` 含非法字符或超长 | `send_account_invalid` |
| 文本超长或含 NUL | `send_text_invalid` |
| 环境变量读取失败 | 返回一个"非法标记"字符串而非空串，从而**不会**回退到默认目录 |

排查时请查阅事件日志里的 `observer_disabled` 事件，其 `reason` 字段即上表原因。

---

## 🚀 6. 启动与停机

### 启动顺序

后端在独立线程上完成初始化，顺序固定：

1. 读取并校验配置；非法即终止；
2. 生成会话 ID（`<进程号>-<文件时间>`）并打开事件日志；
3. 写入 `observer_start`；
4. **轮询等待 `Weixin.dll` 加载完成**（每 100 ms 一次）；
5. 校验目标镜像：文件版本、PE 标识与镜像大小、接收入口特征码；
6. 启动日志刷盘线程；
7. 绑定原生发信 API；
8. 初始化 Hook 引擎；
9. 挂钩接收路径（**必需**，失败即终止）；
10. 挂钩发送观测、上下文观测、提交观测、媒体接收（**可选**，失败仅记录）；
11. 启动命名管道服务端，写入 `command_pipe_ready`；
12. 进入保活循环。

> [!NOTE]
> **第 11 步之后**客户端才能连上管道；读取 `command_pipe_ready` 事件即可得知时机。

### 停机顺序

收到停机信号后，各子系统按**依赖的反序**退出，且共享一个超时预算：

1. 停止命名管道服务端（不再接受新指令）；
2. 等待在途的原生发信任务空闲；
3. 停止媒体接收管线；
4. 停止日志刷盘线程并排空队列；
5. 关闭并刷新日志文件。

若某一步在预算内未完成，停机**不会强行析构**，而是保留资源并每秒重试，直到全部干净退出。

> [!CAUTION]
> 挂钩本身在停机时**不会卸载**。已安装的 Hook 会继续调用原函数，但此后的诊断事件会被丢弃。因此**不要对 `IRIS.dll` 调用 `FreeLibrary`**——在途线程会访问已卸载的代码页。正常使用中该 DLL 与微信进程同生命周期。

---

## 🧪 7. 最小可用示例

```powershell
# 必填：当前登录微信的 wxid（防止切号误发）
$env:WECHATBOT_SEND_ACCOUNT = 'wxid_你的账号'

# 打开指令接口与原生发信
$env:WECHATBOT_COMMAND_PIPE = '1'
$env:WECHATBOT_NATIVE_SEND  = '1'

# 持续模式 + 目标白名单（分号分隔）
$env:WECHATBOT_SEND_MODE    = 'continuous'
$env:WECHATBOT_SEND_TARGETS = 'wxid_好友A;12345678@chatroom'

# 可选：微信不在默认安装位置时指定
$env:WECHATBOT_WEIXIN_EXE   = 'C:\Program Files\Tencent\Weixin\Weixin.exe'

# 可选：接收媒体并落盘
$env:WECHATBOT_MEDIA_RECEIVE = '1'

# 启动（会拉起微信并注入）
.\build\release\bin\Release\IRIS-launcher.exe
```

### 启动器的前置检查

`IRIS-launcher.exe` 在拉起微信前会：

1. 校验配置与 `Weixin.exe`、`IRIS.dll` 是否存在；
2. **检查目标微信是否已在运行**——已在运行则拒绝启动并要求你先完全退出。这是为了确保能捕获微信完整的初始化阶段；
3. 拉起微信并注入；
4. 注入失败时**终止它刚启动的这个微信进程**，避免留下未被观测的实例。

---

## 🔗 相关文档

| 文档 | 内容 |
| :--- | :--- |
| 🧭 **[内部架构](architecture.md)** | 注入与 Hook 链、内存安全模型、UI 线程调度、发信分层、停机顺序 |
| 🛠️ **[构建与编译](building.md)** | 工具链要求、CMake 预设与构建选项、工程结构、测试 |
| 📜 **[事件日志协议](events.md)** | 收信接口：JSONL 事件种类、字段含义、读取诊断、丢事件告警 |
| 🔌 **[命令管道协议](protocol.md)** | 发信接口：分帧、5 个操作、字段约束、状态语义、错误码全表 |
| 🧯 **[疑难排查](troubleshooting.md)** | 发不出消息、连不上管道、看不到事件 |
