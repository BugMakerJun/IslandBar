// =====================================================================
//  AppBar.h
//  Registers the island as a Windows AppBar, so the desktop gives up the
//  strip it occupies.
//
//  This is the difference between a bar that REPLACES the taskbar and a bar
//  that merely floats over the desktop. Without it, a maximized window still
//  covers the top of the screen and the island sits on top of the window's
//  title bar; with it, the work area starts below the island and maximized
//  windows fit the space that is left, exactly as they do for the real
//  taskbar.
//
//  The protocol is older than the rest of this project and has some sharp
//  edges, all of which this file deals with rather than passing on:
//
//   * ABM_NEW can succeed while the reserved space is silently ignored, so
//     success is judged by the work area actually changing, not by the return
//     value.
//   * ABM_QUERYPOS returns a suggested rectangle; the documentation is
//     explicit that the caller must apply its own thickness and then send
//     ABM_SETPOS. Treating the suggestion as final is the usual way this goes
//     wrong.
//   * The shell calls back with ABN_POSCHANGED whenever anything else moves a
//     bar, and a window that ignores it drifts out of sync with the space it
//     claims.
// =====================================================================
#pragma once

#include "platform/Monitor.h"

#include <windows.h>
// APPBARDATA and the ABM_* constants live here, and the struct below holds one as a member, so
// the header needs it and not just the implementation. Leaving it out produced
//     AppBar.h: C3646: 'registration_': unknown override specifier
// on the member declaration: with APPBARDATA undeclared, the compiler read the member's NAME as a
// specifier, so the error pointed nowhere near the missing include.
#include <shellapi.h>

namespace island {

// Why an AppBar registration attempt ended the way it did. The reasons need
// different responses, which is why they are distinguished rather than
// collapsed into a bool.
enum class AppBarResult {
    Registered,          // the work area changed: the desktop gave up the space
    NoWindow,            // nothing to register
    CallbackRefused,     // the shell would not accept our callback message
    NotRegistered,       // ABM_NEW was refused (another bar owns the edge)
    SpaceNotGranted,     // registered, but the work area did not move
};

const wchar_t* AppBarResultName(AppBarResult result);

// Where the bar is docked, expressed the way SHAppBarMessage wants it.
UINT DockEdgeToAppBarEdge(DockEdge edge);

// The rectangle to reserve for a bar on `edge` of a monitor whose bounds are `monitorBounds`.
//
// Takes the MONITOR's bounds and never the work area, and it is a free function because that choice is
// the whole of a bug that is otherwise untestable from here: claiming the strip changes the work area,
// so a rectangle derived from the work area depends on the result of the previous attempt. The bar
// walked itself down the screen one thickness at a time and every step looked legitimate.
//
// Built from the monitor's bounds the answer is the same however many times it is asked for, which is
// the property the self-test asserts.
RECT AppBarStripRect(const RECT& monitorBounds, DockEdge edge, LONG thicknessPx);

// What a reservation actually did, judged from the work area before and after.
//
// A free function so both halves of the judgement can be tested, because they pull in opposite
// directions and each was wrong once:
//
//   * the docked edge must be judged ABSOLUTELY -- "the work area's top is monitor.top + thickness" --
//     because a relative test ("a thickness shorter than it was") accepts a bar that takes another
//     thickness on every re-claim, which walks it down the screen;
//
//   * the other three edges must be judged against the work area AS IT WAS, not against the monitor,
//     because the work area's far edge already stops short of the monitor's by the shell taskbar's own
//     strip. Judging those against the monitor rejects a reservation that was perfectly correct.
//
// Getting either one wrong produces a bar that cannot hold the strip it claimed.
struct AppBarLanding {
    bool landed = false;          // the docked edge moved to exactly where it was asked to
    bool otherEdgesIntact = true; // the other three did not move
    bool sane = true;             // the result is a work area at all, not an inverted rectangle
};

AppBarLanding EvaluateLanding(const RECT& monitorBounds, const RECT& workBefore,
                              const RECT& workAfter, DockEdge edge, LONG thicknessPx);

class AppBar {
public:
    AppBar() = default;
    ~AppBar();

    AppBar(const AppBar&) = delete;
    AppBar& operator=(const AppBar&) = delete;

    // The message the shell will use to call us back. Must be in the WM_APP
    // range and is allocated once per process.
    static UINT CallbackMessage();

    // Claims `thicknessPx` along `edge` of the monitor `hwnd` is on.
    //
    // Called again after a display change or an ABN_POSCHANGED; reclaiming is
    // idempotent and cheap.
    AppBarResult Claim(HWND hwnd, DockEdge edge, LONG thicknessPx);

    // Gives the space back. Always called from the destructor, so a normal exit
    // cannot leave the desktop short of a strip.
    void Release();

    // Handles the shell's callbacks. Returns true when the message was ours and
    // has been dealt with.
    //
    // `hwnd` is needed to re-claim the space on ABN_POSCHANGED.
    bool HandleCallback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool IsRegistered() const { return registered_; }

    // What actually happened, for the log: "work area was 0,0,2560,1408 and is
    // now 0,48,2560,1408".
    const wchar_t* LastDetail() const { return detail_; }

private:
    // Whether a full-screen application is in front. A bar that stays on top of
    // a full-screen game or video is a defect, and the shell tells us when it
    // happens rather than leaving us to guess.
    bool fullscreenApp_ = false;

    bool    registered_ = false;
    // Kept so ABM_REMOVE and the ABN_* callbacks use the same data the registration was made
    // with, rather than reconstructing it.
    APPBARDATA registration_ = {};

    // The strip this bar ASKED FOR and had granted: the thickness, and the edge it was granted on.
    //
    // NOT derivable from `registration_.rc`: the shell rewrites the rectangle it is handed during
    // ABM_SETPOS, so after a granted 42 px request that rect measured 10 px tall -- and a re-claim that
    // read its thickness from there asked for a 10 px strip, failed the landing check and released the
    // space the bar had just been given (HANDOFF 8.50). Recorded on success and cleared on release.
    LONG     claimedThicknessPx_ = 0;
    DockEdge claimedEdge_ = DockEdge::Top;
    wchar_t detail_[256] = {};
};

} // namespace island
