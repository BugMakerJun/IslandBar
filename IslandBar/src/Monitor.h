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

} // namespace island
