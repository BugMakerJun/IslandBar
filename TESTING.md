# IslandBar 手工自测清单

> 用途：**只有你能做的验证**。自检（`island_bar.exe --self-test`，现在 751 项）覆盖几何、解析、像素，
> 但"鼠标点下去到底发生什么"必须在真人手里过一遍。
> **已脱敏**：命令里的 `$env:USERPROFILE` 就是你的用户目录（本文档按 PowerShell 写，所以用这个变量而不是 `%USERPROFILE%`）。
>
> 日志（**每一步的证据都在这里**）：
> `$env:USERPROFILE\IslandBar\island_bar.log`
> 想边点边看：
> `Get-Content $env:USERPROFILE\IslandBar\island_bar.log -Wait -Tail 40`
>
> 启动方式：双击 `$env:USERPROFILE\IslandBar\IslandBar (replace taskbar).lnk`
> （replace = 隐藏原生任务栏；keep = 两个都留着，调试用）。

---

## 1. 系统通知通道（刚做完那条）

**做**：开一个 PowerShell，粘这一行（会弹一条真的 Windows 通知）：

```
powershell -NoProfile -ExecutionPolicy Bypass -File C:\PROJECT\Taskbar\IslandBar\scripts\toast-test.ps1 -Title "构建完成" -Body "0 errors, 3 warnings"
```

**应该看到**：系统通知出现在角落的**同时**，**灵动岛向左右各宽出一截**，里面显示 `ToastTest · 构建完成`
（约 6 秒后恢复成胶囊；把鼠标移到岛上则展开大卡片，可以读完整正文）。

> 变宽是**左右对称**的（对胶囊左右各 +100 DIP），开始按钮、系统按钮、右侧应用图标会自动让位 ——
> 这是 2026-10-11 按你说的"有通知的时候 灵动岛变宽 在里面显示 / 是左右宽展"改成的行为。

**日志里**：`toasts: 1 new system toast(s), 1 taken in total`

**注意**：卡片 6 秒后自动收起；点卡片 = 已读，之后不再弹。
**如果不弹**：把日志里 `toasts:` 那几行发我 —— 它会说明是"没读到数据库"还是"读到了没送进卡片"。

---

## 1b. 微信通知（第三条通道）

微信 4.x **不接 Windows 通知平台**（不在通知设置里注册、从不写通知数据库、也不用托盘气泡），
窗口是自绘的（UI Automation 读不到文字），所以这条通道只能靠**认它的弹窗**。现在的行为是：
检测到它弹通知 → 岛上显示 **“微信 · 有新消息”**（不含内容，按你的要求）。

**做**：正常收一条微信消息（手机发给“文件传输助手”，或让别人发你一条）。

**应该看到**：灵动岛变宽，里面显示 `微信 · 有新消息`；约 6 秒后收回；点一下岛 = 已读。
5 秒内多条消息只提示一次（避免刷屏）。

**日志里**（Get-Content $env:USERPROFILE\IslandBar\island_bar.log -Wait -Tail 20）：

`
wechat: window appeared <句柄> '<类名>' (x,y) 宽x高 visible=1 -> counted as a notification
wechat: announced '有新消息' (window <句柄>, 1 in total)
`

**如果没有弹出来**：把日志里 wechat: 那几行发我 —— 它会列出微信**新出现的每个窗口**以及为什么没算作通知
（尺寸不在范围内 / 位置在屏幕中间 / 窗口不可见…）。我按那几行调判据即可，不用你再描述。

---
## 1c. 微信通话（在岛上的常驻显示）

**做**：打一个微信语音/视频通话（或让别人打给你）。

**应该看到**：灵动岛保持宽着的状态，里面显示 `微信 · 通话中`；通话窗口关掉后自动收回。
点一下岛 = 已读，本次通话期间不再打扰（岛收回）。

**日志里**：

`
wechat: call window <句柄> '微信音视频通话 ' appeared
wechat: call announced as '通话中' (notification 1)
wechat: call ended (notification 1 retired)      <- 通话结束时
`

判据是那个窗口的**标题**（微信自己的标题就是"微信音视频通话"），不读通话内容、也不碰微信。

---
## 1d. 微信消息（第三条通道：靠"提示音"）

微信 4.x 在 PC 上不接系统通知、不弹窗口、也不改标题 —— 唯一的提示是**托盘图标**，而托盘在"替换任务栏"模式下不在屏幕上。
所以这条通道用**声音**：微信安静时**没有音频会话**（实测 sessions 0），来消息响一声时会话就会出现。

**做**：正常收一条微信消息（手机发给"文件传输助手"最方便）。

**应该看到**：灵动岛变宽，显示 `微信 · 有新消息`（不含内容）；约 6 秒后收回；点一下岛 = 已读。

**日志里**：

`
wechat: audio baseline -- sessions 0 (was 0), active 0 (was 0), running 1
wechat: audio changed  -- sessions 1 (was 0), ...
wechat: announced '有新消息' from its notification sound (1 in total)
`

**已知会误报的情况**（不是 bug，是这条路的天性）：你自己发消息、播放语音消息时微信也会出声 → 也会提示。
**不会提示的情况**：微信里关了"新消息通知声音"，或开了免打扰。

---
## 2. 面板（点左侧 ⌃ 展开）

| 做什么 | 应该看到 | 日志里 |
|---|---|---|
| 展开后点 Codex 窗口 / 桌面（不在卡片上） | 面板自己收回 | `a press at (x,y) outside the bar` → `closed (a click outside the bar)` |
| 点"打开设置页"的瓦片（如 辅助功能） | 干净收回，**不再闪出音乐卡** | `click: tile N '辅助功能' action=…` |
| 点 **飞行模式** 瓦片 | 瓦片点亮、旁边 Wi-Fi 变 **已关闭**；再点恢复 | `Wi-Fi radio off/on (result 1, error 0 …)` |
| 按住音量滑条左右拖 | 手柄**紧跟指针**，上方显示百分比 | `volume: press on the slider -> N%` + 连续 `dragging -> N%` |
| 点滑条中间 | 手柄**走过去**（0.12 秒过渡） | `volume: set to N% (drag begins)` |
| 点喇叭 | 静音 / 取消静音 | `volume: muted / unmuted` |
| 点音量行右侧"设备 + >" | 进入"声音输出"子页 | `sound page: open` |
| 子页里滚鼠标滚轮 | 列表跟着滚 | `sound page: wheel -1 -> scroll 180` |
| 点一行**输出设备** | **会真的切换默认输出设备**（介意先别点） | `switching the default device to N …` → `switch default device ok/FAILED` |

### 2b. 展开的那一下，岛自己该纹丝不动（2026-10-12 修的那条）

**做**：点 ⌃ 展开面板，眼睛盯着**岛本身**、左边的 ⌃/⊞ 两个按钮、以及最右边的应用图标。

**应该看到**：展开的整个过程中，**岛一直是那个小胶囊**、时间留在原来的位置，面板像从栏后面
逐行滑出来；⌃/⊞ 和右侧图标**不动**。

**不该看到**（这是修之前的样子）：一块黑方块从胶囊里长出来压住面板瓦片、时间漂到黑方块正中间、
甲板和图标行横着甩出去再甩回来。

**日志里**（判断依据就是这一行，它打印的是岛填充和图标行共用的那个锚定盒）：

`
start button: island x [1170, 1390] ...
`

**在这一整段里它必须始终是 `[1170, 1390]`**，一个别的值都不该出现。修前的日志是
`[1158,1402]` → `[1112,1448]` → 最后跳回 `[1170,1390]`。

### 2c. 开卡片时，整条栏不该"暗一下再亮"（2026-10-12 修的那条）

**做**：两个场景各来一次 —— ① 点 ⌃ 开面板；② 放着音乐，把鼠标移进灵动岛（这会开音乐卡）。

**应该看到**：卡片自己淡入、岛的形状动，但**甲板 ⌃/⊞、右侧应用图标、歌词条**全程一样亮。

**不该看到**：整条栏（含图标行）一起暗下去、大约 1/3 秒后再亮回来。

**日志里**（这就是判定依据）：

`
start button: island x [1170, 1390] -> ... opacity 1.00
`

这一行**只在数值变化时打印**，所以**整段过渡里它只该出现一次**（值 = 1.00）。
修前它会连刷几十行、`opacity` 从 0.40 爬到 1.00 —— 那就是"闪一下"。

> 想看两个强度分开：设 `ISLANDBAR_TRACE_CARD=1` 启栏，卡片过渡每帧一行，
> `cardOpacity` 会从 0.35 涨到 1.00，而 **`barOpacity` 必须全程 1.00**。

> 通知纸条（第 1 节那条）也要顺带看一眼：来通知时岛**仍然左右展宽**、标题显示在纸条里 ——
> 修复动的是同一条规则，所以纸条是它唯一可能被弄坏的地方。

---

## 3. 音乐卡（有歌在放时把鼠标移到胶囊上）

| 做什么 | 应该看到 | 日志里 |
|---|---|---|
| 点 播放/暂停、上一首、下一首 | 歌真的跟着变 | `music: play/pause requested` |
| 看卡片右上角（2026-10-12 HANDOFF 8.29 起） | **没有喇叭、没有百分比、没有滑条** —— 整组控件按用户要求删掉了；名字那条来源行现在通到卡片右边距 | 日志里**不该再出现** `volume: press on the card's bar` / `volume: control opened` / `dragging the card's bar`；出现即回退。设备音量仍看第 2 节的面板滑条，单个应用的音量走子页「音量合成器」 |
| 放一首**歌词很长**的歌，看胶囊两头（2026-10-12 HANDOFF 8.30 起） | 字**从两端淡出/淡入**（左边正在离开的字、右边正在进来的字都变暗），中间全亮；不该再有"被圆角斜切掉半个字" | 没有日志行，看画面。判定标准是自检里量过的那条：同一个字在渐隐区读 `49.84`、紧挨其内侧读 `122.87`（关掉渐隐时两者都读 `122.8`） |
| 盯着胶囊看**歌词换句**的那一下 | **上下各半个行**：旧句往上滚出、新句从下面滚上来，两行各被一行高的窗口切掉一半 —— 这是**竖滚的定义**，不是缺陷（2026-10-12：8.31 曾改成原地淡入淡出，用户当天要求改回，见 HANDOFF 8.32） | 没有日志行，看画面。只有用户明确说"切换时不要出现两行"时才该动它 —— 那是把过渡换成原地淡入淡出的唯一理由 |
| 换一首**没有封面**的歌，把鼠标移到岛上开卡片（2026-10-12 HANDOFF 8.33 起） | 卡片是一块**深色卡面**，字和进度条都在卡面里，**不该**是"字直接飘在壁纸上" | 想确认那首歌到底有没有封面：跑 `build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe`，看 `Thumbnail: present/none` —— `none` 就是这首歌没发布封面，栏只能给它一块自己的底面 |
| 连续切两首歌，第二首**封面晚到**（2026-10-12 HANDOFF 8.42 起） | 第二首的封面必须**真的换成它自己的**；日志全对但画面不换，就是缓存键重复那类缺陷 | 日志正常不足以判定（`dropped`/`arrived late`/`decoded` 都会齐）；判定要看**画面**。核按钮：自检里 `a new cover with a repeated revision is not the old bitmap`（读数 `A cached 1, B cached 1, A still cached 0`）|
| 切歌时盯住卡片左边那块（2026-10-12 HANDOFF 8.41 起） | 新歌**没有封面时**显示 **♪ 占位符**（绝不能还是上一首的专辑图）；播放器随后发出封面就自动换上 | 日志：换歌那行下面应是 `music: cover NONE` + `music: cover dropped (the new track has none yet)`；若封面稍后到达会再出现 `music: cover arrived late (N bytes)` |
| 鼠标**停在**某个应用图标上（2026-10-12 HANDOFF 8.51 起） | 停约 **350 ms** 后，图标下面弹出那个窗口的**实时画面**（宽高比跟着窗口、最大 260×160、靠近屏幕边缘会被推回屏内）；**已经在显示时，顺着图标横移会立刻换成隔壁那个**（不等第二次 350 ms）；鼠标离开图标行就消失。**扫过一排图标时不该每个都闪一下**（只有停住才出）。卡片（音乐卡/面板）开着时不显示。预览本身**点不进去**（穿透） | 日志：`preview: hwnd … -> box …,(window WxH, compositor says AxB), shown=1, hr=0x00000000`。`shown=0` 或 `compositor says 0x0` = 这个窗口画不出来（提权窗口/跨桌面）。想单独验证配方：`build\Debug\tools\thumbprobe\Debug\thumb_probe.exe`（复制到项目树外再跑）—— 它会显示两个自己的窗口，**绿色**出现即说明这台机器的 DWM 缩略图可用 |
| 放一个**全屏应用**（浏览器按 F11）（2026-10-12 HANDOFF 8.50 起） | 顶部条带被一条满宽深色条填掉，**高度等于你任务栏的厚度**（改任务栏大小/用别的机器都跟着变，不再是写死 32）；退出全屏恢复胶囊 | 日志：`fullscreen: over, back to the pill`；`--appbar` 时还有 `appbar: ABM_SETPOS asking for 0,0 2560x42` → `registered -- work area was …, now …`，退出时 `appbar: released (work area restored)` |
| 看 `星期 · 日期 · 时间 · 天气` 这一行长什么样、出现在哪（2026-10-12 HANDOFF 8.45 + 8.46 + 8.48 + 8.49 起） | **一条，两个位置**。样式按参考图（日历图标已按你要求去掉）：`周一 10月12日 | 17:49 | [云雨] 小雨 17°` —— 周一是亮的粗体、日期更暗更小、时间最大最亮、两条细竖线分格、天气前面是蓝色云雨图标。**空闲**在胶囊里面，**放音乐 / 有消息 / 来电**时挪到 ⌄ 系统按钮**左边那条带子里**（同一条，图标也在），**淡入**而不是弹出。鼠标划过它不反应（箭头、点下去穿透）。**关掉播放器**才回到岛内（有会话就算占用，**暂停不算**） | 没有"显示在哪"的日志行，看画面；判定标准是自检量过的：`widest run 195.0 DIP; pill 204.0 available, band 252.0`（两条 fit + "胶囊才是紧的那个"）+ `StatusPlacementFor` 的"两种情况下都恰好一个家"。天气（2026-10-12 HANDOFF 8.47 起**默认走系统定位**）：`weather: position from Windows -> 41.775863, 123.442448` → `weather: 16.7 C, code 51 -> 小雨 17°`；状态行 `state: music-strip` / `notice-strip`。定不到位置时日志会写明原因（`location is denied for this program` / `no fix (0x…)`），那就改 `%USERPROFILE%\IslandBar\weather.txt`（**取消注释**一行城市名，或写 `39.9042,116.4074`）—— 文件里**有**一行时它覆盖定位，重启栏或等 15 分钟。要单独确认定位：`build\Debug\tools\geoprobe\Debug\geo_probe.exe`（复制到项目树外再跑，见 START-HERE）。要在有音乐时看空闲态：`ISLANDBAR_NO_MEDIA=1` 起一份诊断实例 |
| 手动切歌后**封面没出来**（2026-10-12 HANDOFF 8.44 起） | 先别怪栏：日志里若持续出现 `music: cover retry N -> the player still has none`（每约 2 秒一行，最多 5 行），说明**栏在问、播放器没给** | 再跑 `build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe` 看同一首歌是不是 `Thumbnail: none` —— 是，就是播放器没有；若 `present` 而栏一直 `retry ... none`，那才是栏的读取问题，告诉我 |
| 放歌时**别把鼠标移到岛上**，等它自然换歌（2026-10-12 HANDOFF 8.43 起） | 胶囊里那条**声波的颜色应该跟着新歌变**（以前不动鼠标就一直是上一首的颜色）| 日志：换歌后应出现 `music: cover decoded 宽x高`，且那一帧前后**没有** `state: expanded`（= 卡片没打开也解码了）|
| 切歌后**封面还是空的**（2026-10-12 HANDOFF 8.40 起） | 应在一两秒内自己补上；日志里出现 `music: cover arrived late (N bytes)` 就是补上了 | 若仍是空的，跑探针看这首歌到底有没有封面：`gsmtc_probe.exe` 的 `Thumbnail: present/none`。**present 而栏写 `cover NONE` = 栏的读取出了问题**（8.40 修的就是这个：长度为 0 的流以前会被拒绝） |
| 在**QQ音乐自己的窗口里**点切歌（2026-10-12 HANDOFF 8.39 起） | 新歌标题一秒内换上；封面若播放器晚发，**一两秒内自己出现**，不需要重启栏 | 日志：`music: cover NONE (0 bytes)` 之后若出现 **`music: cover arrived late (N bytes)`**，就是"晚到的封面被接住了"（这正是以前必须重启才能看到的那种）；轮询行距应 ≈1 秒（旧版是 2 秒） |
| 点卡片上的 ⏭ ⏮ 切歌（2026-10-12 HANDOFF 8.38 起） | 新歌的**标题、封面、歌词**在**一秒内**换过来（以前要 2-3 秒） | 日志：`music: next requested` → `music: transport next accepted` → 新歌那行的时间差。跳曲后 4 秒内轮询是 ~250 ms 一次，所以新歌一出现就会被读到 |
| 遇到**没有封面**的歌（封面上是个 ♪ 占位符） | 这说明**播放器没发封面**，不是栏丢图 | 两个证据：日志里 `music: cover NONE (0 bytes)`（而不是 `present` + `decoded`）；`build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe` 显示 `Thumbnail: none` |
| 鼠标从**卡片上的按钮**（⏮ ⏸ ⏭）、**图标行**、**开始/系统键**、**面板磁贴和滑条**上慢慢划过去（2026-10-12 HANDOFF 8.37 起） | 这些地方是**小手**；胶囊本身、卡片上的封面/文字、卡片外的空白是**箭头** | 没有日志行，看光标。想复核：自检里 7 条断言（738 项全绿），或临时把指针挪到某点再读 `GetCursorInfo` 与 `LoadCursor(IDC_HAND)` 是否同一个句柄 |
| 想知道**封面到底有没有取到**（2026-10-12 HANDOFF 8.36 起） | 日志里每首歌各一行：`music: cover present (N bytes, revision R)` + `music: cover decoded 宽x高` = 拿到了；`cover NONE` = **播放器没发**（探针也会显示 `Thumbnail: none`）；`COULD NOT DECODE` = 发了但栏解不开 | 三个证据按顺序看：① `build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe` 的 `Thumbnail:`；② 栏日志的两行 `music: cover`；③ 都不对才看渲染/卡片截图 |
| **打一通微信电话**，然后**别碰岛**，等一条消息进来（2026-10-12 HANDOFF 8.34 起） | 通话条（头像+绿计时+电平条）出现 → 消息顶掉它 → **消息 6 秒后通话条自己回来**，整通电话都在；点一下岛则只消掉消息，**通话留着** | 日志：`wechat: call announced as '通话中'`；**不该**在通话还在时出现 `state: collapsed`。没有真人电话时用 `scripts\fake-call-window.ps1`（把 `powershell.exe` 复制成 `WeixinCallProbe.exe` 再跑它，窗口在屏幕外） |
| 让纸条显示一句**长英文**（例如 `--notify "hello there my friend again" --app WeChat`） | 一句话**在一行里**（放不下就滚动），**不该**出现"上面半句 + 下面半个词" | 没有日志行；判定与自检同一条：墨迹必须是**一段**，折行是两段（`16 rows, 2 band(s)` 是缺陷、`13 rows, 1 band(s)` 才对） |
| 放着音乐去**点 ⌃ 开面板**（2026-10-12 修的那条） | 胶囊里**仍然是歌词**（没有歌词就是歌名），**不是时间**；面板自己开着 | 日志没有专门一行，看画面即可 —— 判据见 8.12：有卡片时胶囊那条带的像素必须和没卡片时**完全一样** |
| 放着音乐：鼠标移进岛（出卡片）后**快速左右横移**（2026-10-12 修的那条） | 歌词**待在胶囊里不动**，不该往卡片中间掉一下再回来 | 日志里不该出现 `WARNING: the bar's own line is being drawn in a box taller than the bar` —— 这条告警只在"栏自己那一行画进了比栏更高的盒子"时报一次 |

### 3b. 歌词到底有没有拿到（2026-10-12 修完那条）

**做**：放一首歌，看胶囊里那行字；同时看日志。

**日志里**（判定依据）：

`
lyrics: loaded (43 lines)                                   ← 拿到了
lyrics: none (0 lines)                                      ← 这首歌确实没有歌词（正常，只该出现一次）
lyrics: netease sent no original -- using its TRANSLATION    ← 用的是译文（原文为空）
`

**关键**：`none` 在这首歌里**只该出现一次**。修之前每次都重试 3 次（连刷三行 `none`），
因为"原文为空、文字在译文里"被当成了一次可重试的失败。

### 3c. 歌词跟得上、而且换行有动画（2026-10-12 修的那条）

**做**：放一首**有歌词**的歌，盯着胶囊里那一行。

**应该看到**：歌词**换行时是从下往上滚动**（新的那行从下面升上来、旧的那行往上滚出去，约 0.26 秒，
缓出收尾），滚动是**在一个"一行高"的窗口里**进行的 —— 所以任何时刻最多只看到一行文字的量，
不会出现两行挤在一起的重影。换行的**时刻也要和唱到的位置一致**（不再是晚一两秒"啪"地跳一下）。

> 如果歌词整体**早/晚一段固定时间**（不是偶发），那多半是 LRC 自带的 `[offset:N]` —— 8.16 之前
> 我们忽略它，现在会应用。若还有固定偏差，把歌名发我，我用 `-LyricsProbe` 看原始 LRC 里的 offset。

**还有一个计数**（8.17 修的那条）：日志里 `lyrics: line N at …` 这行**每次换行打印一次**，
所以同一行号**连着出现两次**就是异常（说明"没换行却被当成换行"，屏幕上会出现两行同样的字滚在一起）。
判据：拿最近的播报序列，**行号应该是递增的、没有相邻重复**。修前实测 183 次里 96 次重复，
修后 30 秒里 5 次、0 次重复。

**日志里**（判定依据，每次换行一行）：

`
lyrics: line 7 at 51230 ms (the session last reported 49800 ms)
`

括号里是 SMTC 上次上报的原始值，前面的才是**推算到当前时刻**的值 —— 两者会差 0.5～2 秒，
**那个差值就是以前会晚到的量**。另外报告行里 `redraws: ... lyrics=N` 应当**大于 0**（修前一直是 0：
连入场动画都没有重绘理由，所以什么都不会动）。

想单独试一首歌的抓取（不碰你的栏，只发两次 HTTPS）：

`
powershell -NoProfile -ExecutionPolicy Bypass -File C:\PROJECT\Taskbar\IslandBar\scripts\verify-panel-anchor.ps1 -LyricsProbe -Title "歌名" -Artist "歌手"
`

它会打印网易云与 QQ **两个服务各自的** URL、HTTP 状态、响应前 400 字节和各自结论。

---

## 4. 开始按钮（岛左边那个 Windows 标）

**做**：点它。**应该看到**：开始菜单弹出（再点收起）。
**日志里**：`start click: sent`（或 `sent (keybd_event)`）。
**如果不弹**：日志会写 `this process is BELOW the shell's integrity level …` —— 栏的权限比 Explorer 低，
Windows 把合成按键丢了；必须用那两个快捷方式启动，别从 IDE / 沙箱 / 别的终端里启动。

---

## 5. 通知卡片（本机推送通道）

**做**：

```
& "$env:USERPROFILE\IslandBar\island_bar.exe" --notify "标题" --body "正文" --app "我的工具"
```

**应该看到**：卡片弹出，来源行左边有 **"我的工具" 的图标**（取自发通知那个进程）。
**日志里**：`notification: [我的工具] 标题` 与 `notification: icon from '…' -> 32x32…`

---

## 6. 出问题时

```
# 最近 20 行日志
Get-Content $env:USERPROFILE\IslandBar\island_bar.log -Tail 20

# 全部自检（751 项：几何 / 解析 / 像素）
& "$env:USERPROFILE\IslandBar\island_bar.exe" --self-test

# Windows 侧最新的真通知（这条通道的调试口）
& "$env:USERPROFILE\IslandBar\island_bar.exe" --toast-dump 5
```
