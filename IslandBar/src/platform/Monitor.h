// =====================================================================
//  Monitor.h
//  Per-monitor metrics: work area, DPI, and the inferred taskbar inset.
//
//  Taskbar thickness is NEVER hardcoded. It is the distance between the
//  screen edge and the work-area edge on whichever side the taskbar lives,
//  which this machine measured at 32 px at 100% scaling, but that number
//  belongs to this machine and not to the code.
// =====================================================================
#pragma once

#include <windows.h>

#include <array>

namespace island {

// Physical-pixel bounds of one monitor. All fields come straight from Win32
// and are therefore in physical pixels, not DIPs.
struct MonitorGeometry {
    HMONITOR monitor = nullptr;
    RECT     screen  = {};   // rcMonitor: the whole display
    RECT     work    = {};   // rcWork: excludes registered appbars (taskbar)
    UINT     dpi     = 96;
    UINT     dpiY    = 96;
    bool     primary = false;
};

// Width and height in physical pixels.
inline LONG ScreenWidth(const MonitorGeometry& m)  { return m.screen.right - m.screen.left; }
inline LONG ScreenHeight(const MonitorGeometry& m) { return m.screen.bottom - m.screen.top; }
inline LONG WorkWidth(const MonitorGeometry& m)    { return m.work.right - m.work.left; }
inline LONG WorkHeight(const MonitorGeometry& m)   { return m.work.bottom - m.work.top; }

// Which screen edge the work area was inset on. This is where a taskbar is,
// or would be if one is registered.
enum class DockEdge { None, Top, Bottom, Left, Right };

inline DockEdge InsetEdge(const MonitorGeometry& m) {
    if (m.work.top    > m.screen.top)    return DockEdge::Top;
    if (m.work.bottom < m.screen.bottom) return DockEdge::Bottom;
    if (m.work.left   > m.screen.left)   return DockEdge::Left;
    if (m.work.right  < m.screen.right)  return DockEdge::Right;
    return DockEdge::None;
}

// Taskbar thickness in physical pixels, measured as the work-area inset.
//
// Zero means "nothing is docked here", which is a legitimate state (auto-hide
// enabled, or a shell replacement) and not an error.
inline LONG TaskbarThickness(const MonitorGeometry& m) {
    switch (InsetEdge(m)) {
        case DockEdge::Top:    return m.work.top - m.screen.top;
        case DockEdge::Bottom: return m.screen.bottom - m.work.bottom;
        case DockEdge::Left:   return m.work.left - m.screen.left;
        case DockEdge::Right:  return m.screen.right - m.work.right;
        case DockEdge::None:   break;
    }
    return 0;
}

// The band at the top of `monitor` that the shell's taskbar occupies, in physical pixels, read from the
// taskbar's own window. Zero when the taskbar is not along that display's TOP edge -- a taskbar at the
// bottom or on a side occupies no part of the band the island hangs in.
//
// It exists because the work-area reading above stops being usable once the bar RELEASES the strip the
// taskbar reserves -- see NativeTaskbar::ReleaseWorkArea, which does that so a maximized window can
// reach the top of the screen. After a release the work area is the whole monitor and the band the bar
// draws in has to come from somewhere else; the taskbar window still has it (measured: 0,0-2560,32
// while the window is hidden, which is the state the bar keeps it in). Zero when there is no shell
// taskbar at all, or when it is not on this display.
LONG ShellTopBandThicknessPx(HMONITOR monitor);

// The invisible resize border Windows puts on each side of a MAXIMIZED window, in physical pixels, for
// a monitor at `dpi`. It is SM_CXSIZEFRAME + SM_CXPADDEDBORDER -- 4 + 4 = 8 px at 100%, measured on
// this machine: work area 0,32-2560,1440 gives a maximized window of -8,24-2568,1448.
struct BorderInflation {
    LONG x = 0;
    LONG y = 0;
};
BorderInflation MaximizedBorderPx(UINT dpi);

inline RECT InflateRect(const RECT& r, LONG x, LONG y) {
    return RECT{r.left - x, r.top - y, r.right + x, r.bottom + y};
}

inline bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// Whether a window's rectangle is EXACTLY the one Windows gives a maximized window on that monitor.
//
// This is how a maximized window is told apart from a full-screen application once the strip has been
// released, because then the two are otherwise the same rectangle. Measured, and it is why "is the
// window zoomed" cannot be the test: a window that is maximized and then made borderless over the whole
// screen KEEPS WS_MAXIMIZE (IsZoomed true) and reports the work area itself, which is exactly the
// geometry of a game. A maximized window, by contrast, is always inflated by the resize border -- the
// border is invisible but it is there, and it is the only thing left that separates the two.
inline bool LooksLikeWindowsMaximizedRect(const RECT& window, const RECT& workArea, UINT dpi) {
    const BorderInflation border = MaximizedBorderPx(dpi);
    return SameRect(window, InflateRect(workArea, border.x, border.y));
}

// Read the geometry of the monitor a point falls on (MONITOR_DEFAULTTOPRIMARY
// when the point is nowhere near any display).
MonitorGeometry QueryMonitor(HMONITOR monitor);

// Whether a window's rectangle covers the whole monitor: a full-screen APPLICATION rather than a
// maximized window.
//
// Pure, and separate from the Win32 calls that feed it, because the interesting case is the NEAR MISS.
// A maximized window covers the WORK AREA, which begins below the band the shell's taskbar reserves,
// so its top edge sits a few tens of pixels below the monitor's. A test written "==" instead of
// "<= / >=", or one that settled for "big enough", would call every maximized window full-screen --
// and then the bar would stand down on an ordinary desktop, which is the one thing worse than standing
// down on a game.
//
// That is true while the shell's taskbar reserves its band. Once the bar releases it (see
// NativeTaskbar::ReleaseWorkArea) a maximized window covers the monitor as well, and the two states are
// told apart by the resize border instead -- see LooksLikeWindowsMaximizedRect and FullScreenAppInFront.
inline bool WindowCoversMonitor(const RECT& window, const RECT& monitor) {
    return window.left <= monitor.left && window.top <= monitor.top &&
           window.right >= monitor.right && window.bottom >= monitor.bottom;
}

// Whether a full-screen application is in front of this program on `monitor`.
//
// NOT the shell's answer, and it cannot be: the shell announces a full-screen application only to a bar
// that is REGISTERED as an AppBar (ABN_FULLSCREENAPP), and this program is not one by default -- its own
// log says "appbar: not claimed" on every run. So the flag the AppBar code maintains is false in the
// shipped configuration, and the bar used to keep painting the island and its date-and-time line over a
// game ("现在的灵动岛一行的在玩游戏的时候也显示出来了").
//
// The exceptions are the windows that cover the screen without being an application -- the desktop, the
// shell's own surfaces -- and windows DWM has cloaked, which are not on screen at all (a UWP application
// on another virtual desktop covers the monitor and nobody is looking at it). Windows of this process are
// excluded by pid, which is what makes the answer "another application has the screen".
bool FullScreenAppInFront(const RECT& monitor);

// Brings windows that a just-released strip left behind to where Windows would have maximized them.
//
// Measured: releasing the work area does NOT move a window that is ALREADY maximized -- neither Windows
// nor the application re-lays it out -- so the very band the release was meant to remove stays on screen
// until the user maximizes something again. The rectangle is therefore set by us, with SWP_NOACTIVATE
// (measured: the foreground window does not change), and ONLY for windows whose rectangle is exactly the
// maximized rectangle of the old work area, so nothing else can be moved by this. `monitorScreen` is the
// display whose strip was released; windows on any other display are not in that state and are skipped.
// Returns how many windows it moved.
int NudgeStaleMaximizedWindows(const RECT& monitorScreen, const RECT& workBefore,
                               const RECT& workAfter);

} // namespace island
