# IslandBar 架构与开发方式

写这份文件的目的只有一个：让第一次打开这个仓库的人**读完就知道每块代码住在哪儿、谁可以依赖谁、加一个功能该动哪几个文件、怎么验证自己没改坏。**

历史（每一次改动、每一个实测数字、每一次失败的原因）在 `HANDOFF.md`，那是档案不是入口；改动的流程规则在 `AGENTS.md`。

## 1. 这东西是什么

Windows 11 上替代系统任务栏的一条横条（"岛"）：自己画 pill 与卡片，接管媒体、通知、开始菜单、任务条，并把原生任务栏藏起来。C++20 + Win32 + D3D11 + Direct2D + DirectComposition，单 exe、静态 CRT、无运行时依赖，私有内存 75-95MB。

二进制只有一个：`island_bar.exe`。（2026-10-15 之前还有一个 `island_tray_hook.dll`，装进 explorer 读托盘注册；**托盘功能已按要求整体移除**，这个模块和它的载荷、命令、探针一起走了。见第 7 节。）

## 2. 目录与职责

```
src/
  main.cpp          入口。解析命令行 → 要么跑一条命令，要么跑这条岛
  cli/              那些"代替岛运行"的命令（--toast-dump / --window-list / --icon-dump / --music-preview …）
  domain/           纯逻辑，独立静态库目标 island_domain：几何、布局、动画、状态策略、JSON 读取。
                    这一层不许包含 <windows.h>，也不许链接任何库（见第 3 节）
  core/             所有模块共用的基础设施：日志、进程指标
  app/              窗口与生命周期：IslandWindow（画与命中）、AppBar（占位）、PlayerWindow、
                    BarState.h（bar 自己的状态；正在一片一片从 RunBar 里搬进来，见第 7 节）
  render/           D2D/DComp 渲染：Renderer、TextRenderer、玻璃材质
  media/            GSMTC 会话、歌词、音量、声音输出、图片解码
  shell/            与 shell 打交道：开始菜单、天气、系统开关、原生任务栏
  platform/         平台能力：显示器、窗口列表、缩略图、Win32 薄封装
  services/         Toast 通知读取与通知管道
  selftest/         诊断与 900+ 条断言（--self-test）
tools/              独立探针可执行文件（geo_probe / thumb_probe / jsonprobe / island_smoke）
setup/              安装器（payload 以 RCDATA 内嵌）
scripts/            build.ps1 / capture-*.ps1 / snapshot.ps1
```

**包含写法是硬规则**：一律写 `#include "core/Geometry.h"` 这种"模块/文件"形式，包含根是 `src/`。文件换目录就立刻编译失败，而不是被搜索路径悄悄解析到别处。

## 3. 依赖方向（唯一一条不许违反的规则）

```
island_bar.exe（main.cpp：组装 + 消息泵）
   |-- app/ render/ media/ shell/ platform/ services/
   |      （这些之间可以互相调用，但共用的类型都来自 core/）
   +-- island_domain（纯逻辑：布局、动画、命中、状态策略、JSON）
   +-- core/（共用基础设施：日志、进程指标）
```

- **`src/domain/` 不许包含 `<windows.h>`**。它只做几何、布局、动画、状态策略这些能脱离桌面运行的东西——`--self-test` 能在不开窗口的情况下跑完 900+ 条断言，靠的就是这条规则真的成立。
- 这条规则**不靠自觉**，有两层：① `src/domain/PurityCheck.cpp` 是一个只做一件事的翻译单元——包含 `domain/` 下所有头文件，然后 `#ifdef _WINDOWS_ #error`；② `island_domain` 这个目标**不链接任何库**，所以纯逻辑里出现 Win32 调用会先在编译期就被拦住。
- 需要 Win32 类型的地方（例如 `LONG`/`UINT` 的像素换算）放在 `platform/` 或各模块自己的头里，不放 `domain/`。
- 反向依赖（`domain/` 去依赖 `shell/`、`platform/`、`render/`，或 `island_domain` 去链接它们）在评审里直接拒绝。

## 4. 一条功能住在哪儿

| 想改什么 | 主要落点 | 顺带要看 |
|---|---|---|
| pill 的形状、边距、命中区域 | `core/Geometry.h`（唯一一份圆角矩形判定）、`core/CardLayout.h` | 渲染与命中测试用同一个函数，改一处两边一起变 |
| 展开/收起的动画手感 | `core/Motion.h`（spring，可中断、继承速度） | `core/StatePolicy.h` 决定状态迁移 |
| 卡片里某个控件的位置 | `core/CardLayout.h` | `selftest/Diagnostics.cpp` 里通常已有对它的断言 |
| （托盘已移除） | 无 —— 托盘卡片、按压、学习、explorer 内模块在 2026-10-15 整体退役 | 实测记录留在 `docs/TRAY-RESEARCH-RETIRED.md`，探针在 `scripts/retired/tray/` |
| 媒体卡片 | `media/MusicSession.cpp`、`media/Lyrics.cpp`、`render/` | 延迟初始化：没音乐时不该起线程 |
| 通知纸条 | `services/`（Toast 读取）加 `app/IslandWindow` 的 NoticeStrip 状态 | |
| 与 Win32 对话的细节 | `platform/` | 不要在 `core/` 里加 `<windows.h>` |
| 一条诊断命令（"这个图标到底有没有回调"这类） | `cli/Cli.cpp` | 参数解析留在 `main.cpp`；命令只负责干活并返回退出码 |

## 5. 构建、自检、冒烟、部署

```powershell
# 构建（Debug 与 Release 都要过；/WX 现在是打开的，告警即错误）
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Debug

# 自检（900+ 断言；会开窗口，退出码 0 == 全过）
.\IslandBar\build\Release\src\Release\island_bar.exe --self-test

# 冒烟（编译、链接、运行时）
.\IslandBar\build\Release\tools\smoke\Release\island_smoke.exe
```

**四条环境事实**（都是实测踩出来的，不是传闻）：

1. **exe 要从项目树外运行**。镜像在 `C:\PROJECT\Taskbar` 下的进程会被降成 LOW 完整性，UIPI 会静默丢弃所有发往 Explorer 的消息；GUI 二进制在受限沙箱里起不来（0xC0000142）。正式部署位置是 `%USERPROFILE%\IslandBar`。
2. **有单实例互斥锁**（`IslandBar.SingleInstance`），不能并行跑第二个副本做对照。要对照就先优雅关掉正在运行的那个（它会恢复原生任务栏），测完再启动。
3. **只有 PowerShell 5.1**，所有 `.ps1` 必须是纯 ASCII。
4. **注入到别人进程里的模块必须自己 PIN 住**（`GET_MODULE_HANDLE_EX_FLAG_PIN`），否则那个进程退出时导入表里会留下悬空指针——这件事已经崩过 Explorer 6 次。**现在没有这样的模块了**（托盘 2026-10-15 退役后 IslandBar 是单进程），所以这条只在重新引入注入时适用。

## 6. 一次改动要走完的流程

1. **研究**：带着本机实测数据问外部评审（ChatGPT / Gemini），要求对方明确表态同意或反对，并记下反对点。
2. **方案**：写清改哪些文件、为什么（依据是实测数字或代码位置）、验收指标、回退方式、CPU / 内存 / 观感影响。
3. **等用户确认**，然后才动代码。
4. **实现并部署**：Debug 与 Release 都要能编过，跑 `--self-test` 与 `island_smoke`，部署到 `%USERPROFILE%\IslandBar`。
5. **等用户测**：用户说"没问题"之前不 push（本地提交可以）。
6. **推送**：用户确认后才推。本机 443 到 github.com 不通，走 `captures/push-via-api.ps1`。

## 7. 已知的架构债（写在明处，不是等人发现）

- **只有 `core/` 的边界被构建系统强制**：`island_domain` 是独立静态库，`domain/` 里不许出现 `<windows.h>`（编译期护栏）。`shell/`、`render/`、`media/`、`platform/` 之间仍是同一个目标里的翻译单元，互相 include 的门槛很低——下一步是按依赖方向再切静态库目标，逐层往上锁。
- **`main.cpp` 与 `selftest/Diagnostics.cpp` 仍然太大**（5.5k / 8.1k 行）。已经搬出去的三块：托盘的行为去了 `shell/TrayMenu.*`（234 行，**随托盘一起删了**）、代替岛运行的那些命令去了 `cli/Cli.cpp`（约 950 行）、`RunBar` 里跨帧状态去了 `app/BarState.h`。
  `main.cpp` 剩下的主体仍是 `RunBar`——约 5,000 行的帧循环，装着窗口、渲染、媒体、通知、天气和系统面板的全部驱动逻辑。**状态迁移是"一组一组来"的**：每组搬完都全量重建两套配置 + 自检 + 冒烟 + 部署，这样出错一定归因到刚搬的那组（整块机械替换的风险是"静默用了外层状态"，编译器抓不住）。顺序：面板与声音页那组 → 通知与媒体那组 → 对象组（window / renderer / text / 会话）。等这些搬完，`RunBar` 才谈得上按相位切成函数。
- **2026-10-15 已还的债**（留着这几行是为了说明它们不再是问题）：`core/` 已抽成 `island_domain` 独立静态库；模块↔主程序的通道已经从"文件轮询"换成共享内存环 + ABI 版本号（随托盘删除）；安装器已经能带第二份载荷（随托盘删除，回到单载荷）。
- **托盘那条链路整体退役了**：卡片、按压（回调消息 / UIA）、学习、explorer 内模块、4 条 CLI 命令、18 个探针都不在树里了。实测记录见 `docs/TRAY-RESEARCH-RETIRED.md`。**它的代价要记住**：这条 bar 默认仍然隐藏原生任务栏，所以系统托盘里的东西（微信、Steam、NVIDIA、网络、音量、时钟）现在**屏幕上没有任何入口**，除非用 `--keep-taskbar` 跑。
- **如果托盘要重做，先读 HANDOFF 8.133**（三个判别实验的实测结果）：占名当宿主**失败**（同名窗口收不到任何注册）；把原生任务栏**裁到只剩托盘成功且零成本**（区域能存活、点击穿透正常，代价是那 144 DIP 含时钟且必须让任务栏活着）；`SetParent` 搬走托盘窗口**被系统接受**（返回原父窗口、无错误），但"像素会不会跟着走"**没验证**。想拥有托盘协议本身（应用注册进我们手里）只有一条路：自己当 shell。
