# IslandBar —— 本仓库的规则（2026-10-16 重建）

这份文件是**唯一的规则权威**，Codex 每次在本仓库开新会话都会自动读到。旧的版本已作废；
`START-HERE.txt` 里原来的"规则 0-7"已经并进第 4 节，它现在只作为入口与命令速查用。
与本文冲突的任何旧说法（包括聊天记录里的决定、已删除的文档、上一个月 HANDOFF 里的流程描述）
一律以本文为准；本文每一条都注明它为什么存在（依据的是实测数字或代码位置）。

---

## 1. 每一次改动都要走完六步

**研究 → 方案 → 用户确认 → 实现并部署 → 用户测试 → 推送。** 中间任何一步没走完，不进入下一步。

1. **先研究，再动手。** 任何改动，包括看起来很小的改动，写代码之前先通过**内置浏览器**与本项目的
   评审搭档（ChatGPT）把"改什么、为什么、有没有更优解"研究清楚。提问必须带上本项目的实测数据，
   并要求对方明确表态（同意 / 反对 / 修正），把它的反对点记下来。至少一问一答，不许只发一句概述。
   操作内置浏览器的方法与三个坑见 HANDOFF 8.144.1（回车不提交、要用页面自己的"发送"按钮、
   答复用 TextPattern 读而不是 Ctrl+C）。
2. **给用户一份方案**，必须包含：改哪些文件、改成什么（文件级粒度，不贴整篇代码）；为什么这样改
   （依据是实测数字或代码位置，不是印象）；**验收指标：一个可量化的数字 + 一行日志里能看到的字**；
   回退方式；预计影响（CPU / 内存 / 观感）；评审的反对点，以及我为什么仍然这样决定或因此改成了什么。
3. **等用户确认。** 方案没有被明确确认之前，不写代码。
4. **实现并本地部署。** Debug 与 Release 都要能编过（`/WX`，零告警）；跑 `--self-test` 与
   `island_smoke`；把新副本部署到 `%USERPROFILE%\IslandBar\island_bar.exe`，旧副本先备份。
   自检与部署都要在**项目目录外**做（见第 4 节）。
5. **等用户测试。** 用户说"没问题"之前，`git push` 一律不做。**本地提交可以**（配合
   `scripts\snapshot.ps1`），提交信息写清"改了什么、为什么、测出了什么"。
6. **推送 GitHub。** 用户确认后才推。远程是 `github.com/BugMakerJun/IslandBar-DynamicIsland`。
   （旧文档里的 `captures/push-via-api.ps1` 已经不存在；需要时先确认这条路再走。）

---

## 2. 已经敲定、不要重新讨论的技术决定

* **技术路线**：C++20 + Win32 + D3D11 + Direct2D + DirectComposition，**单 exe、静态 CRT、
  无运行时依赖**。**不**考虑 Zig、Rust 重写、WinUI3、WPF、Electron、Qt、Skia、ImGui。
  **唯一的例外是托盘**（见第 3 节）：允许一个独立模块进 shell，但它必须可关、可回退。
* **内存口径**（实测之后修订过两次）：优秀 75-85 MB 私有字节，合格 85-95 MB；硬指标是
  **24 小时不持续增长**。20-40 MB 与 40-60 MB 两个目标都已撤回。
* **视觉**：不追实时背景折射。默认材质用**静态壁纸取景**（只取岛所在区域，缩到约 256x64，
  模糊一次并缓存，壁纸变更时重算），回退是自绘实心材质；`Windows.UI.Composition` 的 host
  backdrop 只作能力探测，不作主路径。
* **动效**：外层形状两个方向都用 spring，可中断、能继承位移与速度；卡片内部内容淡入淡出
  180-200 ms ease-out；进度条线性，永不做缓动。
* **媒体**：保留 C++/WinRT 的 GSMTC（实测 10.6 MB / 11 线程，判定值得），但要求延迟初始化、
  无音乐不启动、事件驱动、不长期持有 Session 对象。
* **合并 TOP10 的顺序**：渲染节流与 Present 修正、脏渲染模型、Motion Surface 加 DComp 动画、
  崩溃看门狗（可与前两项并行）、输入事件化、音频轮询事件化、内存可观测、日志降噪、玻璃材质、
  Shell 兼容层。

---

## 3. 托盘：它是唯一允许碰 shell 的功能，这是它的边界与硬约束

**边界（用户 2026-10-15 的放宽，只对托盘生效）**：为了实现托盘，允许一个**版本适配层**在 shell
进程内做事（与 MyDockFinder 一致是用户的明确要求），代价是它必须是**独立模块 + 开关 + 可回退**：
关掉就退回不注入任何东西的状态。其余部分仍按第 2 节走。

**正在走的路线（实测定案）**：应用把 (窗口, uid, uCallbackMessage, uVersion, 提示文字) 注册给 shell；
一个**只读**模块在 shell 里读到这些注册并发布出来；然后由我们自己把"应用自己的点击"作为消息发回
应用自己的窗口（指针不动、不注入应用、不闪屏）。真托盘裁到通知区域（`--native-tray`）作为兜底，
自己当 shell 被否掉了。

1. **统一协议入口**：托盘消息的两个字只允许由 `src/domain/TrayEncoding.h` 产生
   （`PlanTrayClick` / `EncodeTrayMessage`）。按 `uVersion` 分派：v0/3 = `wParam=uid`,
   `lParam=鼠标消息`；**v4 = `wParam=MAKELONG(指针 x, 指针 y)`, `lParam=MAKELONG(事件, uid)`**
   （实测字节见 HANDOFF 8.142）。UI、业务逻辑、观察模块、探针一律不得手写编码；每种编码要有
   可重复执行的断言。**编码写反会把应用打崩**：微信实测崩过两次并重启到登录页。
2. **超时不是判据**：`SendMessageTimeout(250 ms)` 只是"单次同步投递的等待上限"。超时 ≠ 消息没送到、
   ≠ 应用没处理、≠ 菜单没出现（应用会在消息处理里跑模态菜单循环，实测 261-297 ms 就是菜单开着）。
   判"菜单出现了没有"只认**窗口级证据**（新出现的顶层窗口 / `#32768`，带创建时间、所属进程、
   位置、可见性），并且要有独立的**观察截止时间**；截止前不得抢着弹我们自己的菜单（否则双菜单）。
   左键与右键各有各的成功判据，不能互相借证据。像素差不能当证据：这台机器跑着动态壁纸，
   噪声底就有 20-25 万像素。
3. **shell 内的观察模块**：只读＝只观察注册、复制字段、发布快照；不改消息、不接管回调、不注入
   应用。协议带 magic + ABI 主次版本 + 结构体大小，不兼容就拒绝；不建线程、不设定时器、回调里
   不做磁盘 I/O 与同步等待；IPC 只允许当前用户的主程序访问并校验长度与边界；明确安装 / 加载 /
   启用 / 禁用 / 退出 / 升级的状态转换；模块失联时主程序降级而不是卡死；禁用后不得无限重试；
   DLL 被 explorer 映射时不得覆盖或删除，升级走版本化目录。"不建线程"不等于"对 explorer 零影响"
   ——回调跑在宿主进程的执行上下文里。
4. **部署**：安装器仍是单 EXE，模块 DLL 作为第二份 RCDATA 内嵌，启用时释放到
   `%LOCALAPPDATA%\IslandBar\modules\tray-observer\<版本>\`；不每次启动重复释放、不覆盖在用的 DLL；
   释放 / 加载 / IPC 任何一步失败都要写明原因并自动降级，不阻止主程序启动。
   `--tray off|self|native` 三档，切换失败也不能让主程序起不来。
5. **诊断能力保留**：`--dump` 那种"抓 shell 原话"的能力要保留，但不以实验探针形式存在——产品里
   保留诊断日志（uVersion、选了哪条编码、目标窗口、调用结果、超时状态、观察到的窗口级证据、
   ABI 版本）；底层协议验证工具放开发工具目录、不随安装包发布。**普通日志只能证明我们准备发什么，
   不能替代对真实协议的验证**。
6. **清理分三阶段**：产品实现完成 → 完成验收（v0、v4、调用超时、迟到菜单、应用重启、模块禁用、
   explorer 重启、安装升级、失败回退）→ 才删除实验探针（`probe-tray-lab.*`、
   `probe-tray-observer.cpp`、`run-tray-live.ps1`、`run-tray-sample.ps1`）与部署目录里的
   `traylab\`、`retired-tray\`；删之前检查构建、安装器资源与文档不再引用它们。
   HANDOFF / reviews / captures 永久保留（**错误案例比成功日志更有价值**）。
7. **不许放大结论**：在 v0 与 v4 两个应用上验证过编码，只证明协议实现的关键部分，**不等于**
   托盘产品已验证；没有实测的兼容性结论必须标成"推断"，不得当实测写。

---

## 4. 这台机器的硬事实（踩过就不要重踩）

* **镜像位于 `C:\PROJECT\Taskbar` 下的进程会被降到 LOW 完整性**：UIPI 会静默丢弃所有发往 Explorer
  的消息；GUI 二进制在受限沙箱内根本起不来（0xC0000142）。所以**任何 exe 都要从项目目录外运行**，
  构建、自检、部署、探针都照此办理（自检用 `%TEMP%` 里的副本跑）。
* **单实例互斥锁 `IslandBar.SingleInstance`**：不能并行跑第二个副本做对照。要测对照，先用
  `CloseMainWindow()` 优雅关闭正在运行的那个（它会恢复原生任务栏），测完再把用户的岛启动回来。
* 窗口是 `WS_EX_NOREDIRECTIONBITMAP` + Composition SwapChain，DWM 的 Mica / Acrylic 对它无效。
* **只有 PowerShell 5.1，没有 pwsh 7**；`.ps1` 必须**纯 ASCII**（5.1 按系统 ANSI 码页读脚本，
  非 ASCII 字面量会被写坏）；脚本里不要硬编码非 ASCII 路径，用 `$PSCommandPath` 推。
* **日志写在 exe 旁边**（不在 `%TEMP%`），逐行 flush，所以日志量本身是一项成本。
* `& "path"` 只在 PowerShell 里成立；cmd 会把 `&` 当分隔符，要用引号直呼或 `start /wait ""`。
* DirectComposition 的透明度必须走 `IDCompositionEffectGroup`（`DCompositionCreateDevice` 返回的
  第一代设备没有 `IDCompositionVisual3`）。
* `ID2D1RenderTarget::FillRoundedRectangle` / `DrawRoundedRectangle` 收的是 `ID2D1Brush`，
  不是 `D2D1_COLOR_F`；`setvbuf(stdout, nullptr, _IOLBF, 0)` 会触发 CRT 断言对话框。
* 任务栏类操作（隐藏、`ABM_SETSTATE`）在本进程完整性低于 Explorer 时**会被 UIPI 静默丢弃**，
  调用返回 0、不报错。`island_bar.exe --probe-taskbar` 就是诊断这件事的。
* `Remove-Item -Recurse` 报"拒绝访问"时，文件**不是**被锁：那是沙箱 ACL 陷阱
  （`DeleteSubdirectoriesAndFiles` 被拒、目录缺 `WRITE_OWNER`）。不要手改权限、不要重试，
  用 diagnose-windows-sandbox-acl 那条一次性命令修。
* 内置浏览器的 ChatGPT 页面：输入框是 `Edit`/`ProseMirror`/名字 `询问 ChatGPT`；主程序自己的输入框
  叫 `Work with Codex`，两者只差几十像素——**动它之前先做焦点校验**。回车不提交，要用页面自己的
  "发送"按钮；答复用 UI Automation 的 TextPattern 读。

---

## 5. 命令速查

```powershell
# 构建（两套配置都要过；/WX 生效）
powershell -NoProfile -ExecutionPolicy Bypass -File .\IslandBar\scripts\build.ps1 -Config Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\IslandBar\scripts\build.ps1 -Config Release

# 自检 / 冒烟（副本拷到项目目录外再跑）
Copy-Item .\IslandBar\build\Debug\src\Debug\island_bar.exe $env:TEMP\islandbar-check\ -Force
& "$env:TEMP\islandbar-check\island_bar.exe" --self-test        # 退出码 0 == 全过
& .\IslandBar\build\Debug\tools\smoke\Debug\island_smoke.exe

# 部署（旧副本先备份）与运行
Copy-Item .\IslandBar\build\Release\src\Release\island_bar.exe "$env:USERPROFILE\IslandBar\" -Force
# 托盘模块与它一起部署：island_tray_hook.dll 必须和 exe 同目录

# 本地存档（提交之外的一份快照）
powershell -NoProfile -ExecutionPolicy Bypass -File .\IslandBar\scripts\snapshot.ps1
```

读文档的顺序：`ARCHITECTURE.md`（结构）→ `ROADMAP.md`（顺序）→ 具体事实查 `HANDOFF.md`；
已完成功能与它的证据等级看 `FEATURES.md`（`README.md` 会夸大进度）；测量方法看 `TESTING.md`；
外部评审原文在 `reviews/`。
