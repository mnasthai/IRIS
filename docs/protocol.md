<div align="center">

# 🔌 命令管道协议

### 发信接口完整参考

<p align="center">
  <b>IRIS</b> · 面向 Bot 开发者<br>
  传输层分帧、全部字段、状态语义与错误码
</p>

</div>

---

本文档是 IRIS **发信接口**的完整参考：传输层分帧、请求编码约束、全部字段、状态语义与错误码。

接收消息不经由本协议，而是写入 JSONL 事件日志，见[事件日志协议](events.md)。

> [!IMPORTANT]
> 协议版本：**1**。响应中的 `protocol_version` 恒为 1。当前不支持任何其它版本。

---

## 🔌 1. 传输层

### 管道

```
\\.\pipe\wechatbot-<observer_session_id>
```

`observer_session_id` 由后端在每次启动时生成，并出现在两处：

- `command_pipe_ready` 事件行（见[事件日志协议](events.md)），字段 `session_id`
- 每条 `hello` 响应的 `observer_session_id` 字段

客户端**不需要预先知道**管道名：读事件日志拿到 `command_pipe_ready` 即可。

### 分帧

双向均为：**4 字节小端无符号整数长度** + 该长度的 UTF-8 JSON 载荷。

- 单帧上限 **65536 字节**（含长度前缀之外的全部载荷）
- 请求与响应同构

### 连接模型

管道为**单实例、短连接**：

1. 客户端连接；
2. 发送一帧请求；
3. 读取一帧响应；
4. **客户端主动关闭连接**，服务端随即断开并等待下一个连接。

服务端在应答后会等待客户端关闭，但不会无界等待：读取请求与回写响应各有 1000 ms 预算，超时即进入下一步。客户端不应复用连接发多条请求。

### 访问控制

管道的安全描述符为 `D:P(A;;GA;;;SY)(A;;GRGW;;;<当前用户 SID>)`，即**仅 LocalSystem 完全控制 + 运行微信的当前用户读写**，并以 `PIPE_REJECT_REMOTE_CLIENTS` 创建，拒绝一切远程网络客户端。

---

## 📏 2. 请求编码约束

请求必须是**扁平的 UTF-8 JSON 对象**。解析器是专为本协议手写的，规则比通用 JSON 更严：

| 约束 | 说明 |
| :--- | :--- |
| 载荷长度 | 1 .. 65536 字节 |
| 有效编码 | 必须是合法 UTF-8（非法即 `invalid_utf8`） |
| 顶层类型 | 必须是对象 `{}`，不接受数组或标量 |
| 嵌套 | **不支持**。对象与数组值一律拒绝 |
| 字段数 | 最多 32 个 |
| 重复键 | 拒绝 |
| 未知键 | 拒绝（见下方各操作的白名单） |
| 数字 | 仅非负整数。不接受符号、小数、指数、前导零（`"007"` 非法，`"0"` 合法） |
| 字符串 | 支持全部 JSON 转义，含 `\uXXXX` 与 UTF-16 代理对（emoji 可用） |
| 控制字符 | 字符串内不允许出现未转义的控制字符 |

> [!IMPORTANT]
> 由于数字只支持整数，**超出 2^53 的 ID 一律用 JSON 字符串传递**（例如 `quote_message_id`）。

---

## 🧭 3. 操作总览

| `op` | 用途 |
| :--- | :--- |
| `hello` | 探测登录态与基础发信能力 |
| `hello_media` | 在 `hello` 基础上额外探测图片/语音能力 |
| `send_text` | 发送文本（私聊或群聊，可带 @成员） |
| `send_rich_text` | 发送带 @成员 或引用卡片的文本 |
| `send_media` | 发送图片或语音 |

> [!TIP]
> `hello` / `hello_media` 不触碰原生发信路径，可安全地反复调用。

---

## 🧩 4. 通用字段

以下字段对所有操作都是允许的：

| 字段 | 类型 | 必填 | 约束 |
| :--- | :--- | :--- | :--- |
| `op` | string | ✅ | 1 .. 32 字符 |
| `protocol_version` | number | ✅ | 必须恰为 `1` |
| `request_id` | string | ✅ | 1 .. 128 字符 |

> [!NOTE]
> `request_id` 是幂等去重的键，必须全局唯一。详见[第 8 节](#-8-幂等与限流)。

---

## 👋 5. `hello` / `hello_media`

请求仅含通用字段：

```json
{"op":"hello","protocol_version":1,"request_id":"1"}
```

响应：

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `op` | string | `hello` 或 `hello_media` |
| `protocol_version` | number | `1` |
| `request_id` | string | 回显 |
| `observer_session_id` | string | 本次运行的会话 ID，后续发信必须携带 |
| `target_version` | string | 后端适配的微信版本，当前为 `4.1.13.12` |
| `mode` | string | `send_enabled` 或 `read_only` |
| `account_id` | string \| null | 当前登录的 wxid；未就绪时为 `null` |
| `account_verified` | boolean | 登录态是否已就绪 |
| `send_text` | boolean | 是否可发文本 |
| `send_group_text` | boolean | 是否可发群聊文本 |
| `send_mention` | boolean | 是否可发群内 @ |
| `send_quote` | boolean | 是否可发引用回复 |
| `max_text_bytes` | number | 当前模式下的正文上限 |
| `send_image` | boolean | **仅 `hello_media`** |
| `send_voice` | boolean | **仅 `hello_media`** |

> [!IMPORTANT]
> **客户端应以本响应判断可用能力，而不是解析日志或凭配置推断。** 发送能力是账号、配置与原生函数签名匹配情况的综合结果；`mode` 为 `read_only` 时任何发信都会被拒。

---

## 📤 6. `send_text` / `send_rich_text` / `send_media`

### 6.1 通用发信字段

| 字段 | 类型 | 必填 | 约束 |
| :--- | :--- | :--- | :--- |
| `attempt_id` | string | ✅ | 1 .. 128 字符 |
| `observer_session_id` | string | ✅ | 必须等于当前会话，否则 `session_mismatch` |
| `expected_account_id` | string | ✅ | 1 .. 256 字符；必须与配置及实际登录账号一致 |
| `target_id` | string | ✅ | 1 .. 512 字符；须为合法接收人且在白名单内 |
| `created_at` | string | ✅ | UTC ISO-8601，1 .. 64 字符 |
| `expires_at` | string | ✅ | UTC ISO-8601，1 .. 64 字符 |
| `origin` | string | ✅ | `manual` / `ai` / `game` 之一 |
| `source_event_key` | string \| null | ❌ | ≤ 512 字符，用于关联触发的源事件 |
| `text` | string | 条件 | 1 .. 16384 字节；**`send_media` 禁止携带** |

### 6.2 `send_rich_text` 专属

> [!WARNING]
> `send_rich_text` 要求**至少提供 `at_user_list` 或一组引用字段**，否则报 `invalid_request`。

| 字段 | 类型 | 约束 |
| :--- | :--- | :--- |
| `at_user_list` | string | 1 .. 2063 字符。逗号分隔，1..16 个互不重复的**私聊** wxid；不接受群 ID，不接受 `filehelper`，不接受尾随逗号 |

群内 @ 还要求 `target_id` 是群（`...@chatroom`），且仅在持续模式下可用。

### 6.3 引用字段

> [!WARNING]
> 引用回复**必须一次性提供全部 9 个字段**（缺任意一个即报错）。仅在 `send_rich_text` 下可用，且需要后端开启 `WECHATBOT_EXPERIMENTAL_QUOTE`。

| 字段 | 类型 | 约束 |
| :--- | :--- | :--- |
| `quote_message_id` | string | **字符串形式的无符号 64 位整数**，不可为 0 |
| `quote_from_id` | string | 1 .. 256 字符，合法接收人 |
| `quote_to_id` | string | 1 .. 256 字符，合法接收人 |
| `quote_sender_id` | string | 1 .. 256 字符，合法接收人，且**不能是群** |
| `quote_conversation_id` | string | 1 .. 256 字符，合法接收人，且**必须等于 `target_id`** |
| `quote_text` | string | 1 .. 16384 字节，不可为空、不可含 NUL |
| `quote_timestamp` | number | ≤ 4294967295 |
| `quote_msg_source` | string | ≤ 8192 字节，**允许为空串**，不可含 NUL |
| `quote_message_type` | number | 必须为 `1` |

> [!CAUTION]
> `quote_*` 字段只能来自事件日志中对应消息的原始字段——后端会逐字段比对，构造与原消息不符的引用会被拒。

### 6.4 媒体字段

适用于 `send_media`。**五个字段全部必填**，且此时不允许携带 `text` / `at_user_list` / 引用字段。

| 字段 | 类型 | 约束 |
| :--- | :--- | :--- |
| `media_kind` | string | `image` 或 `voice` |
| `media_path` | string | 1 .. 4096 字符的本地绝对路径，不可含 NUL |
| `media_sha256` | string | **恰为 64 个小写十六进制字符**，即文件的 SHA-256 |
| `media_bytes` | number | 文件字节数 |
| `duration_ms` | number | 语音时长（毫秒）。**图片也必须提供此字段且值必须为 0** |

`media_kind` 与 `media_bytes` / `duration_ms` 的组合约束：

| `kind` | 大小上限 | `duration_ms` |
| :--- | :--- | :--- |
| `image` | 20 MB | 必须为 `0` |
| `voice` | 1 MB | > 0，≤ 60000，且必须是 **20 的整数倍** |

> [!NOTE]
> 媒体发送仅在持续模式下可用。

---

## 📨 7. 响应格式

### 发信响应

```json
{
  "op": "send_result",
  "protocol_version": 1,
  "request_id": "req-1",
  "attempt_id": "try-1",
  "observer_session_id": "...",
  "status": "accepted",
  "error_code": null,
  "error_detail": null
}
```

失败时 `error_code` 与 `error_detail` 为字符串。

### `status` 语义

| `status` | 含义 | 客户端应对 |
| :--- | :--- | :--- |
| `accepted` | 原生提交函数已正常返回 | **这不等于投递成功**，仅表示已提交入微信内部流程 |
| `rejected` | 在产生任何副作用之前被拒 | 可依据 `error_code` 修正后重试 |
| `unknown` | 已进入原生调用或 UI 执行，结果不可知 | **严禁自动重试**，否则可能重复发送 |

> [!IMPORTANT]
> `unknown` 是刻意设计的第三态。发信一旦越过"提交"边界，任何异常、超时或进程状态变化都无法再判定消息是否已发出，因此后端选择如实报告不确定，而不是猜测成功或失败。

### 错误响应

协议层错误使用独立的响应结构：

```json
{
  "op": "error",
  "protocol_version": 1,
  "request_id": null,
  "error_code": "invalid_request",
  "error_detail": "unknown or unsupported request field"
}
```

> [!NOTE]
> 若连 `request_id` 都无法解出，该字段为 `null`。

---

## 🔁 8. 幂等与限流

发信请求受多重门禁约束，客户端需要理解以下行为：

### 幂等

- 相同的 `request_id` + **完全相同的请求内容**：直接返回首次的结果，**不会重复发送**。
- 相同的 `request_id` 但内容不同：报 `request_conflict`。
- `request_id` 在指令过期后仍会被保留 60 秒，用于拦截网络延迟导致的重发。

### 限流

- 持续模式下强制最小发送间隔（默认 **1000 ms**），过快请求报 `rate_limited`。
- 同一时刻只允许一个原生发送在途，冲突时报 `busy`。
- 去重表容量有限，写满后报 `capacity_unavailable`。

### 单次模式

未设置 `WECHATBOT_SEND_MODE=continuous` 时后端处于**单次模式**：

- 只允许向 `WECHATBOT_SEND_TARGET` 发送一条与 `WECHATBOT_SEND_TEXT` **精确匹配**的文本；
- `origin` 必须为 `manual`；
- 整个进程生命周期内只允许成功发出**一条**消息，之后一律 `single_send_limit`；
- 单次模式**不支持**群聊、媒体与引用。

详见[运行配置](configuration.md)。

---

## ❌ 9. 错误码全表

### 协议层

| `error_code` | 含义 |
| :--- | :--- |
| `invalid_frame` | 载荷为空或超过 65536 字节 |
| `invalid_utf8` | 载荷不是合法 UTF-8 |
| `invalid_request` | JSON 语法、字段白名单、字段约束或引用一致性校验失败；`error_detail` 含具体原因 |
| `unsupported_protocol` | `protocol_version` 缺失或不等于 1 |
| `unsupported_operation` | `op` 未知 |

### 会话与能力

| `error_code` | 含义 |
| :--- | :--- |
| `session_mismatch` | `observer_session_id` 与当前会话不符 |
| `native_sender_unavailable` | 未开启原生发信、签名不匹配或后端不可用 |
| `native_media_sender_unavailable` | 媒体发送未开启或原生媒体入口签名不匹配 |
| `native_quote_sender_unavailable` | 引用回复未开启（缺 `WECHATBOT_EXPERIMENTAL_QUOTE`） |
| `account_not_verified` | 指令账号与配置账号或实际登录账号不符 |

### 时间窗

| `error_code` | 含义 |
| :--- | :--- |
| `invalid_timestamp` | 时间格式非法、`expires_at <= created_at`，或生命周期超过 600 秒 |
| `command_not_yet_valid` | `created_at` 位于未来 |
| `command_expired` | 指令已过期 |

### 目标与内容

| `error_code` | 含义 |
| :--- | :--- |
| `target_not_allowed` | 接收人非法、不在白名单，或单次模式下试图发群聊 |
| `text_not_allowed` | 文本为空、超长，或与配置的测试文本不符 |
| `invalid_media` | 媒体描述非法，或与文本/引用/@ 混用 |
| `invalid_mentions` | @ 列表非法，或在非群聊、非持续模式下使用 |
| `invalid_quote` | 引用字段非法，或引用不属于目标会话 |
| `origin_not_allowed` | `origin` 取值不支持，或单次模式下非 `manual` |

### 限流

| `error_code` | 含义 |
| :--- | :--- |
| `busy` | 已有一次原生发送在途 |
| `rate_limited` | 未满足最小发送间隔 |
| `capacity_unavailable` | 去重表已满 |
| `single_send_limit` | 单次模式下本会话已用过唯一一次机会 |
| `request_conflict` | `request_id` 已存在但内容不同 |

### 执行期

| `error_code` | 含义 |
| :--- | :--- |
| `ui_dispatch_not_started` | 任务在 UI 线程取出前被取消或调度器不可用，**无副作用** |
| `ui_execution_uncertain` | UI 任务已开始但调用端等待超时或抛异常，**副作用可能已发生** |
| `native_preparation_failed` | 原生对象构造阶段失败，**未调用提交函数** |
| `native_start_uncertain` | 已进入原生提交函数，结果不可知 |
| `dispatch_exception` | 门禁已登记但提交过程中抛出异常 |
| `request_record_missing` | 内部状态异常：预留的去重记录丢失 |

> [!CAUTION]
> 最后四项以及 `ui_execution_uncertain`、`native_start_uncertain` 都是 `unknown`，**不可重试**。

---

## 🧪 10. 完整示例

```python
import json, struct, time

def read_exact(pipe, size):
    data = b""
    while len(data) < size:
        chunk = pipe.read(size - len(data))
        if not chunk:
            raise IOError("pipe closed")
        data += chunk
    return data

def call(pipe_path, request):
    payload = json.dumps(request, ensure_ascii=False).encode("utf-8")
    with open(pipe_path, "r+b", buffering=0) as pipe:
        pipe.write(struct.pack("<I", len(payload)))
        pipe.write(payload)
        length = struct.unpack("<I", read_exact(pipe, 4))[0]
        return json.loads(read_exact(pipe, length).decode("utf-8"))

# 1. 握手
hello = call(r"\\.\pipe\wechatbot-<session>", {
    "op": "hello", "protocol_version": 1, "request_id": "1",
})
session = hello["observer_session_id"]

# 2. 发文本
now = time.gmtime()
result = call(r"\\.\pipe\wechatbot-<session>", {
    "op": "send_text", "protocol_version": 1,
    "request_id": "req-1", "attempt_id": "try-1",
    "observer_session_id": session,
    "expected_account_id": "wxid_youraccount",
    "target_id": "wxid_friend",
    "text": "你好",
    "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", now),
    "expires_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + 60)),
    "origin": "manual",
})
assert result["status"] != "unknown", "unknown 表示结果不可知，严禁重试"
```

---

## 🔗 相关文档

| 文档 | 内容 |
| :--- | :--- |
| 📜 **[事件日志协议](events.md)** | 收信接口：JSONL 事件种类、字段含义、读取诊断、丢事件告警 |
| ⚙️ **[运行配置](configuration.md)** | 全部环境变量、目录布局、单次与持续模式、启动与停机行为 |
| 🧯 **[疑难排查](troubleshooting.md)** | 发不出消息、连不上管道、看不到事件 |
| 💡 **[内部架构](architecture.md)** | 注入与 Hook 链、内存安全模型、UI 线程调度、发信分层、停机顺序 |
| 🛠️ **[构建与编译](building.md)** | 工具链要求、CMake 预设与构建选项、工程结构、测试 |
