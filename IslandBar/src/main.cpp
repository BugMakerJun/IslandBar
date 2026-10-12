// =====================================================================
//  main.cpp
//  IslandBar entry point.
//
//  Two modes:
//    island_bar.exe --self-test   run the phase 0-d foundation checks and exit
//                                 with the failure count (0 == pass)
//    island_bar.exe               show the bar and run until closed
//
//  The self-test mode exists because this subsystem has no visible behaviour to
//  judge by eye: what matters is whether Win32 actually gave the window the
//  styles it needs, whether the composition surface presents, and whether the
//  click-through region matches the painted pill. Those are all machine-checkable.
// =====================================================================
#include <windows.h>

#include <shellapi.h>   // CommandLineToArgvW: command-line values as real wide characters

#include <cstdio>
#include <cmath>        // fabs: comparing the last pushed slider fraction
#include <cstring>
#include <cwchar>       // wcstoul / wcstol / wcstof: the diagnostic environment settings
#include <memory>       // make_unique: the sound page's worker object
#include <vector>

#include "AppBar.h"
#include "StartMenu.h"
#include "TaskStripLayout.h"
#include "WindowList.h"
#include "AudioVolume.h"
#include "Diagnostics.h"
#include "Geometry.h"
#include "IslandWindow.h"
#include "Log.h"
#include "Monitor.h"
#include "Motion.h"
#include "MusicSession.h"
#include "NativeTaskbar.h"
#include "Lyrics.h"
#include "Notification.h"
#include "NotificationIcon.h"
#include "NotificationPipe.h"
#include "ToastStore.h"
#include "WeChatWatch.h"
#include "StatePolicy.h"
#include "SoundOutput.h"
#include "SystemStatus.h"
#include "WicImage.h"
#include "Weather.h"
#include "Renderer.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")

namespace {

// This is a Windows-subsystem executable (see src/CMakeLists.txt), so it has no
// console of its own: launched by double-click it shows only the bar, which is
// the point. Diagnostics still need somewhere to go, so:
//
//   * run from a terminal  -> attach to the parent's console, and the output
//                             appears in that terminal as usual;
//   * run by double-click   -> nobody is reading, so stay silent rather than
//                             popping up a console window.
//
// Only the self-test allocates a console when there is no parent to borrow,
// because a test that prints nothing is not a test.
//
// `detail` is filled in with what actually happened. Three separate "the program
// printed nothing" reports made it clear that guessing here is not good enough:
// the probe reports these values so the failure is visible instead of silent.
bool PrepareConsole(bool createIfMissing, wchar_t* detail, size_t detailCapacity) {
    const DWORD pid = GetConsoleProcessList(nullptr, 0);
    const bool inheritedConsole = (pid > 0);

    const BOOL attached = AttachConsole(ATTACH_PARENT_PROCESS);
    const DWORD attachError = attached ? 0 : GetLastError();
    const bool haveConsole = attached != FALSE || inheritedConsole;

    if (!haveConsole && createIfMissing) {
        AllocConsole();
    }

    if (detail && detailCapacity) {
        std::swprintf(detail, detailCapacity,
                      L"inherited=%d attach=%d err=%lu",
                      inheritedConsole ? 1 : 0, attached ? 1 : 0,
                      static_cast<unsigned long>(attachError));
    }

    // Redirect the C runtime's standard streams onto the console ONLY when we
    // created one ourselves.
    //
    // When a console was merely attached or inherited, stdout already points
    // wherever the caller aimed it -- a pipe, a file, the terminal. Reopening it
    // onto "CONOUT$" would silently break `island_bar.exe --self-test > log`,
    // which is exactly how the tests are run from a script.
    if (!haveConsole) {
        FILE* stream = nullptr;
        if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0) return false;
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
    }

    SetConsoleOutputCP(CP_UTF8);

    // Line-buffered, not block-buffered: the bar's few startup lines would
    // otherwise sit in the runtime's buffer. setvbuf demands a real buffer when a
    // mode is given -- passing size 0 with _IOLBF trips the CRT's own
    // assertion (2 <= buffer_size_in_bytes) in setvbuf.cpp.
    static char stdoutBuffer[1024];
    std::setvbuf(stdout, stdoutBuffer, _IOLBF, sizeof(stdoutBuffer));

    return haveConsole;
}

// Per-monitor-v2, requested at runtime as well as in the manifest. The manifest
// is what normally applies it; doing it here as well makes the executable behave
// correctly even when it is launched in a way that ignores the manifest.
void EnablePerMonitorV2() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadCtxFn)(DPI_AWARENESS_CONTEXT);
        auto setContext = reinterpret_cast<SetThreadCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
        if (setContext) {
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
}

// Runs the three states on a fixed schedule with no input at all.
//
// This exists because the hover transition cannot be verified by hand from a
// script: the bar is deliberately click-through, so nothing can be driven through
// it programmatically, and "it looked fine when I moved the mouse" is not
// evidence. Driving the state machine on a clock exercises exactly the same code
// the hover path uses, and makes every state observable to a screenshot.
int RunDemo() {
    island::MonitorGeometry monitor = island::QueryMonitor(nullptr);

    island::IslandWindow window;
    if (!window.Create(monitor, /*show=*/true)) return 1;

    const RECT rect = window.WindowRect();
    island::Renderer renderer;
    if (!renderer.Initialize(window.Handle(),
                             static_cast<UINT>(rect.right - rect.left),
                             static_cast<UINT>(rect.bottom - rect.top),
                             window.Scale())) {
        window.Destroy();
        return 1;
    }

    island::TextRenderer text;
    if (text.Initialize()) renderer.SetTextRenderer(&text);

    island::LogAndPrint("IslandBar demo: collapsed -> hover -> expanded -> hover -> collapsed\n");
    std::fflush(stdout);

    struct Step {
        island::IslandState state;
        double seconds;
    };
    const Step steps[] = {
        {island::IslandState::Collapsed, 1.2},
        {island::IslandState::Hover,     1.5},
        {island::IslandState::Expanded,  2.0},
        {island::IslandState::Hover,     1.5},
        {island::IslandState::Collapsed, 1.2},
    };

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER previous = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    bool clockDriven = renderer.WaitForCompositorClock(1);

    for (const Step& step : steps) {
        window.SetState(step.state);
        island::LogAndPrint("  -> %ls for %.1fs\n", island::StateName(step.state), step.seconds);
        std::fflush(stdout);

        const double deadline = step.seconds;
        double elapsed = 0.0;
        while (elapsed < deadline) {
            if (!window.PumpMessages()) break;

            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            const double dt = static_cast<double>(now.QuadPart - previous.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
            previous = now;
            elapsed += dt;

            const bool animating = window.Tick(static_cast<float>(dt > 0.1 ? 0.1 : dt));

            island::FrameState frame;
            frame.pill.box = window.CurrentBox();
            frame.state = window.State();
            frame.clock = (frame.state == island::IslandState::Expanded)
                              ? island::ClockStyle::Card
                              : island::ClockStyle::Compact;
            // The CARD fades while the shape moves; the bar's own content does not. One number for both
            // is what made the real bar blink (see ContentStrengthsFor).
            frame.cardOpacity = window.IsAnimating() ? 0.5f : 1.0f;

            renderer.Render(frame);

            if (animating && clockDriven) {
                renderer.WaitForCompositorClock(8);
            } else {
                ::MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }
    }

    renderer.SetTextRenderer(nullptr);
    text.Shutdown();
    window.Destroy();
    return 0;
}

// UTF-8 to wide, for an argument that is already UTF-8 (see ArgumentAsUtf8).
bool ArgumentAsUtf8ToWide(const char* utf8, wchar_t* out, size_t capacity) {
    if (!utf8 || !out || capacity == 0) return false;
    out[0] = L'\0';
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, static_cast<int>(capacity)) > 0;
}
struct FrameStats {
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER last = {};
    bool  started = false;

    // Intervals above this are counted as hitches.
    static constexpr double kHitchMs = 50.0;

    long long count = 0;
    double totalMs = 0.0;
    double worstMs = 0.0;
    int hitches = 0;
    // A coarse histogram, in milliseconds, for a rough percentile.
    int buckets[8] = {};   // <8, <16, <24, <33, <50, <100, <250, >=250

    void Start() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&last);
        started = true;
    }

    void Record() {
        if (!started || frequency.QuadPart <= 0) return;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        const double ms = static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 /
                          static_cast<double>(frequency.QuadPart);
        last = now;

        ++count;
        totalMs += ms;
        if (ms > worstMs) worstMs = ms;
        if (ms > kHitchMs) ++hitches;

        if (ms < 8.0)        ++buckets[0];
        else if (ms < 16.0)  ++buckets[1];
        else if (ms < 24.0)  ++buckets[2];
        else if (ms < 33.0)  ++buckets[3];
        else if (ms < 50.0)  ++buckets[4];
        else if (ms < 100.0) ++buckets[5];
        else if (ms < 250.0) ++buckets[6];
        else                 ++buckets[7];
    }

    void Reset() {
        count = 0;
        totalMs = 0.0;
        worstMs = 0.0;
        hitches = 0;
        for (int& bucket : buckets) bucket = 0;
        Start();
    }

    void Describe(wchar_t* out, size_t capacity) const {
        if (!out || capacity == 0) return;
        const double mean = count > 0 ? totalMs / static_cast<double>(count) : 0.0;
        swprintf(out, capacity,
                 L"avg %.1f ms, worst %.1f ms, hitches(>%.0f ms) %d | "
                 L"<8:%d <16:%d <24:%d <33:%d <50:%d <100:%d <250:%d 250+:%d",
                 mean, worstMs, kHitchMs, hitches, buckets[0], buckets[1], buckets[2],
                 buckets[3], buckets[4], buckets[5], buckets[6], buckets[7]);
    }
};

// Builds the system panel's tiles.
//
// Ordered the way the shell orders them -- Wi-Fi, Bluetooth, airplane mode -- and assembled here rather
// than in the reader because WHICH tiles exist is a presentation decision, while the reader only reports
// what the machine says.
//
// A tile is shown whenever the machine has the thing, ON or OFF, because the whole point of a tile is
// to be pressed. "Airplane mode" is always shown even though it cannot be toggled from here: on a
// desktop it is permanently off, and hiding it would make the grid's contents depend on which wireless
// hardware happens to be present.
//
// Pure with respect to its inputs, so the tile list can be asserted without a machine to read from.
// `volumeKnown` / `volume` / `muted` are the LIVE reading, not the sampled one.
//
// This is the difference between a slider and a slider that feels broken. The panel's handle used to be
// drawn from `status.volumePercent`, and that reading is taken every two seconds -- so while the handle
// was being dragged it moved in two-second steps, or not at all until the drag was over, which is exactly
// what "声音进度条那个不顺滑" is. `AudioVolume` publishes the new level the instant it is set, on the
// frame that set it, so the handle is under the pointer for the whole of the drag.
// The call strip's avatar: the icon of the process that is IN the call, which on this machine is WeChat itself.
//
// The CALLER's picture is not readable from outside WeChat -- it is inside a self-drawn surface with no
// accessibility tree -- so the strip shows the application's icon in that place. It is the honest stand-in:
// the same icon the notification rows use, and the same one the user already associates with the call.
std::vector<uint8_t> callIconPixels;
int callIconWidth = 0;
int callIconHeight = 0;
unsigned long long callIconRevision = 0;
wchar_t callIconSource[260] = {};

// The executable behind a process id, for the call strip's icon. False when the process has gone or cannot be
// opened, in which case the strip draws a plain dot rather than nothing.
bool ProcessImagePath(unsigned long pid, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = L'\0';
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD length = static_cast<DWORD>(capacity);
    const bool ok = QueryFullProcessImageNameW(process, 0, out, &length) != 0;
    CloseHandle(process);
    if (!ok) out[0] = L'\0';
    return ok;
}
// The sender's icon, held between frames: the pixels, their size, and the source they came from so the same
// image is not looked up again. File scope because the frame build reads them and the loader fills them, and
// because a notification's icon outlives the message that brought it (the card is drawn while it retracts).
std::vector<uint8_t> notificationIconPixels;
int notificationIconWidth = 0;
int notificationIconHeight = 0;
unsigned long long notificationIconRevision = 0;
wchar_t notificationIconSource[260] = {};
// How long the drawn volume takes to catch up with the reading, in seconds.
//
// Short enough that a click feels immediate and long enough that the eye sees the handle travel: this is the
// "过渡" the volume control was missing. A drag ignores it entirely -- see shownVolume.
constexpr float kVolumeChaseSeconds = 0.12f;
int BuildSystemPanel(const island::SystemStatus& status, int installedLayoutCount, bool volumeKnown,
                     float volume, bool muted, island::TextRenderer::SystemPanelText* out,
                     int pageIndex = 0) {
    if (!out) return 0;
    out->tileCount = 0;
    out->volume = volumeKnown ? volume : static_cast<float>(status.volumePercent) / 100.0f;
    out->muted = volumeKnown ? muted : status.muted;

    // The whole set is built here and the requested PAGE is copied out of it, because the shell's flyout pages
    // through its tiles and a page is a window over the set rather than a different set.
    island::TextRenderer::SystemPanelTile all[island::kMaxSystemPanelTiles] = {};
    int allCount = 0;
    const int perPage = island::kMaxSystemTiles;

    // `stateText` is what the caption under the box says when it is not the name -- the shell's own flyout
    // captions its Wi-Fi tile "可用" rather than "Wi-Fi", because the glyph above it already names the control.
    // Chinese, because that is the flyout this panel sits next to and the strings here are user-facing.
    // `chevron` is passed per tile rather than derived from the action, because the two answer different
    // questions: the arrow means "this opens a LIST", while the action means "this does something". The
    // shell's flyout has tiles of both kinds -- Wi-Fi and Bluetooth lead to their own flyouts and carry the
    // arrow, 飞行模式 and 实时字幕 switch in place and carry none -- and it has tiles with an arrow whose
    // list opens INSIDE the panel, which is not a settings page at all.
    const auto addTile = [&](const wchar_t* label, const wchar_t* stateText,
                             island::TextRenderer::IndicatorIcon icon, int iconValue, bool on,
                             bool chevron, island::TextRenderer::PanelAction action,
                             const wchar_t* uri) {
        if (allCount >= island::kMaxSystemPanelTiles) return;
        island::TextRenderer::SystemPanelTile& tile = all[allCount++];
        tile.label = label;
        tile.stateText = stateText;
        tile.icon = icon;
        tile.iconValue = iconValue;
        tile.on = on;
        tile.chevron = chevron;
        tile.action = action;
        tile.uri = uri;
    };

    // Wi-Fi. The label says WHICH state rather than only on/off, because "Wi-Fi" alone would leave the
    // user guessing what pressing it does -- and on this machine there is no API to switch the radio, so
    // the tile's honest job is to say what the network is doing, and pressing it opens the page where it
    // CAN be changed.
    {
        const bool connected = status.network == island::NetworkKind::Connected;
        const wchar_t* label = status.network == island::NetworkKind::None
                                   ? L"No Wi-Fi"
                                   : (connected ? L"Wi-Fi" : L"Wi-Fi");
        // 可用 when there is a network, which is what the shell's own flyout prints under this tile -- its
        // Wi-Fi tile names the STATE, not the radio, because the glyph already says which control it is.
        const wchar_t* state = status.network == island::NetworkKind::None
                                   ? L"\u65E0 Wi-Fi"          // 无 Wi-Fi
                                   : (connected ? L"\u53EF\u7528"          // 可用
                                                : L"\u672A\u8FDE\u63A5"); // 未连接
        // A radio that has been switched off is not "not connected": it is off, and saying 未连接 for it
        // leaves the user looking for a network problem that is a switch.
        if (!status.wifiRadioOn) state = L"\u5DF2\u5173\u95ED";   // 已关闭
        addTile(label, state, island::TextRenderer::IndicatorIcon::Network,
                static_cast<int>(status.signal), connected && status.wifiRadioOn, true,
                island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:network-wifi");
    }

    // Bluetooth. Captioned with the CONNECTION state rather than the radio's, which is what the shell's own
    // flyout prints (its Bluetooth tile reads 未连接 with the radio on and nothing paired). Whether a device
    // is actually attached is not readable from here -- the audio endpoint would answer for headphones only
    // -- so the honest pair is 关 when the radio is off and 未连接 when it is on.
    addTile(L"Bluetooth", status.bluetoothOn ? L"\u672A\u8FDE\u63A5" : L"\u5173",   // 未连接 / 关
            island::TextRenderer::IndicatorIcon::Bluetooth, status.bluetoothOn ? 1 : 0,
            status.bluetoothOn, true,
            island::TextRenderer::PanelAction::OpenSettings,
            L"ms-settings:bluetooth");

    // 飞行模式 -- the panel's ONE real toggle, and a state that is read back rather than remembered.
    //
    // What it switches is the Wi-Fi radio (island::SetWifiRadioOn). Airplane mode in the shell's own flyout
    // means every radio, and Bluetooth has no documented interface a desktop process can use; switching
    // Wi-Fi and calling it "airplane mode" would be exactly the kind of half-truth this panel has been
    // careful about elsewhere, so the log line names what actually changed. The tile itself lights up when
    // the radio is off, which is the state the user sees reflected in the Wi-Fi tile beside it (已关闭).
    //
    // Captioned with its NAME and not with 开/关, which is how the shell has this tile: the state is the
    // tile's own highlight.
    addTile(L"\u98DE\u884C\u6A21\u5F0F", nullptr,   // 飞行模式
            island::TextRenderer::IndicatorIcon::Airplane, 0, !status.wifiRadioOn, false,
            island::TextRenderer::PanelAction::ToggleRadio, nullptr);

    // --- the second row ---------------------------------------------------------------
    //
    // These are the shell's own second row. Two of them have no readable state on this machine and are
    // shown OFF with a page to open, which is honest: the tile still does something useful (it takes the
    // user where the setting lives) without claiming to know what the setting is.
    //
    // Accessibility is a LAUNCHER, not a toggle. The shell opens a flyout of individual switches
    // (magnifier, colour filters, narrator); this opens the page those switches live on, which is the
    // same destination one level up and needs no state of its own.
    // ... and it DOES carry the arrow: the shell opens a list of the individual switches (magnifier, colour
    // filters, narrator) under it. This one opens the page those switches live on, which is the same
    // destination one level up.
    addTile(L"\u8F85\u52A9\u529F\u80FD", nullptr,   // 辅助功能
            island::TextRenderer::IndicatorIcon::Accessibility, 0, false, true,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:easeofaccess");

    // 节能模式 -- battery saver, which is what the shell calls this tile. Its state IS readable
    // (GetSystemPowerStatus reports it in SystemStatusFlag), so the tile reflects the machine; turning it on
    // and off is a power-scheme write that is not implemented, so the page is opened instead of the tile
    // pretending to toggle. Named in Chinese like the tiles around it, which is how the shell's flyout reads.
    addTile(L"\u8282\u80FD\u6A21\u5F0F", status.batterySaverOn ? L"\u5F00" : L"\u5173",   // 节能模式, 开 / 关
            island::TextRenderer::IndicatorIcon::BatterySaver, 0, status.batterySaverOn, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:batterysaver");

    // Live captions: Windows 11 22H2 and later. Always shown, because whether captions are RUNNING is
    // not something this process can see, and a tile that disappeared on older builds would be a gap in
    // the grid rather than a missing feature.
    addTile(L"\u5B9E\u65F6\u5B57\u5E55", nullptr,   // 实时字幕
            island::TextRenderer::IndicatorIcon::Captions, 0, false, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:easeofaccess-hearing");

    // --- the second page -------------------------------------------------------------
    //
    // The shell's own flyout scrolls to a second row of tiles; these are the ones people reach for next, and
    // each is an honest "take me where this lives" tile rather than a toggle this process cannot perform. No
    // state on any of them, because none of these settings can be READ from here -- a tile that guessed would
    // be worse than one that simply opens the page.
    addTile(L"\u591C\u95F4\u6A21\u5F0F", nullptr,             // 夜间模式
            island::TextRenderer::IndicatorIcon::Moon, 0, false, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:nightlight");
    addTile(L"\u79FB\u52A8\u70ED\u70B9", nullptr,             // 移动热点
            island::TextRenderer::IndicatorIcon::Hotspot, 0, false, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:network-mobilehotspot");
    addTile(L"\u5C31\u8FD1\u5171\u4EAB", nullptr,             // 就近共享
            island::TextRenderer::IndicatorIcon::Share, 0, false, false,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:nearby-sharing");

    // 有线显示器 and 投影, the two tiles that carry the arrow on the shell's own second page: both open a
    // LIST of display modes rather than switching anything in place.
    //
    // 有线显示器 opens the display page -- the list of modes lives there. 投影 sends Win+P, which is the
    // shell's own projection flyout; there is no settings URI for it, and sending the key is exactly what
    // pressing that tile in the shell does.
    addTile(L"\u6709\u7EBF\u663E\u793A\u5668", nullptr,       // 有线显示器
            island::TextRenderer::IndicatorIcon::WiredDisplay, 0, false, true,
            island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:display");
    addTile(L"\u6295\u5F71", nullptr,                         // 投影
            island::TextRenderer::IndicatorIcon::Projection, 0, false, true,
            island::TextRenderer::PanelAction::ProjectionModes, nullptr);

    // --- beyond the shell's own set ---------------------------------------------------
    //
    // Everything the shell's two pages hold is above; these are the tiles this bar has on top of them, and
    // they go after the shell's last one so the two pages the reference pictures show stay exactly as they
    // are. Both are conditional, so on most machines the panel simply ends after 投影.
    //
    // The input method, when there is more than one layout to switch between -- the same rule the indicator
    // row uses, so the two cannot disagree about whether it exists.
    if (island::ShouldDrawInputMethod(status, installedLayoutCount)) {
        // Static, not a local: the tile keeps the pointer until the draw, later in this same frame, and a
        // stack buffer would be gone by then. (The battery percentage below has the same shape.)
        static wchar_t label[8] = {};
        island::InputMethodLabel(status.inputLanguageId, label, 8);
        addTile(label, nullptr, island::TextRenderer::IndicatorIcon::InputMethod, 0, true, false,
                island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:typing");
    }

    if (island::ShouldDrawBattery(status)) {
        static wchar_t percent[8] = {};
        percent[0] = L'\0';
        std::swprintf(percent, 8, L"%d%%", status.batteryPercent);
        addTile(L"Battery", percent, island::TextRenderer::IndicatorIcon::Battery,
                status.batteryPercent, status.onAcPower || status.batteryPercent > 20, false,
                island::TextRenderer::PanelAction::OpenSettings, L"ms-settings:batterysaver");
    }

    // Copy the requested page out of the set.
    const int pages = (allCount + perPage - 1) / perPage;
    out->pageCount = pages > 0 ? pages : 1;
    out->pageIndex = pageIndex >= 0 && pageIndex < out->pageCount ? pageIndex : 0;
    const int first = out->pageIndex * perPage;
    for (int i = 0; i < perPage && first + i < allCount; ++i) {
        out->tiles[i] = all[first + i];
        out->tileCount = i + 1;
    }
    return out->tileCount;
}

// Fills the 声音输出 page's text from an audio snapshot.
//
// The names in the result are POINTERS into `snapshot`, which the caller keeps in the same scope as the
// frame that is drawn from it -- the page's rows cannot own their strings, because they are rebuilt every
// frame and a copy per row per frame would be work for nothing.
//
// The mixer's FIRST row is the output device itself, which is how the shell's own mixer is built: the
// device's own volume at the top, then one row per application. Its volume comes from the master reading
// rather than from the session list, because the device is not a session.
void BuildSoundPage(const island::SoundOutputSnapshot& snapshot, const island::SystemStatus& status,
                    bool haveMasterLevel, float masterLevel, bool masterMuted, float scroll,
                    island::TextRenderer::SoundPageText* out) {
    if (!out) return;
    *out = island::TextRenderer::SoundPageText{};

    // The page's own words, exactly as the shell's page prints them.
    out->title = L"\u58F0\u97F3\u8F93\u51FA";                       // 声音输出
    out->deviceLabel = L"\u8F93\u51FA\u8BBE\u5907";                 // 输出设备
    out->spatialLabel = L"\u7A7A\u95F4\u97F3\u6548";                // 空间音效
    out->mixerLabel = L"\u97F3\u91CF\u5408\u6210\u5668";            // 音量合成器
    out->footer = L"\u66F4\u591A\u97F3\u91CF\u8BBE\u7F6E";          // 更多音量设置

    // 空间音效. The first row is the one in use: turning spatial sound off is the default on this machine
    // and the setting itself is not readable or writable through any public interface, so the page shows
    // the shell's two choices with the safer one marked and reports honestly when one is pressed.
    out->spatial[0].label = L"\u5173";                              // 关
    out->spatial[0].selected = true;
    out->spatial[1].label = L"\u7528\u4E8E\u8033\u673A\u7684 Windows Sonic";   // 用于耳机的 Windows Sonic
    out->spatial[1].selected = false;

    for (int i = 0; i < snapshot.deviceCount && i < island::kMaxSoundDevices; ++i) {
        out->devices[i].name = snapshot.devices[i].name;
        out->devices[i].formFactor = snapshot.devices[i].formFactor;
        out->devices[i].isDefault = snapshot.devices[i].isDefault;
        out->deviceCount = i + 1;
    }

    int row = 0;
    {
        island::TextRenderer::SoundPageText::MixRow& device = out->mix[row];
        // The short name -- "扬声器" out of "扬声器 (Realtek(R) Audio)" -- which is what the shell prints on
        // this row, because the long name would not fit and every device's long name says the same thing.
        device.label = snapshot.defaultDeviceName[0] ? snapshot.defaultDeviceName : L"Output device";
        device.volume = haveMasterLevel ? masterLevel
                                        : static_cast<float>(status.volumePercent) / 100.0f;
        device.muted = haveMasterLevel ? masterMuted : status.muted;
        ++row;
    }
    for (int i = 0; i < snapshot.sessionCount && row < island::TextRenderer::SoundPageText::kMaxMixRows;
         ++i) {
        const island::SoundSessionInfo& session = snapshot.sessions[i];
        island::TextRenderer::SoundPageText::MixRow& mix = out->mix[row];
        mix.label = session.name;
        mix.volume = session.volume;
        mix.muted = false;
        if (session.hasIcon && session.iconSize > 0) {
            mix.iconPng = session.icon;
            mix.iconSize = session.iconSize;
            mix.iconRevision = session.iconRevision;
        }
        ++row;
    }
    out->mixCount = row;
    out->scroll = scroll;
}

// `forcedState` pins the shape so it can be looked at, and `forcedStartHover` pins the Start button's
// hover. Debug only, and they exist because a shape can satisfy every assertion and still be wrong to
// look at: a screenshot of a named state settles in seconds what reasoning about geometry does not.
int RunBar(bool hideNativeTaskbar, bool claimAppBar, const wchar_t* forcedState,
           bool forcedStartHover, bool forcedSystemPanel) {
    island::MonitorGeometry monitor = island::QueryMonitor(nullptr);

    // The machine's own state, sampled on a timer rather than per frame.
    island::SystemStatusReader statusReader;
    island::SystemStatus systemStatus;
    statusReader.Open();

    // How long between samples. Most of these values come from APIs that open a COM object or walk
    // a device tree, and they do not change quickly enough to justify paying that every frame.
    // Two seconds is short enough that plugging in a network cable looks immediate.
    constexpr unsigned long long kStatusSampleMs = 2000;
    unsigned long long lastStatusSampleMs = 0;

    // Whether the system panel is open. It is a TOGGLE, like the shell flyout it mirrors.
    bool systemPanelOpen = forcedSystemPanel;
    // Which page of tiles the panel is showing. The shell's flyout pages through them; a page is a window over
    // the whole set, and the arrows and dots are the controls for it.
    int systemPanelPage = 0;
    // Which page to start on, for looking at the second page without a pointer. Diagnostic only.
    int startPanelPage = 0;
    // The volume slider, while the pointer has the mouse button down on it.
    //
    // A press jumps the handle to where the click landed, and HOLDING it has to keep moving the handle --
    // a slider that only jumped once per click is not a slider, and the shell's own control follows the
    // pointer. The state lives out here because the click arrives through the message queue while the
    // movement is read from the pointer each pass.
    bool panelSliderDrag = false;
    // The fraction last actually pushed to the device, so a drag does not re-send the same value every frame.
    // The handle is drawn from the live reading either way, so nothing is lost by skipping the repeats.
    float lastAppliedVolumeFraction = -1.0f;
    float lastAppliedMixerFraction = -1.0f;
    float lastLoggedVolumeFraction = -1.0f;
    // Set by a press that started a slider drag, taken by the release that ends it: see the Slider branch.
    bool sliderPressHandled = false;
    // The volume the sliders are DRAWN at: the live reading, chased over a tenth of a second.
    //
    // "没有动画过渡" -- the handle used to be drawn at whatever the reading was, so every change that did not
    // come from a drag (a click on the track, a keyboard volume key, the shell's own flyout) was a jump with
    // no movement in it. The chase is on the DRAWING only: a drag sets the device from the pointer's own
    // position and is drawn from the same place, so following the pointer stays 1:1 and the pointer never
    // feels like it is dragging something heavy.
    //
    // The music card's own bar was the other reader of this value; it was removed on request ("把音乐卡片里
    // 右上角的喇叭图标去掉吧"), and this stays because the SYSTEM PANEL's slider is drawn at it.
    float shownVolume = -1.0f;
    // Until when the volume's percentage stays on screen. Set for a moment after any change, so the number
    // is visible for a moment even when the slider was clicked rather than dragged.
    unsigned long long panelVolumeEchoUntil = 0;
    // The "声音输出" page: the shell's second level off the volume row, with the output devices, the spatial
    // sound row and the per-application mixer. A page INSIDE the card rather than a second card.
    bool soundPageOpen = false;
    float soundPageScroll = 0.0f;
    // The mixer's own sliders, while one of them has the mouse button down. -1 is none; row 0 is the device's
    // master volume and the rows after it are the applications. The PID is held rather than the row number
    // alone, because the rows are re-read once a second and a row can be a different application by the time
    // the pointer comes back to it -- the one the drag started on is the one it must keep moving.
    int mixerSliderDragRow = -1;
    unsigned long mixerSliderDragPid = 0;
    // Diagnostics only: hold the panel open, and the sound page with it, so a photograph of either is not at
    // the mercy of a click landing somewhere else on the machine. See ISLANDBAR_PANEL_LOCK.
    bool panelLocked = false;
    bool lockedSoundPage = false;
    // Whether the left button was down the last time the frame loop looked. The edge is what matters: a
    // press that starts outside the bar puts the panel away, and a button already held does not.
    bool leftWasDown = false;
    // Diagnostics: a one-shot synthetic click, so the outside-click dismissal can be exercised on a machine
    // whose integrity levels make a script's own input invisible to the bar. See ISLANDBAR_CLICK_AT.
    long injectClick_ = 0;
    long injectClickY_ = 0;
    unsigned long long injectClickAtMs_ = 0;
    // The last audio reading, kept for the CLICK handler as well as the draw: a click on a device row has to
    // name the endpoint it wants, and the row the user is looking at is the one in this snapshot. On the
    // heap, like the worker that fills it: it is tens of kilobytes and this function's frame is not the
    // place for that.
    auto soundSnapshot = std::make_unique<island::SoundOutputSnapshot>();

    island::LogAndPrint("IslandBar\n");
    island::LogAndPrint("  monitor %ldx%ld, work %ldx%ld, %u dpi, taskbar inset %ld px\n",
                island::ScreenWidth(monitor), island::ScreenHeight(monitor),
                island::WorkWidth(monitor), island::WorkHeight(monitor),
                monitor.dpi, island::TaskbarThickness(monitor));
    island::LogAndPrint("  move the pointer onto the pill to spread it; click to open the card\n");
    island::LogAndPrint("  close the window to exit\n");

    // Hiding the shell's taskbar is the point of the exercise, but it is also the
    // only thing this program does that the user can see outside its own window,
    // so it is announced and it is always undone on the way out.
    //
    // If this process is killed rather than closed, the restore never runs. The
    // documented recovery then is to restart Explorer, or to run:
    //   ShowWindow(FindWindow("Shell_TrayWnd", NULL), SW_SHOW)
    island::NativeTaskbar taskbar;
    bool haveTaskbar = false;
    if (hideNativeTaskbar) {
        haveTaskbar = taskbar.Locate();
        if (haveTaskbar) {
            // Check the integrity level as well as the result. When this process is
            // confined to a lower integrity level than the shell, every method of
            // hiding the taskbar is silently dropped by UIPI: the calls return 0,
            // no error is reported, and the taskbar simply stays. Saying so up
            // front is the difference between a two-minute fix and an afternoon of
            // suspecting the code.
            const bool blocked = taskbar.IsBlockedByIntegrity();
            const bool hidden = taskbar.Hide();

            if (hidden && !blocked) {
                island::LogAndPrint("  native taskbar: hidden");
                island::LogAndPrint("  (restored on exit; if this process is killed"
                                    " instead, restart Explorer)");
            } else if (blocked) {
                island::LogAndPrint("  native taskbar: CANNOT BE HIDDEN from this process");
                island::LogAndPrint("    island_bar.exe runs at a LOWER integrity level than the");
                island::LogAndPrint("    shell's taskbar, so Windows drops the request silently");
                island::LogAndPrint("    (UIPI: no error, the call just returns 0).");
                island::LogAndPrint("    FIX: start island_bar.exe from your own shell -- Win+R, or a");
                island::LogAndPrint("    PowerShell/cmd window opened from the Start menu -- NOT from");
                island::LogAndPrint("    inside a sandbox, an IDE, or a restricted launcher.");
                island::LogAndPrint("    Confirm with --probe-taskbar there before starting the bar.");
            } else {
                island::LogAndPrint("  native taskbar: could NOT be hidden (reason unknown;"
                                    " try --probe-taskbar)");
            }
        } else {
            island::LogAndPrint("  native taskbar: not found (nothing to hide)");
        }
    } else {
        island::LogAndPrint("  native taskbar: left alone (--keep-taskbar)");
    }
    // Flushed explicitly rather than left to the buffer: this process runs for
    // hours and is then killed, and a killed process discards whatever the
    // runtime still holds. Redirecting stdout makes _IOLBF block-buffered
    // anyway, so the mode alone cannot be relied on here.
    std::fflush(stdout);

    // --- the AppBar: claim the strip so the desktop gives up the space -----
    //
    // This is what makes the island a TASKBAR rather than a floating decoration. Without it a
    // maximized window still covers the top of the screen and the island sits on its title bar;
    // with it the work area starts below the island, exactly as it does for the real taskbar.
    //
    // Claimed AFTER the window exists (the shell needs an HWND that is a real top-level window)
    // but the window is NOT repositioned afterwards: the pill is anchored to the window's own
    // top edge and does not read the work area, so a later work-area change cannot move it. That
    // independence is deliberate -- it is why the tests can assert the pill's position without
    // knowing anything about bars.
    island::AppBar appBar;
    // kReservedBarHeightDip, not margin + collapsed height: the reservation has to cover the whole
    // chrome, and the two differ by the pill's own height. See the constant.
    const LONG barThicknessPx =
        island::DipToPixels(island::kReservedBarHeightDip, monitor.dpi);
    island::AppBarResult appBarResult = island::AppBarResult::NoWindow;
    // The full-screen state as of the last frame, so the transition is acted on once rather
    // than every frame.
    bool fullscreenLast = false;

    island::IslandWindow window;
    if (!window.Create(monitor, /*show=*/true)) {
        island::LogAndPrint("FATAL: could not create the window (error %lu)\n", GetLastError());
        std::fflush(stdout);
        // The taskbar was already hidden by this point, so it must not be left
        // that way because startup failed.
        taskbar.Restore();
        return 1;
    }

    window.SetAppBar(&appBar);

    if (!claimAppBar) {
        // Off by default, and that is a deliberate safety decision rather than an unfinished
        // feature.
        //
        // Registering as an AppBar rewrites the desktop's work area, and every maximized window
        // follows it. A first attempt at this moved every window on the machine -- including the
        // island, because the window positioned itself from the work area it was changing -- to
        // the bottom of the screen, and no error was reported anywhere. It cannot be reproduced
        // from the development sandbox, where the request is refused outright as
        // "access denied" before it can do anything.
        //
        // So it is opt-in until it has been seen to work on a real desktop, and the code that
        // performs it verifies the resulting work area and gives the space back if it did not land
        // where it was asked to. --appbar turns it on.
        island::LogAndPrint("  appbar: not claimed (pass --appbar to claim the strip; the space"
                            " is only reserved when asked)");
    } else {
        // This block is only reached when the native bar is being hidden -- the
        // --keep-taskbar case skips the branch above entirely -- so there is no guard here.
        //
        // Claiming the strip while the shell's own taskbar is on screen cannot work: it already
        // owns the top edge. A refusal is reported rather than retried, because retrying cannot
        // change the answer.
        appBarResult = appBar.Claim(window.Handle(), island::DockEdge::Top, barThicknessPx);
        island::LogAndPrint("  appbar: %ls -- %ls", island::AppBarResultName(appBarResult),
                            appBar.LastDetail());

        // Each outcome needs a different response, which is why they are distinguished rather
        // than collapsed into a bool. SpaceNotGranted is the interesting one: the registration
        // stands, so the space may become available once the shell's own bar is gone, and the
        // shell says so with ABN_POSCHANGED.
        if (appBarResult == island::AppBarResult::NotRegistered) {
            island::LogAndPrint("    The shell refused to register another bar on this edge;");
            island::LogAndPrint("    that is expected while the native taskbar still owns it.");
        } else if (appBarResult == island::AppBarResult::SpaceNotGranted) {
            island::LogAndPrint("    Registered, but the desktop did not give up the strip yet.");
            island::LogAndPrint("    It is claimed again automatically on ABN_POSCHANGED.");
        }
        std::fflush(stdout);
    }

    const RECT rect = window.WindowRect();
    island::Renderer renderer;
    const bool renderOk = renderer.Initialize(
        window.Handle(),
        static_cast<UINT>(rect.right - rect.left),
        static_cast<UINT>(rect.bottom - rect.top),
        window.Scale());

    island::TextRenderer text;
    if (renderOk && text.Initialize()) {
        renderer.SetTextRenderer(&text);
    }

    // --- the local push channel ---
    //
    // Started before the loop so a sender that connects immediately is served
    // rather than refused. A failure here is reported and the bar keeps working:
    // the clock and the hover states do not depend on notifications.
    island::NotificationStore notifications;
    island::NotificationServer notificationServer;
    const bool listening = notificationServer.Start();
    island::LogAndPrint(listening ? "  notifications: listening on \\\\.\\pipe\\%ls"
                                  : "  notifications: DISABLED (could not create the pipe)",
                        island::kPipeName);

    // The SECOND notification channel: Windows' own toasts, read out of the database the notification centre
    // uses. Polled on the frame loop's slow timer rather than by a thread of its own -- one indexed SELECT
    // every couple of seconds costs less than a thread would -- and a failure to read it leaves the bar exactly
    // as it was, because the push channel does not depend on it.
    island::ToastReader toasts;
    const bool toastsAvailable = toasts.Start();
    if (!toastsAvailable) {
        island::LogAndPrint("  notifications: system toasts are NOT being read (see above)");
    }
    unsigned long long lastToastPoll = 0;

    // The THIRD channel: WeChat, which tells Windows nothing at all and shows its own popup instead.
    //
    // Polled faster than the toast database because a WeChat popup is on screen for a few seconds: two
    // seconds of polling would miss it about as often as not. The scan is a handful of windows.
    island::WeChatWatcher wechat;
    unsigned long long lastWeChatPoll = 0;
    // When the call strip's meter was last redrawn. See the redraw reasons below.
    unsigned long long lastCallMeterMs = 0;
// ... and the same clock for the pill's background meter (see musicMeterDue).
unsigned long long lastMusicMeterMs = 0;
    // The WeChat sound channel's own clock: 200 ms, because the edge it looks for lasts about a second.
    unsigned long long lastWeChatSoundPoll = 0;
    std::fflush(stdout);

    // --- what is playing ---
    //
    // Polled on a worker thread; the frame loop only reads a snapshot. A failure
    // here is not fatal: the bar is a taskbar replacement, not a music player.
    island::MusicSession music;
    //
    // ISLANDBAR_NO_MEDIA=1 leaves the reader unstarted, which is how the bar looks on a machine
    // where nothing is playing -- a state that cannot otherwise be photographed on this one, whose
    // owner keeps a player running all day. It is the switch the idle island's own line needs: with
    // no session the pill shows the date and the time, and with one it shows the lyric and the line
    // stands beside the deck (see StatusPlacementFor).
    wchar_t noMediaSetting[8] = {};
    const bool noMedia = GetEnvironmentVariableW(L"ISLANDBAR_NO_MEDIA", noMediaSetting, 8) > 0 &&
                         noMediaSetting[0] == L'1';
    const bool musicAvailable = !noMedia && music.Start();
    island::LogAndPrint(musicAvailable
                            ? "  music: reading the system media session"
                            : (noMedia ? "  music: DISABLED BY ISLANDBAR_NO_MEDIA (as if nothing were"
                                         " playing)"
                                       : "  music: DISABLED (the media API is unavailable)"));
    std::fflush(stdout);

    // --- lyrics ---
    //
    // Also on a worker thread, because fetching them is a network round trip. A
    // failure here is not fatal either: the strip falls back to the track name.
    // --- the output volume ---
    //
    // On its own worker, because IMMDeviceEnumerator touches the audio service and can block
    // for tens of milliseconds; blocking the frame loop is exactly what the stutter reports
    // were about.
    island::AudioVolume volume;
    const bool volumeAvailable = volume.Start();
    island::LogAndPrint(volumeAvailable ? "  volume: reading the default output endpoint"
                                        : "  volume: DISABLED (no audio endpoint)");
    std::fflush(stdout);

    // --- the 声音输出 page's own data -------------------------------------
    //
    // The output devices and the per-application mixer, on a worker of its own for the same reason the
    // volume is: enumeration touches the audio service, and an application's icon comes off the disk.
    //
    // On the HEAP, because the object carries the mixer's icons and a whole snapshot of the list -- tens of
    // kilobytes -- and this function's own frame is not the place for that.
    //
    // STARTED AT STARTUP, not when the 声音输出 page is first opened, and that changed when the WeChat channel
    // was added: its SOUND signal is read from the per-application session list, and that signal has to work
    // whether or not anybody has ever looked at the mixer. (It did not, silently: the page's lazy start meant
    // the list was never read at all, and the sound channel sat there with nothing to look at.)
    auto soundOutput = std::make_unique<island::SoundOutput>();
    const bool soundOutputStarted = soundOutput->Start();
    island::LogAndPrint(soundOutputStarted
                            ? "  sound: audio sessions are being read (for the mixer and the WeChat channel)"
                            : "  sound: audio sessions are NOT readable; the mixer and the WeChat sound"
                              " channel are unavailable");
    std::fflush(stdout);

    island::LyricsProvider lyrics;
    const bool lyricsAvailable = lyrics.Start();
    island::LogAndPrint(lyricsAvailable
                            ? "  lyrics: on demand, NetEase then QQ Music"
                            : "  lyrics: DISABLED (the worker could not start)");
    std::fflush(stdout);

    // --- the weather on the status strip ----------------------------------
    //
    // Polled on a worker, like the lyrics and for the same reason: it is a network call, and a
    // slow or unreachable service must not be able to stall the animation.
    //
    // Started unconditionally. A machine with no weather.txt (or no network) is the ordinary
    // case for a first run: the reader logs where the city goes and the strip shows the date and
    // the time without a reading, which is what it is designed to do.
    island::WeatherReader weather;
    const bool weatherAvailable = weather.Start();
    island::LogAndPrint(weatherAvailable
                            ? "  weather: position from Windows, forecast from Open-Meteo "
                              "(weather.txt can pin a city instead)"
                            : "  weather: DISABLED (the worker could not start)");
    std::fflush(stdout);

    // --- the taskbar's window list ---
    //
    // Refreshed from the frame loop rather than polled here, because it is only needed while the
    // strip is on screen. The titles are copied into this array for the frame to point at: the list
    // can refresh between frames, and a frame that pointed into the list would read freed entries.
    island::WindowList taskWindows;
    wchar_t taskTitles[island::kMaxTaskWindows][island::kTaskTitleCapacity] = {};
    // How far each icon button's growth has run, 0 to 1. Held across frames because it is an
    // animation, not a state: the button opens over several frames rather than snapping.
    //
    // ONE value, not two. An earlier version animated the width and the push-along separately, and
    // while the two disagreed the widened button's right edge ran past the next button's left edge
    // and painted over it -- which is the "the icons are covered / it did not make room" report that
    // survived two rounds of diagnosis. Both the width and the push now come from this value, so
    // they cannot disagree.
    float taskHoverAmount[island::kMaxTaskWindows] = {};
    // True while any button is mid-transition, so the loop keeps drawing until it settles.
    bool iconAnimating = false;
// Whether any icon in the row is still arriving or leaving. Set inside the draw (where the row is
// updated) and read by the next frame's redraw decision, so the arrival animation keeps running.
bool rowAppearAnimating = false;

// --- the date / weekday / time / weather line -----------------------------
//
// How far the BESIDE-THE-DECK copy of it has faded in, 0 to 1, held across frames because it is an
// animation and not a state. It follows the one rule that decides where the line goes: while the
// pill's own line is free the line is INSIDE the pill (no fade -- the pill is always there), and
// while the pill's line is taken (the lyric, a message, a call) the same words stand beside the
// deck and fade in. See StatusPlacementFor for the rule and StatusStripFadeStep for the fade.
float statusStripOpacity = 0.0f;
// True while that fade is in flight, which is what asks for the next frame. Set inside the draw and
// read by the next pass, like the two arrival flags above.
bool statusStripAnimating = false;
// The weather reader's revision as of the last draw, so a new reading is a redraw reason of its own:
// the reading arrives on a worker, a few seconds after the bar has settled.
unsigned long long lastWeatherRevision = 0;
// Where the strip's weather text lives for the frame to point at. A member of this scope rather than
// the reader's own storage, because a reading can be replaced mid-frame -- replace it and the frame
// would print half of one reading and half of the next.
wchar_t statusWeatherText[64] = {};
// One entry per window the row is drawing or fading out: an icon's membership of the row and how far
// in it is (see TaskIconAppearStep).
struct RowSlot {
    HWND  handle = nullptr;
    float appear = 0.0f;
};
RowSlot rowSlots[island::FrameState::kMaxTaskButtons] = {};
int rowSlotCount = 0;
    // How many application icons the row is currently showing. The click handler bounds
    // itself by this and not by the window list, which can hold windows the row had no room
    // for -- activating one of those would switch to a window with no icon on screen.
    int  taskWindowsShown = 0;
    // The window behind each drawn icon, frozen when the frame was built.
    //
    // The click resolves through THIS and not through the live list. The list is
    // re-enumerated every 500 ms and its order can differ from the one on screen, so
    // indexing into it at click time could activate a window the user did not point at.
    HWND taskHandles[island::kMaxTaskWindows] = {};
    // What was last written to the trace, so it reports changes rather than every frame.
    int   lastReportedHoveredIcon = -2;
    float lastReportedHoverAmount = -1.0f;
    int   lastReportedPointed = -2;
    bool  lastReportedAnimating = false;
    unsigned long long lastTaskRefresh = 0;

    if (!renderOk) {
        island::LogAndPrint("WARNING: composition surface unavailable; the window stays empty\n");
        std::fflush(stdout);
    } else {
        // Frame pacing. The compositor clock drives this when available, so an
        // idle desktop costs nothing; otherwise MsgWaitForMultipleObjects blocks
        // on input, which is also event-driven rather than a spin.
        const bool clockDriven = renderer.WaitForCompositorClock(1);
        island::LogAndPrint("  frame pacing: %s\n", clockDriven ? "compositor clock" : "message wait");
        std::fflush(stdout);

        LARGE_INTEGER frequency = {};
        LARGE_INTEGER previous = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&previous);

        int  lastSecond = -1;
        long long lastNotificationId = 0;
        island::IslandState lastLogged = window.State();
        bool haveDrawn = false;
        unsigned long long lastDecodedArtworkRevision = 0;
        const uint8_t* artworkPixels = nullptr;
// The wave's colour, taken from the cover when it is decoded (see AccentColourFromArtwork). The default
// is what a track with no artwork -- or an unsaturated one -- keeps.
island::AccentColour waveAccent;
float musicWavePhase = 0.0f;
        int artworkWidth = 0;
        int artworkHeight = 0;
        const uint8_t* iconPixels = nullptr;
        int iconWidth = 0;
        int iconHeight = 0;
        unsigned long long lastDecodedIconRevision = 0;
        int lastReportedIconWidth = -1;
        bool iconWasDecoded = false;
        const uint8_t* backgroundPixels = nullptr;
        int backgroundWidth = 0;
        int backgroundHeight = 0;

        // --- now-playing display policy ---
        //
        // The card opens when playback starts and closes a few seconds after it
        // stops. It deliberately does NOT open for a paused session: the bar is
        // always on top of everything, and a card that unfolds every time a browser
        // tab pauses a video would be worse than no feature at all.
        island::MusicInfo nowPlaying;
        unsigned long long musicPositionAt = GetTickCount64();
        long long lastShownSecond = -1;
        unsigned long long lastVolumeRevision = volumeAvailable ? volume.Revision() : 0;
        // The 声音输出 page: whether it is up, how far it is scrolled, and the revision of the audio reading
        // it was last drawn from. The mixer's rows change on their own -- an application starts or stops
        // playing -- so a change in the reading is a reason to draw, and nothing else on this bar is.
        bool lastSoundPageOpen = false;
        float lastSoundScroll = 0.0f;
        unsigned long long lastSoundRevision = 0;
        unsigned long long lastIntervalReport = GetTickCount64();
    int lastReportedButtons = -1;
    island::IslandState lastReportedState = island::IslandState::Collapsed;
        unsigned long long lastRegionApplications = 0;
        // Which card owns the shape, held for the whole life of the card rather than for the life of the
        // request that opened it. See NextCardOwner.
        island::CardOwner cardOwner = island::CardOwner::None;
        // The message the card is showing, COPIED, and held for the same reason the owner is.
        //
        // A pointer into the notification store would be reused the moment the store's ring comes round
        // -- and the card is still on screen while it retracts, so a new message arriving during the exit
        // would change what the outgoing card says. The copy keeps the exit honest.
        island::Notification notificationShown;
        bool haveNotificationShown = false;
        DWORD lastStartTogglePhase = 0xFFFFFFFFul;   // no toggle has happened yet
        int whyAnimating = 0, whyClock = 0, whyNotification = 0, whyMusic = 0;
        int whyLyrics = 0, whyProgress = 0, whyFirst = 0, whyStatusStrip = 0;
        FrameStats liveStats;
        liveStats.Start();
        unsigned long long lastMusicRevision = music.Revision();
        unsigned long long lastDrawnMusicRevision = music.Revision();

        // --- diagnostic hooks, all off unless the environment asks for them ---
        //
        // The panel's opening animation has been argued about from still captures three times, because
        // this machine cannot move the pointer (so no transition can be performed on demand) and the
        // interesting part of a 0.36 s movement is invisible in a photograph of either end of it.
        //
        //   ISLANDBAR_PANEL_MS    open the system panel after N ms, close it again at 2N -- a real
        //                         transition, with no input needed
        //   ISLANDBAR_SLOWMO      stretch every transition's duration by this factor, so a capture can
        //                         land inside one
        //   ISLANDBAR_TRACE_CARD  one line per frame of a card transition: frame interval, eased
        //                         progress, the card's rectangle, the content's opacity and the window
        //                         region
        //
        // Read once, here. Unset, they cost a few microseconds and change nothing.
        DWORD autoPanelMs = 0;
        DWORD autoPanelCloseMs = 0;
        DWORD autoPanelCycleMs = 0;
        DWORD autoStartMs = 0;
        DWORD autoStartCycleMs = 0;
    // >0: fire one play/pause on the media session after this long. The transport is otherwise reachable
    // only by clicking the card, and a script's click cannot reach it (the window's own synthetic click
    // lands wherever the REAL cursor is), so this is the only way to exercise the request path and the
    // promptness of the button's state without a person.
    DWORD autoTransportMs = 0;
    bool transportForced = false;
        int forcedHoverIcon = -2;   // -2 none, -1 the last icon, 0..n that icon
        DWORD autoDeckHoverMs = 0;  // >0: pretend the pointer reaches a deck button after this long
        bool deckHoverForced = false;
        // >0: flick the pointer on and off the island with this period, which is what a mouse crossing
        // the bar quickly does. Reproduces an INTERRUPTED card retraction -- see the pill's line rule in
        // Renderer.cpp -- which is otherwise only reachable by hand.
        DWORD autoInsideFlickerMs = 0;
        bool flickerInside = true;
        DWORD autoCardHoverMs = 0;  // >0: pretend the pointer reaches the card after this long
        bool cardHoverForced = false;
        int  autoCardMode = 3;      // 3 = on the card, 4 = in the gap above it
        float slowMotion = 1.0f;
        bool traceCardFrames = false;
        {
            wchar_t setting[32] = {};
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_MS", setting, 32) > 0) {
                autoPanelMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // Which page of tiles to open on, so the second page can be photographed without a pointer.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_PAGE", setting, 32) > 0) {
                startPanelPage = static_cast<int>(wcstol(setting, nullptr, 10));
                systemPanelPage = startPanelPage;
            }
            // Open the panel's second level -- the 声音输出 page -- straight away, so it can be photographed
            // without a pointer, and at a chosen scroll offset so its own second half (the mixer) can be too.
            if (GetEnvironmentVariableW(L"ISLANDBAR_SOUND_PAGE", setting, 32) > 0) {
                soundPageOpen = wcstol(setting, nullptr, 10) != 0;
                systemPanelOpen = true;
                island::LogAndPrint("  diagnostic: the sound page is opened at startup\n");
                std::fflush(stdout);
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_SOUND_SCROLL", setting, 32) > 0) {
                soundPageScroll = wcstof(setting, nullptr);
            }
            // Synthesize ONE click at a screen point, after the bar has settled.
            //
            // This exists because the outside-click dismissal CANNOT be tested from a script on a machine
            // like this one: the bar runs at the shell's integrity level and a script's synthetic input is
            // sent from lower down, so Windows drops it before the bar's own GetAsyncKeyState can see it --
            // the test looks exactly like a broken feature. A click the BAR sends to itself is not filtered
            // that way, so this is the only way to check "a press outside the card puts it away" end to end.
            //
            // ISLANDBAR_CLICK_AT="x,y" (screen pixels), one shot, reported before and after.
            if (GetEnvironmentVariableW(L"ISLANDBAR_CLICK_AT", setting, 32) > 0) {
                wchar_t point[32] = {};
                GetEnvironmentVariableW(L"ISLANDBAR_CLICK_AT", point, 32);
                if (swscanf_s(point, L"%ld,%ld", &injectClick_, &injectClickY_) == 2) {
                    injectClickAtMs_ = GetTickCount64() + 3000;
                    island::LogAndPrint("  diagnostic: will click at (%ld,%ld) in 3 s\n", injectClick_,
                                        injectClickY_);
                    std::fflush(stdout);
                }
            }
            // Hold the panel -- and, if it is up, the sound page -- open whatever happens, so a photograph of
            // it cannot be undone by a stray click elsewhere on the machine. Debug only, and it says so where
            // it takes effect.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_LOCK", setting, 32) > 0) {
                panelLocked = wcstol(setting, nullptr, 10) != 0;
                lockedSoundPage = soundPageOpen;
                island::LogAndPrint("  diagnostic: the panel is held open (lock %d)\n",
                                    panelLocked ? 1 : 0);
                std::fflush(stdout);
            }
            // Separate from the opening time on purpose: with the transition stretched, closing at
            // twice the opening delay would cut the movement off in the middle of itself, and the
            // capture would show a reversal rather than the thing being measured.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_CLOSE_MS", setting, 32) > 0) {
                autoPanelCloseMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // Toggles the panel over and over, which is the only way to exercise the SECOND, THIRD and
            // fourth transitions: pressing the control again is what the request flag does, and the
            // defects that survived the first round lived entirely in the repeats.
            if (GetEnvironmentVariableW(L"ISLANDBAR_PANEL_CYCLE_MS", setting, 32) > 0) {
                autoPanelCycleMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // Presses the Start button on a timer. The one part of this program that has to inject input
            // into the SHELL, and therefore the one part that cannot be tried by hand from a script --
            // it needs a click on the bar. A timer gives it one, and the result is reported: "the
            // channel is blocked" and "the keystroke went and nothing happened" are the same picture.
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_MS", setting, 32) > 0) {
                autoStartMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_TRANSPORT_MS", setting, 32) > 0) {
                autoTransportMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_CYCLE_MS", setting, 32) > 0) {
                autoStartCycleMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // 1..5 chooses how the Start menu is asked for; see StartMenuRoute.
            if (GetEnvironmentVariableW(L"ISLANDBAR_START_ROUTE", setting, 32) > 0) {
                const long route = wcstol(setting, nullptr, 10);
                if (route >= 1 && route <= 5) {
                    island::SetStartMenuRoute(static_cast<island::StartMenuRoute>(route - 1));
                }
            }
            // Which icon counts as hovered. The icon row only exists for the pointer, and a script cannot
            // move the pointer here, so this is how a row with its last button opened can be photographed.
            if (GetEnvironmentVariableW(L"ISLANDBAR_HOVER_ICON", setting, 32) > 0) {
                forcedHoverIcon = static_cast<int>(wcstol(setting, nullptr, 10));
                window.SetForcedHoverIcon(forcedHoverIcon);
                island::LogAndPrint("  diagnostic: icon %d is treated as hovered\n", forcedHoverIcon);
                std::fflush(stdout);
            }
            // Treat the pointer as being on the bar. The hover is what opens the media card, and the
            // pointer cannot be moved from a script here, so this is how that card gets photographed.
            if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_INSIDE", setting, 32) > 0) {
                const int mode = static_cast<int>(wcstol(setting, nullptr, 10));
                window.SetForcedPointerInside(mode);
                island::LogAndPrint("  diagnostic: the pointer is treated as %ls\n",
                                    mode == 4   ? L"in the GAP between the bar and the card"
                                    : mode == 3 ? L"on the CARD"
                                    : mode == 2 ? L"on the BAR but off the island"
                                    : mode == 1 ? L"on the pill"
                                                : L"off");
                std::fflush(stdout);
            }
            // ... and as resting on a deck button AFTER N ms, for the rule that gives the card up when the
            // pointer moves off the pill. Timed rather than immediate because the case worth checking is the
            // TRANSITION -- a card that is already open being put away -- and at startup nothing is open yet.
            if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_DECK_MS", setting, 32) > 0) {
                autoDeckHoverMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
            }
            // The pointer flicking in and out, which is the gesture that interrupts a card's exit. A
            // script cannot do it here (its input is dropped before it reaches the bar), and the
            // interruption is the only way to get a frame where the shape is card-sized with no card up.
            if (GetEnvironmentVariableW(L"ISLANDBAR_INSIDE_FLICKER_MS", setting, 32) > 0) {
                autoInsideFlickerMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
                island::LogAndPrint("  diagnostic: the pointer flicks in and out every %lu ms\n",
                                    static_cast<unsigned long>(autoInsideFlickerMs));
                std::fflush(stdout);
            }
            // ... or onto the CARD, which is the same kind of check for the other direction: the pointer
            // walks from the pill down onto the card, and the card must stay for its buttons to be usable.
            if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_CARD_MS", setting, 32) > 0) {
                autoCardHoverMs = static_cast<DWORD>(wcstoul(setting, nullptr, 10));
                // Which half of the column to move to: 3 = the card itself (the default), 4 = the gap above
                // it. The gap is the half that matters for the walk from the pill, and it is the one the
                // first version of this missed.
                if (GetEnvironmentVariableW(L"ISLANDBAR_FORCE_CARD_MODE", setting, 32) > 0) {
                    autoCardMode = static_cast<int>(wcstol(setting, nullptr, 10));
                }
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_SLOWMO", setting, 32) > 0) {
                slowMotion = wcstof(setting, nullptr);
            }
            if (GetEnvironmentVariableW(L"ISLANDBAR_TRACE_CARD", setting, 32) > 0) {
                traceCardFrames = (wcstol(setting, nullptr, 10) != 0);
            }
        }
        if (slowMotion > 1.0f) {
            window.ScaleTransitionDurations(slowMotion);
            island::LogAndPrint("  diagnostic: transitions stretched %.0fx\n", slowMotion);
            std::fflush(stdout);
        }
        if (autoPanelMs > 0) {
            if (autoPanelCloseMs == 0) autoPanelCloseMs = autoPanelMs * 2;
            island::LogAndPrint("  diagnostic: the system panel opens at %lu ms, closes at %lu ms\n",
                                static_cast<unsigned long>(autoPanelMs),
                                static_cast<unsigned long>(autoPanelCloseMs));
            std::fflush(stdout);
        }
        if (autoStartMs > 0) {
            island::LogAndPrint("  diagnostic: the Start button is pressed at %lu ms and every %lu ms "
                                "after that, route = %ls\n",
                                static_cast<unsigned long>(autoStartMs),
                                static_cast<unsigned long>(autoStartCycleMs > 0 ? autoStartCycleMs
                                                                                : 2000),
                                island::StartMenuRouteName(island::CurrentStartMenuRoute()));
            std::fflush(stdout);
        }
        const DWORD diagnosticStart = GetTickCount();
        bool autoPanelOpened = false;
        bool autoPanelClosed = false;

        // --- lyrics state ---
        // A pointer into the provider, not a copy: Lyrics is ~132 KB.
        const island::Lyrics* currentLyrics = nullptr;
        unsigned long long lastLyricsRevision = lyrics.Revision();
        int  lastLyricIndex = -2;      // -2 forces the first evaluation
        float lyricsScrollDip = 0.0f;
    // Seconds since the line on screen changed, for its entrance (see LyricLineRollFor). Starts past
    // the end of the entrance so nothing fades until a real line change resets it.
    float lyricLineAge = 1.0f;
    // The line on screen and the one it replaced, copied rather than pointed at.
    //
    // The cross-fade lasts several frames, so the line being replaced has to survive past the frame that
    // changed it, and the provider's own text cannot be borrowed: a track change replaces that whole store.
    char  shownLyricText[island::kLyricLineCapacity] = {};
    char  replacedLyricText[island::kLyricLineCapacity] = {};
    float replacedLyricScrollDip = 0.0f;
    // The TRACK the lyric state above belongs to. The session's revision is not that question -- it
    // moves for a position update as well -- so the track is compared by name (see the music block).
    char  drawnTrackTitle[island::kMusicTitleCapacity] = {};
    char  drawnTrackArtist[island::kMusicArtistCapacity] = {};
        // Marquee speed. Slow enough to read a Chinese lyric line.
        constexpr float kLyricScrollDipPerSec = 26.0f;

        // Re-hiding the taskbar is checked on a slow cadence. The shell puts it
        // back on its own -- an Explorer restart, a display change, the Windows
        // key -- but nothing about that is urgent enough to poll at frame rate,
        // and each check is a handful of window queries.
        int taskbarCheckCountdown = 0;

        while (true) {
            if (!window.PumpMessages()) break;

            if (hideNativeTaskbar && haveTaskbar && --taskbarCheckCountdown <= 0) {
                taskbarCheckCountdown = 50;   // roughly every 5 s at an 8 ms tick
                if (taskbar.EnsureHidden()) {
                    island::LogAndPrint("  native taskbar: re-hidden (the shell put it back)\n");
                    std::fflush(stdout);
                }
            }

            // --- notifications ---
            //
            // Drain everything the listener thread has received. Each one opens the
            // card, so a burst ends with the last notification on screen rather than
            // flashing through all of them.
            if (listening) {
                island::Notification incoming;
                int drained = 0;
                while (notificationServer.Take(&incoming)) {
                    const island::Notification* stored = notifications.Push(incoming);
                    ++drained;
                    if (stored) {
                        // Wide, and converted here: the store is UTF-8 and the narrow overload would print
                        // every Chinese character as its bytes ("¹¹½¨Íê³É" for 构建完成), which reads like a
                        // corrupt message rather than like a logging bug.
                        wchar_t appW[island::kAppNameCapacity] = {};
                        wchar_t titleW[island::kTitleCapacity] = {};
                        MultiByteToWideChar(CP_UTF8, 0, stored->app, -1, appW,
                                            static_cast<int>(island::kAppNameCapacity));
                        MultiByteToWideChar(CP_UTF8, 0,
                                            stored->title[0] ? stored->title : stored->body, -1, titleW,
                                            static_cast<int>(island::kTitleCapacity));
                        island::LogAndPrint(L"  notification: [%ls] %ls\n",
                                            stored->app[0] ? appW : L"?", titleW);
                        std::fflush(stdout);
                    }
                }
                if (drained > 0) {
                    // NOT a card any more. A message widens the island and shows itself inside it (see
                    // NextShape's notice strip); the card is what the pointer opens when it goes to the island
                    // to read the whole thing. Forcing Expanded here -- which is what this did -- would fight
                    // that on every arrival: the strip would exist and never be seen.
                    window.SetState(island::IslandState::NoticeStrip);
                }
            }

            // ... and the same for the OTHER channel, on a slow timer.
            //
            // Two seconds rather than every frame because it is a database query, and the delay is invisible:
            // the shell's own toast appears at the same moment and the bar's card follows it. Nothing here
            // depends on the push channel and nothing in the push channel depends on this.
            if (toastsAvailable && GetTickCount64() - lastToastPoll >= 2000) {
                lastToastPoll = GetTickCount64();
                const int arrived = toasts.Poll(&notifications);
                if (arrived > 0) {
                    island::LogAndPrint("  toasts: %d new system toast(s), %d taken in total", arrived,
                                        toasts.Taken());
                    std::fflush(stdout);
                    // A message from another application deserves the same attention as one pushed here --
                    // which is the strip, not the card. See the push channel above.
                    window.SetState(island::IslandState::NoticeStrip);
                }
            }

            // ... and WeChat, which has no channel of its own and is recognised by its popup appearing.
            if (GetTickCount64() - lastWeChatPoll >= 500) {
                lastWeChatPoll = GetTickCount64();
                if (wechat.Poll(&notifications, GetTickCount64()) > 0) {
                    island::LogAndPrint("  wechat: %d message announcement(s) so far", wechat.Announced());
                    std::fflush(stdout);
                    window.SetState(island::IslandState::NoticeStrip);
                }
            }

            // ... and the SOUND channel, which is the one signal WeChat gives when its window does nothing.
            //
            // Its own clock (200 ms) rather than the window scan's: a notification sound is about a second
            // long, and a check every half second could read the session as active twice in a row and never
            // see the edge -- the sound would play with no announcement and no evidence anywhere.
            if (soundOutputStarted && soundOutput && GetTickCount64() - lastWeChatSoundPoll >= 200) {
                lastWeChatSoundPoll = GetTickCount64();
                soundOutput->Snapshot(soundSnapshot.get());

                // Is WeChat running, and is one of its audio sessions making sound?
                const bool wechatRunning = island::AnyWeChatProcessRunning();
                bool wechatSessionActive = false;
                int wechatSessions = 0;
                for (int i = 0; i < soundSnapshot->sessionCount; ++i) {
                    const island::SoundSessionInfo& session = soundSnapshot->sessions[i];
                    if (!island::IsWeChatProcess(session.pid)) continue;
                    ++wechatSessions;
                    if (session.active) wechatSessionActive = true;
                }
                if (wechat.PollSound(&notifications, wechatRunning, wechatSessionActive, wechatSessions,
                                     GetTickCount64()) > 0) {
                    window.SetState(island::IslandState::NoticeStrip);
                }
            }

            // Is something asking for attention right now? Computed before the
            // pointer logic so both branches agree on the same answer.
            const island::Notification* active =
                notifications.ActiveDisplay(GetTickCount64());

            // ... AND A CALL HOLDS THE ISLAND, even when it is no longer the newest thing in the store.
            //
            // A call is pushed as a STICKY item so the island keeps it for as long as the call lasts -- but
            // `ActiveDisplay` answers with the NEWEST item only, so a WeChat message arriving during the call
            // took the island, expired six seconds later, and the call was never shown again for the rest of
            // the call. Reported as "微信通话 不显示了", and the log says exactly that:
            //
            //   13:37:22.307  wechat: call announced as '通话中' (notification 9)
            //   13:37:23.232  wechat: announced '有新消息' from its notification sound
            //   13:37:29.394  state: music-strip            <- the call is still up; the island is not
            //
            // The rule is the one the reference uses: a FRESH message wins (a call is not a reason to hide
            // what someone just sent), and the call comes back as soon as the message is done. Reading it --
            // a click on the island -- retires it for good, which is what "read" means everywhere else here.
            if (active == nullptr && wechat.CallNotificationId() != 0) {
                const island::Notification* call = notifications.Find(wechat.CallNotificationId());
                if (call != nullptr && !call->read) active = call;
            }

            // --- what is playing ---
            if (musicAvailable) {
                if (music.Revision() != lastMusicRevision) {
                    lastMusicRevision = music.Revision();
                    island::MusicInfo latest;
                    music.Snapshot(&latest);
                    nowPlaying = latest;
                    // The moment this position was read, so it can be projected forward.
                    musicPositionAt = GetTickCount64();

                    // A track change invalidates the lyrics and their scroll position.
                    //
                    // AND ONLY A TRACK CHANGE. This block runs whenever the session's revision moves,
                    // which a position update does too (once every second or two) -- so resetting here
                    // re-announced the same line as if it had changed, restarted a long line's marquee
                    // and replayed the roll with the same words on both sides: "歌词每1、2秒就会上下出现
                    // 一模一样的歌词然后和在一起似的". The comparison is on the track, not on the
                    // revision.
                    const bool newTrack =
                        std::strcmp(drawnTrackTitle, nowPlaying.title) != 0 ||
                        std::strcmp(drawnTrackArtist, nowPlaying.artist) != 0;
                    strncpy_s(drawnTrackTitle, sizeof(drawnTrackTitle), nowPlaying.title, _TRUNCATE);
                    strncpy_s(drawnTrackArtist, sizeof(drawnTrackArtist), nowPlaying.artist, _TRUNCATE);
                    if (newTrack) {
                        lyricsScrollDip = 0.0f;
                        lastLyricIndex = -2;
                    }
                    if (nowPlaying.present && nowPlaying.title[0] != '\0') {
                        lyrics.Request(nowPlaying.title, nowPlaying.artist);
                    }

                    island::LogAndPrint("  music: [%s] %s - %s (%s)",
                                        nowPlaying.app[0] ? nowPlaying.app : "?",
                                        nowPlaying.artist[0] ? nowPlaying.artist : "?",
                                        nowPlaying.title[0] ? nowPlaying.title : "?",
                                        island::PlaybackStateName(nowPlaying.state));
                    std::fflush(stdout);

                    // ... AND WHETHER THERE IS A COVER, once per track.
                    //
                    // Asked in as many words -- "现在音乐获取 封面 能获取到不" -- and there was no line anywhere
                    // that answered it: the card either had a cover or it did not, and the only way to tell was
                    // to photograph it. The bytes below are what the MEDIA SESSION published, so this one line
                    // separates the three cases that all look alike on screen: the player published nothing
                    // (`gsmtc_probe` says `Thumbnail: none`), it published something this bar could not decode,
                    // and it published a cover that is on the card right now.
                    if (newTrack) {
                        island::LogAndPrint("  music: cover %s (%zu bytes, revision %llu)\n",
                                            nowPlaying.artworkSize > 0 ? "present" : "NONE",
                                            nowPlaying.artworkSize, nowPlaying.artworkRevision);
                        std::fflush(stdout);
                    }
                }

                // Lyrics arrive later than the track does, so a fetch completing is
                // its own reason to redraw.
                if (lyrics.Revision() != lastLyricsRevision) {
                    lastLyricsRevision = lyrics.Revision();
                    currentLyrics = lyrics.Current();
                    lastLyricIndex = -2;
                    lyricsScrollDip = 0.0f;
                    island::LogAndPrint("  lyrics: %s (%d lines)",
                                        currentLyrics ? "loaded" : "none",
                                        currentLyrics ? currentLyrics->count : 0);
                    std::fflush(stdout);
                }
            }

            // --- a full-screen application gets the whole screen ----------------
            //
            // The shell tells us when a full-screen window comes to the front. It has done so since the
            // AppBar was written and NOTHING EVER LISTENED: the accessor existed with no callers.
            //
            // What can be done about it is limited by the desktop, not by this program. A screen edge
            // holds ONE AppBar, and the shell's own taskbar is a registered AppBar even while hidden --
            // it keeps its claim on whatever edge it is docked to. So the desktop work area stays inset
            // by its thickness, a maximized window keeps an unusable band above it, and claiming that
            // edge for ourselves is refused for the same reason.
            //
            // The bar therefore FILLS the band rather than fighting for it: while an application is
            // full-screen the shape becomes the full-width strip (see IslandState::FullBar), which is
            // exactly as tall as the reservation, so the band is covered and nothing is wasted.
            //
            // The reservation is released meanwhile, because holding it would be reserving space for a
            // bar that is not in its usual place.
            {
                const bool fullscreenNow = appBar.IsFullscreenAppInFront();
                if (fullscreenNow != fullscreenLast) {
                    fullscreenLast = fullscreenNow;

                    if (fullscreenNow) {
                        if (appBar.IsRegistered()) {
                            appBar.Release();
                            island::LogAndPrint("  fullscreen: space released; the bar fills the band");
                        } else {
                            island::LogAndPrint("  fullscreen: the bar fills the band");
                        }
                    } else if (appBarResult == island::AppBarResult::Registered) {
                        // Reclaimed only if it had been claimed: `--keep-taskbar` never claims, and
                        // claiming on the way out of full-screen would reserve space never asked for.
                        appBarResult =
                            appBar.Claim(window.Handle(), island::DockEdge::Top, barThicknessPx);
                        island::LogAndPrint("  fullscreen: over, space re-claimed -- %ls",
                                            island::AppBarResultName(appBarResult));
                    } else {
                        island::LogAndPrint("  fullscreen: over, back to the pill");
                    }
                    std::fflush(stdout);
                }
            }

            // --- the resting state ---
            //
            // A MEDIA SESSION -- playing or PAUSED -- means the lyric strip; no session at all means the
            // plain pill. Named once here so the click handling, the pointer handling and the renderer
            // cannot disagree about where "rest" is.
            //
            // It used to require PLAYING, so pausing swapped the lyric for the clock. Asked for in these
            // words: "暂停音乐后灵动岛不显示歌词了 应该显示" -- the words are what the strip is for, and a
            // paused player still has them.
            const bool musicResting =
                nowPlaying.present && nowPlaying.title[0] != '\0';
            const island::IslandState resting =
                musicResting ? island::IslandState::MusicStrip : island::IslandState::Collapsed;

            // Puts the panel away -- and puts the SHAPE away with it.
            //
            // Two things, and leaving the second out is what the user saw as "音乐卡片会闪一下". Closing the
            // panel withdraws the REQUEST, but the shape was still on the card: the pointer is over the bar
            // area and music is playing, so nothing in the policy pulled it back down. The card's owner is
            // then chosen afresh (a card is still wanted, the panel is no longer asking) and the answer is
            // the MEDIA card -- which appeared in the panel's place and stayed there for as long as the
            // pointer did.
            //
            // With the shape told to go back to rest, cardWanted is false from that moment on, so the owner
            // is KEPT and the panel is drawn all the way out (see NextCardOwner): the card retracts, and
            // nothing takes its place.
            const auto dismissPanel = [&]() {
                systemPanelOpen = false;
                soundPageOpen = false;
                panelSliderDrag = false;
                mixerSliderDragRow = -1;
                mixerSliderDragPid = 0;
                window.SetState(resting);
            };
            // A click, routed to whatever it landed on. The icon row is checked FIRST because the
            // icons are not the island: a click on one asks to switch to that window, and must not
            // also be read as an acknowledgement of the notifications or as a transport control.
            // A while loop, not an if: clicks are counted now, and two can arrive inside one
            // frame. With a flag the second was silently dropped.
            //
            // The volume slider's DRAG is followed here rather than in the click handler, because the
            // movement is not an event: a click arrives once, and the handle has to keep up with the
            // pointer for as long as the button is held.
            //
            // The value is only pushed when the FRACTION changes. Every frame at 60 Hz would be sixty COM
            // writes a second to the audio endpoint, each of which reopens the device on the worker -- the
            // handle would still follow the pointer, but the level would arrive in a lagging stream behind
            // it, which is half of what "不顺滑" meant.
            if (panelSliderDrag) {
                POINT dragPoint = {};
                GetCursorPos(&dragPoint);
                // From the pointer's x alone, not from the hit test: see PanelSliderFractionForDrag.
                float fraction = 0.0f;
                if (volumeAvailable &&
                    window.PanelSliderFractionForDrag(dragPoint, &fraction)) {
                    if (std::fabs(fraction - lastAppliedVolumeFraction) > 0.004f) {
                        lastAppliedVolumeFraction = fraction;
                        volume.SetLevel(fraction);
                        // Reported per real step rather than per frame, so "the handle follows the pointer"
                        // and "it only moved twice" can be told apart in the log.
                        if (std::fabs(fraction - lastLoggedVolumeFraction) >= 0.03f) {
                            lastLoggedVolumeFraction = fraction;
                            island::LogAndPrint("  volume: dragging -> %.0f%%\n",
                                                fraction * 100.0f);
                            std::fflush(stdout);
                        }
                    }
                    panelVolumeEchoUntil = GetTickCount64() + 1200;
                }
                if (!window.LeftButtonDown()) {
                    panelSliderDrag = false;
                    float finalLevel = 0.0f;
                    if (volume.Get(&finalLevel)) {
                        island::LogAndPrint("  volume: drag ended at %.0f%%\n",
                                            finalLevel * 100.0f);
                    } else {
                        island::LogAndPrint("  volume: drag ended\n");
                    }
                    std::fflush(stdout);
                }
            }

            // The mixer's sliders are dragged the same way the panel's volume slider is, and for the same
            // reason: a press sets the level, and HOLDING it has to keep the handle under the pointer.
            if (mixerSliderDragRow >= 0) {
                POINT dragPoint = {};
                GetCursorPos(&dragPoint);
                float fraction = 0.0f;
                if (window.MixerSliderFractionForDrag(mixerSliderDragRow, dragPoint, &fraction) &&
                    std::fabs(fraction - lastAppliedMixerFraction) > 0.004f) {
                    lastAppliedMixerFraction = fraction;
                    if (mixerSliderDragRow == 0) {
                        if (volumeAvailable) volume.SetLevel(fraction);
                    } else if (soundOutputStarted && soundOutput) {
                        soundOutput->SetSessionVolume(mixerSliderDragPid, fraction);
                    }
                }
                if (!window.LeftButtonDown()) {
                    mixerSliderDragRow = -1;
                    mixerSliderDragPid = 0;
                    island::LogAndPrint("  mixer: drag ended\n");
                    std::fflush(stdout);
                }
            }

            // --- the wheel, for the one list that scrolls ---
            //
            // The window counts the notches and hands them over; the OFFSET belongs here, where the page is
            // built and where the same value goes out to the drawing and the hit test. The window clamps it
            // (it is the one that knows how many rows there are) and the clamped value is copied straight
            // back, so there is still only one offset in play.
            {
                int wheelNotches = 0;
                if (soundPageOpen && window.TakeSoundWheel(&wheelNotches)) {
                    soundPageScroll -= static_cast<float>(wheelNotches) * 60.0f;
                    window.SetSoundPageScroll(soundPageScroll);
                    soundPageScroll = window.SoundPageScroll();
                    island::LogAndPrint("  sound page: wheel %d -> scroll %.0f\n", wheelNotches,
                                        soundPageScroll);
                    std::fflush(stdout);
                }
            }

            // --- a press on a slider, which is where a drag BEGINS ---
            //
            // Handled before the clicks, because a press is the earlier event of the two and the value has to
            // be under the pointer from the moment the button goes down. The release that follows is then
            // only the END of the gesture: see the top of the click loop below.
            {
                POINT pressed = {};
                while (window.TakePress(&pressed)) {
                    if (!systemPanelOpen) continue;
                    if (soundPageOpen) {
                        const island::SoundPageHit hit = window.SoundPageHitAtScreen(pressed);
                        if (hit.target == island::SoundPageTarget::MixerSlider) {
                            mixerSliderDragRow = hit.index;
                            lastAppliedMixerFraction = hit.fraction;
                            if (hit.index == 0) {
                                if (volumeAvailable) volume.SetLevel(hit.fraction);
                            } else if (soundOutputStarted && soundOutput &&
                                       hit.index - 1 < soundSnapshot->sessionCount) {
                                mixerSliderDragPid = soundSnapshot->sessions[hit.index - 1].pid;
                                soundOutput->SetSessionVolume(mixerSliderDragPid, hit.fraction);
                            }
                            island::LogAndPrint("  mixer: press on row %d -> %.0f%%\n", hit.index,
                                                hit.fraction * 100.0f);
                            std::fflush(stdout);
                        }
                    } else {
                        const island::SystemPanelHit hit = window.SystemPanelHitAtScreen(pressed);
                        if (hit.target == island::SystemPanelTarget::Slider && volumeAvailable) {
                            sliderPressHandled = true;
                            panelSliderDrag = true;
                            lastAppliedVolumeFraction = hit.fraction;
                            volume.SetLevel(hit.fraction);
                            panelVolumeEchoUntil = GetTickCount64() + 1200;
                            island::LogAndPrint("  volume: press on the slider -> %.0f%%\n",
                                                hit.fraction * 100.0f);
                            std::fflush(stdout);
                        }
                    }
                }
            }

            while (window.TakeClick()) {
                POINT cursor = {};
                GetCursorPos(&cursor);

                // A release that ENDS a drag is not a click on whatever happens to be under the pointer. The
                // value was already set by the press and by every frame of the drag in between; letting the
                // click run as well would be a second handler for one gesture, and on the mixer it would
                // re-apply the value to whichever row the pointer finished over.
                if (panelSliderDrag || mixerSliderDragRow >= 0) {
                    const bool wasMixer = mixerSliderDragRow >= 0;
                    panelSliderDrag = false;
                    mixerSliderDragRow = -1;
                    mixerSliderDragPid = 0;
                    island::LogAndPrint("  %ls: drag ended by the release\n",
                                        wasMixer ? L"mixer" : L"volume");
                    std::fflush(stdout);
                    continue;
                }

                // What the panel's own controls are under the click, asked ONCE.
                //
                // The chain below asks this same question for every kind of target it knows, and each of
                // those calls re-lays the panel out to answer it. One value, read once, so the answers cannot
                // disagree with each other within a single click either.
                const island::SystemPanelHit panelHit =
                    systemPanelOpen ? window.SystemPanelHitAtScreen(cursor) : island::SystemPanelHit{};

                // What the click was shown to mean, before anything acts on it: which level is up, what the
                // panel's own hit test answered, and what shape the window is in. Without this, "the tile did
                // nothing" and "the click was routed somewhere else entirely" look the same from outside.
                // The WIDE overload, and it matters: the narrow one byte-widens its output, which turns any
                // non-ASCII argument into an invalid conversion -- and an invalid conversion makes the whole
                // line EMPTY while still writing the newline. That is why "the tile did nothing" was the
                // report for three rounds: the tile's own log line was being swallowed, and an empty line in
                // the log looks like nothing happened.
                island::LogAndPrint(L"  click: panel=%d soundPage=%d target=%d shape=%ls\n",
                                    systemPanelOpen ? 1 : 0, soundPageOpen ? 1 : 0,
                                    static_cast<int>(panelHit.target),
                                    island::StateName(window.State()));

                // Nothing about this can be concluded from a screenshot, so it reports what it
                // decided: which icon was hit, which window that was, and whether the activation
                // succeeded. A click that never arrives and a click that arrives and is ignored look
                // identical from outside, and the two need opposite fixes -- that is why this exists.
                // Bounded by the number of icons actually DRAWN, not by the number of windows in the
                // list. `IconAtScreen` answers from the same count the window was given, so it cannot
                // return more than that anyway -- but the list can hold windows the row had no room
                // for, and indexing straight into it would switch to a window with no icon on screen.
                const int icon = window.IconAtScreen(cursor);
                island::LogAndPrint("  click: icon under the pointer = %d\n", icon);

                // The START BUTTON first, because it is on the far side of the island from the icons
                // and a click on it must not be read as anything else.
                //
                // Its outcome is reported rather than assumed: asking for the Start menu can fail in
                // ways that leave no trace afterwards, and "the button did nothing" is the same symptom
                // for every one of them.
                if (window.StartAtScreen(cursor)) {
                    const island::StartMenuResult startResult = island::ToggleStartMenu();
                    island::LogAndPrint("  start click: %ls", island::StartMenuResultName(startResult));

                    // A keystroke the shell will not act on has a reason, and the one that can be read from
                    // here is the common one on this machine: UIPI discards synthetic input aimed at a
                    // process of a HIGHER integrity level, and the Explorer that answers Ctrl+Esc sits
                    // above a bar started from a restricted launcher, an IDE, or a terminal opened by
                    // another application. Reported at the moment it matters rather than only in the
                    // startup banner, because the symptom is a button that does nothing, with no error
                    // anywhere to explain it.
                    taskbar.Locate();
                    if (taskbar.IsBlockedByIntegrity()) {
                        island::LogAndPrint(
                            "  start click: this process is BELOW the shell's integrity level, and the shell "
                            "discards synthetic input from below it. Start the bar from your own shell "
                            "(Win+R, or a shortcut), not from a sandbox, an IDE, or a terminal opened by "
                            "another application.\n");
                    }
                    std::fflush(stdout);
                } else if (window.SystemButtonAtScreen(cursor)) {
                    // The system panel is a TOGGLE, like the shell's own flyout: pressing the control
                    // again closes what it opened. Without that the only way out would be to click
                    // somewhere unrelated, which is not a way out a user can guess.
                    systemPanelOpen = !systemPanelOpen;
                    systemPanelPage = 0;   // always opens on the first page, like the shell's flyout
                    // The page is a LEVEL of the panel, so closing the panel closes it too -- and a drag
                    // left half-done is ended, or the next press anywhere would go on dragging a slider
                    // that is no longer on screen.
                    soundPageOpen = false;
                    mixerSliderDragRow = -1;
                    mixerSliderDragPid = 0;
                    island::LogAndPrint("  system panel: %ls\n",
                                        systemPanelOpen ? L"open" : L"closed");
                    std::fflush(stdout);
                } else if (systemPanelOpen && soundPageOpen) {
                    // --- the 声音输出 page's own controls ---
                    //
                    // Reached BEFORE the panel's, because the two are laid out over the same card: while this
                    // level is showing, a click at the position of a tile is a click on a device row, and
                    // asking the panel first would fire the wrong action entirely.
                    const island::SoundPageHit soundHit = window.SoundPageHitAtScreen(cursor);
                    switch (soundHit.target) {
                        case island::SoundPageTarget::Back:
                            soundPageOpen = false;
                            island::LogAndPrint("  sound page: back to the panel\n");
                            break;

                        case island::SoundPageTarget::Device:
                            if (soundHit.index >= 0 &&
                                soundHit.index < soundSnapshot->deviceCount) {
                                const island::SoundDeviceInfo& wanted =
                                    soundSnapshot->devices[soundHit.index];
                                if (wanted.isDefault) {
                                    island::LogAndPrint(
                                        L"  sound page: device %d '%ls' is already the default\n",
                                        soundHit.index, wanted.name);
                                } else {
                                    // Handed to the worker: switching the default goes through an interface
                                    // that has no public documentation (see SoundOutput), and the worker's
                                    // own log line is what says whether it took.
                                    soundOutput->SetDefaultDevice(wanted.id);
                                    // And a fresh reading asked for at once, rather than up to a second of
                                    // showing the old device as the current one.
                                    soundOutput->Refresh();
                                    island::LogAndPrint(
                                        L"  sound page: switching the default device to %d '%ls'\n",
                                        soundHit.index, wanted.name);
                                }
                            }
                            break;

                        case island::SoundPageTarget::Spatial:
                            // Reported rather than pretended: there is no public interface for reading or
                            // writing the spatial-sound setting, so the page shows the shell's two choices
                            // and says so when one is pressed.
                            island::LogAndPrint(
                                "  sound page: spatial sound row %d -- no public interface to change it;"
                                " open 更多音量设置 for this one\n",
                                soundHit.index);
                            break;

                        case island::SoundPageTarget::MixerSlider: {
                            const int row = soundHit.index;
                            if (row == 0) {
                                // The device's own row is the master volume, which is AudioVolume's.
                                if (volumeAvailable) {
                                    volume.SetLevel(soundHit.fraction);
                                    mixerSliderDragRow = 0;
                                    mixerSliderDragPid = 0;
                                    island::LogAndPrint("  mixer: master -> %.0f%%\n",
                                                        soundHit.fraction * 100.0f);
                                }
                            } else if (row - 1 < soundSnapshot->sessionCount) {
                                const island::SoundSessionInfo& session =
                                    soundSnapshot->sessions[row - 1];
                                soundOutput->SetSessionVolume(session.pid, soundHit.fraction);
                                mixerSliderDragRow = row;
                                mixerSliderDragPid = session.pid;
                                island::LogAndPrint(L"  mixer: '%ls' (pid %lu) -> %.0f%%\n",
                                                    session.name, session.pid,
                                                    soundHit.fraction * 100.0f);
                            }
                            std::fflush(stdout);
                            break;
                        }

                        case island::SoundPageTarget::MixerGear:
                        case island::SoundPageTarget::Footer: {
                            // 更多音量设置, and the mixer's own gear: both are the settings page, which is
                            // where the parts of this page that have no interface of their own live.
                            const HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                                                                   nullptr, nullptr, SW_SHOWNORMAL);
                            island::LogAndPrint(
                                "  sound page: %ls -> ms-settings:sound (result %lld)\n",
                                soundHit.target == island::SoundPageTarget::Footer
                                    ? L"more volume settings"
                                    : L"mixer gear",
                                static_cast<long long>(reinterpret_cast<INT_PTR>(opened)));
                            // The settings page is opening, so the card steps aside -- the same rule the
                            // panel's tiles follow.
                            dismissPanel();
                            break;
                        }

                        case island::SoundPageTarget::Scrollbar: {
                            // A press on the track moves the page a screenful towards where it was pressed,
                            // which is what the shell's own scrollbar does. The window says WHICH WAY -- it is
                            // the one that knows where the thumb is, from the same layout the press was
                            // hit-tested against -- and the OFFSET stays here with the page it belongs to.
                            const int jump = window.SoundScrollJumpFor(cursor);
                            if (jump != 0) {
                                soundPageScroll += static_cast<float>(jump) * 140.0f;
                                window.SetSoundPageScroll(soundPageScroll);
                                soundPageScroll = window.SoundPageScroll();
                                island::LogAndPrint("  sound page: scroll -> %.0f\n", soundPageScroll);
                            }
                            break;
                        }

                        case island::SoundPageTarget::None:
                        default:
                            // Anything else on the page -- the padding between rows, a section label -- is
                            // the page itself, and pressing it means nothing rather than "close".
                            break;
                    }
                    std::fflush(stdout);
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::PageDown) {
                    const int pages = window.SystemPanel().pageCount;
                    if (pages > 1) {
                        systemPanelPage = (window.SystemPanel().pageIndex + 1) % pages;
                        island::LogAndPrint("  system panel: page %d of %d\n", systemPanelPage + 1,
                                            pages);
                        std::fflush(stdout);
                    }
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::PageUp) {
                    const int pages = window.SystemPanel().pageCount;
                    if (pages > 1) {
                        systemPanelPage = (window.SystemPanel().pageIndex + pages - 1) % pages;
                        island::LogAndPrint("  system panel: page %d of %d\n", systemPanelPage + 1,
                                            pages);
                        std::fflush(stdout);
                    }
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::Slider) {
                    // Already handled by the press that this release ends. Without this the same gesture
                    // would be applied twice -- once when the button went down and once when it came up --
                    // and the log would show a second "drag begins" for a drag that had just finished.
                    if (sliderPressHandled) {
                        sliderPressHandled = false;
                    } else if (volumeAvailable) {
                        // Pressed without a press event having been seen -- a click that arrived on its own,
                        // which happens when the panel opened between the two. The fraction comes from the
                        // same layout the bar was drawn from, so the handle goes exactly where the click was.
                        volume.SetLevel(panelHit.fraction);
                        panelSliderDrag = true;   // and keep following the pointer while the button is held
                        panelVolumeEchoUntil = GetTickCount64() + 1200;
                        island::LogAndPrint("  volume: set to %.0f%% (drag begins)\n",
                                            panelHit.fraction * 100.0f);
                        std::fflush(stdout);
                    }
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::Speaker) {
                    // The speaker glyph is the mute control, as it is in the shell's own flyout.
                    if (volumeAvailable) {
                        volume.ToggleMute();
                        panelVolumeEchoUntil = GetTickCount64() + 1200;
                        island::LogAndPrint("  volume: %ls\n",
                                            volume.IsMuted() ? L"muted" : L"unmuted");
                        std::fflush(stdout);
                    }
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::VolumeDevice) {
                    // The output-device button: the shell's own second level, where the devices and the
                    // per-application mixer live.
                    soundPageOpen = true;
                    soundPageScroll = 0.0f;
                    island::LogAndPrint("  sound page: open\n");
                    std::fflush(stdout);
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::Gear) {
                    // The settings button, which has been drawn since the panel was built and has never had an
                    // action. It opens the sound settings, which is where this panel's own contents live.
                    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                                                           nullptr, nullptr, SW_SHOWNORMAL);
                    island::LogAndPrint("  system panel: gear -> ms-settings:sound (result %lld)\n",
                                        static_cast<long long>(reinterpret_cast<INT_PTR>(opened)));
                    // Same rule as the tiles that take the user elsewhere: the page they asked for is opening,
                    // so the panel gets out of the way.
                    dismissPanel();
                    std::fflush(stdout);
                } else if (systemPanelOpen && panelHit.target ==
                                                  island::SystemPanelTarget::Tile) {
                    // A tile was pressed. What it does is carried ON the tile, so this does not have to
                    // know which tile is which -- and renaming one cannot silently change its action.
                    //
                    // Read from the WINDOW rather than from a frame, because the click handler runs
                    // before this frame's state exists: the frame is filled further down the loop.
                    const int tileIndex = panelHit.index;
                    const island::TextRenderer::SystemPanelText& panel = window.SystemPanel();
                    if (tileIndex >= 0 && tileIndex < panel.tileCount) {
                        const island::TextRenderer::SystemPanelTile& tile = panel.tiles[tileIndex];
                        island::LogAndPrint(L"  click: tile %d '%ls' action=%d chevron=%d\n", tileIndex,
                                            tile.label ? tile.label : L"?",
                                            static_cast<int>(tile.action), tile.chevron ? 1 : 0);
                        switch (tile.action) {
                            case island::TextRenderer::PanelAction::OpenSettings: {
                                // ShellExecute with the ms-settings URI, which is how the shell's own
                                // flyout opens these pages. Reported with its result because "the tile
                                // did nothing" and "the page failed to open" look identical otherwise.
                                // A result above 32 means success; the numeric value is kept because
                                // the failure codes are what would be needed to diagnose it.
                                const HINSTANCE opened =
                                    tile.uri ? ShellExecuteW(nullptr, L"open", tile.uri, nullptr,
                                                             nullptr, SW_SHOWNORMAL)
                                             : nullptr;
                                island::LogAndPrint(L"  tile %d '%ls' -> %ls (result %lld)\n",
                                                    tileIndex, tile.label ? tile.label : L"?",
                                                    tile.uri ? tile.uri : L"no uri",
                                                    static_cast<long long>(
                                                        reinterpret_cast<INT_PTR>(opened)));
                                // ... and the panel puts itself away.
                                //
                                // This tile's whole job was to take the user somewhere else, and leaving the
                                // card up would leave them looking at a flyout for a page they are no longer
                                // on -- which is what "点击应该…自动收回去" asked for. The tiles that apply
                                // their change HERE (the volume row, and any toggle) stay open instead, so
                                // their new state is there to be seen and several can be flipped in a row.
                                dismissPanel();
                                break;
                            }
                            case island::TextRenderer::PanelAction::ToggleMute:
                            case island::TextRenderer::PanelAction::None:
                            default:
                                island::LogAndPrint(L"  tile %d '%ls': no action yet\n", tileIndex,
                                                    tile.label ? tile.label : L"?");
                                break;

                            case island::TextRenderer::PanelAction::OpenSoundPage:
                                // A tile that opens the sound page rather than a settings URI. None of them do
                                // today -- the page is reached from the volume row's device button -- but the
                                // action exists, so it is handled rather than falling into "no action yet".
                                soundPageOpen = true;
                                soundPageScroll = 0.0f;
                                island::LogAndPrint(L"  tile %d '%ls' -> sound page\n", tileIndex,
                                                    tile.label ? tile.label : L"?");
                                break;

                            case island::TextRenderer::PanelAction::ToggleRadio: {
                                // The Wi-Fi radio, switched here and now. The state is READ BACK from the
                                // WLAN API on the next sample rather than flipped locally, so a switch the
                                // machine refuses (a hardware switch, a managed adapter) shows as what it
                                // really is instead of as what was asked for.
                                unsigned long radioError = 0;
                                const bool wanted = !systemStatus.wifiRadioOn;
                                const bool switched = island::SetWifiRadioOn(wanted, &radioError);
                                island::LogAndPrint(
                                    L"  tile %d '%ls' -> Wi-Fi radio %ls (result %d, error %lu;"
                                    L" Bluetooth is NOT switched -- no documented interface)\n",
                                    tileIndex, tile.label ? tile.label : L"?",
                                    wanted ? L"on" : L"off", switched ? 1 : 0, radioError);
                                // The reading behind the tiles is a two-second sample, and waiting for it
                                // would leave the tile showing the old state right after the press that
                                // changed it.
                                statusReader.Sample(&systemStatus);
                                lastStatusSampleMs = GetTickCount64();
                                // Deliberately NOT closed: a toggle applies its change here, and its new
                                // state is the thing to look at. Several can be flipped in a row.
                                break;
                            }

                            case island::TextRenderer::PanelAction::ProjectionModes: {
                                // 投影, which the shell answers by sending Win+P. Synthetic input, so it is
                                // dropped by UIPI when this process sits below the shell -- the same caveat
                                // the Start button carries, and the reason the outcome is reported.
                                island::SendWindowsKeyChord('P');
                                island::LogAndPrint(L"  tile %d '%ls' -> Win+P (projection modes)\n",
                                                    tileIndex, tile.label ? tile.label : L"?");
                                // The projection list is the shell's own flyout and appears over this one:
                                // staying open would put two flyouts on top of each other.
                                dismissPanel();
                                break;
                            }
                        }
                        std::fflush(stdout);
                    }
                } else if (systemPanelOpen) {
                    // Anything else while the panel is open is treated as "put it away".
                    //
                    // A flyout closes when you click outside it, and on this bar the island IS outside
                    // it. Without this the island's own click was still handled as a request for the
                    // card, so pressing it opened a SECOND card-style surface while the panel was up --
                    // reported as "clicking this one opened it too".
                    //
                    // Closing on any other click is also the behaviour a user expects, and it is the only
                    // way out that does not require finding the chevron again.
                    dismissPanel();
                    island::LogAndPrint("  system panel: closed (click elsewhere)\n");
                    std::fflush(stdout);
                } else if (icon >= 0 && icon < taskWindowsShown) {
                    // The handle that was DRAWN at this position, not whatever the live list holds now.
                    const HWND target = taskHandles[icon];
                    island::LogAndPrint("  icon click: index %d -> %p", icon,
                                        reinterpret_cast<void*>(target));

                    // The taskbar button is a TOGGLE: the window already in front is put away, any
                    // other comes forward. Without this the bar could only ever raise things, which is
                    // half the gesture -- reported as "clicking an application on the desktop does not
                    // minimise it".
                    //
                    // Asked LIVE rather than from the list's cached foreground, because the cache is
                    // refreshed twice a second and the whole question is which window is in front at
                    // the instant of the click.
                    const bool alreadyInFront = (GetForegroundWindow() == target);

                    if (alreadyInFront) {
                        if (taskWindows.Minimize(target)) {
                            island::LogAndPrint("  icon click: minimised %p (it was already in front)",
                                                reinterpret_cast<void*>(target));
                        } else {
                            island::LogAndPrint("  icon click: minimise FAILED for %p",
                                                reinterpret_cast<void*>(target));
                        }
                    } else if (taskWindows.Activate(target)) {
                        island::LogAndPrint("  icon click: activated %p",
                                            reinterpret_cast<void*>(target));
                    } else {
                        // Not reported as success. A minimised window that stayed minimised, or a
                        // refused SetForegroundWindow, both land here -- and reporting "activated"
                        // regardless would make the next investigation start from a false fact.
                        island::LogAndPrint("  icon click: activation FAILED for %p",
                                            reinterpret_cast<void*>(target));
                    }
                    std::fflush(stdout);
                } else {
                    // A click that is not on an icon, reported with the numbers that decided it.
                    //
                    // "The click was never routed" and "it was routed and the hit test missed" are
                    // different faults and the log otherwise shows neither. This is the line that
                    // separates them.
                    island::LogAndPrint("  icon click: no icon (index %d, %d shown)", icon,
                                        taskWindowsShown);
                    std::fflush(stdout);

                const island::CardControl control = window.MusicControlAtScreen(cursor);
                // Every click reports what it was taken to mean.
                //
                // "The card does not respond" and "the click never reached the process" look the
                // same from outside, and the second one cannot even be logged from in here -- so
                // this line's presence or absence is itself the answer.
                {
                    const char* what = "dismiss";
                    if (control == island::CardControl::Previous) {
                        what = "previous";
                    } else if (control == island::CardControl::PlayPause) {
                        what = "play/pause";
                    } else if (control == island::CardControl::Next) {
                        what = "next";
                    }
                    island::LogAndPrint("  click routed: state=%ls, meaning=%s",
                                        island::StateName(window.State()), what);
                    std::fflush(stdout);
                }
                if (control != island::CardControl::None) {
                    island::MusicSession::Transport action =
                        island::MusicSession::Transport::PlayPause;
                    const char* name = "play/pause";
                    if (control == island::CardControl::Previous) {
                        action = island::MusicSession::Transport::Previous;
                        name = "previous";
                    } else if (control == island::CardControl::Next) {
                        action = island::MusicSession::Transport::Next;
                        name = "next";
                    }
                    if (music.RequestTransport(action)) {
                        island::LogAndPrint("  music: %s requested", name);
                        std::fflush(stdout);
                    }
                } else {
                    // A CLICK DISMISSES MESSAGES, NOT A CALL.
                    //
                    // Every other item on the strip is news: showing it for a few seconds and marking it read
                    // when it is clicked is what "read" means. A CALL is a STATE, and the user asked for it to
                    // stay on the island for as long as it lasts ("微信通话 也应该在灵动岛 上") -- so a click
                    // must leave it alone. It did not, and that is one of the two reasons the call strip
                    // disappeared for good: `notifications: 1 marked read (click)` at 13:43:19 with the call
                    // still up, after which the island never showed it again. The call is retired by the call
                    // ENDING, which the watcher detects and reports.
                    const int marked = notifications.MarkAllReadExcept(wechat.CallNotificationId());
                    if (marked > 0) {
                        island::LogAndPrint("  notifications: %d marked read (click)", marked);
                        std::fflush(stdout);
                        // Recompute immediately so the same frame reflects the click
                        // instead of waiting for the next tick to notice.
                        window.SetState(window.IsPointerInside()
                                            ? island::IslandState::Hover
                                            : resting);
                    }
                }
                }
            }

            // Held open for a photograph, if the diagnostic asked for it. Re-asserted AFTER the clicks above,
            // which is the only place either flag is cleared -- so the next frame draws the same level
            // whatever happened to the mouse in between.
            if (panelLocked) {
                systemPanelOpen = true;
                // The page only rides along when the diagnostic OPENED it at startup: a lock that also pinned
                // the page shut would make the page's own controls impossible to exercise by hand or by a
                // script, which is the opposite of what a diagnostic is for.
                if (lockedSoundPage) soundPageOpen = true;
            }

            // --- a click OUTSIDE the bar puts the panel away -------------------------------
            //
            // The card is open until the user says otherwise, and "otherwise" is a click that the bar never
            // hears about: the window is CLICK-THROUGH everywhere except the pill and the card, so a click on
            // the desktop, on another application, or in the shell's own tray is delivered to whatever was
            // clicked and not to us. The panel therefore looked stuck -- reported as exactly that, with the
            // card sitting over a window the user had already clicked into.
            //
            // So the button is LOOKED AT rather than waited on. This is what the shell's own flyout does as
            // well: its outside click dismisses the flyout and also acts on whatever was clicked.
            //
            // Polled, not hooked. A WH_MOUSE_LL hook would see the click first, and it would also mean this
            // process watching every click on the machine for days at a time for the sake of one flyout --
            // a disproportionate thing for a bar to do, and the kind of thing that gets a program flagged.
            if (injectClickAtMs_ != 0 && GetTickCount64() >= injectClickAtMs_) {
                injectClickAtMs_ = 0;
                island::LogAndPrint("  diagnostic: clicking at (%ld,%ld) from the bar itself\n",
                                    injectClick_, injectClickY_);
                std::fflush(stdout);
                SetCursorPos(injectClick_, injectClickY_);
                Sleep(80);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                Sleep(80);
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            }
            if (systemPanelOpen) {
                POINT where = {};
                GetCursorPos(&where);
                const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                if (leftDown && !leftWasDown) {
                    // Reported even when the press was on the bar: "the panel did not close" and "the press
                    // was never seen" are different faults, and only this line tells them apart.
                    const bool onBar = window.HitTestScreen(where);
                    island::LogAndPrint(
                        "  system panel: a press at (%ld,%ld) %ls the bar\n", where.x, where.y,
                        onBar ? L"on" : L"outside");
                    if (onBar) {
                        // A press on the bar has its own meaning, decided by the click handler; this poll is
                        // only about the presses the bar never hears about.
                    } else {
                        dismissPanel();
                        island::LogAndPrint("  system panel: closed (a click outside the bar)\n");
                        std::fflush(stdout);
                    }
                }
                leftWasDown = leftDown;
            } else {
                // The state is tracked even while the panel is shut, so that a panel OPENED by pressing the
                // deck button -- which is itself a click -- is not dismissed by the button being down at the
                // moment it opened.
                leftWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            }

            // --- the panel's own timer, when the diagnostic is on ---
            //
            // Placed where a click would have left it -- `systemPanelOpen` is the same flag the chevron
            // sets -- so what the frame loop does next is what it does for a real press, with no
            // pointer involved.
            if (autoPanelMs > 0 && !forcedState) {
                const DWORD diagnosticElapsed = GetTickCount() - diagnosticStart;
                if (autoPanelCycleMs > 0 && autoPanelOpened && autoPanelClosed) {
                    // Open, close, open, close ... on a fixed period, which is what a user pressing the
                    // control repeatedly does.
                    const DWORD phase =
                        (diagnosticElapsed - autoPanelMs) / autoPanelCycleMs;
                    const bool wantOpen = (phase % 2) == 0;
                    if (wantOpen != systemPanelOpen) {
                        systemPanelOpen = wantOpen;
                        island::LogAndPrint("  diagnostic: panel %ls at %lu ms (cycle %lu)\n",
                                            wantOpen ? L"opened" : L"closed",
                                            static_cast<unsigned long>(diagnosticElapsed),
                                            static_cast<unsigned long>(phase));
                        std::fflush(stdout);
                    }
                } else if (!autoPanelOpened && diagnosticElapsed >= autoPanelMs) {
                    autoPanelOpened = true;
                    systemPanelOpen = true;
                    island::LogAndPrint("  diagnostic: panel opened at %lu ms\n",
                                        static_cast<unsigned long>(diagnosticElapsed));
                    std::fflush(stdout);
                } else if (autoPanelOpened && !autoPanelClosed &&
                           diagnosticElapsed >= autoPanelCloseMs) {
                    autoPanelClosed = true;
                    // Through the same door a press uses, so the diagnostic reproduces the real thing rather
                    // than a version of it that leaves the shape where it was.
                    dismissPanel();
                    island::LogAndPrint("  diagnostic: panel closed at %lu ms\n",
                                        static_cast<unsigned long>(diagnosticElapsed));
                    std::fflush(stdout);
                }
            }

            // --- the Start button's own timer, when the diagnostic is on ---
            //
            // This presses the control that has to inject input into the SHELL, on a period, so the one
            // path that needs a hand on the mouse can be exercised from a script. It is a toggle, like the
            // key: the first call opens the menu, the next closes it.
            //
            // The integrity level matters here and nowhere else in this program: UIPI discards synthetic
            // input aimed at a process of a HIGHER integrity level, and the shell that answers Ctrl+Esc is
            // Explorer. A bar started from a restricted launcher is below it, and the keystroke then goes
            // nowhere with no error anywhere -- which is exactly what the report "the Start button no
            // longer works" looked like. Whether that is the case is stated at startup ("native taskbar:
            // CANNOT BE HIDDEN"), and repeated here with the result of the attempt.
            if (autoStartMs > 0 && !forcedState) {
                const DWORD diagnosticElapsed = GetTickCount() - diagnosticStart;
                if (diagnosticElapsed >= autoStartMs) {
                    const DWORD period = (autoStartCycleMs > 0) ? autoStartCycleMs : 2000;
                    const DWORD phase = (diagnosticElapsed - autoStartMs) / period;
                    if (phase != lastStartTogglePhase) {
                        lastStartTogglePhase = phase;
                        const island::StartMenuResult startResult = island::ToggleStartMenu();
                        island::LogAndPrint("  diagnostic: start menu toggle %lu -> %ls at %lu ms\n",
                                            static_cast<unsigned long>(phase),
                                            island::StartMenuResultName(startResult),
                                            static_cast<unsigned long>(diagnosticElapsed));
                        std::fflush(stdout);
                    }
                }
            }

            // --- the pointer flicking in and out of the island, when the environment asks for it ---
            //
            // ... and one play/pause on the media session, which is otherwise reachable only by clicking
            // the card: "点击暂停 歌曲停止了 但是这个差不多2秒左右才变播放按钮" could not be checked here at all
            // until this existed (a script's synthetic click goes to the REAL cursor's position).
            if (autoTransportMs > 0 && !forcedState && !transportForced) {
                const DWORD elapsed = static_cast<DWORD>(GetTickCount() - diagnosticStart);
                if (elapsed >= autoTransportMs) {
                    transportForced = true;
                    const bool asked = music.RequestTogglePlayPause();
                    island::LogAndPrint("  diagnostic: play/pause %ls at %lu ms\n",
                                        asked ? L"requested" : L"REFUSED (no session to control)",
                                        static_cast<unsigned long>(elapsed));
                    std::fflush(stdout);
                }
            }
            //
            // A mouse crossing the island quickly leaves it and comes back within a few milliseconds, and
            // THAT is what interrupts a card's exit: the retarget is a pill state while the shape is still
            // card-sized, which is the one frame range where the animated box is card-sized with no card
            // up. See the pill's line rule in Renderer.cpp. The gesture cannot be performed from a script
            // here (its input is dropped before it reaches the bar), so this does it on a period.
            if (autoInsideFlickerMs > 0 && !forcedState) {
                const DWORD since = static_cast<DWORD>(GetTickCount() - diagnosticStart);
                const bool insideNow = ((since / autoInsideFlickerMs) % 2) == 0;
                if (insideNow != flickerInside) {
                    flickerInside = insideNow;
                    window.SetForcedPointerInside(insideNow ? 1 : 0);
                    island::LogAndPrint("  diagnostic: the pointer flicks %ls at %lu ms\n",
                                        insideNow ? L"ONTO the island" : L"OFF the island", since);
                    std::fflush(stdout);
                }
            }

            // --- the deck-hover diagnostic, when the environment asks for it ---
            //
            // Turns on at a given moment rather than at startup: what it is for is the TRANSITION -- a card
            // that is already open being given up because the pointer moved off the pill -- and nothing is
            // open yet at startup.
            if (autoDeckHoverMs > 0 && !forcedState && !deckHoverForced &&
                GetTickCount() - diagnosticStart >= autoDeckHoverMs) {
                deckHoverForced = true;
                window.SetForcedDeckHover(true);
                island::LogAndPrint("  diagnostic: the pointer is treated as ON a deck button at %lu ms\n",
                                    static_cast<unsigned long>(GetTickCount() - diagnosticStart));
                std::fflush(stdout);
            }

            // ... and the same for the CARD, which is the pill's territory: the card must survive the pointer
            // arriving on it, or its buttons cannot be pressed.
            if (autoCardHoverMs > 0 && !forcedState && !cardHoverForced &&
                GetTickCount() - diagnosticStart >= autoCardHoverMs) {
                cardHoverForced = true;
                window.SetForcedPointerInside(autoCardMode);
                island::LogAndPrint("  diagnostic: the pointer is treated as %ls at %lu ms\n",
                                    autoCardMode == 4 ? L"in the GAP above the card" : L"ON the card",
                                    static_cast<unsigned long>(GetTickCount() - diagnosticStart));
                std::fflush(stdout);
            }

            // --- pointer -> state ---
            //
            // The pointer is polled HERE, from the frame loop, rather than only from a window
            // timer.
            //
            // The window ran an 8 ms timer for this and reported it as running, but a trace of the
            // messages actually delivered to the window procedure showed `WM_TIMER=0`: the timer
            // message never arrived, so this poll effectively never happened, `pointerInside_`
            // stayed false for the life of the process, the shape never promoted to a card, and no
            // click was ever accepted. It was silent -- the bar drew perfectly and ignored the
            // pointer.
            //
            // The frame loop runs at the compositor's rate, which is better than 8 ms and cannot
            // be starved, so it is the reliable place to ask. The timer is left in place as a
            // fallback for the paths that do not pump this loop.
            window.UpdatePointerState();

            // The window only reports that the pointer entered or left; what that
            // means is decided here, which keeps the window procedure free of policy.
            //
            // Pointer handling must not fight the notification state: while a
            // notification is showing, leaving the pill returns to the card rather
            // than collapsing it, or an arriving message would be cut off the instant
            // the pointer drifted away.
            bool pointerInside = false;
            if (window.TakePointerInsideChanged(&pointerInside)) {
                // THE EDGE IS CONSUMED AND NOTHING ELSE HAPPENS HERE.
                //
                // The state is the POLICY's answer (NextShape, below). Every state this handler used to
                // pick was a second policy, and the two could disagree -- which is what kept the
                // notification card on screen after it was removed on request:
                //
                //   * with the pointer inside, this frame asked for Expanded (`active` -- a message must
                //     not be cut off when the pointer drifts away) while the policy asked for the notice
                //     strip, so the two alternated every frame;
                //   * measured in the bar's own log: `state: notice-strip` and `state: expanded` swapping
                //     every ~0.35 s while a notification was up, and a screenshot of the card STILL being
                //     drawn after the card had been removed. Reported as "但是现在还是会触发啊".
                //
                // The panel's case below is the policy's too: `systemPanelOpen` returns Expanded whatever
                // the pointer is doing, so nothing was lost by deleting this.
                (void)pointerInside;
            }
            // A layout change (DPI, work-area change) has already been applied by
            // the window; draining the flag keeps it from accumulating.
            window.TakeLayoutChanged();

            // --- pointer -> shape, once the motion has settled ---
            //
            // The rule itself lives in StatePolicy.h as a pure function, so the frame loop
            // has no policy of its own to drift from what the tests check. It used to be
            // three inline conditions with the promotion to the card buried inside the
            // branch that lowers an already-open card, so nothing ever moved
            // Hover -> Expanded and the card was unreachable while music played.
            //
            // Only consulted when the motion has stopped, so a shape is never re-targeted
            // mid-flight (that would restart the animation every frame).
            if (!window.IsAnimating()) {
                // The icon row is described to the policy so it can tell "the pointer is on the
                // island" from "the pointer is on one of the icons". Those need different answers:
                // the first widens the island, the second widens one icon and leaves the island
                // exactly as it is.
                island::PointerContext context;
                context.shape = window.State();
                context.resting = resting;
                context.pointerInside = window.IsPointerInside();
                context.notificationActive = active != nullptr;
                // ... and whether there is a player at all: a PAUSED session still opens the card, which
                // is how the play button is reached after a pause (see ShouldPromoteToCard).
                context.musicPresent = nowPlaying.present;
                context.fullscreenApp = appBar.IsFullscreenAppInFront();
                // The panel is a request for the CARD, so the policy has to know about it -- otherwise
                // the state stays Hover and the card is never drawn. That was a whole debugging round:
                // the flag was set, the renderer read it, and the shape never changed.
                context.systemPanelOpen = systemPanelOpen;
                context.iconCount = window.IconCount();
                context.hoveredIcon = window.HoveredIcon();
                // Where the pointer is, for the rule that gives the card up: an icon or a deck button is
                // asking about itself, not about the island. The window is the only thing that knows where
                // those buttons are.
                context.pointerOnDeck = window.SystemButtonHovered() || window.StartHovered();
                // ... and whether it is on the PILL rather than elsewhere on the bar. The card is the
                // pill's, so this, not `pointerInside`, is what asks for it.
                context.pointerOnIsland = window.IsPointerOnIsland();
                // ... and whether it is in the CARD's COLUMN -- the card and the gap between it and the bar.
                // That column is the pill's territory as much as the pill is: the pointer has to be able to
                // walk from the pill down onto the card whose buttons it wants, and the gap is part of the walk.
                context.pointerInCardColumn = window.IsPointerInCardColumn();

                const island::IslandState next = island::NextShape(context);

                if (forcedState) {
                    // Pinned. The policy above still ran and is still reported; its answer is simply not
                    // acted on, so what is on screen is a named state rather than whatever the pointer
                    // happened to be doing.
                    const island::IslandState pinned = island::StateFromName(forcedState);
                    if (window.State() != pinned) window.SetState(pinned);
                } else if (next != window.State()) {
                    window.SetState(next);
                }

                // A DRAG UNDER WAY HOLDS THE CARD OPEN, whatever the pointer does.
                //
                // A slider's gesture is "press it, drag anywhere, release" -- the pointer leaves the row,
                // leaves the card, and often leaves the bar itself on its way to the end of the track. With
                // the policy left in charge the card retracted mid-drag: the handle stopped updating at the
                // exact moment the user was watching it, which is the other half of "卡卡的，没有过渡".
                // Same shape as the rule that already holds the card for a notification: something explicit
                // is in progress, so the pointer's position is not the question.
                if (panelSliderDrag || mixerSliderDragRow >= 0) {
                    if (window.State() != island::IslandState::Expanded) {
                        window.SetState(island::IslandState::Expanded);
                    }
                }
            }

            // --- advance the animation ---
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            const float dt = static_cast<float>(
                static_cast<double>(now.QuadPart - previous.QuadPart) /
                static_cast<double>(frequency.QuadPart));
            previous = now;

            // --- open the pointed-at icon button, over a few frames ----------------
            //
            // The width used to change in a single frame, and a width that jumps reads as a stutter
            // even when nothing is slow -- which is exactly how it was reported. Driven from the
            // frame's own delta, so it is smooth rather than tied to the frame rate.
            //
            // TWO amounts are animated, not one. `hover` is how far the pointed-at button has grown;
            // `shift` is how far the buttons before it have pushed it along. Animating both is what
            // makes the row open as ONE movement: growing in place leaves no room for the title and
            // is drawn over the neighbours, while shifting in a single frame shuffles the row.
            {
                // ~160 ms to open: fast enough not to feel sluggish, slow enough to read as movement.
                const float speed = 1.0f / 0.16f;
                const float step = speed * (dt > 0.05f ? 0.05f : dt);

                // The island's own expansion already opens the whole row, so the pointed-at button is
                // left alone in that state: growing it on top would be two animations where one is
                // wanted.
                const bool islandOpen = (window.State() == island::IslandState::Hover);
                const int pointed = islandOpen ? -1 : window.HoveredIcon();

                const auto approach = [step](float& value, float target) {
                    if (value < target) {
                        value += step;
                        if (value > target) value = target;
                        return true;
                    }
                    if (value > target) {
                        value -= step;
                        if (value < target) value = target;
                        return true;
                    }
                    return false;
                };

                iconAnimating = false;
                for (int i = 0; i < island::kMaxTaskWindows; ++i) {
                    const float target = (i == pointed) ? 1.0f : 0.0f;
                    if (approach(taskHoverAmount[i], target)) iconAnimating = true;
                }

                // Reported whenever the pointer's icon changes, and once when the animation starts or
                // stops.
                //
                // NOT on every change of `step`: the step is derived from the real frame time, so it
                // differs slightly every frame, and including it in the condition produced a line per
                // frame -- hundreds of lines in five seconds, burying everything else. A trace
                // condition has to be a value that is actually stable between the events being traced.
                if (pointed != lastReportedPointed || iconAnimating != lastReportedAnimating) {
                    lastReportedPointed = pointed;
                    lastReportedAnimating = iconAnimating;
                    island::LogAndPrint("  anim: pointed=%d animating=%d step=%.4f hover0=%.2f",
                                        pointed, iconAnimating ? 1 : 0, step,
                                        taskHoverAmount[0]);
                    std::fflush(stdout);
                }
            }

            // Clamp: a long stall (a debugger break, a sleep) must not teleport
            // the animation to its end, which would read as a jump cut.
            const bool animating = window.Tick(dt > 0.1f ? 0.1f : dt);

            // --- draw ---
            //
            // Three reasons to draw: the shape moved, the clock's second changed, or
            // the notification being shown changed. Nothing else can alter a frame,
            // so when none holds the loop does no drawing at all.
            int second = -1;
            text.CurrentSecond(&second);
            const bool clockChanged = (second != lastSecond);
            lastSecond = second;

            const long long activeId = active ? active->id : 0;
            const bool notificationChanged = (activeId != lastNotificationId);
            lastNotificationId = activeId;

            // A track change is a reason to redraw even when nothing moved.
            const bool musicChanged = (music.Revision() != lastDrawnMusicRevision);
            lastDrawnMusicRevision = music.Revision();

            // --- the lyric line for the resting strip ---
            //
            // The line scrolls, so the strip needs frames even when nothing else
            // changed -- but only while the strip is actually on screen. Scrolling a
            // shape nobody can see would keep the loop awake for nothing.
            const char* lyricLine = nullptr;
            const char* fallbackLine = nullptr;

            // ... and the line the NOTICE STRIP shows: who it is from, and what it says.
            //
            // Built here rather than in the renderer because it is a decision about the message (which field,
            // in which order, what to do when one of them is empty), and the renderer is handed finished
            // words. Held in a member of this function's scope so the pointer in the frame stays valid for
            // the frame's whole life.
            std::string noticeLine;
            if (active) {
                if (active->app[0] != '\0') {
                    noticeLine = active->app;
                    noticeLine += " \xC2\xB7 ";   // " · "
                }
                noticeLine += (active->title[0] != '\0') ? active->title : active->body;
            }
            bool lyricsScrolling = false;
            // How long the line on screen has been there, for its entrance. Kept past the end of the
            // entrance so a frame that changes nothing does not animate anything.
            bool lyricLineAnimating = false;
            bool lyricLineChanged = false;
            // While MUSIC RESTS -- not "while the strip is the state".
            //
            // The pill keeps showing the lyric when a card is up (see the pill's text rule in
            // Renderer.cpp), so the line has to be kept up to date then too. Keyed on the state, the
            // lyric vanished the moment the system button opened a panel, with the music still playing:
            // "我现在正在播放音乐 但是点击系统按钮的时候 灵动岛就不显示歌词了".
            if (musicResting) {
                lyricLineAge += static_cast<float>(dt > 0.1 ? 0.1 : dt);
                // The wave drifts even while the level sits still, which is what keeps it looking
                // alive between the beats. Wrapped, so a session that runs for hours cannot lose
                // precision in the sine's argument.
                musicWavePhase += static_cast<float>(dt > 0.1 ? 0.1 : dt) * 1.15f;
                if (musicWavePhase > 6.2831853f * 64.0f) musicWavePhase -= 6.2831853f * 64.0f;
                if (currentLyrics && !currentLyrics->Empty()) {
                    // The position is what selects the line, so it advances within a track without the
                    // track itself changing -- and it has to be the PROJECTED position, not the raw
                    // reading: the session reports once every second or two, so the line changed a
                    // couple of seconds after the words were sung. Reported as "现在歌词和唱的有点
                    // 对应不上". The same correction the card's progress bar uses (see
                    // ProjectedPositionMs).
                    const long long sungMs = island::ProjectedPositionMs(
                        nowPlaying.positionMs, nowPlaying.durationMs,
                        static_cast<long long>(GetTickCount64() - musicPositionAt),
                        nowPlaying.state == island::PlaybackState::Playing);
                    const int index = currentLyrics->LineAt(sungMs);
                    const island::LyricLine* line = currentLyrics->At(index);
                    const char* nextText = (line && line->text[0] != '\0') ? line->text : "";
                    if (index != lastLyricIndex) {
                        lastLyricIndex = index;
                        // A ROLL ONLY WHEN THE WORDS CHANGE.
                        //
                        // The index is re-evaluated for a reason that is not a line change: the media
                        // session bumps its revision for a position update (once every second or two)
                        // and this loop then re-asks which line is current. Rolling on that put the SAME
                        // text on both sides of the window -- "歌词每1、2秒就会上下出现一模一样的歌词
                        // 然后和在一起似的" -- and it restarted a long line's marquee from the top.
                        if (island::LyricLineChanged(nextText, shownLyricText)) {
                            // The line being replaced is kept -- with the marquee offset it had reached --
                            // because the cross-fade draws it in the same place for the whole change.
                            strncpy_s(replacedLyricText, sizeof(replacedLyricText), shownLyricText,
                                      _TRUNCATE);
                            replacedLyricScrollDip = lyricsScrollDip;
                            lyricsScrollDip = 0.0f;   // a new line starts at its beginning
                            lyricLineAge = 0.0f;      // ... and fades in rather than appearing
                            lyricLineChanged = true;
                            island::LogAndPrint("  lyrics: line %d at %lld ms (the session last reported %lld ms)\n",
                                                index, sungMs, nowPlaying.positionMs);
                        }
                    }
                    lyricLineAnimating = lyricLineAge < island::kLyricLineEnterSeconds;
                    // What is on screen, kept in step whether or not the words changed -- the
                    // comparison above is against this.
                    strncpy_s(shownLyricText, sizeof(shownLyricText), nextText, _TRUNCATE);

                    if (line && line->text[0] != '\0') {
                        lyricLine = line->text;

                        // Advance the marquee only when there is something to scroll: a
                        // line that fits is centred and stays put, so the loop can
                        // sleep instead of redrawing 60 times a second.
                        // The PILL's width, which is what the line is drawn in -- not `CurrentBox`,
                        // which is card-sized for the whole of a card transition and would tell the
                        // marquee the line fits while the renderer clips it to 220 DIP.
                        const island::RoundedBox lineBox =
                            island::BoxForState(island::IslandState::Collapsed, window.CurrentAnchor());
                        const float stripWidth = lineBox.rect.right - lineBox.rect.left;
                        wchar_t wide[512] = {};
                        if (MultiByteToWideChar(CP_UTF8, 0, line->text, -1, wide, 512) > 0) {
                            // The same predicate the renderer uses to place the line.
                            // They used to work it out separately from slightly different
                            // numbers, so the renderer could centre a line while the loop
                            // scrolled it -- the text would jump sideways for no reason.
                            // ... and the same WIDTH measurement: `TrackLineWidth` is the width in the format
                            // that draws the line. With `TextWidth` (the narrow, non-bold face) a line just
                            // over the threshold was judged to fit, centred, and cut at both ends.
                            if (island::LyricNeedsScroll(text.TrackLineWidth(wide),
                                                         stripWidth)) {
                                // A PAUSED SESSION'S LINE STANDS STILL. The words stay on the pill -- they
                                // are what it is for -- but a line that kept creeping across a paused player
                                // would be reading a playhead that is not moving, and asking for frames
                                // forever to do it.
                                if (nowPlaying.state == island::PlaybackState::Playing) {
                                    lyricsScrolling = true;
                                    lyricsScrollDip +=
                                        static_cast<float>(dt) * kLyricScrollDipPerSec;
                                }
                            }
                        }
                    }
                }
                // No lyrics for this track: the title is the honest fallback. The
                // clock is only shown when there is no music at all.
                if (!lyricLine) fallbackLine = nowPlaying.title;
            }

            // Does the card's printed position need to change?
            //
            // The bar and the elapsed time advance locally, so without this they would only be
            // redrawn when the media session happened to report a new position -- every couple
            // of seconds, which is the jumping the user saw. One redraw per second is enough
            // for a text clock and a bar that moves about 1% a second, and it only applies
            // while the card is actually open.
            const long long shownPosition =
                (window.State() == island::IslandState::Expanded)
                    ? island::ProjectedPositionMs(
                          nowPlaying.positionMs, nowPlaying.durationMs,
                          static_cast<long long>(GetTickCount64() - musicPositionAt),
                          nowPlaying.state == island::PlaybackState::Playing)
                    : 0;
            // The volume can be changed by the keyboard or the shell flyout, not only here, so
            // it is a redraw reason too.
            bool volumeChanged = false;
            if (volumeAvailable && volume.Revision() != lastVolumeRevision) {
                lastVolumeRevision = volume.Revision();
                volumeChanged = true;
            }

            // A new weather reading is the same kind of reason: it arrives on its own schedule, and
            // nothing else about the frame has to move for the strip to have something new to say.
            bool weatherChanged = false;
            if (weather.Revision() != lastWeatherRevision) {
                lastWeatherRevision = weather.Revision();
                weatherChanged = true;
            }

            const bool progressChanged = (shownPosition / 1000) != (lastShownSecond);
            lastShownSecond = shownPosition / 1000;

            // The page changes for three reasons of its own: it was opened or closed, it was scrolled, or
            // the audio reading behind it moved on. Read BEFORE the frame is built, because the build is
            // what records the new values.
            const bool soundPageChanged =
                soundPageOpen != lastSoundPageOpen ||
                (soundPageOpen && window.SoundPageScroll() != lastSoundScroll) ||
                (soundPageOpen && soundOutputStarted && soundOutput &&
                 soundOutput->Revision() != lastSoundRevision);

            // A call's level meter is the one thing on this bar that changes without an event, so it asks for
            // frames of its own. 20 Hz: twice a second looks broken, and sixty times a second is a meter nobody
            // can see paid for with a loop that never idles during a call that may last an hour.
            const bool callMeterDue = (wechat.CallNotificationId() != 0) &&
                                      (GetTickCount64() - lastCallMeterMs >= 50);
            if (callMeterDue) lastCallMeterMs = GetTickCount64();

            // The pill's background meter is the other thing here that moves without an event, and it
            // asks for frames at 30 Hz: the rate the reading behind it is refreshed at (see
            // AudioVolume::Peak), and what a drifting shape needs to look continuous rather than stepped.
            // Only while music is what the bar is showing: a wave nobody can see is a redraw nobody
            // asked for.
            const bool musicMeterDue =
                musicResting && nowPlaying.state == island::PlaybackState::Playing &&
                volumeAvailable && (GetTickCount64() - lastMusicMeterMs >= 33);
            if (musicMeterDue) lastMusicMeterMs = GetTickCount64();

            const bool needDraw = animating || clockChanged || notificationChanged ||
                                  musicChanged || lyricsScrolling || lyricLineChanged ||
                                  lyricLineAnimating || progressChanged || musicMeterDue ||
                                  iconAnimating || rowAppearAnimating || soundPageChanged ||
                                  callMeterDue || statusStripAnimating || weatherChanged ||
                                  !haveDrawn;
            if (needDraw) {
                // Which condition forced this redraw, counted per report interval.
                //
                // The frame INTERVAL only says the loop is running at 62 fps; that is a
                // symptom, not a cause. Without knowing which term keeps the condition true,
                // the only options are to guess or to keep optimising the draw -- and the draw
                // is already only 1.3 ms. This names the cause.
                if (animating) ++whyAnimating;
                if (clockChanged) ++whyClock;
                if (notificationChanged) ++whyNotification;
                if (musicChanged) ++whyMusic;
                // The scrolling, the arrival of a new line and the entrance itself are all "the
                // lyric asked for this frame" -- one counter, because that is the question it answers.
                if (lyricsScrolling || lyricLineChanged || lyricLineAnimating) ++whyLyrics;
                if (musicMeterDue) ++whyMusic;
                if (progressChanged) ++whyProgress;
                if (statusStripAnimating || weatherChanged) ++whyStatusStrip;
                if (!haveDrawn) ++whyFirst;

                haveDrawn = true;

                island::FrameState frame;

                // The system panel's flag and the card's position, set FIRST -- before the shape is
                // captured and before anything is composed.
                //
                // These were set at the BOTTOM of the loop, after `frame.pill.box` had already been
                // taken and after the card's box had been derived from it. The result was a card placed
                // from the OLD position while a second copy was placed from the new one: two panels, one
                // hanging from the button and one in the middle of the screen. Setting a field late is
                // the same defect as not setting it at all, and it looks like a rendering fault.
                // Whether an outer card is up OR ON ITS WAY, taken from the motion rather than from the
                // panel's flag. MUST BE COMPUTED BEFORE ANYTHING USES IT.
                //
                // It was first written beside the other frame fields, well AFTER the panel's content and
                // position had been derived from it -- so for the whole opening transition the flag was a
                // frame stale, `cardLeftDip` stayed zero, and the card was drawn as a 220-wide pill
                // growing in place instead of as a 520-wide panel sliding out from under its button.
                // Measured: `w=220` on every frame of the transition, with the tile count at zero.
                //
                // Setting a field late is the same defect as not setting it at all; this project has now
                // paid for that lesson four times, which is why this comment is this emphatic.
                frame.outerCardUp = window.State() == island::IslandState::Expanded ||
                                    window.FromState() == island::IslandState::Expanded;

                frame.systemPanelOpen = systemPanelOpen;

                // Who owns the card for this frame. The panel keeps it until the card is gone.
                //
                // `systemPanelOpen` is the request, and the request is withdrawn the moment the control is
                // pressed again; the card that request opened is still on screen for another 0.24 s.
                // Drawing the panel only while the request was set made the retraction invisible -- the
                // panel disappeared in one frame -- and, because the state had been left on the card, left
                // the next press with no transition to animate at all. Both halves of the report are this
                // one latch.
                //
                // `cardWanted` is the TARGET, which is the question "is a card still asked for" -- as
                // opposed to `frame.state`, which is where the shape is now. While the answer is yes the
                // owner is chosen afresh (panel, then a message, then what is playing); while it is no the
                // owner is kept, so the card is drawn all the way out instead of vanishing on the first
                // frame of its own exit.
                const bool cardWanted = (window.State() == island::IslandState::Expanded);
                cardOwner = island::NextCardOwner(cardOwner, frame.outerCardUp, cardWanted,
                                                 systemPanelOpen, active != nullptr,
                                                 nowPlaying.present);
                frame.cardOwner = cardOwner;
                // ... and the WINDOW is told too, because the cursor is decided there (WM_SETCURSOR) and it
                // has to know which card's controls are the ones on screen. See SetCardOwner.
                window.SetCardOwner(cardOwner);

                // The message the card is showing, held for the whole life of the card.
                if (active) {
                    notificationShown = *active;
                    haveNotificationShown = true;
                }
                if (cardOwner != island::CardOwner::Notification) haveNotificationShown = false;
                frame.systemButtonHovered = window.SystemButtonHovered();
                frame.systemPanel.tileCount = 0;
                frame.cardLeftDip = 0.0f;

                // The panel's CONTENT is built whenever a card is up OR ON ITS WAY -- not only while the
                // panel flag is set.
                //
                // Building it only while the flag was set meant that the moment the panel was asked to
                // close, the tiles and the slider vanished and the close animation played on an EMPTY
                // card: the panel's contents disappeared first and a blank rounded rectangle shrank
                // afterwards. Reported as the panel "appearing and disappearing" with the pointer.
                const bool cardUp = frame.outerCardUp;
                if (cardUp) {
                    if (soundPageOpen) {
                        // --- the panel's second level: the 声音输出 page ---
                        //
                        // The worker is already running -- it starts with the bar, because the WeChat sound
                        // channel reads the same session list. This used to start it here, on the first open of
                        // the page, which quietly meant "nothing that needs the session list works until
                        // somebody opens the mixer".
                        //
                        // A snapshot per frame while the page is up. Cheap enough -- it is a copy of a list
                        // that changes about once a second -- and the alternative, holding the live object,
                        // would put the worker's own state on screen mid-update.
                        soundOutput->Snapshot(soundSnapshot.get());
                        float masterLevel = 0.0f;
                        const bool haveMaster = volume.Get(&masterLevel);
                        // The device's own row is drawn at the chased value as well, and at the raw reading
                        // while that row is the one being dragged -- the same rule as the other two bars.
                        const float drawnMaster =
                            (mixerSliderDragRow == 0 || shownVolume < 0.0f) ? masterLevel : shownVolume;
                        BuildSoundPage(*soundSnapshot, systemStatus, haveMaster, drawnMaster,
                                       volume.IsMuted(), soundPageScroll, &frame.soundPage);
                        frame.soundPageOpen = true;
                        lastSoundRevision = soundOutput->Revision();
                        window.SetSoundPage(frame.soundPage, soundPageScroll);
                    } else {
                        // Rebuilding every frame is what the tiles need: their on/off state comes from the
                        // status readings, and the readings are sampled on a timer. The text is copied into
                        // the frame's own buffers, so nothing here points at a shorter-lived string.
                        float liveLevel = 0.0f;
                        const bool liveKnown = volume.Get(&liveLevel);
                        BuildSystemPanel(systemStatus, statusReader.InstalledLayoutCount(), liveKnown,
                                         liveLevel, volume.IsMuted(), &frame.systemPanel,
                                         systemPanelPage);

                        // The volume's own percentage, shown while the slider is the control in hand: being
                        // dragged, used within the last second, or simply hovered. The handle's position is
                        // the only reading the slider carries by itself, and "about three quarters along" is
                        // not an answer to "how loud is it".
                        {
                            POINT echoPoint = {};
                            GetCursorPos(&echoPoint);
                            const bool onSlider =
                                window.SystemPanelHitAtScreen(echoPoint).target ==
                                island::SystemPanelTarget::Slider;
                            frame.systemPanel.showVolumePercent =
                                panelSliderDrag || onSlider ||
                                GetTickCount64() < panelVolumeEchoUntil;
                            // The panel's handle is drawn at the chased value too, and at the pointer's own
                            // value while it is being dragged -- the same rule as the card's bar.
                            frame.systemPanel.volume =
                                panelSliderDrag
                                    ? (liveKnown ? liveLevel : frame.systemPanel.volume)
                                    : (shownVolume >= 0.0f ? shownVolume : frame.systemPanel.volume);
                        }
                        // The page's hit test is closed the moment the page is not showing: leaving it open
                        // would let a click on the panel's tiles be read as a device row.
                        window.CloseSoundPage();
                    }

                    // The card is CENTRED on the system button that opened it.
                    //
                    // Chosen by the user after being shown the numbers: centring the card on the button
                    // puts its right half under the ISLAND, and the alternative -- keeping the card
                    // wholly to the island's left -- leaves its centre about 100 DIP left of the
                    // button's, which does not read as "under the button".
                    //
                    // The overlap is not a collision. The card hangs BELOW the bar (top at 50 while the
                    // island ends at 42), so the card's right half simply continues down the screen
                    // underneath the island; nothing of the bar's is covered, and the application icon
                    // row -- which is to the RIGHT of the island -- is never reached at all, so it stays
                    // exactly where it is.
                    //
                    // Left-aligning to the button instead -- tried first -- sent the card 520 DIP to the
                    // RIGHT, over the icon row, which then had to be pushed aside. The user reported
                    // exactly that: "the icons beside the island were all squeezed out".
                    //
                    // The anchor comes from the RESTING island, so the position does not move while the
                    // card's own width is animating.
                    const island::RoundedBox anchorIsland = window.LayoutIslandFor();
                    const island::RectF systemButton = island::SystemButtonFor(anchorIsland);
                    const float buttonCentre = (systemButton.left + systemButton.right) * 0.5f;
                    // WHICH THING THE CARD HANGS FROM DEPENDS ON WHICH CARD IT IS.
                    //
                    // The system panel hangs from the control that opened it: the user chose that
                    // ("面板居中于按钮", 7.76) and it is what a flyout does -- press a button, get a panel
                    // under that button.
                    //
                    // The other two belong to the ISLAND. The media card is the pill's own detail view, and a
                    // message is about the bar as a whole; both were originally centred under the pill (the
                    // screenshots in docs/ from that phase show it), and 7.76 moved all three together
                    // because the question was asked about the panel. Reported as "this should be under the
                    // island, not under this [the button]".
                    const float islandCentre = (anchorIsland.rect.left + anchorIsland.rect.right) * 0.5f;
                    const float anchorCentre = (cardOwner == island::CardOwner::SystemPanel)
                                                   ? buttonCentre
                                                   : islandCentre;
                    const float anchorLeft = anchorCentre - island::kExpandedWidthDip * 0.5f;
                    const float windowWidthDipNow =
                        static_cast<float>(window.Monitor().screen.right -
                                           window.Monitor().screen.left) /
                        window.Scale();
                    frame.cardLeftDip = island::CardLeftFor(anchorLeft, island::kExpandedWidthDip,
                                                            windowWidthDipNow);
                }

                // Given to the window here as well, because the click handler reads it to work out which
                // tile was pressed -- and the click handler runs near the TOP of the loop, before the
                // renderer's own bookkeeping.
                window.SetSystemPanel(frame.systemPanel, frame.cardLeftDip);

                frame.pill.box = window.CurrentBox();
                frame.state = window.State();

                // The pill's own box, which is NOT the animated one while a card is opening or closing.
                //
                // `CurrentBox` is the shape in flight: it is card-sized for the whole of a transition to
                // or from the card, and the pill's fill drew that rectangle in the pill's near-opaque
                // black -- a dark band under the island for 200 ms per transition. The resting box is
                // derived from the anchor, so it is the pill regardless of what is animating.
                frame.restingPill = island::BoxForState(island::IslandState::Collapsed,
                                                        window.CurrentAnchor());
                // ... and WHAT the bar shows at rest, which is what the pill's own line is chosen by
                // (see FrameState::restingState). A card does not change this, which is the point.
                frame.restingState = resting;

                if (GetTickCount64() - lastTaskRefresh >= 500) {
                    lastTaskRefresh = GetTickCount64();
                    taskWindows.Refresh();
                }

                // Trimmed to what fits before the row would leave the monitor, so the count the
                // renderer draws is the count the click handler will hit-test.
                // Trimmed to what fits before the row would leave the monitor, so the count the
                // renderer draws is the count the click handler will hit-test.
                //
                // The indicators are NOT subtracted here any more: they live on the LEFT deck,
                // which is bounded by the island's left edge and cannot be reached by the icon row
                // whatever its length. Subtracting them would cost icons for space that the icons
                // cannot use.
                const float monitorWidthDip =
                    static_cast<float>(window.Monitor().screen.right -
                                       window.Monitor().screen.left) / window.Scale();
                int listed = island::TaskIconsThatFit(monitorWidthDip, taskWindows.Count());

                // EACH ICON'S ARRIVAL, and the row as the UNION of what is listed and what is leaving.
                //
                // The membership used to change in a single frame: a window opened and its icon was
                // simply THERE, a window closed and it was simply gone. Reported as "灵动岛右侧 图标出现和
                // 消失有点太突兀了没有动画". So every window the row has shown keeps an amount that eases
                // towards 1 while it is listed and towards 0 while it is not (TaskIconAppearStep), and the
                // frame is given the listed windows first and the departing ones after them -- a departing
                // icon is drawn, fading and shrinking, for the length of its exit.
                //
                // The order of the LISTED ones is untouched: the departing ones are appended, so nothing
                // that is staying moves because something else left.
                const float rowDt = static_cast<float>(dt > 0.1 ? 0.1 : dt);
                for (int i = 0; i < rowSlotCount;) {
                    bool listedNow = false;
                    for (int j = 0; j < listed; ++j) {
                        if (taskWindows.At(j).handle == rowSlots[i].handle) {
                            listedNow = true;
                            break;
                        }
                    }
                    rowSlots[i].appear =
                        island::TaskIconAppearStep(rowSlots[i].appear, listedNow, rowDt);
                    if (!listedNow && rowSlots[i].appear <= 0.0f) {
                        rowSlots[i] = rowSlots[rowSlotCount - 1];   // swap the last one in
                        --rowSlotCount;
                        continue;
                    }
                    ++i;
                }
                for (int j = 0; j < listed && rowSlotCount < island::FrameState::kMaxTaskButtons; ++j) {
                    const HWND handle = taskWindows.At(j).handle;
                    bool tracked = false;
                    for (int i = 0; i < rowSlotCount; ++i) {
                        if (rowSlots[i].handle == handle) {
                            tracked = true;
                            break;
                        }
                    }
                    if (!tracked) {
                        rowSlots[rowSlotCount].handle = handle;
                        rowSlots[rowSlotCount].appear = 0.0f;   // arrives from nothing
                        ++rowSlotCount;
                    }
                }

                int drawn = 0;
                const auto appearanceOf = [&](HWND handle) {
                    for (int i = 0; i < rowSlotCount; ++i) {
                        if (rowSlots[i].handle == handle) return rowSlots[i].appear;
                    }
                    return 1.0f;
                };
                for (int j = 0; j < listed && drawn < island::FrameState::kMaxTaskButtons; ++j) {
                    const island::TaskEntry& entry = taskWindows.At(j);
                    // Copied into a persistent buffer: the frame is consumed after this loop
                    // body returns, and the list can refresh in between, so pointing at the
                    // entry's own title would be a dangling read.
                    wcsncpy_s(taskTitles[drawn], island::kTaskTitleCapacity, entry.title,
                              _TRUNCATE);
                    frame.taskButtons[drawn].title = taskTitles[drawn];
                    frame.taskButtons[drawn].active =
                        (entry.handle == taskWindows.ForegroundHandle());
                    frame.taskButtons[drawn].minimised = entry.iconic;
                    // The icon is resolved by the RENDERER, which owns the GPU bitmaps and
                    // caches one per window. The caller only names the window.
                    frame.taskButtonWindow[drawn] = entry.handle;
                    taskHandles[drawn] = entry.handle;
                    frame.taskButtonAppear[drawn] = appearanceOf(entry.handle);
                    ++drawn;
                }
                // ... and the ones on their way out, which have no title to show any more (their window is
                // usually gone) -- only the icon, which the renderer still has cached.
                for (int i = 0; i < rowSlotCount && drawn < island::FrameState::kMaxTaskButtons; ++i) {
                    if (rowSlots[i].appear <= 0.0f) continue;
                    bool stillListed = false;
                    for (int j = 0; j < listed; ++j) {
                        if (taskWindows.At(j).handle == rowSlots[i].handle) {
                            stillListed = true;
                            break;
                        }
                    }
                    if (stillListed) continue;
                    frame.taskButtons[drawn].title = nullptr;
                    frame.taskButtons[drawn].active = false;
                    frame.taskButtons[drawn].minimised = false;
                    frame.taskButtonWindow[drawn] = rowSlots[i].handle;
                    taskHandles[drawn] = rowSlots[i].handle;
                    frame.taskButtonAppear[drawn] = rowSlots[i].appear;
                    ++drawn;
                }

                frame.taskButtonCount = drawn;
                frame.taskButtonAppearValid = true;
                // Whether any icon is still on its way in or out, which is what asks for the next frame.
                // Read by needDraw on the NEXT pass, because the row is updated inside the draw.
                rowAppearAnimating = false;
                for (int i = 0; i < drawn; ++i) {
                    if (frame.taskButtonAppear[i] < 0.999f) rowAppearAnimating = true;
                }
                // The count the WINDOW is told is the count that is DRAWN, because the row's geometry is
                // laid out from it: a departing icon occupies a slot for the length of its exit, and a hit
                // test against a different count would point at the wrong slot for 0.2 s.
                taskWindowsShown = drawn;

                // Told to the window so it can work out whether the pointer is on an icon, and
                // which one. The window owns the hit test; the frame loop owns the list, and this
                // is the handover between them.
                window.SetTaskIconCount(drawn);

                // The system READINGS, sampled on a timer.
                //
                // Most of them come from APIs that open a COM object or walk a device tree, and the bar
                // draws at the compositor's rate, so they are sampled on a clock rather than per frame. A
                // value that cannot be read keeps its previous meaning, which is why the reader is handed
                // the struct instead of filling a fresh one.
                //
                // They used to be turned into a row of indicators on the left deck as well. That row is
                // gone -- the deck holds the Start and system buttons now -- but the readings themselves
                // are what the system panel is built from, so they stay.
                {
                    const unsigned long long nowMs = GetTickCount64();
                    if (nowMs - lastStatusSampleMs >= kStatusSampleMs) {
                        lastStatusSampleMs = nowMs;
                        statusReader.Sample(&systemStatus);
                    }
                }

                // The ONE button to widen. Read back from the window, which computed it from the
                // pointer's position using the geometry that is actually on screen.
                frame.taskHoveredIndex = window.HoveredIcon();
                frame.startHovered = window.StartHovered() || forcedStartHover;

                // Each icon gets a background in the island's own colour, so the row reads as part
                // of the island rather than as icons floating on the wallpaper -- a bright
                // wallpaper made both the icons and their titles unreadable.
                //
                // Only the COLOUR is set here; the rectangle is derived inside the draw from the
                // same layout call that positions the button. Computing it here as well would
                // mean the layout was evaluated twice, and the two would part company the moment
                // one of them used a different hovered index.
                for (int i = 0; i < drawn; ++i) {
                    frame.taskButtons[i].fill = frame.pill.fill;
                    // Handed over from the animation, which runs before this draw. An icon on its way OUT
                    // is never widened: the amount belongs to the slot it used to occupy.
                    frame.taskButtons[i].hoverAmount = (i < listed) ? taskHoverAmount[i] : 0.0f;
                }
                // What the loop worked out, before the draw: the pointer's icon, and the two
                // animated amounts for the first button.
                //
                // Both halves of the row are now reported -- this says what the loop DECIDED, and
                // the renderer's own `row:` line says what the layout PRODUCED. A row that does not
                // move is either a decision that never changed or a layout that ignored it, and only
                // having both makes the difference visible.
                if (window.HoveredIcon() != lastReportedHoveredIcon ||
                    taskHoverAmount[0] != lastReportedHoverAmount) {
                    lastReportedHoveredIcon = window.HoveredIcon();
                    lastReportedHoverAmount = taskHoverAmount[0];
                    island::LogAndPrint("  loop: hoveredIcon=%d, button0 hover=%.2f, %d icons",
                                        window.HoveredIcon(), taskHoverAmount[0],
                                        window.IconCount());
                    std::fflush(stdout);
                }

                frame.clock = (frame.state == island::IslandState::Expanded)
                                  ? island::ClockStyle::Card
                                  : island::ClockStyle::Compact;

                // What the taskbar actually got, when it changed.
                //
                // "No buttons appear" cannot be diagnosed from the development machine: the sandbox
                // has its own cursor, so a real hover can never be performed here, and the preview
                // forces the state directly -- which proves the drawing works and says nothing about
                // whether the state is ever reached. This line separates the two: it reports the
                // shape the loop is drawing, how many windows were listed, and how many buttons the
                // frame was given. If the shape is not Hover, the problem is the pointer; if it is
                // Hover with zero windows, the problem is the enumeration; if the count is right and
                // nothing is on screen, the problem is the drawing.
                if (frame.taskButtonCount != lastReportedButtons ||
                    frame.state != lastReportedState) {
                    lastReportedButtons = frame.taskButtonCount;
                    lastReportedState = frame.state;
                    island::LogAndPrint("  taskbar: state=%ls, %d buttons from %d windows listed "
                                        "(%ls)",
                                        island::StateName(frame.state), frame.taskButtonCount,
                                        taskWindows.Count(), taskWindows.LastSummary());
                    std::fflush(stdout);
                }
                // The system panel is a request for the CARD, so the frame has to be told about it in
                // EVERY state -- including the card itself.
                //
                // This lived inside the `shortShape` block, which by definition EXCLUDES Expanded. The
                // result was a flag that was true at the top of the loop and never reached the frame:
                // the policy promoted the bar to the card, the renderer then checked a flag that had
                // silently stayed false, and the panel was never composed. Nothing errored, and the only
                // symptom was an empty card-shaped outline -- which is what made it expensive to find.
                // The window region LAST, once the state and the flag for this frame are both settled.
                //
                // `UpdatePointerState` applies it too, but that runs near the TOP of the loop and knows
                // only what the previous frame concluded. The region follows the animated shape, so a
                // region applied one frame late keeps the CARD clipped to the pill's height for as long
                // as the panel is open -- which is what "the panel is open but I only see its top edge"
                // was. Harmless to call again: it returns immediately when the rectangle is unchanged.
                // The strip's own telling, before the region is applied: the band's width while the line
                // is beside the deck, zero while it is inside the pill. The window needs it because the
                // region DECIDES which pixels belong to this window, so a strip outside it is clipped --
                // see SetStatusStripWidth. Keyed on the FADE rather than on the placement flag, because
                // the strip is still on screen for the length of its exit.
                window.SetStatusStripWidth(statusStripOpacity > 0.001f
                                               ? island::kStatusStripWidthDip
                                               : 0.0f);

                window.ApplyInputRegion();

                // HOW STRONG, and for WHOM. The card's own content follows the reveal; the BAR's does
                // not fade at all.
                //
                // FADED with the animation, not switched at the end of it. This was
                // `IsAnimating() ? 0.35f : 1.0f`, which is a step: the panel's own fill is 0.97 * this
                // number, so for the whole 0.36 s of the slide the card was drawn at 34% alpha -- the
                // desktop showing straight through it -- and then went to 97% in a single frame. A
                // one-frame change of that size at the end of a movement is what reads as a stutter, and
                // it was the one part of this animation that no amount of easing or duration could fix.
                //
                // The eased progress and not the linear one, because the reveal is masked by the shape
                // drawn from that same eased value: driven by the linear clock the fade would run ahead
                // of the edge and then wait for it.
                //
                // AND ONLY THE CARD'S. One number used to serve both, so entering the island with music
                // playing (which opens the music card) dimmed the deck, the icon row and the lyric line
                // to 35% and ramped them back -- the bar itself blinking for a card that grows BELOW it
                // and never touches it. Reported as "点击系统按钮 或者 播放音乐的时候 鼠标移入灵动岛时候
                // 图片中红框里的会闪一下". See ContentStrengthsFor.
                const island::ContentStrengths strengths =
                    island::ContentStrengthsFor(cardUp, window.EasedProgress());
                frame.contentOpacity = strengths.bar;
                frame.cardOpacity = strengths.card;

                // One line per frame of a card transition, when asked for.
                //
                // Everything that decides how this movement LOOKS, on one line: how long the frame took,
                // how far the curve has pushed the shape, where the card is, how solid its content is,
                // and how much of the window the region admits. "It looks janky" becomes checkable --
                // uneven intervals, a rectangle that jumps, an opacity that steps.
                if (traceCardFrames && frame.outerCardUp) {
                    const island::RoundedBox animated = frame.pill.box;
                    const float animatedHeight = animated.rect.bottom - animated.rect.top;
                    const float cardTopDip = frame.restingPill.rect.bottom + island::kCardTopGapDip;
                    const RECT applied = window.AppliedRegion();
                    island::LogAndPrint("  card dt=%5.1fms eased=%.3f h=%5.1f left=%.0f "
                                        "rect=[%.0f,%.0f %.0f,%.0f] cardOpacity=%.2f barOpacity=%.2f "
                                        "region=[%ld,%ld %ld,%ld] panel=%d state=%ls",
                                        dt * 1000.0f, window.EasedProgress(), animatedHeight,
                                        frame.cardLeftDip, frame.cardLeftDip, cardTopDip,
                                        frame.cardLeftDip + island::kExpandedWidthDip,
                                        cardTopDip + animatedHeight, frame.cardOpacity,
                                        frame.contentOpacity,
                                        applied.left, applied.top, applied.right, applied.bottom,
                                        static_cast<int>(frame.cardOwner),
                                        island::StateName(frame.state));
                    std::fflush(stdout);
                }
                // What the card shows, decided by its OWNER rather than by the state.
                //
                // `state == Expanded` was false for the whole of a retraction, so the content vanished on
                // the first frame of its own exit and the exit was invisible. The owner is held until the
                // shape is back, so the content is there to be seen leaving.
                frame.notification =
                    (frame.cardOwner == island::CardOwner::Notification && haveNotificationShown)
                        ? &notificationShown
                        : nullptr;
                // ... and the sender's icon, which is what makes the card say WHO as well as what.
                //
                // Resolved once per change rather than per frame: getting an executable's icon is a trip to
                // the shell and the disk, and a notification is on screen for six seconds -- about 360
                // frames of the same image.
                if (frame.notification) {
                    wchar_t source[260] = {};
                    if (NotificationIconSource(*frame.notification, source, 260)) {
                        if (std::wcscmp(source, notificationIconSource) != 0) {
                            wcscpy_s(notificationIconSource, 260, source);
                            notificationIconPixels.clear();
                            notificationIconWidth = 0;
                            notificationIconHeight = 0;
                            if (island::LoadNotificationIconPixels(source, &notificationIconPixels,
                                                             &notificationIconWidth,
                                                             &notificationIconHeight)) {
                                // nothing more to do: the pixels are the frame's
                            }
                            ++notificationIconRevision;
                            island::LogAndPrint(
                                L"  notification: icon from '%ls' -> %dx%d, %zu byte(s)\n", source,
                                notificationIconWidth, notificationIconHeight,
                                notificationIconPixels.size());
                            std::fflush(stdout);
                        }
                        frame.notificationIconPixels =
                            notificationIconPixels.empty() ? nullptr : notificationIconPixels.data();
                        frame.notificationIconWidth = notificationIconWidth;
                        frame.notificationIconHeight = notificationIconHeight;
                        frame.notificationIconRevision = notificationIconRevision;
                    }
                }
                // Music is offered only when the card is the media card, matching the renderer's
                // one-message-per-card rule.
                frame.music = (frame.cardOwner == island::CardOwner::Music && nowPlaying.present)
                                  ? &nowPlaying
                                  : nullptr;
                // The output's peak, for the pill's background meter. This is the reader's CACHED
                // sample -- its worker refreshes it about thirty times a second (see
                // AudioVolume::Peak) -- so this is a load rather than a trip to the audio engine.
                frame.musicLevelKnown = volume.Peak(&frame.musicLevel);
                frame.musicWavePhase = musicWavePhase;
                frame.musicAccent = D2D1::ColorF(waveAccent.r, waveAccent.g, waveAccent.b, 1.0f);
                frame.transportGlyph =
                    (nowPlaying.state == island::PlaybackState::Playing) ? ">" : "||";
                frame.lyricLine = lyricLine;
                frame.fallbackLine = fallbackLine;
                // How far through its arrival the pill's line is, and what it is replacing: 1 and
                // nothing once it has settled.
                frame.lyricLineRoll = island::LyricLineRollFor(lyricLineAge);
                frame.lyricLinePrev = replacedLyricText[0] != '\0' ? replacedLyricText : nullptr;
                frame.lyricLinePrevScrollDip = replacedLyricScrollDip;
                frame.noticeLine = noticeLine.empty() ? nullptr : noticeLine.c_str();

                // --- the CALL strip ------------------------------------------------------
                //
                // When the notification on show is WeChat's CALL one, the strip says it the way the reference
                // does: an avatar, a green running timer, and a level meter from the output's peak. The timer
                // counts from when the WATCHER saw the call begin, not from when this frame was drawn, so
                // restarting the bar in the middle of a call does not restart the count.
                if (active && wechat.CallNotificationId() != 0 && active->id == wechat.CallNotificationId()) {
                    frame.callUp = true;
                    // The avatar, looked up once per call: the icon of the process that is in it.
                    {
                        wchar_t source[260] = {};
                        if (ProcessImagePath(wechat.CallPid(), source, 260) &&
                            std::wcscmp(source, callIconSource) != 0) {
                            wcscpy_s(callIconSource, 260, source);
                            callIconPixels.clear();
                            callIconWidth = 0;
                            callIconHeight = 0;
                            if (island::LoadNotificationIconPixels(source, &callIconPixels, &callIconWidth,
                                                                   &callIconHeight)) {
                                ++callIconRevision;
                                island::LogAndPrint(
                                    L"  wechat: call icon from '%ls' -> %dx%d\n", source, callIconWidth,
                                    callIconHeight);
                                std::fflush(stdout);
                            }
                        }
                    }                    const unsigned long long startedMs = wechat.CallStartMs();
                    frame.callStrip.seconds =
                        startedMs != 0 ? static_cast<int>((GetTickCount64() - startedMs) / 1000) : 0;
                    float peak = 0.0f;
                    frame.callStrip.levelKnown = volume.Peak(&peak);
                    frame.callStrip.level = peak;
                    frame.callStrip.iconPixels =
                        callIconPixels.empty() ? nullptr : callIconPixels.data();
                    frame.callStrip.iconWidth = callIconWidth;
                    frame.callStrip.iconHeight = callIconHeight;
                    frame.callStrip.iconRevision = callIconRevision;
                }
                frame.lyricScrollDip = lyricsScrollDip;

                // --- the date, the weekday, the time and the weather --------------------------
                //
                // IS THE PILL'S OWN LINE TAKEN? That is the whole question, and it is exactly "the
                // island has something else to say": a call, a message, or a lyric. The answer decides
                // WHICH OF THE TWO HOMES the line is drawn in -- inside the pill, or beside the deck --
                // and it is asked ONCE, in StatusPlacementFor, because the one thing that must not
                // happen is the same words in both places at once.
                //
                // The three values are the ones the renderer chooses the pill's own content from, so
                // the two draw sites cannot disagree: a pill showing the lyric always has the line
                // beside it, and a pill showing the date and the time never has.
                const bool islandLineTaken =
                    (frame.restingState == island::IslandState::MusicStrip &&
                     (frame.lyricLine || frame.fallbackLine)) ||
                    (frame.state == island::IslandState::NoticeStrip &&
                     (frame.callUp || (frame.noticeLine && frame.noticeLine[0] != '\0')));
                const island::StatusPlacement placement =
                    island::StatusPlacementFor(islandLineTaken);

                statusStripOpacity =
                    island::StatusStripFadeStep(statusStripOpacity, placement.besideDeck, dt);
                frame.statusStripOpacity = statusStripOpacity;

                // The reading is COPIED into this scope's buffer rather than pointed at where the
                // worker keeps it: the worker can publish between this line and the draw, and a frame
                // that read half of one reading and half of the next would print a temperature that
                // never happened.
                statusWeatherText[0] = L'\0';
                weather.Line(statusWeatherText, 64);
                frame.statusWeather = statusWeatherText[0] != L'\0' ? statusWeatherText : nullptr;

                // Read by the next pass's redraw decision, like the icon row's own arrival flags.
                statusStripAnimating = statusStripOpacity > 0.001f && statusStripOpacity < 0.999f;

                // Progress, advanced locally between polls.
                //
                // The media session reports a position only once every couple of seconds, so
                // using it directly made the bar jump in 2-second steps. The position is
                // therefore projected forward from the moment it was received, and resynced
                // whenever a fresh one arrives. Only when the duration is known: a live
                // stream reports zero, and an empty bar would read as a bug.
                if (nowPlaying.durationMs > 0 && nowPlaying.positionMs > 0) {
                    long long projected = nowPlaying.positionMs;
                    if (nowPlaying.state == island::PlaybackState::Playing) {
                        projected += static_cast<long long>(GetTickCount64() - musicPositionAt);
                    }
                    if (projected > nowPlaying.durationMs) projected = nowPlaying.durationMs;
                    if (projected < 0) projected = 0;

                    const double fraction = static_cast<double>(projected) /
                                            static_cast<double>(nowPlaying.durationMs);
                    frame.progressFraction = static_cast<float>(fraction > 1.0 ? 1.0 : fraction);
                    // The card prints this same projected value, so the time and the bar
                    // cannot disagree.
                    frame.musicPositionMs = projected;
                }

                // The chased master volume, which the SYSTEM PANEL's slider is drawn at (see shownVolume).
                //
                // It used to feed the music card's own bar as well. The card has no volume control any more --
                // the speaker in its top-right corner was removed on request -- but the chase stays, because it
                // is what makes the panel's handle MOVE to a new level instead of appearing there, and that is
                // the fix for "调节声音的还是卡卡的，没有动画过渡".
                if (volumeAvailable) {
                    float level = 0.0f;
                    if (volume.Get(&level)) {
                        // A drag is drawn at the pointer's own value (see the panel's volume block), so the
                        // chase never lags the finger.
                        if (shownVolume < 0.0f) {
                            shownVolume = level;
                        } else {
                            shownVolume += (level - shownVolume) *
                                           (1.0f - std::exp(-dt / kVolumeChaseSeconds));
                            if (std::fabs(level - shownVolume) < 0.002f) shownVolume = level;
                        }
                    }
                }

                // Album art is decoded once per artwork revision, not per frame: the
                // decode is the expensive part and the bytes only change when the
                // track does. The blurred background is built here too -- it is a
                // second encode/decode pair, and doing it per frame would be wasteful
                // for a wash that only changes with the track.
                //
                // ... AND IT IS DECODED WHETHER OR NOT THE CARD IS UP.
                //
                // This used to be gated on `frame.music` -- "nothing is displaying it, so do not decode it" --
                // and that gate had two costs, which the user found one after the other:
                //
                //   * the pill's audio wave takes its COLOUR from this same decode, so with the card closed the
                //     strip kept the previous track's accent for the whole song. Reported as "歌词后面的 随音乐
                //     跳动的的颜色 如果鼠标不移入 就一直一个颜色" -- the colour only changed when the pointer
                //     went onto the island, because that is what opened the card, and opening the card is what
                //     ran the decode.
                //   * a cover that arrived while the card was closed had not been decoded by the time the card
                //     opened, so the card showed the placeholder for a moment and then caught up.
                //
                // One decode per track either way: the same work the card would have done a moment later.
                if (nowPlaying.artworkSize > 0) {
                    if (nowPlaying.artworkRevision != lastDecodedArtworkRevision) {
                        lastDecodedArtworkRevision = nowPlaying.artworkRevision;
                        island::DecodedImage decoded;
                        if (island::DecodeImage(nowPlaying.artwork, nowPlaying.artworkSize,
                                                &decoded)) {
                            artworkPixels = decoded.pixels;
                            artworkWidth = decoded.width;
                            artworkHeight = decoded.height;
                            // The wave's colour, taken from the same decode: once per cover, not per
                            // frame. See AccentColourFromArtwork.
                            waveAccent = island::AccentColourFromArtwork(decoded.pixels, decoded.width,
                                                                         decoded.height);
                        } else {
                            artworkPixels = nullptr;
                            artworkWidth = 0;
                            artworkHeight = 0;
                            waveAccent = island::AccentColour{};
                        }

                        // THE ONE CASE THE LINE ABOVE CANNOT TELL: bytes arrived and would not decode. A cover
                        // that never appears looks exactly like a cover that was never sent (`music: cover
                        // NONE`), and this is the line that tells the two apart.
                        island::LogAndPrint("  music: cover %ls %dx%d\n",
                                            artworkPixels ? L"decoded" : L"COULD NOT DECODE",
                                            artworkWidth, artworkHeight);
                        std::fflush(stdout);

                        // The card is 420x236 DIP; a wash at a quarter of that is
                        // plenty, since it is out of focus by construction.
                        static uint8_t backgroundPng[island::kMusicArtworkCapacity] = {};
                        const size_t backgroundSize = island::MakeBlurredBackgroundPng(
                            nowPlaying.artwork, nowPlaying.artworkSize, 105, 59,
                            backgroundPng, sizeof(backgroundPng));
                        if (backgroundSize > 0) {
                            island::DecodedImage blurred;
                            if (island::DecodeImage(backgroundPng, backgroundSize, &blurred)) {
                                backgroundPixels = blurred.pixels;
                                backgroundWidth = blurred.width;
                                backgroundHeight = blurred.height;
                            } else {
                                backgroundPixels = nullptr;
                            }
                        } else {
                            backgroundPixels = nullptr;
                        }
                    }
                    if (artworkPixels) {
                        frame.artworkPixels = artworkPixels;
                        frame.artworkWidth = artworkWidth;
                        frame.artworkHeight = artworkHeight;
                        // The renderer caches the GPU bitmap against this, so it is
                        // uploaded once per track rather than once per frame.
                        frame.artworkRevision = nowPlaying.artworkRevision;
                    }
                    if (backgroundPixels) {
                        frame.backgroundPixels = backgroundPixels;
                        frame.backgroundWidth = backgroundWidth;
                        frame.backgroundHeight = backgroundHeight;
                        // Derived from the artwork revision: the wash is a function of the
                        // same cover, so it changes exactly when the cover does.
                        frame.backgroundRevision = nowPlaying.artworkRevision;
                        // The card's own fill has to become translucent or the wash
                        // would be hidden behind it -- and only the CARD's: with no card up this would change
                        // the island's own fill, which is the one thing the wash has nothing to do with.
                        if (frame.music) {
                            frame.pill.fill = D2D1::ColorF(0.04f, 0.05f, 0.08f, 0.98f);
                        }
                    }
                } else if (nowPlaying.artworkRevision == 0 && lastDecodedArtworkRevision != 0) {
                    // THE COVER BELONGS TO THE TRACK, SO DROP IT WHEN THE TRACK HAS NONE.
                    //
                    // Everything the block above produces lives on in `artworkPixels` and `backgroundPixels`
                    // -- file-scope pointers the renderer caches its GPU copies against -- and that block only
                    // runs while the session HAS artwork. So a track that starts without any kept drawing the
                    // PREVIOUS track's cover: reported as "歌曲切换了 但是封面图没有换", which is the opposite
                    // of the complaint before it (an empty slot) and the same root cause seen from the other
                    // side.
                    //
                    // A revision of 0 is the session saying "this track has no cover yet", and the retry in
                    // MusicSession keeps asking for one -- when it arrives the revision goes to 1 and the
                    // decode above takes over. Until then the card shows the PLACEHOLDER (see DrawMusicCard),
                    // not someone else's album art.
                    lastDecodedArtworkRevision = 0;
                    artworkPixels = nullptr;
                    artworkWidth = 0;
                    artworkHeight = 0;
                    backgroundPixels = nullptr;
                    backgroundWidth = 0;
                    backgroundHeight = 0;
                    waveAccent = island::AccentColour{};
                    island::LogAndPrint("  music: cover dropped (the new track has none yet)\n");
                    std::fflush(stdout);
                }

                // The source application's icon, decoded once per icon revision. It is not
                // inside the artwork block: it does not depend on the album art at all, and it
                // must still show for a session that publishes no cover.
                if (nowPlaying.iconSize > 0 &&
                    nowPlaying.iconRevision != lastDecodedIconRevision) {
                    lastDecodedIconRevision = nowPlaying.iconRevision;
                    island::DecodedImage decodedIcon;
                    if (island::DecodeImage(nowPlaying.icon, nowPlaying.iconSize,
                                            &decodedIcon)) {
                        iconPixels = decodedIcon.pixels;
                        iconWidth = decodedIcon.width;
                        iconHeight = decodedIcon.height;
                    } else {
                        iconPixels = nullptr;
                    }
                }
                if (iconPixels) {
                    frame.iconPixels = iconPixels;
                    frame.iconWidth = iconWidth;
                    frame.iconHeight = iconHeight;
                    frame.iconRevision = nowPlaying.iconRevision;
                }
                // Reported whenever the answer changes, so "the icon is missing" splits into: no
                // bytes, bytes that will not decode, or bytes that decode and are never drawn.
                // Those have three different fixes, and from the screen they look identical.
                if (iconWidth != lastReportedIconWidth ||
                    (iconPixels != nullptr) != iconWasDecoded) {
                    lastReportedIconWidth = iconWidth;
                    iconWasDecoded = iconPixels != nullptr;
                    island::LogAndPrint("  icon decode: %s, %dx%d, rev %llu",
                                        iconPixels ? "ok" : "NO PIXELS", iconWidth, iconHeight,
                                        nowPlaying.iconRevision);
                }

                renderer.Render(frame);

                // What the page looked like, recorded AFTER the draw: these are the values the next
                // iteration's redraw decision compares against, so recording them before the draw would
                // swallow the very change that asked for it.
                lastSoundPageOpen = soundPageOpen;
                lastSoundScroll = window.SoundPageScroll();
            }

            // Log the settled shape once per change. Without this, "the card never
            // appears" is indistinguishable from "the pointer was never detected", and
            // the two need completely different fixes -- it took adding exactly this line
            // to find that a settled hover never promoted to the card.
            if (window.State() != lastLogged && !window.IsAnimating()) {
                lastLogged = window.State();
                island::LogAndPrint("  state: %ls (pointer inside=%d, resting=%ls)\n",
                                    island::StateName(lastLogged),
                                    window.IsPointerInside() ? 1 : 0,
                                    island::StateName(resting));
                std::fflush(stdout);
            }

            // Report the frame-interval tail and what forced each redraw, LIVE, every 4 s.
            //
            // Kept in the shipping path on purpose. It says three things nothing else does:
            // whether the loop hiccups (the worst interval and the >50 ms bucket), which term
            // keeps the draw condition true, and how many frames were drawn. An earlier round
            // optimised the DRAW cost for two attempts while this kind of data showed the draw
            // was 1.3 ms and the loop was not hitching at all -- measuring the wrong quantity
            // is how a fix gets reported that changes nothing. A stutter is a maximum, so the
            // worst value matters more than the average.
            liveStats.Record();
            if (GetTickCount64() - lastIntervalReport >= 4000) {
                lastIntervalReport = GetTickCount64();
                wchar_t intervalSummary[256] = {};
                liveStats.Describe(intervalSummary, 256);
                island::LogAndPrint("  interval [%ls] %ls", island::StateName(window.State()),
                                    intervalSummary);
                island::LogAndPrint("  redraws: animating=%d clock=%d notify=%d music=%d "
                                    "lyrics=%d progress=%d strip=%d first=%d",
                                    whyAnimating, whyClock, whyNotification, whyMusic,
                                    whyLyrics, whyProgress, whyStatusStrip, whyFirst);
                // Reported beside the frame intervals because the two are the same question: a region
                // re-applied per frame is a window-manager call inside the animation's own loop. A
                // hundred of them in a 4 s window containing one 0.36 s transition means the transition
                // paid for them, and "one per state change" means it did not.
                {
                    const unsigned long long applied = window.RegionApplications();
                    island::LogAndPrint("  window region: %llu applied in total (%llu in the last 4 s)",
                                        applied, applied - lastRegionApplications);
                    lastRegionApplications = applied;
                }
                std::fflush(stdout);
                liveStats.Reset();
                whyAnimating = whyClock = whyNotification = whyMusic = 0;
                whyLyrics = whyProgress = whyFirst = whyStatusStrip = 0;
            }

            // --- pace ---
            //
            // While animating, wait for the next compositor tick (or a short
            // message wait as a fallback). When idle, block on input instead of
            // spinning: the clock only needs a wake-up once a second, so a 100 ms
            // cap is plenty and costs nothing on an idle desktop.
            if (window.IsAnimating()) {
                if (clockDriven) {
                    renderer.WaitForCompositorClock(8);
                } else {
                    ::MsgWaitForMultipleObjectsEx(0, nullptr, 4, QS_ALLINPUT,
                                                  MWMO_INPUTAVAILABLE);
                }
            } else {
                ::MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
            }
        }
    }

    renderer.SetTextRenderer(nullptr);
    text.Shutdown();
    // Stop the listener before tearing the window down: a client mid-send should be
    // finished with, not left holding a pipe that is about to disappear.
    notificationServer.Stop();
    lyrics.Stop();
    weather.Stop();
    volume.Stop();
    music.Stop();

    // Give the reserved strip back while the window still exists.
    //
    // The destructor would do this, but releasing here makes the order explicit and puts the
    // result in the log: a work area left inset after the program exits is a permanent gap on
    // the desktop that looks like a broken system rather than a program that failed to clean up.
    // Doing it before window.Destroy() means the shell still has a live HWND to talk to.
    if (appBar.IsRegistered()) {
        appBar.Release();
    }

    window.Destroy();

    // Put the shell's taskbar back before exiting. This runs on every normal exit
    // path -- the window closing, WM_QUIT, or the loop breaking for any other
    // reason. Only a hard kill skips it, which is why the recovery command is
    // documented in the startup message.
    if (hideNativeTaskbar && haveTaskbar) {
        taskbar.Restore();
        island::LogAndPrint("  native taskbar: restored\n");
        std::fflush(stdout);
    }

    // Its handles, on the same path. Closing the WLAN client here rather than leaving it to process
    // teardown matters because the shell's wireless service keeps a per-client context, and a bar
    // that is started and stopped repeatedly would accumulate them.
    statusReader.Close();
    return 0;
}

// Frame-to-frame intervals, so a stutter can be seen as a number.
//
// Per-frame DRAW time is the wrong instrument for this: it measures how long one frame takes
// to paint, while a stutter is a long GAP between frames. Two rounds of optimising the draw
// cost therefore missed a hitch that was never in the drawing. This records the interval
// between frames and reports the tail, because a hitch is a maximum and averaging hides it.

// Retrieves a command-line value as UTF-8.
//
// NOT from `argv`. On Windows `argv` gives each argument converted to the process ANSI code
// page, so a Chinese song title arrives as GBK bytes -- and percent-encoding those produces
// `%B9%A9%CF%FA...`, which is what a search endpoint was actually sent while it expected
// `%E4%BE%9B...` in UTF-8. Every Chinese query was malformed, which is exactly the kind of
// failure that looks like "the service has no lyrics for this track".
//
// `CommandLineToArgvW` returns the real wide characters, and the rest of the program works
// in UTF-8, so the conversion happens here, once. ASCII flags are unaffected either way.
bool ArgumentAsUtf8(const wchar_t* argument, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';
    if (!argument) return false;
    if (WideCharToMultiByte(CP_UTF8, 0, argument, -1, out, static_cast<int>(capacity),
                            nullptr, nullptr) <= 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

// Writes the decoded form of an app id's icon to a PNG, so the pixels can be looked at.
//
// "The icon does not appear" has several causes that a screenshot cannot separate: bytes that
// never arrive, bytes that decode to something transparent, and bytes that decode correctly and
// are never drawn. This makes the decoded pixels themselves inspectable, which is the one thing
// the card cannot show.
int RunIconDump(const char* appId) {
    if (!appId || appId[0] == '\0') {
        island::LogAndPrint("icon dump: an app id is required, e.g. QQMusic.exe");
        return 2;
    }

    wchar_t wideAppId[512] = {};
    if (!ArgumentAsUtf8ToWide(appId, wideAppId, 512)) {
        island::LogAndPrint("icon dump: the app id could not be read");
        return 2;
    }

    wchar_t resolved[MAX_PATH] = {};
    if (!island::ResolveAppIconPath(wideAppId, resolved, MAX_PATH)) {
        island::LogAndPrint("icon dump: '%s' did not resolve to an executable", appId);
        return 1;
    }
    island::LogAndPrint("icon dump: resolved to %ls", resolved);

    uint8_t png[island::kMusicArtworkCapacity] = {};
    const size_t pngSize = island::FetchAppIconForTest(resolved, png, sizeof(png));
    island::LogAndPrint("icon dump: encoded %zu bytes of PNG", pngSize);
    if (pngSize == 0) return 1;

    island::DecodedImage decoded;
    if (!island::DecodeImage(png, pngSize, &decoded)) {
        island::LogAndPrint("icon dump: the PNG did not decode");
        return 1;
    }

    // What the pixels actually contain. A fully transparent or all-black decode looks exactly
    // like "no icon" on screen, and this is what tells the difference.
    size_t opaque = 0;
    uint64_t alphaSum = 0;
    uint8_t maxAlpha = 0;
    for (int i = 0; i < decoded.width * decoded.height; ++i) {
        const uint8_t alpha = decoded.pixels[static_cast<size_t>(i) * 4 + 3];
        alphaSum += alpha;
        if (alpha > maxAlpha) maxAlpha = alpha;
        if (alpha > 8) ++opaque;
    }
    const size_t total = static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height);
    island::LogAndPrint("icon dump: decoded %dx%d, %zu of %zu pixels visible, max alpha %u, "
                        "mean alpha %.1f",
                        decoded.width, decoded.height, opaque, total,
                        static_cast<unsigned>(maxAlpha),
                        total ? static_cast<double>(alphaSum) / static_cast<double>(total)
                              : 0.0);

    // Written next to the log, so it is found the same way the log is.
    wchar_t outPath[MAX_PATH] = {};
    const wchar_t* logPath = island::LogPath();
    wcsncpy_s(outPath, MAX_PATH, logPath, _TRUNCATE);
    wchar_t* dot = wcsrchr(outPath, L'.');
    if (dot) *dot = L'\0';
    wcscat_s(outPath, MAX_PATH, L"-icon.png");

    const size_t written = island::EncodeBgraToPng(decoded.pixels, decoded.width,
                                                   decoded.height, png, sizeof(png));
    if (written == 0) {
        island::LogAndPrint("icon dump: could not re-encode the decoded pixels");
        return 1;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, outPath, L"wb") != 0 || !file) {
        island::LogAndPrint("icon dump: could not write %ls", outPath);
        return 1;
    }
    // The encoder reports the buffer capacity rather than its output length, so the PNG end
    // marker is what says where the image stops; the decoder stops there too.
    fwrite(png, 1, written, file);
    fclose(file);
    island::LogAndPrint("icon dump: wrote %ls (%zu bytes)", outPath, written);
    return 0;
}

// Reports what the bar would put on itself as buttons.
//
// The filtering rules are covered by the self-test, but a rule only decides about a window that has
// been DESCRIBED correctly, and the describing is done by a dozen Win32 calls that no test can
// exercise without a desktop full of real windows. This prints the result of those calls, so the
// two halves can be told apart: a wrong list means either the rules are wrong or the descriptions
// are, and this says which.
int RunWindowList(bool activate) {
    island::WindowList windows;
    const int count = windows.Refresh();

    island::LogAndPrint("IslandBar window list\n");
    island::LogAndPrint("-----------------------------------------------\n");
    island::LogAndPrint("  %ls\n", windows.LastSummary());
    island::LogAndPrint("  foreground: %p\n", reinterpret_cast<void*>(windows.ForegroundHandle()));

    if (count == 0) {
        island::LogAndPrint("\n  Nothing would be listed. If application windows ARE open, the\n");
        island::LogAndPrint("  DESCRIPTIONS are wrong rather than the rules.\n");
        return 0;
    }

    island::LogAndPrint("\n  these would appear as buttons, front to back:\n");
    for (int i = 0; i < count; ++i) {
        const island::TaskEntry& entry = windows.At(i);
        // Narrowed for the log: window titles are UTF-16 and the log is UTF-8.
        char title[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, entry.title, -1, title, sizeof(title) - 1, nullptr,
                            nullptr);
        island::LogAndPrint("    %2d. pid %-6lu %-6s %s\n", i + 1,
                            static_cast<unsigned long>(entry.processId),
                            entry.iconic ? "[min]" : "", title);
    }

    // Activating is the half no unit test can reach: it changes the desktop, so it can only be
    // judged by looking at it. Kept behind a flag and done last.
    if (activate) {
        const HWND current = windows.ForegroundHandle();
        int index = -1;
        for (int i = 0; i < count && index < 0; ++i) {
            if (windows.At(i).handle != current) index = i;
        }
        if (index < 0) {
            island::LogAndPrint("\n  nothing to activate\n");
            return 0;
        }

        const island::TaskEntry& target = windows.At(index);
        island::LogAndPrint("\n  activating entry %d (pid %lu, %sminimised)...\n", index + 1,
                            static_cast<unsigned long>(target.processId),
                            target.iconic ? "" : "not ");
        const bool raised = windows.Activate(target.handle);
        island::LogAndPrint("  activate %s\n", raised ? "SUCCEEDED" : "was REFUSED by Windows");
        if (!raised) {
            // SetForegroundWindow is allowed to fail and does so silently. Windows restricts who
            // may take the foreground to stop programs stealing focus; a real taskbar click
            // qualifies, a background shell may not.
            island::LogAndPrint("    Windows only lets a process take the foreground under the "
                                "conditions meant\n");
            island::LogAndPrint("    to stop programs stealing focus. A taskbar click qualifies; "
                                "a command\n");
            island::LogAndPrint("    run from another process may not.\n");
            return 1;
        }
    }
    return 0;
}

// Exercises the volume control: reads it, changes it, reads it back, restores it.
//
// Reads the newest toasts out of Windows' own notification database and prints them.
//
// A command line rather than only a card on screen, because this channel's evidence has to be obtainable
// without a screenshot: "no toasts appeared" and "the database was not read" look identical on a bar, and
// they are different problems.
int RunToastDump(int limit) {
    if (limit <= 0) limit = 10;

    island::ToastReader reader;
    if (!reader.Start()) {
        island::LogAndPrint("toast dump: the system toast database is not readable\n");
        return 1;
    }

    std::vector<wchar_t> text(64 * 1024);
    const int rows = reader.Dump(limit, text.data(), text.size());
    // The WIDE overload: the dump is full of Chinese message text, and the narrow one would print it as
    // byte-widened mojibake -- the same trap that once swallowed a tile's own log line.
    island::LogAndPrint(L"toast dump: %d of the newest %d toast(s) in %ls\n", rows, limit,
                        reader.Available() ? L"the live database" : L"nowhere");
    if (rows > 0) {
        island::LogAndPrint(L"%ls", text.data());
    }
    reader.Stop();
    return rows > 0 ? 0 : 1;
}

// Sets the system output volume to a percentage and reports what the device says afterwards.
//
// A command line rather than a click, because the click cannot be delivered from every context: the bar
// runs as an overlay and a script's synthetic mouse input is dropped whenever the session is not in the
// state the input assumes (locked, asleep, another desktop). The volume itself, though, can always be set
// -- and a test that changes the machine's own setting has to be able to put it back afterwards.
int RunVolumeSet(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    island::AudioVolume volume;
    if (!volume.Start()) {
        island::LogAndPrint("volume set: no audio endpoint available\n");
        return 1;
    }

    float before = 0.0f;
    const bool hadBefore = volume.Get(&before);
    volume.SetLevel(static_cast<float>(percent) / 100.0f);

    // The write goes through the worker, so the level is read back rather than assumed: this reports what
    // the DEVICE ended up at, which is the only claim worth making about someone else's machine.
    float after = 0.0f;
    for (int i = 0; i < 200; ++i) {
        Sleep(10);
        if (volume.Get(&after) && std::fabs(after - static_cast<float>(percent) / 100.0f) < 0.011f) {
            break;
        }
    }
    island::LogAndPrint("volume set: %s%.0f%% -> %.0f%% (asked for %d%%)\n",
                        hadBefore ? "" : "unknown -> ", hadBefore ? before * 100.0f : 0.0f,
                        after * 100.0f, percent);
    return 0;
}

// The card's volume control is the one feature whose effect is on the AUDIO DEVICE rather than
// on the screen, so no screenshot can show that it works. This does the round trip and reports
// both ends, which is the only way to tell "the bar moved" apart from "the volume actually
// changed".
int RunVolumeTest() {
    island::AudioVolume volume;
    if (!volume.Start()) {
        island::LogAndPrint("volume test: no audio endpoint available");
        return 1;
    }

    float original = 0.0f;
    if (!volume.Get(&original)) {
        island::LogAndPrint("volume test: the endpoint gave no level");
        volume.Stop();
        return 1;
    }
    island::LogAndPrint("volume test: read %.0f%% (muted=%d)", original * 100.0f,
                        volume.IsMuted() ? 1 : 0);

    int failures = 0;

    // Step down, then up, checking the DEVICE agrees each time rather than trusting the value
    // this process happens to hold.
    struct Step {
        float level;
        const char* label;
    };
    const Step steps[] = {
        {0.25f, "set to 25%"},
        {0.75f, "set to 75%"},
    };
    for (const Step& step : steps) {
        volume.SetLevel(step.level);
        Sleep(900);   // long enough for the worker to write and read back

        float read = 0.0f;
        volume.Get(&read);
        island::LogAndPrint("volume test: %s -> device reports %.0f%%", step.label,
                            read * 100.0f);
        if (std::fabs(read - step.level) > 0.05f) ++failures;
    }

    // Mute, and confirm the device flipped.
    const bool mutedBefore = volume.IsMuted();
    volume.ToggleMute();
    Sleep(900);
    const bool mutedAfter = volume.IsMuted();
    island::LogAndPrint("volume test: mute %d -> %d", mutedBefore ? 1 : 0, mutedAfter ? 1 : 0);
    if (mutedAfter == mutedBefore) ++failures;
    volume.ToggleMute();   // put mute back before restoring the level
    Sleep(400);

    // Restore: a diagnostic must not leave the machine's volume somewhere else.
    volume.SetLevel(original);
    Sleep(900);
    float restored = 0.0f;
    volume.Get(&restored);
    island::LogAndPrint("volume test: restored to %.0f%% (was %.0f%%)", restored * 100.0f,
                        original * 100.0f);
    if (std::fabs(restored - original) > 0.05f) ++failures;

    volume.Stop();
    island::LogAndPrint("volume test: %s", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// Loads the source application's icon exactly as the live path does, for the preview.
//
// The preview has no media session to read one from, and a card drawn with the fallback dot
// would not show whether the icon pipeline works at all -- which is the thing being checked.
// The executable is passed in so the preview can use a real player's icon.
size_t MakeSampleAppIcon(const wchar_t* executablePath, uint8_t* out, size_t capacity) {
    return island::FetchAppIconForTest(executablePath, out, capacity);
}


// Sends one notification and exits -- the client half of the push channel.
//
// Having this in the same binary means the channel can be exercised end to end
// with no external tooling: run the bar, then run `island_bar.exe --notify ...`.
// It also gives other programs a ready-made sender.
//
// Flags: --notify <title> [--body <text>] [--app <name>] [--urgency low|normal|high]
//        [--sticky]
int RunNotify(int argc, char** argv) {
    island::Notification notification;

    // THE TEXT ARGUMENTS COME FROM THE WIDE COMMAND LINE, not from `argv`.
    //
    // `main(int, char**)` hands out the command line in the CONSOLE'S ANSI CODE PAGE, so on this machine a
    // Chinese title arrives as GBK bytes -- eight of them for four characters -- and every one of them is an
    // invalid UTF-8 sequence by the time the card tries to draw it. Measured: the notice strip showed
    // `DSH ·` followed by EIGHT boxes, which is what eight undecypherable bytes look like. The same message
    // raised through the notification platform rendered perfectly, which is how the text was cleared and the
    // drawing acquitted.
    //
    // Reading the command line again with CommandLineToArgvW gives the real characters, and the program is
    // UTF-8 everywhere else, so the conversion happens once, here.
    int wideArgc = 0;
    wchar_t** wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    const auto wideValueOf = [&](const char* flag, wchar_t* out, size_t capacity) -> bool {
        if (!out || capacity == 0) return false;
        out[0] = L'\0';
        if (!wideArgv) return false;
        for (int i = 1; i + 1 < wideArgc; ++i) {
            wchar_t wideFlag[32] = {};
            if (MultiByteToWideChar(CP_UTF8, 0, flag, -1, wideFlag, 32) <= 0) continue;
            if (std::wcscmp(wideArgv[i], wideFlag) == 0) {
                wcscpy_s(out, capacity, wideArgv[i + 1]);
                return true;
            }
        }
        return false;
    };
    // UTF-8 out, so the rest of the pipeline (the JSON wire format and the store) is unchanged.
    const auto wideValueAsUtf8 = [&](const char* flag, char* out, size_t capacity) -> bool {
        if (!out || capacity == 0) return false;
        out[0] = '\0';
        wchar_t wide[512] = {};
        if (!wideValueOf(flag, wide, 512)) return false;
        return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(capacity), nullptr,
                                   nullptr) > 0;
    };

    const auto valueOf = [&](const char* flag) -> const char* {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
        }
        return nullptr;
    };

    // The title is the first bare argument after --notify that is not a flag.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--notify") == 0 && i + 1 < argc) {
            if (!wideValueAsUtf8("--notify", notification.title, island::kTitleCapacity)) {
                strncpy_s(notification.title, island::kTitleCapacity, argv[i + 1], _TRUNCATE);
            }
            break;
        }
    }

    if (const char* body = valueOf("--body")) {
        if (!wideValueAsUtf8("--body", notification.body, island::kBodyCapacity)) {
            strncpy_s(notification.body, island::kBodyCapacity, body, _TRUNCATE);
        }
    }
    if (const char* app = valueOf("--app")) {
        if (!wideValueAsUtf8("--app", notification.app, island::kAppNameCapacity)) {
            strncpy_s(notification.app, island::kAppNameCapacity, app, _TRUNCATE);
        }
    }
    // The icon is optional on purpose: with none, the card draws the SENDER's own executable icon, because
    // the pipe says who connected. --icon is for a sender that wants a particular picture instead.
    if (const char* icon = valueOf("--icon")) {
        strncpy_s(notification.iconPath, 260, icon, _TRUNCATE);
    }
    if (valueOf("--sticky")) {
        notification.sticky = true;
    }
    if (const char* urgency = valueOf("--urgency")) {
        if (_stricmp(urgency, "high") == 0) notification.urgency = island::Urgency::High;
        else if (_stricmp(urgency, "low") == 0) notification.urgency = island::Urgency::Low;
    }
    // A caller that only supplies a body is still legitimate.
    if (notification.title[0] == '\0') {
        if (const char* body = valueOf("--body")) {
            strncpy_s(notification.title, island::kTitleCapacity, body, _TRUNCATE);
            notification.body[0] = '\0';
        }
    }

    if (notification.title[0] == '\0' && notification.body[0] == '\0') {
        island::LogAndPrint("usage: island_bar.exe --notify <title> [--body <text>] "
                            "[--app <name>] [--icon <path>] [--urgency low|normal|high] [--sticky]");
        if (wideArgv) LocalFree(wideArgv);
        return 2;
    }

    char response[512] = {};
    const bool sent = island::SendNotification(notification, island::kPipeName,
                                               response, sizeof(response));
    if (sent) {
        island::LogAndPrint("sent: %s", response[0] ? response : "(no response body)");
        if (wideArgv) LocalFree(wideArgv);
        return 0;
    }

    island::LogAndPrint("could not reach the bar on \\\\.\\pipe\\%ls: %s",
                        island::kPipeName,
                        response[0] ? response : "(no reason reported)");
    if (wideArgv) LocalFree(wideArgv);
    return 1;
}

// Builds a small PNG in memory for the music preview.
//
// The preview must exercise the real artwork path -- encode, decode through WIC,
// draw through Direct2D -- rather than a special case that skips it. Generating the
// image at runtime does that without shipping a test file.
//
// The pattern is a diagonal gradient with a dark block in it, so a decode that
// mangled stride, channel order or orientation would be visible rather than
// plausible-looking.
size_t MakeSampleArtworkPng(uint8_t* out, size_t capacity) {
    constexpr int kSize = 128;
    std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4);

    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const size_t index = (static_cast<size_t>(y) * kSize + x) * 4;
            // BGRA, premultiplied (alpha is 255, so premultiplied == straight).
            pixels[index + 0] = static_cast<uint8_t>(60 + (x * 140) / kSize);
            pixels[index + 1] = static_cast<uint8_t>(40 + (y * 120) / kSize);
            pixels[index + 2] = static_cast<uint8_t>(180 - (x * 90) / kSize);
            pixels[index + 3] = 255;

            if (x > 20 && x < 56 && y > 20 && y < 56) {
                pixels[index + 0] = 20;
                pixels[index + 1] = 20;
                pixels[index + 2] = 20;
            }
        }
    }

    return island::EncodeBgraToPng(pixels.data(), kSize, kSize, out, capacity);
}

// Shows the four island shapes in order with sample data, then exits.
//
// This exists because the shapes cannot be reached on demand from a test -- they depend
// on what the system media session reports and where the pointer is -- and because a
// pointer cannot always be moved: this sandbox refuses SetCursorPos (error 203), so a
// hover CANNOT be reproduced by driving the mouse. What this does instead is drive the
// state machine directly and, on every stage, print the two facts that decide whether a
// real hover would work:
//
//   * where the pointer actually is, and
//   * whether the window would accept it there (ShouldAcceptPointer).
//
// A stage that reports "would accept: no" is a stage the user could never hover, which
// is exactly the class of bug that makes a card appear unreachable.
int RunMusicPreview() {
    island::MonitorGeometry monitor = island::QueryMonitor(nullptr);

    island::IslandWindow window;
    if (!window.Create(monitor, /*show=*/true)) return 1;

    const RECT rect = window.WindowRect();
    island::Renderer renderer;
    if (!renderer.Initialize(window.Handle(),
                             static_cast<UINT>(rect.right - rect.left),
                             static_cast<UINT>(rect.bottom - rect.top),
                             window.Scale())) {
        window.Destroy();
        return 1;
    }

    island::TextRenderer text;
    if (text.Initialize()) renderer.SetTextRenderer(&text);

    island::MusicInfo sample;
    sample.present = true;
    sample.state = island::PlaybackState::Playing;
    // The track from the reference image, so the preview can be compared with it
    // directly rather than approximated.
    strncpy_s(sample.title, island::kMusicTitleCapacity, "\xe4\xb8\x9c\xe5\xa4\xa7\xe8\xa1\x97",
              _TRUNCATE);
    strncpy_s(sample.artist, island::kMusicArtistCapacity,
              "\xe5\xbc\xa0\xe8\xb6\x85\xe6\xb4\x8b", _TRUNCATE);
    strncpy_s(sample.album, island::kMusicAlbumCapacity,
              "\xe4\xb8\x9c\xe5\xa4\xa7\xe8\xa1\x97", _TRUNCATE);
    strncpy_s(sample.app, island::kMusicAppCapacity, "QQ\xe9\x9f\xb3\xe4\xb9\x90", _TRUNCATE);

    // The times from the reference image too, so the card can be held next to it.
    sample.positionMs = 93000;
    sample.durationMs = 340000;

    // Real artwork, built in memory and passed through the same WIC decode the live
    // path uses. A preview that skipped the decode would not be testing anything.
    sample.artworkSize = MakeSampleArtworkPng(sample.artwork, island::kMusicArtworkCapacity);
    if (sample.artworkSize > 0) sample.artworkRevision = 1;

    island::DecodedImage artwork;
    const bool haveArtwork =
        sample.artworkSize > 0 &&
        island::DecodeImage(sample.artwork, sample.artworkSize, &artwork);

    // The source application's icon, loaded through the SAME function the live path uses.
    //
    // A preview drawn with the fallback dot would not show whether the icon path works at all,
    // which is the thing being checked here. The real player's executable is preferred; if it is
    // not installed, this executable's own icon is used, so the path is still exercised.
    {
        wchar_t playerPath[MAX_PATH] = {};
        // Through the SAME lookup the live path uses, so this exercises App Paths and the
        // install-root search rather than a path that only exists on one machine.
        const bool havePlayer = island::ResolveAppIconPath(L"QQMusic.exe", playerPath, MAX_PATH);
        if (havePlayer) {
            sample.iconSize = island::FetchAppIconForTest(playerPath, sample.icon,
                                                          island::kMusicArtworkCapacity);
        }
        if (sample.iconSize == 0) {
            wchar_t selfPath[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH)) {
                sample.iconSize = island::FetchAppIconForTest(selfPath, sample.icon,
                                                              island::kMusicArtworkCapacity);
            }
        }
        if (sample.iconSize > 0) sample.iconRevision = 1;
        island::LogAndPrint("music preview: source icon %s (%zu bytes)",
                            sample.iconSize > 0 ? "loaded" : "NOT loaded", sample.iconSize);
        // Which icon is actually on screen: the player's, or this program's as a fallback.
        island::LogAndPrint("music preview: icon resolved to %ls",
                            havePlayer ? playerPath : L"(fallback: this executable)");
    }

    // The background wash, through the same blur the live path uses.
    static uint8_t backgroundPng[island::kMusicArtworkCapacity] = {};
    const size_t backgroundSize = island::MakeBlurredBackgroundPng(
        sample.artwork, sample.artworkSize, 105, 59, backgroundPng, sizeof(backgroundPng));
    island::DecodedImage iconImage;
    const bool haveIcon =
        sample.iconSize > 0 &&
        island::DecodeImage(sample.icon, sample.iconSize, &iconImage);

    island::DecodedImage background;
    const bool haveBackground =
        backgroundSize > 0 && island::DecodeImage(backgroundPng, backgroundSize, &background);

    island::LogAndPrint("music preview: five shapes, 4 seconds each "
                        "(pill, strip+lyric, strip+title, hover, card; decoded %s, wash %s)",
                        haveArtwork ? "yes" : "no", haveBackground ? "yes" : "no");

    // A lyric long enough to need scrolling, so the marquee is exercised rather than
    // sitting still in the middle of the strip.
    const char* lyric =
        "\xe6\x88\x91\xe6\x83\xb3\xe8\xa6\x81\xe5\xb8\xa6\xe7\x9d\x80\xe4\xbd\xa0"
        "\xe5\x8e\xbb\xe6\x88\x91\xe7\x9a\x84\xe5\xb0\x8f\xe5\x9f\x8e\xe5\xb8\x82"
        "\xe7\x9c\x8b\xe6\x97\xa5\xe8\x90\xbd\xe7\x9a\x84\xe6\x97\xb6\xe5\x80\x99";

    // The shapes the user sees, in order: the plain pill, the lyric strip with actual
    // lyrics, the strip falling back to the track NAME (which is what shows when no
    // lyrics arrive -- reported as rendering only a single character), the plain hover
    // shape, and the details card.
    struct Stage {
        island::IslandState state;
        const char* name;
        bool stripShowsFallback;   // strip only: show the title instead of the lyric
    };
    const Stage stages[5] = {
        {island::IslandState::Collapsed, "resting pill", false},
        {island::IslandState::MusicStrip, "strip + lyric", false},
        {island::IslandState::MusicStrip, "strip + title", true},
        {island::IslandState::Hover, "hover", false},
        {island::IslandState::Expanded, "card", false},
    };

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER previous = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    FrameStats frameStats;
    frameStats.Start();

    float scrollDip = 0.0f;
    for (const Stage& stage : stages) {
        window.SetState(stage.state);
        // Let the transition settle before reporting and photographing the stage.
        for (int i = 0; i < 40; ++i) {
            if (!window.PumpMessages()) break;
            window.Tick(0.02f);
            ::MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        // Report whether this shape could actually be hovered by a real pointer, using
        // the same predicate the window uses. A "no" here means the user cannot reach
        // the shape, which is the bug class this preview exists to catch.
        POINT cursor = {};
        GetCursorPos(&cursor);
        const bool acceptsCursor =
            island::IslandWindow::ShouldAcceptPointer(monitor, window.WindowRect(), cursor,
                                                      window.State());
        // Probed at the shape's own centre as well as at the physical cursor: the cursor
        // position only says "the pointer is elsewhere", while the centre says whether the
        // shape is reachable at all. Conflating the two is how "the pointer was never over
        // it" gets mistaken for "unreachable".
        const island::RoundedBox box = window.CurrentBox();
        const RECT windowRect = window.WindowRect();
        const POINT centre = {
            static_cast<LONG>((box.rect.left + box.rect.right) * 0.5f) + windowRect.left,
            static_cast<LONG>((box.rect.top + box.rect.bottom) * 0.5f) + windowRect.top};
        const bool acceptsCentre =
            island::IslandWindow::ShouldAcceptPointer(monitor, windowRect, centre,
                                                      window.State());
        island::LogAndPrint(
            "  music preview: %-13s box x=[%.0f..%.0f] y=[%.0f..%.0f] | "
            "centre (%ld,%ld) accepts=%s | pointer (%ld,%ld) accepts=%s",
            stage.name, box.rect.left, box.rect.right, box.rect.top, box.rect.bottom,
            centre.x, centre.y, acceptsCentre ? "yes" : "NO", cursor.x, cursor.y,
            acceptsCursor ? "yes" : "no");
        std::fflush(stdout);

        // How long a card frame takes to draw, reported once at the end.
        //
        // The card is the only shape that uploads images, and "hovering it feels laggy"
        // was the report. A number makes that falsifiable: the fix caches the GPU bitmaps
        // instead of rebuilding them per frame, and this says by how much.
        renderer.ResetFrameStats();

        frameStats.Reset();

        const DWORD stageStart = GetTickCount();
        while (GetTickCount() - stageStart < 4000) {
            if (!window.PumpMessages()) break;

            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            const double dt = static_cast<double>(now.QuadPart - previous.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
            previous = now;
            const float step = static_cast<float>(dt > 0.1 ? 0.1 : dt);
            window.Tick(step);
            // The window's own region, per frame -- and this is why the card never appeared here before.
            //
            // The region is what CLIPS the window, and the live loop applies it every frame for exactly
            // this reason (its comment says so): a region applied once at the state change keeps the card
            // clipped to the pill's height for as long as the card is open, so the card is composed and
            // then cut off. Harmless to call when nothing moved -- it returns immediately.
            window.ApplyInputRegion();

            // Advance the marquee so the strip demonstrably scrolls.
            scrollDip += step * 26.0f;

            island::FrameState frame;
            frame.pill.box = window.CurrentBox();
            frame.state = window.State();
            frame.clock = (frame.state == island::IslandState::Expanded)
                              ? island::ClockStyle::Card
                              : island::ClockStyle::Compact;
            // The card fades, the bar does not -- see ContentStrengthsFor.
            frame.cardOpacity = window.IsAnimating() ? 0.5f : 1.0f;
            // WHAT THE BAR SHOWS AT REST, which is the field the pill's own line is chosen by: a preview of
            // music has music playing, so its resting state is the lyric strip. The live loop sets this every
            // frame and the preview did not -- and the default is Collapsed, so every "strip" stage here drew
            // the CLOCK while the lyric was only in `frame.lyricLine` (a null line falls through to the clock,
            // see the renderer's pill-line block). That is why a photograph of this stage never showed a lyric.
            frame.restingState = island::IslandState::MusicStrip;
            // The pill's own box, which is NOT the animated one: an outer card's top edge and the bar's
            // own fill are both measured from it (see FrameState::restingPill). The live loop sets it
            // every frame and this preview did not, so a card drawn here had no pill to hang below.
            frame.restingPill = island::BoxForState(island::IslandState::Collapsed,
                                                    window.CurrentAnchor());
            frame.music = (frame.state == island::IslandState::Expanded) ? &sample : nullptr;
            // ... and the card has to be ASKED for, not merely sized.
            //
            // The renderer draws the music card only while `outerCardUp` is set and the frame names it as the
            // owner; the preview set neither, so its "card" stage drew the pill and nothing else. Measured
            // against the stages that do draw: `draw avg 417 us` here against ~1200 us there, and
            // `card layers: total 0 us`. That made a photograph of the card impossible from this tool, which is
            // the one shape whose layout is most often looked at.
            if (stage.state == island::IslandState::Expanded) {
                frame.outerCardUp = true;
                frame.cardOwner = island::CardOwner::Music;
            }
            frame.transportGlyph = ">";
            frame.progressFraction = 0.66f;
            // The strip shows the lyric when there is one, and the track NAME otherwise --
            // which is the case that was reported as rendering only a single character.
            frame.lyricLine = stage.stripShowsFallback ? nullptr : lyric;
            frame.fallbackLine = sample.title;
            frame.lyricScrollDip = scrollDip;

            // The application icons beside the island, in every SHORT stage.
            //
            // Not just the hover stage. The icons are not a hover feature: opening an application
            // adds an icon next to the island and it is there whether or not the pointer is, with
            // hover only widening them. Populating this for Hover alone was part of what made the
            // row look like a hover-only strip.
            if (stage.state == island::IslandState::Collapsed ||
                stage.state == island::IslandState::MusicStrip ||
                stage.state == island::IslandState::Hover) {
                static island::WindowList previewWindows;
                static wchar_t previewTitles[island::kMaxTaskWindows][island::kTaskTitleCapacity];
                static unsigned long long lastPreviewRefresh = 0;
                static int lastPreviewListed = -1;

                // Refreshed on the same timer the live loop uses, not every frame.
                //
                // Refreshing per frame walks 300-odd windows sixty times a second and logs the
                // summary each time: the first version produced four hundred identical lines in one
                // stage and buried everything else in the output.
                if (GetTickCount64() - lastPreviewRefresh >= 500) {
                    lastPreviewRefresh = GetTickCount64();
                    previewWindows.Refresh();
                }

                int listed = previewWindows.Count();
                const int fits = island::TaskIconsThatFit(
                    static_cast<float>(window.Monitor().screen.right -
                                       window.Monitor().screen.left) / window.Scale(),
                    previewWindows.Count());
                listed = fits;

                if (listed != lastPreviewListed) {
                    lastPreviewListed = listed;
                    island::LogAndPrint("      taskbar            %ls",
                                        previewWindows.LastSummary());
                }

                frame.taskButtonCount = listed;
                for (int i = 0; i < listed; ++i) {
                    wcsncpy_s(previewTitles[i], island::kTaskTitleCapacity,
                              previewWindows.At(i).title, _TRUNCATE);
                    frame.taskButtons[i].title = previewTitles[i];
                    frame.taskButtons[i].active =
                        previewWindows.At(i).handle == previewWindows.ForegroundHandle();
                    frame.taskButtons[i].minimised = previewWindows.At(i).iconic;
                    // The renderer resolves the icon from the window; see IconForWindow.
                    frame.taskButtonWindow[i] = previewWindows.At(i).handle;
                    // The island's own colour, so the preview shows what the live bar shows.
                    frame.taskButtons[i].fill = frame.pill.fill;
                }
                // The preview has no pointer, so nothing is pointed at: the row is shown at rest.
                // Forcing an index made the drawing respond to a hover that never happened, which is
                // exactly the kind of preview-versus-live difference that has misled before.
                frame.taskHoveredIndex = -1;
            }

            if (haveIcon && frame.music) {
                frame.iconPixels = iconImage.pixels;
                frame.iconWidth = iconImage.width;
                frame.iconHeight = iconImage.height;
                frame.iconRevision = 1;
            }
            if (frame.music && haveArtwork) {
                frame.artworkPixels = artwork.pixels;
                frame.artworkWidth = artwork.width;
                frame.artworkHeight = artwork.height;
                frame.artworkRevision = 1;
            }
            if (frame.music && haveBackground) {
                frame.backgroundPixels = background.pixels;
                frame.backgroundWidth = background.width;
                frame.backgroundHeight = background.height;
                // The same revision as the cover: one track, one set of images, so the
                // renderer's cache is invalidated once rather than every frame.
                frame.backgroundRevision = 1;
                // The translucent fill the wash needs behind it, as in the live path.
                frame.pill.fill = D2D1::ColorF(0.04f, 0.05f, 0.08f, 0.98f);
            }
            renderer.Render(frame);
            frameStats.Record();

            ::MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        // Report what a frame of this shape costs.
        //
        // The per-frame DRAW time is not enough on its own: it says nothing about the
        // INTERVAL between frames, and a stutter is a long interval. Both are reported, plus
        // the worst interval seen, because a hitch is a maximum and not an average.
        island::LogAndPrint("      %-13s %lld frames, draw avg %.0f us, worst %.0f us",
                            stage.name, renderer.FrameCount(),
                            renderer.AverageFrameMicroseconds(),
                            renderer.LastFrameMicroseconds());
        {
            wchar_t summary[256] = {};
            frameStats.Describe(summary, 256);
            island::LogAndPrint("      %-13s interval %ls", stage.name, summary);
        }
        if (stage.state == island::IslandState::Expanded) {
            island::LogAndPrint("      card layers: total %.0f us, background %.0f us, "
                                "cover %.0f us",
                                text.LastCardMicroseconds(),
                                text.LastCardBackgroundMicroseconds(),
                                text.LastCardArtworkMicroseconds());
        }
        std::fflush(stdout);
    }

    renderer.SetTextRenderer(nullptr);
    text.Shutdown();
    window.Destroy();
    return 0;
}

// Fetches lyrics for one track and reports what came back, then exits.
//
// This exists because the fetch cannot be verified from every environment: the
// development sandbox has no outbound TLS at all, so the only place the network path
// can be exercised is a normal shell on the machine, and the live bar would only say
// "none". With this, one command answers whether the service is reachable, whether
// the search matched the right track, and how many lines the parser found.
//
// A leading "[offline]" marker on the output makes it unambiguous which half failed:
// the network or the parsing.
//
// `verbose` also dumps the exact URL requested and the first part of each response, which
// is the only way to tell a 403 apart from an empty result.
int RunLyricsTest(const char* title, const char* artist, bool verbose) {
    island::LogAndPrint("lyrics test: title=\"%s\" artist=\"%s\"",
                        title ? title : "", artist ? artist : "");
    std::fflush(stdout);

    if (!title || title[0] == '\0') {
        island::LogAndPrint("lyrics test: no title given; use --lyrics-test <title> [artist]");
        return 2;
    }

    const DWORD started = GetTickCount();
    static island::Lyrics lyrics;
    static island::LyricsFetchReport report;
    char why[192] = {};
    const bool ok = island::FetchLyricsVerbose(title, artist ? artist : "", 10000, &lyrics,
                                               why, sizeof(why), &report);
    const DWORD elapsed = GetTickCount() - started;

    // The requests and responses first: when something fails, this is the part that says
    // why, and printing it after a success is still useful as confirmation.
    if (verbose) {
        // BOTH services, each with its own URLs, bodies and verdict.
        //
        // The old shape printed a single record under NetEase's name while the reason string named
        // both services, so one service's response could be read against the other's conclusion --
        // which is how "the search found no matching track" came to be believed about a search that
        // had answered 200 with a populated list.
        const auto dump = [](const char* who, const island::LyricsFetchReport::Attempt& attempt) {
            island::LogAndPrint("  %s search: HTTP %d  %s", who, attempt.searchStatus,
                                attempt.searchUrl);
            if (attempt.searchBody[0]) island::LogAndPrint("    <- %.400s", attempt.searchBody);
            island::LogAndPrint("  %s lyric : HTTP %d  %s", who, attempt.lyricStatus,
                                attempt.lyricUrl);
            if (attempt.lyricBody[0]) island::LogAndPrint("    <- %.400s", attempt.lyricBody);
            island::LogAndPrint("  %s verdict: %s", who,
                                attempt.verdict[0] ? attempt.verdict : "(nothing recorded)");
        };
        dump("netease", report.netease);
        dump("qq     ", report.qq);
        std::fflush(stdout);
    }

    if (!ok) {
        island::LogAndPrint("lyrics test: FAILED after %lu ms -- %s", elapsed,
                            why[0] ? why : "no reason recorded");
        return 1;
    }

    island::LogAndPrint("lyrics test: OK in %lu ms, %d lines, [ti:]=\"%s\" [ar:]=\"%s\"",
                        elapsed, lyrics.count, lyrics.title, lyrics.artist);

    // A few lines, with their times, so the parse can be judged rather than trusted.
    const int show = lyrics.count < 6 ? lyrics.count : 6;
    for (int i = 0; i < show; ++i) {
        const island::LyricLine* line = lyrics.At(i);
        if (!line) break;
        island::LogAndPrint("  [%02lld:%02lld.%03lld] %s", line->timeMs / 60000,
                            (line->timeMs / 1000) % 60, line->timeMs % 1000, line->text);
    }
    if (lyrics.count > show) {
        island::LogAndPrint("  ... and %d more", lyrics.count - show);
    }

    // And what the strip would show a third of the way in, which is the value the
    // display actually uses.
    const long long probe = lyrics.lines[lyrics.count - 1].timeMs / 3;
    const island::LyricLine* at = lyrics.At(lyrics.LineAt(probe));
    island::LogAndPrint("  at %lld ms the strip shows: %s", probe, at ? at->text : "(nothing)");
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Decide the console before anything prints. The self-test needs somewhere to
    // report even when double-clicked; the bar itself must never pop one up.
    bool selfTest = false;
    bool demo = false;
    bool keepTaskbar = false;
    bool claimAppBar = false;
    bool windowList = false;
    bool probe = false;
    bool noConsole = false;
    bool notify = false;
    bool musicPreview = false;
    const wchar_t* forcedState = nullptr;
    bool forcedStartHover = false;
    bool forcedSystemPanel = false;
    bool lyricsTest = false;
    bool lyricsVerbose = false;
    bool volumeTest = false;
    bool iconDump = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        if (std::strcmp(argv[i], "--demo") == 0) demo = true;
        if (std::strcmp(argv[i], "--keep-taskbar") == 0) keepTaskbar = true;
        if (std::strcmp(argv[i], "--appbar") == 0) claimAppBar = true;
        if (std::strcmp(argv[i], "--window-list") == 0) windowList = true;
        if (std::strcmp(argv[i], "--probe-taskbar") == 0) probe = true;
        if (std::strcmp(argv[i], "--notify") == 0) notify = true;
        if (std::strcmp(argv[i], "--music-preview") == 0) musicPreview = true;
        if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            // A static buffer rather than an allocation: this is read once at startup and the pointer is
            // kept for the life of the process.
            static wchar_t stateName[64] = {};
            if (ArgumentAsUtf8ToWide(argv[++i], stateName, 64)) {
                forcedState = stateName;
            }
        }
        if (std::strcmp(argv[i], "--start-hover") == 0) forcedStartHover = true;
        if (std::strcmp(argv[i], "--system-panel") == 0) forcedSystemPanel = true;
        if (std::strcmp(argv[i], "--lyrics-test") == 0) lyricsTest = true;
        if (std::strcmp(argv[i], "--lyrics-verbose") == 0) lyricsVerbose = true;
        if (std::strcmp(argv[i], "--volume-test") == 0) volumeTest = true;
        if (std::strcmp(argv[i], "--icon-dump") == 0) iconDump = true;
        // Reproduces the console-less launch on purpose. That is the case where
        // stdout has an invalid descriptor and an unguarded write trips a CRT
        // assertion instead of printing nothing, so it has to be testable from a
        // shell rather than only by double-clicking the executable.
        if (std::strcmp(argv[i], "--no-console") == 0) noConsole = true;
    }

    wchar_t consoleDetail[128] = {};
    const bool haveConsole = noConsole
                                 ? false
                                 : PrepareConsole(selfTest || demo || probe, consoleDetail, 128);

    // The log is opened before anything else can fail, and every diagnostic line
    // goes there as well as to stdout. Output on the console has been lost three
    // times on this machine; the file has not.
    island::OpenLog();
    island::LogLine(L"console: %s -> %s; log = %s", consoleDetail,
                    haveConsole ? L"usable" : L"unavailable", island::LogPath());

    // What the token looks like, said the same way whether or not the taskbar is being hidden.
    //
    // This property decides whether the bar can act on the SHELL at all: UIPI discards window messages
    // and synthetic input sent from a lower integrity level to a higher one, so a bar started from a
    // sandboxed terminal can neither hide Explorer's taskbar nor open the Start menu -- and both of those
    // fail with no error anywhere. It used to be reported only along the hide path, which is silent
    // exactly when the operator is running with --keep-taskbar and looking for the reason.
    //
    // Determined here, before anything is decided or mutated, and cheap: it only reads two tokens.
    {
        island::NativeTaskbar integrityProbe;
        integrityProbe.Locate();
        const bool belowShell = integrityProbe.IsBlockedByIntegrity();
        island::LogLine(L"integrity: %s", belowShell
                            ? L"LOWER than the shell's -- synthetic input and window messages aimed at "
                              L"Explorer will be discarded (start the bar from your own shell)"
                            : L"same as the shell's -- this process may act on Explorer");
    }

    EnablePerMonitorV2();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            const int failures = island::RunSelfTest();
            island::LogLine(L"self-test finished with %d failure(s)", failures);
            island::CloseLog();
            return failures;
        }
        if (std::strcmp(argv[i], "--probe-taskbar") == 0) {
            const int result = island::RunTaskbarProbe();
            island::LogLine(L"log written to %s", island::LogPath());
            island::CloseLog();
            return result;
        }
        if (std::strcmp(argv[i], "--window-list") == 0) {
            const bool activate = (i + 1 < argc) && std::strcmp(argv[i + 1], "--activate") == 0;
            const int listResult = RunWindowList(activate);
            island::CloseLog();
            return listResult;
        }
        if (std::strcmp(argv[i], "--volume-test") == 0) {
            const int testResult = RunVolumeTest();
            island::CloseLog();
            return testResult;
        }
        if (std::strcmp(argv[i], "--volume-set") == 0 && i + 1 < argc) {
            const int setResult = RunVolumeSet(static_cast<int>(std::strtol(argv[++i], nullptr, 10)));
            island::CloseLog();
            return setResult;
        }
        if (std::strcmp(argv[i], "--toast-dump") == 0) {
            const int count = (i + 1 < argc) ? static_cast<int>(std::strtol(argv[++i], nullptr, 10)) : 10;
            const int dumpResult = RunToastDump(count);
            island::CloseLog();
            return dumpResult;
        }
        if (std::strcmp(argv[i], "--icon-dump") == 0) {
            const char* appId = (i + 1 < argc) ? argv[i + 1] : "";
            // Shell icon extraction goes through the shell's COM objects, so an apartment is
            // required. Without it SHGetFileInfoW reports success and hands back a NULL icon,
            // which reads exactly like "this executable has no icon" -- that is what made this
            // command print "encoded 0 bytes" for a file the live path loads fine.
            const HRESULT iconHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(iconHr) && iconHr != RPC_E_CHANGED_MODE) return 1;
            const int dumpResult = RunIconDump(appId);
            CoUninitialize();
            island::CloseLog();
            return dumpResult;
        }
        if (std::strcmp(argv[i], "--lyrics-test") == 0) {
            // Taken from the WIDE command line, not from argv: see ArgumentAsUtf8. A
            // Chinese title in argv is GBK, and percent-encoding GBK is what made every
            // Chinese search query malformed.
            int wideCount = 0;
            LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
            char testTitle[512] = {};
            char testArtist[512] = {};
            if (wideArgs) {
                if (i + 1 < wideCount) ArgumentAsUtf8(wideArgs[i + 1], testTitle, sizeof(testTitle));
                if (i + 2 < wideCount) ArgumentAsUtf8(wideArgs[i + 2], testArtist, sizeof(testArtist));
                LocalFree(wideArgs);
            }
            const HRESULT testHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(testHr) && testHr != RPC_E_CHANGED_MODE) return 1;
            const int testResult = RunLyricsTest(testTitle, testArtist, lyricsVerbose);
            CoUninitialize();
            island::CloseLog();
            return testResult;
        }        if (std::strcmp(argv[i], "--music-preview") == 0) {
            const HRESULT previewHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(previewHr) && previewHr != RPC_E_CHANGED_MODE) return 1;
            const int previewResult = RunMusicPreview();
            CoUninitialize();
            island::CloseLog();
            return previewResult;
        }        if (std::strcmp(argv[i], "--notify") == 0) {
            const int result = RunNotify(argc, argv);
            island::CloseLog();
            return result;
        }
        if (std::strcmp(argv[i], "--demo") == 0) {
            const HRESULT demoHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(demoHr) && demoHr != RPC_E_CHANGED_MODE) return 1;
            const int demoResult = RunDemo();
            CoUninitialize();
            island::CloseLog();
            return demoResult;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            island::LogAndPrint("usage: island_bar.exe [options]");
            island::LogAndPrint("  (no arguments)   show the bar and hide the native taskbar");
            island::LogAndPrint("  --keep-taskbar   leave the native taskbar visible");
            island::LogAndPrint("  --self-test      run the runtime assertions; exit code = failures");
            island::LogAndPrint("  --probe-taskbar  report whether this process may hide the shell taskbar");
            island::LogAndPrint("  --demo           cycle collapsed/hover/expanded with no input needed");
            island::LogAndPrint("  --music-preview  cycle the three music states with sample data");
            island::LogAndPrint("  --lyrics-test <title> [artist]  fetch lyrics and report what came back");
            island::LogAndPrint("  --no-console     behave as if there were no console (diagnostic)");
            island::LogAndPrint("");
            island::LogAndPrint("Every run also writes a timestamped log, which is the reliable way to");
            island::LogAndPrint("see what happened -- the console has been known to swallow output:");
            island::LogAndPrint("      %%TEMP%%\\island_bar.log");
            island::LogAndPrint("");
            island::LogAndPrint("HOW TO RUN THIS");
            island::LogAndPrint("  From PowerShell, & is the call operator and is required for a quoted");
            island::LogAndPrint("  path:");
            island::LogAndPrint("      & \"C:\\path\\to\\island_bar.exe\" --probe-taskbar");
            island::LogAndPrint("  From cmd.exe, & is a command separator and CANNOT precede the path.");
            island::LogAndPrint("  Quote the path directly, or use start /wait:");
            island::LogAndPrint("      \"C:\\path\\to\\island_bar.exe\" --probe-taskbar");
            island::LogAndPrint("      start /wait \"\" \"C:\\path\\to\\island_bar.exe\" --probe-taskbar");
            island::LogAndPrint("  To capture the output from either shell, redirect it:");
            island::LogAndPrint("      island_bar.exe --probe-taskbar > probe.txt 2>&1");
            island::LogAndPrint("");
            island::LogAndPrint("The native taskbar is hidden on startup and restored on exit. If this");
            island::LogAndPrint("process is killed instead of closed, bring it back with either:");
            island::LogAndPrint("  taskkill /f /im explorer.exe && start explorer.exe");
            island::LogAndPrint("or, without restarting the shell, this one-liner:");
            island::LogAndPrint("  powershell -c \"Add-Type -Name W -Namespace N -MemberDefinition "
                              "'[DllImport(\\\"user32.dll\\\")]public static extern bool "
                              "ShowWindow(IntPtr h,int c);[DllImport(\\\"user32.dll\\\")]public static "
                              "extern IntPtr FindWindow(string c,string n);'; "
                              "[N.W]::ShowWindow([N.W]::FindWindow('Shell_TrayWnd',$null),5)\"");
            return 0;
        }
    }

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!comOk) {
        island::LogAndPrint("FATAL: CoInitializeEx failed (0x%08lX)\n",
                    static_cast<unsigned long>(hr));
        return 1;
    }

    // One bar at a time.
    //
    // Two instances draw two bars on top of each other, and the symptom does not look like "two
    // programs are running": it looks like ONE bar drawn in the wrong place, because the second copy
    // is somewhere the first one is not. That is exactly how it was reported -- a panel anchored to its
    // button AND a second card-shaped box in the middle of the screen -- and it was diagnosed as a
    // placement bug for two rounds.
    //
    // A named mutex is used rather than a window lookup because it is the same check whether the other
    // instance is starting, running, or shutting down, and it cannot be defeated by the other instance
    // being on a different desktop or having renamed its window class.
    //
    // The bar is a single global thing (it hides the shell's taskbar and may claim a screen edge), so
    // refusing to start a second copy is the correct behaviour rather than a limitation.
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"IslandBar.SingleInstance");
    const DWORD mutexError = GetLastError();
    if (singleInstance && mutexError == ERROR_ALREADY_EXISTS) {
        island::LogAndPrint("IslandBar is already running -- this copy exits now.\n");

        // REPORTED AND GONE. No dialog at all.
        //
        // The first version popped a MessageBox, and `MessageBoxTimeoutW` -- the undocumented call that
        // was supposed to dismiss it on a timer -- did not do so here: three launches in a row left TWO
        // refused copies alive, each sitting in a modal box. That is not a cosmetic wart, it is the same
        // defect the guard exists to prevent: the copies that were refused were still drawing their own
        // bars, which is exactly the "two panels on screen" that took three rounds to find.
        //
        // So the refusal is now a log line and an immediate exit. A guard that can itself linger is worse
        // than no guard, because it produces the very state it was written to prevent.
        island::CloseLog();
        CloseHandle(singleInstance);
        CoUninitialize();
        return 1;
    }

    // Hiding the native taskbar is the default: the whole point of the project is
    // to be the taskbar. --keep-taskbar exists for developing the bar alongside
    // the real one, and for comparing the two.
    const int result = RunBar(/*hideNativeTaskbar=*/!keepTaskbar, claimAppBar, forcedState,
                           forcedStartHover, forcedSystemPanel);
    // The status reader is closed by RunBar, where it was opened.
    CoUninitialize();
    return result;
}
