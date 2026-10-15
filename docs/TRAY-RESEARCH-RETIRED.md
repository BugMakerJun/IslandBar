# 已退役：托盘注册线路格式（2026-10-15 实测记录）

> **状态：功能已退役，代码已删除。** 这份文件只保留实测证据，不描述任何现行代码路径。
> 2026-10-15，托盘（托盘卡片、按压、学习、explorer 内模块、它的命令行与安装器载荷）整体
> 从 IslandBar 移除，原因是用户要求去掉它。删除时把下面这段记录搬了出来，
> 以免一次有价值的逆向测量跟着死代码一起消失。

## 这份记录是什么

Windows 11 上，应用注册托盘图标 = **给 shell 的 `Shell_TrayWnd` 发一条同步的 `WM_COPYDATA`**，
`dwData = 1`，载荷是 shell 自己那份扩展版 `NOTIFYICONDATA`（实测 1,484 字节）。

实测原文（`reviews/tray-copydata-dump.txt` 里是原始抓取）：

```
copydata SENT pid 11936 receiver 0x10146 class "Shell_TrayWnd" sender 0x1F0A82 dwData 0x1 cbData 1484
```

## 布局是怎么定出来的

不是读头文件读出来的：让一个对照程序注册**已知值**（`probe-tray-registrar.exe`：uid 7、
消息 0xABCD、tip `ISLANDBAR-PROBE-4711`，随后 `NIM_SETVERSION 4`），再在字节里找这些值出现的位置。
随后与 bar 已经知道的答案交叉验证：Steam 的副本是 uid 1 / 消息 0x0401 / tip "Steam"；
GUID 注册的图标带的 guid 与 shell 注册表条目**逐字节一致**。

| 偏移 | 内容 |
|---|---|
| `+0x000` | 标记 `0x34753423`（每一条都有） |
| `+0x004` | `NIM_` 命令：0 ADD、1 MODIFY、2 DELETE、3 SETFOCUS、4 SETVERSION |
| `+0x00C` | 托盘窗口（低 32 位） |
| `+0x010` | uid |
| `+0x014` | uFlags（`NIF_GUID = 0x20` 表示这一格由 guid 命名） |
| `+0x018` | uCallbackMessage —— 应用自己要收的通知消息号 |
| `+0x020` | szTip，宽字符 128 个 |
| `+0x328` | uVersion（只有 SETVERSION 那条非零） |
| `+0x3B0` | guidItem，16 字节原始字节 |

载荷最小可信长度 1024；tag 与长度都要检查，**不匹配就拒绝而不是照旧偏移读**——照旧读的失败
形态是"从没看懂过的格式里悄悄学到错的消息号"，那是不可见的，而"什么都没学到"是可见的。

## 已知局限

* 这是**一次实测的布局**（Windows 11，10.0.26220，2026-10-15），不是 Windows 承诺的稳定 ABI。
* 只覆盖"经过这条通道的注册"。观察窗口之前就注册完的图标（例如 NVIDIA 那几个开机即注册的）
  不会因此被发现；`TaskbarCreated` 广播对它们无效。
* 观察必须发生在 shell 进程内（那里是同步 `WM_COPYDATA` 的接收端），因此需要向 explorer 注入模块；
  实测模块必须自己 PIN 住，否则进程退出时导入表会留下悬空指针（历史上崩过 explorer 6 次、
  wallpaper64 4 次）。

## 相关的已退役工具

`IslandBar/scripts/retired/tray/` 下是当时的探针（`probe-tray-registrar.cpp`、ETW/CALLWNDPROC
观察钩子、UIA 与面板测量脚本、`measure-tray-panel.ps1`）。它们**不在**构建里，也不再是日常工具；
要重新验证这份布局时才需要它们。
