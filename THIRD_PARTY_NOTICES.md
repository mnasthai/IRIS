# 第三方组件声明

本项目包含以下第三方组件。各组件的版权归其原作者所有，其许可证条款完整保留于对应目录中。

---

## MinHook

- **位置**：`third_party/minhook/`
- **用途**：x64 API Hook 引擎，用于挂钩微信原生函数的入口点
- **版本**：见 `third_party/minhook/include/MinHook.h`
- **许可证**：BSD 2-Clause License
- **许可证全文**：`third_party/minhook/LICENSE.txt`
- **版权**：Copyright (C) 2009-2017 Tsuda Kageyu

### 许可证兼容性说明

BSD 2-Clause 属于 GPL 兼容许可证。本项目以 GPL-3.0 发布，可以在满足以下条件的前提下链接并分发 MinHook：

1. 保留 MinHook 的版权声明（`AUTHORS.txt` 与 `LICENSE.txt` 均保持原样未修改）；
2. 在文档中声明使用了该组件（即本文件）；
3. 不对 MinHook 的许可证条款施加额外限制。

由于本项目分发的是**完整源码**（包含 `third_party/minhook/` 的全部原始文件），上述条件已自然满足。

---

## 添加新依赖时

若你引入新的第三方组件，请：

1. 将其放入 `third_party/<name>/`，并**完整保留**其原始 LICENSE / COPYING 文件；
2. 确认其许可证与 GPL-3.0 兼容（宽松许可证如 MIT / BSD / Apache-2.0 通常兼容；GPL-2.0-only、专有许可证通常**不**兼容）；
3. 在本文件中追加对应条目，说明位置、用途、版本、许可证与版权归属。
