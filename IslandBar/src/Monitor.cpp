// =====================================================================
//  Monitor.cpp
// =====================================================================
#include "Monitor.h"

#include <shellscalingapi.h>

namespace island {
namespace {

// GetDpiForMonitor is Windows 8.1+. Fall back to the device caps so the bar
// still lays out sensibly on a machine that lacks it, instead of assuming 96.
UINT QueryDpi(HMONITOR monitor, MONITOR_DPI_TYPE type) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, type, &dpiX, &dpiY))) {
        return dpiX ? dpiX : 96;
    }

    // Fallback: derive from the GDI device caps of the monitor's virtual screen.
    HDC screen = GetDC(nullptr);
    if (screen) {
        const int gdiDpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (gdiDpi > 0) return static_cast<UINT>(gdiDpi);
    }
    return 96;
}

} // namespace

MonitorGeometry QueryMonitor(HMONITOR monitor) {
    if (!monitor) {
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    MonitorGeometry geometry;
    geometry.monitor = monitor;

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        geometry.screen  = info.rcMonitor;
        geometry.work    = info.rcWork;
        geometry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    } else {
        // No information is better than a zero rect: fall back to the desktop.
        geometry.screen.left   = 0;
        geometry.screen.top    = 0;
        geometry.screen.right  = GetSystemMetrics(SM_CXSCREEN);
        geometry.screen.bottom = GetSystemMetrics(SM_CYSCREEN);
        geometry.work          = geometry.screen;
    }

    geometry.dpi  = QueryDpi(monitor, MDT_EFFECTIVE_DPI);
    geometry.dpiY = geometry.dpi;

    return geometry;
}

int QueryAllMonitors(MonitorGeometry* out, int capacity) {
    if (!out || capacity <= 0) return 0;

    struct Collector {
        MonitorGeometry* items;
        int capacity;
        int count;
        bool overflow;
    } collector{out, capacity, 0, false};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Collector*>(param);
            if (c->count >= c->capacity) {
                c->overflow = true;
                return FALSE;   // stop enumerating
            }
            c->items[c->count++] = QueryMonitor(monitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));

    return collector.count;
}

} // namespace island
