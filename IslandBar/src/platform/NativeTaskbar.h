// =====================================================================
//  NativeTaskbar.h
//  Hides and restores the Windows shell taskbar.
//
//  This is the first thing the project does that affects the rest of the
//  desktop, so it is built around one rule: whatever is hidden must come back.
//  Hiding is idempotent, restoring is idempotent, and the state actually found is
//  remembered rather than assumed -- if the taskbar was already hidden when the
//  bar started (by the user, by an auto-hide setting, or by a previous crashed
//  run) then restoring must not fight that choice.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

class NativeTaskbar {
public:
    NativeTaskbar() = default;
    ~NativeTaskbar();

    NativeTaskbar(const NativeTaskbar&) = delete;
    NativeTaskbar& operator=(const NativeTaskbar&) = delete;

    // Locates the shell's taskbar windows. Safe to call when none exist.
    bool Locate();

    // Hides the taskbar, remembering whether it was visible first. Repeated calls
    // are cheap and harmless.
    //
    // Returns true if the taskbar is hidden after the call.
    bool Hide();

    // Makes the taskbar visible again, but only if this object hid it, or if the
    // caller insists. Never resurrects a taskbar the user had hidden themselves.
    void Restore();

    // Re-hides if the shell brought the taskbar back -- Explorer restarting, a
    // display change, or the user pressing the Windows key.
    //
    // Returns true if it had to act, so the caller can throttle the check.
    bool EnsureHidden();

    // --- the strip the work area reserves ---------------------------------------------------
    //
    // A hidden AppBar keeps its claim: the shell's taskbar holds the desktop's WORK AREA inset by its
    // thickness whether or not it is on screen, so a maximized window stops below a band that holds
    // nothing but our island, and the user photographs the empty space either side of it. Released here
    // so that a maximized window fills the screen, with the island floating over the application.
    //
    // All of it is a no-op unless this object is the reason the taskbar is gone.

    // Gives the strip back, reporting what it found so the caller can write its own line and act on the
    // one follow-up: an ALREADY maximized window does not follow the work area (measured), so it has to
    // be brought forward by hand if the user is to see anything change.
    struct WorkAreaRelease {
        bool released = false;   // the work area has no strip in it after the call
        bool changed = false;    // ... and this call is what removed it
        RECT before = {};        // the work area as found
        RECT after = {};         // what it is now
    };
    WorkAreaRelease ReleaseWorkArea();

    // Puts the strip back out of the way if something re-took it (the shell re-derives the work area
    // whenever the reservation is announced, and on a display change). `changed` is true when it had to
    // act, and `before`/`after` then describe the pair the caller needs to bring forward any window that
    // was maximized in the meantime.
    WorkAreaRelease EnsureWorkAreaReleased();

    // Puts the work area back exactly as it was found, and forgets it. Called on the way out.
    void RestoreWorkArea();

    // True when this process is confined to a lower integrity level than the
    // shell's taskbar, which makes hiding it impossible.
    //
    // This is the difference between "the code is wrong" and "this process is not
    // allowed": UIPI drops window messages sent from a lower integrity level to a
    // higher one, so ShowWindow on the taskbar returns FALSE and does nothing, and
    // SHAppBarMessage returns 0. No error code is produced. Detecting the cause
    // turns a silent no-op into a sentence the user can act on.
    //
    // It happens when the binary is launched inside a sandbox that lowers the
    // token (some security tools do this), or from a low-integrity host. Running
    // it normally, at medium integrity alongside Explorer, is enough to fix it.
    bool IsBlockedByIntegrity();

private:
    void HandleExplorerRestart();
    // The screen bounds of the display the shell's taskbar is on: what the strip is released TO.
    bool TaskbarMonitorBounds(RECT* out) const;

    HWND trayWnd_ = nullptr;        // Shell_TrayWnd: the primary taskbar
    HWND trayNotify_ = nullptr;     // TrayNotifyWnd: the clock/notification corner
    bool wasVisible_ = false;       // as found, before we touched anything
    bool weHidIt_ = false;
    bool located_ = false;

    // The work area this process found, and whether it has been touched. Saved even when the values
    // look familiar: the point is to put back what was there, not what the shell would compute.
    RECT workBefore_ = {};
    bool haveWorkBefore_ = false;
    bool releasedWorkArea_ = false;
};

} // namespace island
