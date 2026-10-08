<div align="center">

# 📜 事件日志协议

### 收信接口完整参考

<p align="center">
  <b>IRIS</b> · 面向 Bot 开发者<br>
  JSONL 事件种类、字段含义、读取诊断与丢事件告警
</p>

</div>

本文档是 IRIS **接收接口**的完整参考。后端把观测到的一切追加写入一个 JSONL 文件，Bot 只需顺序读取它。

发信不经由此文件，而是走命名管道，见[命令管道协议](protocol.md)。

---

## 📁 1. 文件位置与格式

```
<运行数据根目录>/observer-4.1.13.12.jsonl
```

默认运行数据根目录是**仓库根目录下的 `runtime/`**；可用环境变量覆盖，见[运行配置](configuration.md)。

| 属性 | 说明 |
| :--- | :--- |
| 编码 | UTF-8 |
| 结构 | 每行一个完整 JSON 对象，行尾 `\r\n` |
| 追加 | 只追加，从不重写或截断 |
| 共享 | 以 `FILE_SHARE_READ \| FILE_SHARE_WRITE \| FILE_SHARE_DELETE` 打开，**其它进程可同时读取** |
| 刷盘 | 每一行写入后立即 `FlushFileBuffers`，读到即为落盘 |

因为每行都独立刷盘，Bot 可以用「记录当前偏移 → 循环 `readline()`」的方式稳定地增量读取。

> [!IMPORTANT]
> **需要处理不完整行。** 虽然写入方逐行刷盘，但读取方仍可能恰好读到只写了一半的行。稳妥做法是：读到 `JSONDecodeError` 时**不要把偏移推进到行尾**，下次重试该行。

---

## 🧾 2. 通用字段

绝大多数事件都带以下头部字段：

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `kind` | string | 事件类型，见下文各节 |
| `schema_version` | number | 当前为 `2`（部分手写的诊断事件没有该字段） |
| `seq` | number | 全局单调递增序号 |
| `tick` | number | `GetTickCount64()`，仅用于同进程内相对计时 |
| `tid` | number | 产生该事件的线程 ID |
| `call_id` | number | 一次"调用"内多个事件共享，用于把进入/返回配对 |
| `observed_unix_ms` | number | 观测时刻的 Unix 毫秒时间戳 |
| `session_id` | string | 本次运行的会话 ID，所有行都有 |

> [!NOTE]
> `seq` 与 `call_id` 都是进程内计数器，**不跨运行连续**。请用 `session_id` 区分不同的运行实例。

---

## 🔄 3. 生命周期事件

### `observer_start`

后端开始初始化。字段：`build`、`target_version`、`mode`、`pid`。

### `command_pipe_ready`

**Bot 的握手事件。** 收到它即可建立管道连接。

| 字段 | 说明 |
| :--- | :--- |
| `protocol_version` | `1` |
| `pipe` | 完整的管道路径，形如 `\\.\pipe\wechatbot-<session_id>`，**可直接用于打开管道** |
| `session_id` | 即 `observer_session_id`，发信时必须回填 |

### `observer_disabled`

后端拒绝启动，之后不会再有消息事件。字段：`reason`。常见值包括版本不匹配、特征码不匹配、配置非法。

### `hook_error`

某一挂钩阶段失败。字段：`stage`（如 `create` / `enable` / `logger_thread` / `initialize`），部分带 `source`。

---

## 💬 4. 消息事件 `item`

每收到一条消息产生一个 `item` 事件。这是 Bot 最主要的输入。

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `kind` | string | `item` |
| `seq` / `call_id` / `tick` / `tid` / `observed_unix_ms` / `session_id` | — | 见通用字段 |
| `source` | string | `receive_batch` |
| `vtable_match` | boolean | 消息对象类型指纹是否匹配。**为 `false` 时后续字段不可信，应忽略该条** |
| `msg_type` | number | 消息类型：`1` 文本、`3` 图片、`34` 语音、`47` 表情、`49` App/卡片等 |
| `from` | string | 会话 ID。私聊为对方 wxid，群聊为 `...@chatroom`。**回复要发回这里** |
| `to` | string | 接收方（通常是本机账号） |
| `content` | string | 正文。群聊消息形如 `wxid_发送者:\n实际正文` |
| `msg_source` | string | 消息源 XML，群 `@` 列表与引用信息在此 |
| `field11` | string | 附加字段，多为空 |
| `raw_fields` | object | 原始标量字段，见第 8 节 |
| `snapshot_limit` | number | 单批次最多记录多少条（当前 32） |
| `*_read` | object | 每个文本字段的读取诊断，见第 7 节 |

### 截断

`from` / `to` 上限 256 字节，`content` 4096 字节，`msg_source` 8192 字节。超长时字段被**按 UTF-8 码点边界**截断，并在对应的 `*_read` 对象里报告 `status: "truncated"` 与真实 `original_bytes`。截断不会切断多字节字符。

### 群聊解析

群消息的正文形如：

```
wxid_abc123:
这周末有空吗
```

因此 `from` 是群 ID，真实发送者藏在 `content` 的前缀里。按第一个 `:\n` 拆分即可：

```python
conversation = event["from"]
content      = event["content"]
sender       = conversation
if conversation.endswith("@chatroom") and ":\n" in content:
    sender, content = content.split(":\n", 1)
```

---

## 🖼️ 5. 媒体事件

开启媒体接收后（`WECHATBOT_MEDIA_RECEIVE=1`），下载完成的图片/语音会被校验并原子落盘，随后产生：

### `media_asset` / `media_encoded_asset`

媒体文件已保存。`status` 为 `available`（明文）或 `encoded`（微信私有编码格式，如 `.wxgf`）。

| 字段 | 说明 |
| :--- | :--- |
| `schema_version` / `seq` / `observed_unix_ms` | — |
| `msg_type` | `3` 图片 / `34` 语音 |
| `message_id` | 字符串形式的服务端消息 ID |
| `from` / `to` / `sender` | 仅在非空时出现 |
| `sha256` | 已保存文件的 SHA-256 |
| `byte_length` | 文件字节数 |
| `asset_name` | 落盘文件名（位于 `<运行数据根目录>/media/inbound/`） |
| `image_variant` / `resource_kind` / `resource_path_offset` / `width` / `height` | 仅图片 |

### `media_image_candidate`

同一张图片的两个候选路径各自的探测结果，**属于诊断信息**。字段含 `capture_status`、`file_size`、`win32_error`、`source_path`、`detected_format`、`plaintext_format`、`asset_published`、`asset_error`。

### `media_asset_error` / `media_asset_dropped`

媒体落盘失败或快照被丢弃。字段：`reason` / `status`，或 `total`。

---

## ⚠️ 6. 丢事件告警

### `dropped`

日志环形队列写满时产生，字段 `total` 为累计丢弃条数。

> [!CAUTION]
> **这个事件必须被 Bot 处理。** 它意味着在两次 `dropped` 之间，**部分消息事件永久丢失且无从补回**。若你的业务不能容忍漏消息，应在看到它时告警。
>
> 队列容量固定，无法通过配置扩大。

---

## 🩺 7. 读取诊断对象与状态枚举

每个文本字段都配有一个同名的 `*_read` 对象：

```json
"content_read": {"status": "ok", "original_bytes": 28, "captured_bytes": 28}
```

`original_bytes` 在长度未知时为 `null`。`status` 取值：

| `status` | 含义 |
| :--- | :--- |
| `ok` | 正常读取 |
| `empty` | 字段存在但内容为空 |
| `missing` | 字段存在位为 0，微信未发送该字段 |
| `inner_missing` | 外层存在但内层标志位为 0 |
| `invalid_object` | 指针为空或不可读 |
| `invalid_wrapper` | 包装对象类型指纹不匹配 |
| `invalid_layout` | 长度或布局自相矛盾 |
| `unreadable` | 目标内存不可读 |
| `exception` | 读取期间触发硬件异常并被隔离 |
| `truncated` | 超过缓冲区上限，已按码点边界截断 |
| `invalid_utf8` | 内容不是合法 UTF-8，未捕获任何字节 |
| `not_read` | 该字段不适用于此事件类型 |

> [!TIP]
> `status` 不是 `ok` 也不一定是错误：`empty` 与 `missing` 在正常消息里很常见（例如文本消息没有 `field11`）。**切勿把非 `ok` 一律当异常**。

---

## 🗂️ 8. `raw_fields` 映射

`raw_fields` 暴露消息结构体里的原始标量，键是**协议字段编号**（字符串形式），值是**字符串形式的整数**或 `null`：

```json
"raw_fields": {"1":"1","6":"9007199254740993","7":null,"13":"1789702480"}
```

- 值为 `null` 表示该字段的存在位为 0，即微信未发送它；
- 超过 2^53 的值（如服务端消息 ID）以字符串传递，避免 JSON 双精度丢精度；
- 有符号字段以十进制负数字符串出现。

| 编号 | 含义 |
| :--- | :--- |
| `1` | 消息类型 `msg_type` |
| `6` | 消息状态 |
| `7` | 消息状态（扩展） |
| `9` | — |
| `12` | 服务端消息 ID（64 位） |
| `13` | 消息时间戳（秒） |
| `15` | — |

> [!NOTE]
> 上表是当前版本适配的字段编号。`raw_fields` 的价值在于：即使高层字段解析失败，原始标量仍在，便于自定义排查。

---

## 🧪 9. 诊断事件（需显式开启）

以下事件默认**不产生**，需通过环境变量开启对应追踪开关（见[运行配置](configuration.md)）。它们面向逆向与排查，Bot 通常不需要。

| 开关 | 产生的事件 |
| :--- | :--- |
| `WECHATBOT_SEND_TRACE` | `outbound_request`、`outbound_item`、`outbound_return`，以及 `send_trace_enabled` |
| `WECHATBOT_SEND_CONTEXT_TRACE` | `send_context_enter`、`send_context_return`，以及 `send_context_trace_enabled` |
| `WECHATBOT_SEND_SUBMIT_TRACE` | `send_submit_enter`、`send_submit_return`，以及 `send_submit_trace_enabled` |
| `WECHATBOT_MEDIA_TRACE` | `media_receive_sample`、`media_image_candidate`，以及 `media_receive_trace_enabled` |

其它始终可能出现的诊断事件：

| `kind` | 说明 |
| :--- | :--- |
| `hook_enabled` | 某挂钩点安装成功，带 `source` 与 `rva` |
| `hook_error` | 某挂钩点失败，带 `stage` 与 `source` |
| `*_hook_disabled` | 因特征码不匹配而未启用某项观测 |
| `native_sender_ready` / `native_sender_disabled` | 原生发信后端初始化结果 |
| `native_send_request` / `native_send_result` | 每次发信的结构化结果，含 `request_id`、`attempt_id`、`status`、`reason` |
| `media_receive_enabled` / `media_receive_disabled` | 媒体接收管线状态 |

> [!NOTE]
> 追踪类事件的字段极其详尽（指针、虚表、偏移、栈 RVA 采样等），且每条都带 `session_id`，此处不逐一展开；它们的设计目标是让你能在**离线状态下**复现一次原生调用的完整内存视图。

---

## 🚀 10. 最小可用消费者

```python
import json, os, time

LOG = r"<仓库根>\runtime\observer-4.1.13.12.jsonl"

while not os.path.exists(LOG):
    time.sleep(0.2)

with open(LOG, "r", encoding="utf-8", errors="replace") as log:
    log.seek(0, os.SEEK_END)
    while True:
        offset = log.tell()
        line = log.readline()
        if not line:
            time.sleep(0.05)
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            log.seek(offset)      # 可能是不完整的半行，回退重试
            time.sleep(0.05)
            continue

        kind = event.get("kind")
        if kind == "item" and event.get("msg_type") == 1:
            if not event.get("vtable_match"):
                continue          # 对象指纹不匹配，字段不可信
            print(event["from"], event["content"])
        elif kind == "dropped":
            print("警告：事件队列溢出，已丢失", event["total"], "条")
        elif kind == "observer_disabled":
            print("后端未启动：", event.get("reason"))
            break
```

---

## 🔗 相关文档

- 🔌 [命令管道协议](protocol.md)
- ⚙️ [运行配置](configuration.md)
- 🧯 [疑难排查](troubleshooting.md)
- 💡 [内部架构](architecture.md)
- 🛠️ [构建与编译](building.md)
