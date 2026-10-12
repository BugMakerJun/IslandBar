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

// Read the geometry of the monitor a point falls on (MONITOR_DEFAULTTOPRIMARY
// when the point is nowhere near any display).
MonitorGeometry QueryMonitor(HMONITOR monitor);

// Read every monitor. The bar follows the pointer across displays, so this is
// the multi-monitor path even though the first release drives one monitor.
int QueryAllMonitors(MonitorGeometry* out, int capacity);

// Whether a window's rectangle covers the whole monitor: a full-screen APPLICATION rather than a
// maximized window.
//
// Pure, and separate from the Win32 calls that feed it, because the interesting case is the NEAR MISS.
// A maximized window covers the WORK AREA, which begins below the band the shell's taskbar reserves,
// so its top edge sits a few tens of pixels below the monitor's. A test written "==" instead of
// "<= / >=", or one that settled for "big enough", would call every maximized window full-screen --
// and then the bar would stand down on an ordinary desktop, which is the one thing worse than standing
// down on a game.
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

} // namespace island
