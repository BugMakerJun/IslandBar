// =====================================================================
//  AppBar.cpp
// =====================================================================
#include "AppBar.h"

#include "Log.h"

#include <shellapi.h>

#include <cstdio>

#pragma comment(lib, "shell32.lib")

namespace island {
namespace {

// One callback message per process, in the WM_APP range as the documentation
// requires. Registered once and cached: the shell rejects a second registration
// of the same message for the same window, and a program that re-registers on
// every display change would stop receiving callbacks after the first.
UINT g_callbackMessage = 0;

APPBARDATA MakeData(HWND hwnd) {
    APPBARDATA data = {};
    data.cbSize = sizeof(APPBARDATA);
    data.hWnd = hwnd;
    return data;
}

// The full bounds of the monitor a window is on, in screen coordinates.
//
// THIS IS THE ANCHOR, and using the work area instead was the whole of a runaway loop: the
// reservation SHORTENS the work area, so a bar that computes its position from the work area's top
// edge is measuring from the thing it just changed. The bar walked itself down the screen 42 px at a
// time and the user's desktop was pushed down and unclickable. The monitor's bounds do not move.
RECT MonitorBoundsOf(HWND hwnd) {
    RECT bounds = {};
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &info)) {
        bounds = info.rcMonitor;
    }
    return bounds;
}

// The work area of the monitor a window is on, for judging whether space was granted.
//
// Read only to CHECK the result. Never used to compute the rectangle: see above.
RECT WorkAreaOf(HWND hwnd) {
    RECT work = {};
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
    }
    return work;
}

} // namespace

// The rectangle to reserve for a bar docked to `edge` of a monitor whose bounds are `monitorBounds`.
//
// Takes the MONITOR's bounds and not the work area, and that is the entire reason it exists as a
// separate function. See the note on MonitorBoundsOf for what happened when the work area was used,
// and the self-test for the property that now prevents it.
RECT AppBarStripRect(const RECT& monitorBounds, DockEdge edge, LONG thicknessPx) {
    RECT target = monitorBounds;

    // Every edge comes from `monitorBounds` and none from the work area, so the same inputs always give
    // the same rectangle however many times it is asked for. A rectangle derived from the work area
    // depends on the result of the previous attempt, which is how a bar walks itself across a screen.
    switch (edge) {
        case DockEdge::Top:
            target.top = monitorBounds.top;
            target.bottom = monitorBounds.top + thicknessPx;
            break;
        case DockEdge::Bottom:
            target.bottom = monitorBounds.bottom;
            target.top = monitorBounds.bottom - thicknessPx;
            break;
        case DockEdge::Left:
            target.left = monitorBounds.left;
            target.right = monitorBounds.left + thicknessPx;
            break;
        case DockEdge::Right:
            target.right = monitorBounds.right;
            target.left = monitorBounds.right - thicknessPx;
            break;
        case DockEdge::None:
            // An empty rectangle, which the caller turns into a refusal.
            target.right = target.left;
            target.bottom = target.top;
            break;
    }
    return target;
}
AppBarLanding EvaluateLanding(const RECT& monitorBounds, const RECT& workBefore,
                              const RECT& workAfter, DockEdge edge, LONG thicknessPx) {
    AppBarLanding result;

    // A work area that is inside out is not a work area, whatever else is true of its edges.
    result.sane = workAfter.right > workAfter.left && workAfter.bottom > workAfter.top;

    switch (edge) {
        case DockEdge::Top:    result.landed = workAfter.top == monitorBounds.top + thicknessPx; break;
        case DockEdge::Bottom: result.landed = workAfter.bottom == monitorBounds.bottom - thicknessPx; break;
        case DockEdge::Left:   result.landed = workAfter.left == monitorBounds.left + thicknessPx; break;
        case DockEdge::Right:  result.landed = workAfter.right == monitorBounds.right - thicknessPx; break;
        case DockEdge::None:   result.landed = false; break;
    }

    if (edge != DockEdge::Top && workAfter.top != workBefore.top) result.otherEdgesIntact = false;
    if (edge != DockEdge::Bottom && workAfter.bottom != workBefore.bottom) {
        result.otherEdgesIntact = false;
    }
    if (edge != DockEdge::Left && workAfter.left != workBefore.left) result.otherEdgesIntact = false;
    if (edge != DockEdge::Right && workAfter.right != workBefore.right) {
        result.otherEdgesIntact = false;
    }

    return result;
}

const wchar_t* AppBarResultName(AppBarResult result) {
    switch (result) {
        case AppBarResult::Registered:      return L"registered";
        case AppBarResult::NoWindow:        return L"no window";
        case AppBarResult::CallbackRefused: return L"callback message refused";
        case AppBarResult::NotRegistered:   return L"ABM_NEW refused";
        case AppBarResult::SpaceNotGranted: return L"registered but no space granted";
    }
    return L"?";
}

UINT DockEdgeToAppBarEdge(DockEdge edge) {
    switch (edge) {
        case DockEdge::Top:    return ABE_TOP;
        case DockEdge::Bottom: return ABE_BOTTOM;
        case DockEdge::Left:   return ABE_LEFT;
        case DockEdge::Right:  return ABE_RIGHT;
        case DockEdge::None:   break;
    }
    // ABE_TOP is returned for None rather than 0, because 0 is not a valid edge
    // and SHAppBarMessage would fail with a meaningless error.
    return ABE_TOP;
}

UINT AppBar::CallbackMessage() {
    if (g_callbackMessage == 0) {
        g_callbackMessage = RegisterWindowMessageW(L"IslandBarAppBarCallback");
    }
    return g_callbackMessage;
}

AppBar::~AppBar() {
    Release();
}

AppBarResult AppBar::Claim(HWND hwnd, DockEdge edge, LONG thicknessPx) {
    detail_[0] = L'\0';

    if (!hwnd) {
        std::swprintf(detail_, 256, L"no window to register");
        return AppBarResult::NoWindow;
    }

    // Register ourselves as a bar. This is what allows the shell to send us
    // ABN_POSCHANGED and what makes ABM_SETPOS meaningful; it does NOT reserve
    // anything by itself, which is the trap: ABM_NEW returns TRUE and the
    // desktop is unchanged.
    if (!registered_) {
        const UINT callback = CallbackMessage();
        if (callback == 0) {
            std::swprintf(detail_, 256, L"RegisterWindowMessage failed (error %lu)",
                          static_cast<unsigned long>(GetLastError()));
            return AppBarResult::CallbackRefused;
        }

        APPBARDATA data = MakeData(hwnd);
        data.uCallbackMessage = callback;
        if (!SHAppBarMessage(ABM_NEW, &data)) {
            // The usual cause is that another process already owns every edge,
            // which is exactly the case while the shell's own taskbar is on
            // screen. Reported rather than retried: retrying cannot help.
            std::swprintf(detail_, 256,
                          L"ABM_NEW refused (error %lu); another bar may own the edge",
                          static_cast<unsigned long>(GetLastError()));
            return AppBarResult::NotRegistered;
        }
        registered_ = true;
    }

    const RECT workBefore = WorkAreaOf(hwnd);

    // The MONITOR's bounds, which do not move, are what the rectangle is built from. The work area
    // shrinks every time a bar is granted space, so anchoring to it makes each re-claim start further
    // down -- see MonitorBoundsOf.
    const RECT anchor = MonitorBoundsOf(hwnd);

    // --- refuse to hand the shell a rectangle that could wreck the desktop ---
    //
    // Everything above this point is reversible; ABM_SETPOS is not. If the rectangle it is given
    // is nonsense, the WORK AREA becomes nonsense, and every maximized window on the machine moves
    // with it. There is no error to catch afterwards and the program has no way to tell that it
    // just rearranged someone's desktop -- so the sanity checks have to happen before the call,
    // and the numbers involved are recorded so the log can be read afterwards.
    const LONG screenWidth = anchor.right - anchor.left;
    const LONG screenHeight = anchor.bottom - anchor.top;
    if (screenWidth <= 0 || screenHeight <= 0) {
        std::swprintf(detail_, 256, L"refused: the monitor bounds are empty (%ldx%ld)", screenWidth,
                      screenHeight);
        return AppBarResult::SpaceNotGranted;
    }
    if (thicknessPx <= 0 || thicknessPx > screenHeight / 4) {
        // A quarter of the screen is already far more than any bar should take; anything beyond
        // that is a bug in the caller, and passing it on would shrink the desktop to a sliver.
        std::swprintf(detail_, 256,
                      L"refused: %ld px is not a sane bar thickness for a %ld px high screen",
                      thicknessPx, screenHeight);
        return AppBarResult::SpaceNotGranted;
    }

    // Ask where the shell would put a bar of this thickness on this edge.
    //
    // The answer is a SUGGESTION and the documentation says so: the caller is expected to force
    // its own thickness onto the edge it wants and then send ABM_SETPOS. The suggestion is still
    // used as the crop for the OTHER axis, because it accounts for bars already on those edges.
    APPBARDATA query = MakeData(hwnd);
    query.uEdge = DockEdgeToAppBarEdge(edge);
    query.rc = anchor;
    SHAppBarMessage(ABM_QUERYPOS, &query);

    // The reserved rectangle is built explicitly instead of adjusting the suggestion.
    //
    // Adjusting in place assumes the suggestion came back well-formed on the axis being kept. It
    // does not have to: a failed or degenerate ABM_QUERYPOS leaves the rectangle as it was passed
    // in, or shortens it to nothing, and a bar that then writes its thickness onto that produces a
    // rectangle spanning whatever was there. Building it from the monitor's own bounds means the
    // values handed to ABM_SETPOS are always ones this code computed.
    //
    // Done by a free function so it can be tested without a window, a shell or a desktop. See
    // AppBarStripRect -- the property it guarantees is the one whose absence walked a bar down the
    // screen.
    RECT target = AppBarStripRect(anchor, edge, thicknessPx);
    if (target.right <= target.left || target.bottom <= target.top) {
        std::swprintf(detail_, 256, L"no edge to dock to");
        return AppBarResult::NoWindow;
    }

    // Whatever the shell suggested for the axis the bar does not use on its docked edge. For a
    // top bar that is the horizontal extent, and honouring the suggestion there keeps the bar from
    // covering a bar already docked left or right.
    if (edge == DockEdge::Top || edge == DockEdge::Bottom) {
        if (query.rc.left > target.left && query.rc.left < target.right) target.left = query.rc.left;
        if (query.rc.right < target.right && query.rc.right > target.left) {
            target.right = query.rc.right;
        }
    } else {
        if (query.rc.top > target.top && query.rc.top < target.bottom) target.top = query.rc.top;
        if (query.rc.bottom < target.bottom && query.rc.bottom > target.top) {
            target.bottom = query.rc.bottom;
        }
    }

    // A bar with no extent would reserve nothing and confuse the shell; treat it as a refusal.
    if (target.right <= target.left || target.bottom <= target.top) {
        std::swprintf(detail_, 256, L"refused: the computed rectangle is empty (%ld,%ld %ldx%ld)",
                      target.left, target.top, target.right - target.left,
                      target.bottom - target.top);
        return AppBarResult::SpaceNotGranted;
    }

    APPBARDATA set = MakeData(hwnd);
    set.uEdge = query.uEdge;
    set.rc = target;

    // The exact numbers are logged BEFORE the call, because after it the work area may already
    // have changed and there is no way back. When a desktop comes out wrong, this line is the only
    // record of what was asked for.
    LogLine(L"  appbar: ABM_SETPOS asking for %ld,%ld %ldx%ld", target.left, target.top,
            target.right - target.left, target.bottom - target.top);

    SHAppBarMessage(ABM_SETPOS, &set);

    reserved_ = set.rc;
    registration_ = set;

    // WHAT THE SHELL ANSWERED, logged before anything judges it.
    //
    // The shell is allowed to rewrite `set.rc` in place to what it actually granted, and it does:
    // measured on this machine, a request for 42 px came back with a 10 px rectangle in `set.rc`
    // while the work area really did move by 42. A caller that re-derives its own thickness from
    // `registration_.rc` therefore re-claims a different strip -- which is exactly what happened
    // (HANDOFF 8.50: the re-claim asked for 10 px, the landing check refused it, and the bar gave up
    // the space it had just been granted). The requested thickness is remembered below instead.
    LogLine(L"  appbar: ABM_SETPOS returned rc %ld,%ld %ldx%ld", set.rc.left, set.rc.top,
            set.rc.right - set.rc.left, set.rc.bottom - set.rc.top);

    // Judge success by the DESKTOP, not by the return values.
    //
    // Every call in this sequence can report success while the work area stays
    // exactly where it was -- when the edge is already taken, or when the shell
    // declines to give up the space. The work area actually moving is the only
    // evidence that the space was granted, and it is what the rest of the
    // program depends on.
    const RECT workAfter = WorkAreaOf(hwnd);
    const bool moved = workAfter.top != workBefore.top ||
                       workAfter.bottom != workBefore.bottom ||
                       workAfter.left != workBefore.left ||
                       workAfter.right != workBefore.right;

    // --- is the space already ours? ------------------------------------------
    //
    // Checked BEFORE asking again, and this is what stops a re-claim from moving the bar.
    //
    // The shell sends ABN_POSCHANGED after our own reservation, among other reasons. Re-claiming in
    // response was harmless while the rectangle was computed from the work area -- and, once that
    // feedback loop is fixed, it would still be a needless write to the desktop's layout on every
    // event. When the work area already shows `thicknessPx` taken off the docked edge, the bar is
    // where it asked to be and there is nothing to do.
    if (edge == DockEdge::Top && workBefore.top == anchor.top + thicknessPx) {
        LogLine(L"  appbar: already reserved (%ld px off the top); nothing to re-claim", thicknessPx);
        return AppBarResult::Registered;
    }
    if (edge == DockEdge::Bottom && workBefore.bottom == anchor.bottom - thicknessPx) {
        LogLine(L"  appbar: already reserved (%ld px off the bottom); nothing to re-claim",
                thicknessPx);
        return AppBarResult::Registered;
    }

    // --- did the reservation actually land where it was asked to? ---
    //
    // This is the check that would have caught the failure that moved every window on the machine
    // to the bottom of the screen. Reserving space from an AppBar is not reversible by observing
    // it: ABM_SETPOS rewrites the desktop's work area, every maximized window follows, and from
    // inside the program there is nothing that looks wrong. So the result is VERIFIED against what
    // was requested, and a mismatch is undone immediately.
    //
    // VERIFIED AGAINST THE MONITOR, NOT AGAINST workBefore.
    //
    // The first version compared each edge to its own previous value -- "the work area must now be
    // `thicknessPx` shorter than it was". That is a RELATIVE test, and it passes for a bar that keeps
    // taking another thickness every time it is asked, because every step is a legitimate-looking
    // thickness. It is how a bar walks itself down a 1440 px screen 42 px at a time with the check
    // reporting success at every step.
    //
    // What a top bar must actually satisfy is an ABSOLUTE statement about the monitor: the work area's
    // top edge is exactly `thicknessPx` below the monitor's own top edge.
    if (moved) {
        // One call answers both halves, so the two cannot drift apart as they did when they were
        // written out here: the docked edge judged absolutely, the rest judged unchanged.
        const AppBarLanding landing =
            EvaluateLanding(anchor, workBefore, workAfter, edge, thicknessPx);

        if (!landing.sane || !landing.landed || !landing.otherEdgesIntact) {
            LogLine(L"  appbar: the work area did NOT land as asked "
                    L"(wanted %ld px off the %ls; landed=%d, otherEdges=%d, sane=%d); releasing",
                    thicknessPx,
                    edge == DockEdge::Top      ? L"top"
                    : edge == DockEdge::Bottom ? L"bottom"
                    : edge == DockEdge::Left   ? L"left"
                                               : L"right",
                    landing.landed ? 1 : 0, landing.otherEdgesIntact ? 1 : 0, landing.sane ? 1 : 0);
            Release();
            std::swprintf(detail_, 256,
                          L"released: work area moved to %ld,%ld %ldx%ld instead of %ld px off "
                          L"the requested edge",
                          workAfter.left, workAfter.top, workAfter.right - workAfter.left,
                          workAfter.bottom - workAfter.top, thicknessPx);
            return AppBarResult::SpaceNotGranted;
        }
    }

    std::swprintf(detail_, 256,
                  L"work area was %ld,%ld %ldx%ld, now %ld,%ld %ldx%ld; asked for %ld px on %ls",
                  workBefore.left, workBefore.top,
                  workBefore.right - workBefore.left, workBefore.bottom - workBefore.top,
                  workAfter.left, workAfter.top,
                  workAfter.right - workAfter.left, workAfter.bottom - workAfter.top,
                  thicknessPx,
                  edge == DockEdge::Top      ? L"top"
                  : edge == DockEdge::Bottom ? L"bottom"
                  : edge == DockEdge::Left   ? L"left"
                                             : L"right");

    if (!moved) {
        // Keep the registration: the space may become available later when the
        // shell's own bar is hidden, and Release still has to undo ABM_NEW.
        return AppBarResult::SpaceNotGranted;
    }

    // Remembered for the re-claim: the thickness the bar ASKED for and had granted, not the rectangle
    // the shell wrote back. See the log line above.
    claimedThicknessPx_ = thicknessPx;
    claimedEdge_ = edge;
    return AppBarResult::Registered;
}

void AppBar::Release() {
    if (!registered_) return;

    // ABM_REMOVE is what makes the desktop take the strip back. Without it the
    // work area stays inset after the program exits, leaving a permanent gap
    // where the bar used to be -- which looks like a broken desktop rather than
    // a program that did not clean up.
    APPBARDATA data = registration_;
    data.cbSize = sizeof(APPBARDATA);
    SHAppBarMessage(ABM_REMOVE, &data);

    registered_ = false;
    reserved_ = {};
    fullscreenApp_ = false;
    // Cleared with the rest: a released bar has no strip, and a later ABN_POSCHANGED must not use the
    // thickness of one that no longer exists. See claimedThicknessPx_.
    claimedThicknessPx_ = 0;
    LogLine(L"  appbar: released (work area restored)");
}

bool AppBar::HandleCallback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const UINT callback = CallbackMessage();
    if (callback == 0 || message != callback) return false;

    switch (wParam) {
        case ABN_POSCHANGED:
            // Something else moved a bar -- a display change, the shell
            // restarting, the user resizing the taskbar. Our reserved rectangle
            // is now stale, so it is re-claimed immediately; a bar that ignores
            // this drifts out of sync with the space it claims.
            LogLine(L"  appbar: ABN_POSCHANGED -- re-claiming the space");
            if (registered_) {
                // THE THICKNESS THIS BAR ASKED FOR, and not `registration_.rc`'s height.
                //
                // The shell rewrites the rectangle it is handed, so that height is not the strip that
                // was granted: measured here, a granted 42 px came back as a 10 px rect, and the
                // re-claim asked for 10 -- which the landing check correctly refused, after which the
                // bar gave up the space it had. `claimedThicknessPx_` is what was asked for and
                // verified, so re-claiming it is the same request again.
                //
                // The edge is remembered with it. The fallback is for a callback that arrives before
                // any claim has been verified (which cannot happen in practice, but leaves this
                // function total rather than reading a member nothing has written).
                const bool haveClaim = claimedThicknessPx_ > 0;
                const DockEdge edge =
                    haveClaim ? claimedEdge_
                              : (registration_.uEdge == ABE_BOTTOM ? DockEdge::Bottom
                                 : registration_.uEdge == ABE_LEFT ? DockEdge::Left
                                 : registration_.uEdge == ABE_RIGHT ? DockEdge::Right
                                                                    : DockEdge::Top);
                const LONG thickness = haveClaim ? claimedThicknessPx_
                                                 : registration_.rc.bottom - registration_.rc.top;
                if (thickness > 0) Claim(hwnd, edge, thickness);
            }
            return true;

        case ABN_FULLSCREENAPP:
            // A full-screen window just came to the front, or went away.
            // Staying on top of one is a defect, so this is recorded and the
            // caller can act on it.
            fullscreenApp_ = lParam != FALSE;
            LogLine(L"  appbar: full-screen app %s", fullscreenApp_ ? L"opened" : L"closed");
            return true;

        case ABN_STATECHANGE:
            LogLine(L"  appbar: ABN_STATECHANGE");
            return true;

        case ABN_WINDOWARRANGE:
            LogLine(L"  appbar: ABN_WINDOWARRANGE");
            return true;

        default:
            break;
    }
    return true;   // it was our message, even if nothing needed doing
}

} // namespace island
