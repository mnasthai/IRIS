<div align="center">

# 🔐 微信版本 Profile

### 逆向数据的存放与使用方式

<p align="center">
  <b>IRIS</b> · 面向 C++ / 逆向开发者<br>
  RVA / 虚表 / 结构体偏移 / 机器码指纹的 JSON 维护与构建期代码生成
</p>

</div>

---

## 📌 它是什么

IRIS 的所有版本相关常量——函数入口 RVA、虚表地址、结构体字段偏移、机器码特征指纹、预期镜像大小——都集中在一个 **Profile JSON** 中：

```
profile/
├── example.json                     # 结构模板（占位值），随仓库分发
└── weixin-4.1.13.12.json            # 真实数据，不进仓库（见下）
```

构建时由 `cmake/GenerateProfile.cmake` 把它转成 C++ 头文件 `iris_profile_generated.hpp`，生成到构建目录。

> [!IMPORTANT]
> **真实 Profile 不随源码仓库分发。** 它包含针对特定微信版本逆向得到的偏移数据，出于合规考虑，仓库只保留结构模板，真实数据通过网盘单独提供。

---

## 📥 获取与安装

### 下载

> **网盘链接**：https://pan.baidu.com/s/1EYrobx6mqF8CsZrSXPYe_A
>
> **提取码**：`2e5s`

下载得到形如 `weixin-<版本>.json` 的文件。

### 放置

放到仓库的 `profile/` 目录下即可，构建时会自动选用该目录下唯一的 `.json`（`example.json` 除外）：

```
profile/weixin-4.1.13.12.json
```

也可以用 CMake 变量显式指定：

```powershell
.\build.ps1 -Clean
# 或
cmake --preset release -DIRIS_PROFILE_JSON=D:\somewhere\weixin-4.1.13.12.json
```

若 `profile/` 下存在多个候选文件，配置阶段会报错并要求你显式指定。

> [!WARNING]
> 下载 Profile 后**必须重新运行 CMake 配置**——CMake 不会自动发现新出现的文件。
> 最省事的做法是 `.\build.ps1 -Clean`。

缺少 Profile 时的报错信息会直接给出上述指引。

---

## ⚙️ 它如何生效

```
profile/*.json  ──[ cmake/GenerateProfile.cmake ]──▶  <build>/generated/iris_profile_generated.hpp
                                                              │
                                                              ▼
                                              include/monitor/config/version_profile.hpp
                                                              │
                                                              ▼
                                     全部业务代码通过 active_profile:: 使用这些常量
```

### 为什么做成构建期代码生成，而不是运行期加载

这是本项目一个**刻意的安全选择**：

| | 构建期生成（当前方案） | 运行期加载 |
|---|---|---|
| 常量形态 | `inline constexpr` | 运行期变量 |
| 布局写错 | **编译失败** | 运行到一半越界 |
| `static_assert` 校验 | 全部保留 | 全部失效 |
| 换取的能力 | 需要先下载 JSON 才能构建 | 换版本不用重新编译 |

`include/monitor/native/native_text.hpp` 里有 6 个 `static_assert`，逐一比对我们的包装结构体尺寸与原生对象尺寸；`native_text.cpp` 里还有一个断言检查载荷偏移不越界。**这些编译期契约是本项目内存安全模型的一部分**，把它们降级为运行期检查是实质性的安全降级。

代价是：没有 Profile 就无法构建。考虑到这是一个**版本绑定**的注入器——你本来也无法为一个未知的微信版本构建它——这个要求在语义上是诚实的。

> [!NOTE]
> 无论哪种方案，编译产物中都必然包含这些常量（它们要参与寻址）。区别只在于**源码仓库是否含有明文数据**。

---

## 🗂️ JSON 结构

Profile 按**作用域**（scope）组织，每个作用域下再按**类型桶**（bucket）分组。

### 作用域

| 作用域 | 含义 | 对应 C++ 命名空间 |
| :--- | :--- | :--- |
| `root` | 顶层：版本号、镜像大小、各 Hook 入口 RVA 与指纹、消息结构体布局与字段描述 | `active_profile` |
| `session` | 账号信息读取链：根对象指针、虚表、getter 槽位与偏移 | `active_profile::session` |
| `text` | 发送源对象字段偏移、引用消息字段、内存包装容器尺寸 | `active_profile::text` |
| `reference_message` | 引用消息转换器使用的原生 W 结构布局 | `active_profile::reference_message` |
| `sender` | 原生文本发信各入口 RVA 与指纹 | `active_profile::sender` |
| `media_send` | 图片 / 语音发送入口与虚表 | `active_profile::media_send` |
| `media_receive` | 媒体下载完成回调入口、结果结构偏移 | `active_profile::media_receive` |

### 类型桶与取值格式

| 桶名 | 生成的 C++ 类型 | 值格式 | 示例 |
| :--- | :--- | :--- | :--- |
| `uptr` | `uintptr_t` | 十六进制字符串 | `"0x017955E0"` |
| `size` | `size_t` | 十六进制字符串 | `"0x70"` |
| `uint32` | `uint32_t` | 十六进制字符串 | `"0x0BC2E000"` |
| `uint64` | `uint64_t` | 带后缀的十六进制字符串 | `"0x0000003900000031ULL"` |
| `uint` | `unsigned` | 带后缀的十进制/十六进制 | `"1U"` |
| `char_arr` | `char[]` | 字符串 | `"4.1.13.12"` |
| `u16_arr` | `uint16_t[]` | 数字数组 | `[4, 1, 13, 12]` |
| `u8arr` | `uint8_t[]` | **连续十六进制串**（无分隔） | `"5541574156415541..."` |
| `wchar_arr` | `wchar_t[]` | 字符串 | `"Qt51514QWindowIcon"` |
| `uptr_arr` / `size_arr` | 对应数组 | 字符串数组 | `["0x017E5ECE", "0x017E62B1"]` |
| `layout` | `MessageLayout` | 对象 | `{ "size": "0x70", "has_bits": "0x6C" }` |
| `strfield` | `StringField` | 对象 | `{ "offset": "0x08", "bit": "0x02", "wrapped": true }` |
| `scalar_arr` | `ScalarField[]` | 对象数组 | `[{ "number": 1, "offset": "0x10", "bit": "0x01", "wide": false, "signed32": true }]` |
| `sig_arr` | `Signature[]` | 对象数组 | `[{ "rva": "0x6DFD90", "bytes": "55565753..." }]` |

**完整的字段清单**（144 个常量）见随仓库分发的 [`profile/example.json`](../profile/example.json)——它与真实 Profile 结构完全一致，只是值全为占位符。

### 符号引用

标量值除了直接写字面量，还可以引用另一个常量，避免两处数值不同步：

```json
"kRootGlobalRva": { "ref": "kRootObjectPointerRva" }
```

生成结果是 `inline constexpr uintptr_t kRootGlobalRva = kRootObjectPointerRva;`。

### 生成器会拒绝的情况

任何缺失字段、类型不符、空作用域、非法十六进制串，都会在**配置阶段**报错并点名具体路径，例如：

```
Profile JSON 中 scope 'reference_message' 为空或字段名拼写有误
```

---

## 🧩 适配新的微信版本

1. **复制模板**：`profile/example.json` → `profile/weixin-<新版本>.json`
2. **更新元信息**：`profile_name`、`target_version`、`root.char_arr.kTargetVersion`、`root.u16_arr.kVersionParts`
3. **重新逆向各偏移**，逐项填入
4. **更新指纹**：`root.u8arr.*` 与 `sender.sig_arr` / `media_send.sig_arr` 的 `bytes` 字段是**入口处前若干字节的机器码**
5. **构建并观察**：生成的头文件在 `<build>/generated/iris_profile_generated.hpp`，可对照检查

> [!TIP]
> 结构体尺寸类常量（`kCallableHolderSize`、`kDelayedHolderSize`、`kStartupHandleSize`、`kMetadataSize`、`kSharedPairSize` 等）如果填错，会在**编译期**由 `native_text.hpp` 的 `static_assert` 直接拦下。这是这套机制最有价值的地方——它把最容易越界的一类错误变成了编译错误。

### 指纹的作用

`VerifyTarget` 在挂钩前做三重校验：

1. `Weixin.dll` 的**文件版本**四段号必须与 `target_version` 一致；
2. **PE 标识**：NT 头签名、`Machine` 为 AMD64、`SizeOfImage` 等于 `root.uint32.kExpectedImageSize`；
3. **入口机器码**逐一比对 `*Entry[]` 与各 `sig_arr`。

三者任一不符即拒绝挂钩并输出 `observer_disabled`。这样设计是因为：**偏移错了不会报错，只会读到垃圾或崩溃**——三层校验让绝大多数版本漂移在挂钩前就被拦住。

---

## ⚠️ 安全提示

Profile 是**外部输入**，它能决定 Hook 指向哪里。虽然入口指纹校验会拦住绝大多数错误与篡改，但仍应注意：

- **只使用可信来源的 Profile**。不要随手拿网上来路不明的文件替换。
- **Profile 与可执行文件同等敏感**。它含有的偏移信息足以刻画目标客户端的内部结构。
- 校验不通过的 Profile 会让后端**拒绝启动**（而不是带病运行），这是预期行为。

---

## 🔗 相关文档

| 文档 | 内容 |
| :--- | :--- |
| [💡 内部架构](architecture.md) | 目标三重校验、Hook 链与内存安全模型 |
| [🛠️ 构建与编译](building.md) | 工具链要求、CMake 预设与构建选项 |
| [🧯 疑难排查](troubleshooting.md) | `observer_disabled` 等启动失败的排查 |
