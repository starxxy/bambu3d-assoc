# 变更记录 / Changelog

本文件记录 **Bambu 3D 文件关联管理器** 的全部版本变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

> 说明：每条变更的「依据」段落列出可核对的证据来源（安装包字节数、源码文件、编译日志），
> 便于独立复核。该习惯沿用自项目内部归档规范。

---

## [1.3.0] — 2026-10-01

**软件完全免费 + 品牌署名。**

### 新增

- **品牌与署名体系**
  - 新图标 `resources/app.ico`（9号造物「9 + 画框」，16 / 24 / 32 / 48 / 64 / 128 / 256 七档）
  - 页头版本胶囊显示 `9号造物 · 免费提供`
- 发布二进制启用 MSVC `/guard:cf` 控制流防护。
- 「关于 9号造物」窗口：免费声明 + 三个官方频道链接。

### 变更

- **软件完全免费**：移除「30 天试用 + 每日兑换码」的付费闸门。
  - 授权范围限定为**个人非商业用途**，商业用途需事先取得著作权人书面许可（见 `LICENSE`）。
  - `src/license.cpp` / `include/bs3d/license.h` 退役出编译（文件保留作历史记录）。
  - `thumbnailcore.cpp` 的过期占位图分支删除，所有文件一律真实渲染。
  - 主窗口底条「激活」按钮 → 「关于 9号造物」。
  - `redeem_gen` 发码工具下线；VPS 付款发码页方案作废。
- 版本号 1.2.4 → 1.3.0（`registry.cpp` / `.rc` / `.iss` 三处同步）。

> 依据：安装包 `Bambu3DAssoc-Setup-1.3.0.exe` = 12,249,557 B；
> `CMakeLists.txt` 第 170 行注释「v1.3.0: redeem_gen 退役」；
> `src/license.cpp` 头注释标 RETIRED。

---

## [1.2.4] — 2026-09-29

**卸载残留专项（本版只改安装器，二进制不变）。**

### 背景

用户反馈「卸载后 `.3mf` 还显示缩略图」，看起来像没卸干净。

### 调查结论

处理器确实已卸干净（全机扫描 155 处缩略图注册键，我们的 CLSID 为 0）。
真因是 **Windows 缩略图缓存**：Explorer 命中 `thumbcache_*.db` 就直接返回，
从不回头检查处理器是否还在注册。用「复制到新路径必然未命中」的对照实验证实。

### 修复

- `RegDeleteKeyIfEmpty` 清掉空的 `<ext>\ShellEx` 与
  `SystemFileAssociations\.<ext>\ShellEx` 父键（若第三方占用则自动拒绝删除）。
- 清理 Explorer 记录的 `FileExts\...\OpenWithProgids\<ProgID>` 与
  `OpenWithList` 中的 `bs3d_assoc.exe`（卸载后都是悬空项）。
- **卸载时按需清空缩略图缓存**（`thumbcache_*.db`），仅在确认处理器真的注册过时才动手。
- 收尾调用 `SHChangeNotify(SHCNE_ASSOCCHANGED)`，让资源管理器立即刷新关联 / 图标状态。

### 验证

- 安装目录整个消失，HKCU 侧残留为 0，`.3mf` / `.stl` 全部回到通用图标。

---

## [1.2.3] — 2026-09-29

**闪烁修复（三个上层窗口统一处理）。**

### 修复

- 消除界面闪烁的五个来源：
  1. 窗口加 `WS_CLIPCHILDREN`，消除「父窗先画一遍、子控件再补一遍」的闪动；
  2. `WM_PAINT` 一律整窗双缓冲（内存 DC → 一次 `BitBlt` 贴回）；
  3. `WM_ERASEBKGND` 全部 `return 1`，`hbrBackground = nullptr`；
  4. `InvalidateRect(..., TRUE)` 全部改为 `FALSE`（`TRUE` 会请求擦除，在自绘窗口上就是白闪）；
  5. 按钮悬停只在 hover 状态翻转时重绘（原来每次 `WM_MOUSEMOVE` 都全量重绘）。
- 高级设置 `.prev` 预览框改为父窗口自绘：原 `SS_BITMAP` 子控件在 `WS_CLIPCHILDREN`
  下会把底部说明条整块裁掉，导致说明文字永远显示不出来。
- 格式列表文案逐字对齐设计稿（`step` / `gcode` / `3mf` 的名称与说明）。

### 验证

- 闪烁探针：398 帧 / 31 次强制整窗重绘 / **0 擦白帧**（基准 0.0322）。

---

## [1.2.2] — 2026-09-29

**安装范围移入向导 + 内核必带。**

### 变更

- **安装范围由安装程序在安装时选择**（软件内不再有 HKLM 开关）：
  - 仅为我安装 → `HKCU` + `%LOCALAPPDATA%\Programs\Bambu3DAssoc`
  - 为所有用户安装 → `HKLM` + `C:\Program Files\Bambu3DAssoc`
  - 实现：`PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=commandline dialog`，
    `[Registry]` 段按 `IsAdminInstallMode` 写 `InstallScope`（`user` / `machine`）。
  - ⚠️ **静默安装必须显式指定范围**（`/ALLUSERS` 或 `/CURRENTUSER`）。
- **OCCT 内核改为必带**：删除 `occpack.url` 与「插件包下载地址」UI，
  环境检查文案改为「已就绪 / 随安装包自带」。
- **主窗口去掉「缩略图预览」列**，格式列表从 5 列变 4 列。

### 修复

- `blitBackdrop` 目标坐标错误 → owner-draw 按钮露出窗口类默认底色灰块。
- 150% 缩放下 `AdjustWindowRectEx` 不可靠 → 客户区差 22×56，底部条带被切掉。
- 卡片说明文案压住开关按钮。
- 「还原 / 卸载」按钮改为危险色（白底红字 `#C2261C`）。
- 页头版本胶囊此前一直停在 v1.2.0，本版修正。

---

## [1.2.1] — 2026-09-29

**回归系统标准窗口边框。**

### 变更

- 移除自绘的标题栏红黄绿灯（macOS 风格窗口装饰），改用系统标准边框。
  该决定被后续版本一直沿用。
- 本版首次留存 ISCC 编译日志（`installer/iscc_121.log`）。

---

## [1.2.0] — 2026-09-29

**不再抢占默认打开方式。**

### 变更

- **只注册缩略图处理器，不改扩展名的默认打开程序**：不再抢走用户原有的双击行为；
  卸载时按 `HKLM` / `HKCU` 双根还原 20 个扩展名。

### 已知问题（已在后续版本修复）

- 本版写入了一枚**非标准 ShellEx GUID** `{E1775614-BA1F-4B66-85BB-33800A1D334F}`
  （正确值应为 `IThumbnailProvider` 的 IID `{E357FCCD-A995-4576-B01F-234630154E96}`）。
  后续版本的安装 / 卸载都会主动清扫它。

> 依据：`installer/Bambu3DAssoc.iss` 注释「Non-standard GUID written by v1.2.0;
> swept away so it leaves no cruft」；`src/registry.cpp` 的 `kLegacyThumbHandlerGuid`。

---

## [1.1.1] — 2026-09-28

**维护性修复。**

- 1.1.0 → 1.2.0 之间的小修版本，包体增加 39,785 B。
- ⚠️ **该版本没有留存的变更记录**（当时的开发日志未覆盖），仅能确认其存在与时间位置，
  不对具体内容作猜测。

---

## [1.1.0] — 2026-09-28

**格式集换代 + OCCT 内核随包内置。**

### 变更

- **格式集整体更换**为「Bambu Studio 实际能打开的格式」（经其程序内置对话框字符串核实）：
  `3mf` `stl` `obj` `amf` `fbx` `step` `stp` `gcode` `g` `gco`（10 个，含 3 个别名）。
  v1.0.0 的 10 个旧扩展名退役为 `kLegacy[]`，仅用于清理旧版残留。
- **OCCT CAD 内核改为随安装包自带**：包体从 2.2 MB → 11.6 MB
  （41 MB 内核经 LZMA2 solid 压缩到约 11 MB）。

> 依据：包体 12,136,373 B；`installer/iscc_121.log` 的压缩清单逐行列出
> `occt\bs3d_occ.dll` + `TKSTEP` / `TKMesh` / `TKIGES` 等全套 TK* 运行时。

---

## [1.0.0] — 2026-09-27

**首个可用版本。**

### 新增

- Win32 原生「缩略图 + 文件关联」管理器成型：主窗口格式列表 + 一键启用 +
  高级设置 + 状态条带。
- 收录 10 个扩展名：`ply` `3ds` `off` `dae` `x3d` `wrl` `iges` `igs` `brep` `bgcode`。
- **CAD 内核按需下载**：安装包仅 2.2 MB，程序内置 WinHTTP 下载器
  （TLS 1.2/1.3、SHA-256 校验、ZIP 安全解包、进度回调），内核地址由 `occpack.url` 提供。

> 依据：安装包 2,295,478 B；`src/envcheck.cpp` 至今保留完整下载器与
> `occpack.url` / `occ-pack.info` 逻辑；`src/formats.cpp` 的 `kLegacy[]`
> 正是这批扩展名。

---

## 安装包校验值（SHA-256）

```
5863d190eb1a461d9ed4f887ef037bb90be4f32a6b3c870cb45154278bdb3d47  Bambu3DAssoc-Setup-1.0.0.exe
57d1bf9bf933d811310a18cfe83500858a26a52544bfc153d3f793e9b19ebc69  Bambu3DAssoc-Setup-1.1.0.exe
11ac1ad5e087a05e40a64cf62fb4397f6587a514cd1b7c71f9b1b98c5c2b3ed9  Bambu3DAssoc-Setup-1.1.1.exe
aba8bb43ea9311e599054bfb6bc6e787fc676fd338b5f63a911972e036752937  Bambu3DAssoc-Setup-1.2.0.exe
2a1937bd8cb8d3fe27d9ff92b50a3d1f1b9fac9c1362c8d990f6b33d633289d8  Bambu3DAssoc-Setup-1.2.1.exe
375fb1ccb6691affc992e68a603b408a6c5793e90ca1a74cd04bd487cd933548  Bambu3DAssoc-Setup-1.2.2.exe
45432ef313b42048e677637047797c2ef029c6e2921543cae04e831c9fa3129a  Bambu3DAssoc-Setup-1.2.3.exe
4b1cbe4f12ea5031d2734c5a01228b1af0968ecc0dce3d33c4ac18574499565f  Bambu3DAssoc-Setup-1.2.4.exe
aa1d61f4124cbdc97575ac82d7c86198e2184216db8df5836895add207b8aed3  Bambu3DAssoc-Setup-1.3.0.exe
```

> v1.1.1 及更早版本的安装包不包含在本仓库的独立目录中。
> v1.2.0 ~ v1.2.4 与 v1.3.0 的安装包在对应 tag 下可取。
