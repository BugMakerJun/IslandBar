# IslandBar 架构与开发方式

写这份文件的目的只有一个：让第一次打开这个仓库的人**读完就知道每块代码住在哪儿、谁可以依赖谁、加一个功能该动哪几个文件、怎么验证自己没改坏。**

历史（每一次改动、每一个实测数字、每一次失败的原因）在 `HANDOFF.md`，那是档案不是入口；改动的流程规则在 `AGENTS.md`。

## 1. 这东西是什么

Windows 11 上替代系统任务栏的一条横条（"岛"）：自己画 pill 与卡片，接管托盘、媒体、通知、开始菜单、任务条，并把原生任务栏藏起来。C++20 + Win32 + D3D11 + Direct2D + DirectComposition，单 exe、静态 CRT、无运行时依赖，私有内存 75-95MB。

二进制只有两个：`island_bar.exe`（主程序，干所有活）和 `island_tray_hook.dll`（装进 explorer 的托盘观察模块）。

## 2. 目录与职责

```
src/
  main.cpp          入口。解析命令行 → 要么跑一条命令，要么跑这条岛
  core/             纯逻辑：几何、布局、动画、状态策略、日志、进程指标
  app/              窗口与生命周期：IslandWindow（画与命中）、AppBar（占位）、PlayerWindow
  render/           D2D/DComp 渲染：Renderer、TextRenderer、玻璃材质
  media/            GSMTC 会话、歌词、音量、声音输出、图片解码
  shell/            与 shell 打交道：托盘（TrayIcons/TrayPress/TrayLearn）、开始菜单、天气、系统开关、原生任务栏
  platform/         平台能力：显示器、窗口列表、缩略图、Win32 薄封装
  services/         Toast 通知读取与通知管道
  selftest/         诊断与 900+ 条断言（--self-test）
  trayhook/         island_tray_hook.dll 的全部代码（独立目标，只导出 IslandTrayHookProc）
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
   +-- core/（纯逻辑：布局、动画、命中、状态策略）
```

- **`core/` 不许包含 `<windows.h>`**。它只做几何、布局、动画、状态策略这些能脱离桌面运行的东西——`--self-test` 能在不开窗口的情况下跑完 900+ 条断言，靠的就是这条规则真的成立。
- 这条规则**不靠自觉**：`src/core/PurityCheck.cpp` 是一个只做一件事的翻译单元——包含所有 `core/` 头文件，然后 `#ifdef _WINDOWS_ #error`。谁把 `<windows.h>` 加回纯逻辑层，谁的构建**当场失败**，错误信息里写着该往哪儿搬。
- 需要 Win32 类型的地方（例如 `LONG`/`UINT` 的像素换算）放在 `platform/` 或各模块自己的头里，不放 `core/`。
- 反向依赖（`core/` 去依赖 `shell/`、`platform/`、`render/`）在评审里直接拒绝。

## 4. 一条功能住在哪儿

| 想改什么 | 主要落点 | 顺带要看 |
|---|---|---|
| pill 的形状、边距、命中区域 | `core/Geometry.h`（唯一一份圆角矩形判定）、`core/CardLayout.h` | 渲染与命中测试用同一个函数，改一处两边一起变 |
| 展开/收起的动画手感 | `core/Motion.h`（spring，可中断、继承速度） | `core/StatePolicy.h` 决定状态迁移 |
| 卡片里某个控件的位置 | `core/CardLayout.h` | `selftest/Diagnostics.cpp` 里通常已有对它的断言 |
| 托盘格子的行为 | `shell/TrayPress.cpp`（按压）、`shell/TrayLearn.cpp`（学习）、`trayhook/TrayHook.cpp`（模块侧） | 缓存格式 `%USERPROFILE%\IslandBar\tray_callbacks.txt` |
| 媒体卡片 | `media/MusicSession.cpp`、`media/Lyrics.cpp`、`render/` | 延迟初始化：没音乐时不该起线程 |
| 通知纸条 | `services/`（Toast 读取）加 `app/IslandWindow` 的 NoticeStrip 状态 | |
| 与 Win32 对话的细节 | `platform/` | 不要在 `core/` 里加 `<windows.h>` |

## 5. 构建、自检、冒烟、部署

```powershell
# 构建（Debug 与 Release 都要过；/WX 现在是打开的，告警即错误）
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Debug

# 自检（900+ 断言，不开窗口，退出码 0 == 全过）
.\IslandBar\build\Release\src\Release\island_bar.exe --self-test

# 冒烟（编译、链接、运行时）
.\IslandBar\build\Release\tools\smoke\Release\island_smoke.exe
```

**四条环境事实**（都是实测踩出来的，不是传闻）：

1. **exe 要从项目树外运行**。镜像在 `C:\PROJECT\Taskbar` 下的进程会被降成 LOW 完整性，UIPI 会静默丢弃所有发往 Explorer 的消息；GUI 二进制在受限沙箱里起不来（0xC0000142）。正式部署位置是 `%USERPROFILE%\IslandBar`。
2. **有单实例互斥锁**（`IslandBar.SingleInstance`），不能并行跑第二个副本做对照。要对照就先优雅关掉正在运行的那个（它会恢复原生任务栏），测完再启动。
3. **只有 PowerShell 5.1**，所有 `.ps1` 必须是纯 ASCII。
4. **注入到别人进程里的模块必须自己 PIN 住**（`GET_MODULE_HANDLE_EX_FLAG_PIN`），否则那个进程退出时导入表里会留下悬空指针——这件事已经崩过 Explorer 6 次。

## 6. 一次改动要走完的流程

1. **研究**：带着本机实测数据问外部评审（ChatGPT / Gemini），要求对方明确表态同意或反对，并记下反对点。
2. **方案**：写清改哪些文件、为什么（依据是实测数字或代码位置）、验收指标、回退方式、CPU / 内存 / 观感影响。
3. **等用户确认**，然后才动代码。
4. **实现并部署**：Debug 与 Release 都要能编过，跑 `--self-test` 与 `island_smoke`，部署到 `%USERPROFILE%\IslandBar`。
5. **等用户测**：用户说"没问题"之前不 push（本地提交可以）。
6. **推送**：用户确认后才推。本机 443 到 github.com 不通，走 `captures/push-via-api.ps1`。

## 7. 已知的架构债（写在明处，不是等人发现）

- **模块边界还没有被构建系统强制**：`core/` 有了编译期护栏，但 `shell/`、`render/`、`media/` 之间仍是同一个目标里的翻译单元，互相 include 的门槛很低。下一步是把 `core/` 抽成独立静态库目标，再逐层往上锁。
- **`main.cpp` 与 `selftest/Diagnostics.cpp` 太大**（7.3k / 8.6k 行）：命令行子命令、帧循环、托盘菜单动作、诊断注入路径都还在 `main.cpp` 里。按自然缝隙拆（CLI 先出去），但要在托盘链路稳定之后单独做一轮。
- **模块与主程序之间靠文件轮询**：`trayhook` 写 `tray_learned.txt`，bar 每 5 秒读一次；模块也没有 ABI 版本契约（现在靠内容哈希命名来避免文件锁）。正式做法应是共享内存或管道加版本号，把文件降级为可回放语料。
- **安装器还没带 `island_tray_hook.dll`**：托盘学习是正式功能，所以全新安装缺这个 DLL 就是功能缺口。
- **托盘按压没有自动化回归**：只能靠 CLI 探针（`--tray-surface` / `--tray-learn` / `--tray-press N`）加人工验证。
