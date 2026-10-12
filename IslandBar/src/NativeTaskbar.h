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

    bool IsFound() const { return trayWnd_ != nullptr; }

    // Whether the taskbar is visible right now, as the OS reports it.
    bool IsVisible() const;

    // True if this object is the reason the taskbar is hidden.
    bool WeHidIt() const { return weHidIt_; }

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

    HWND trayWnd_ = nullptr;        // Shell_TrayWnd: the primary taskbar
    HWND trayNotify_ = nullptr;     // TrayNotifyWnd: the clock/notification corner
    bool wasVisible_ = false;       // as found, before we touched anything
    bool weHidIt_ = false;
    bool located_ = false;
};

} // namespace island
