// =====================================================================
//  NativeTaskbar.cpp
// =====================================================================
#include "NativeTaskbar.h"

#include "Log.h"
#include "Monitor.h"

namespace island {
namespace {

// The shell's taskbar windows. Shell_TrayWnd is the primary bar; there is one
// Shell_SecondaryTrayWnd per additional monitor. Both must go, or hiding the
// primary one just looks like the bar moved to the other screen.
constexpr const wchar_t* kTrayClass          = L"Shell_TrayWnd";
constexpr const wchar_t* kSecondaryTrayClass = L"Shell_SecondaryTrayWnd";
constexpr const wchar_t* kTrayNotifyClass    = L"TrayNotifyWnd";

// The work area, read and written with SPI_GETWORKAREA / SPI_SETWORKAREA.
//
// NO SPIF_* FLAGS ON THE WRITE, and that is the whole finding. Measured on the real desktop:
//
//   SPI_SETWORKAREA, SPIF_UPDATEINIFILE|SPIF_SENDCHANGE -> returns TRUE, work area does NOT move
//   SPI_SETWORKAREA, SPIF_SENDCHANGE only               -> returns TRUE, work area does NOT move
//   SPI_SETWORKAREA, no flags                           -> work area moves, and is still moved 12 s
//                                                          later (longer than the bar's 5 s cycle)
//
// The announcement is what undoes it: sending WM_SETTINGCHANGE(SPI_SETWORKAREA) to HWND_BROADCAST
// afterwards put the 32 px strip straight back, because the shell re-derives the work area from its own
// taskbar when it hears about a change. So the release is silent on purpose, and re-asserted on the
// same slow cadence as the re-hide.
bool ReadWorkArea(RECT* out) {
    return SystemParametersInfoW(SPI_GETWORKAREA, 0, out, 0) != FALSE;
}

bool WriteWorkArea(const RECT& wanted) {
    RECT value = wanted;
    return SystemParametersInfoW(SPI_SETWORKAREA, 0, &value, 0) != FALSE;
}

} // namespace

NativeTaskbar::~NativeTaskbar() {
    Restore();
}

bool NativeTaskbar::Locate() {
    // Re-locate from scratch every time. Caching the handles across an Explorer
    // restart would leave us holding a dead HWND, and ShowWindow on a destroyed
    // window fails silently -- the taskbar would come back and we would never
    // notice.
    trayWnd_ = FindWindowW(kTrayClass, nullptr);
    trayNotify_ = trayWnd_ ? FindWindowExW(trayWnd_, nullptr, kTrayNotifyClass, nullptr) : nullptr;

    if (trayWnd_) {
        wasVisible_ = IsWindowVisible(trayWnd_) != FALSE;
        located_ = true;
    } else {
        located_ = false;
    }
    return trayWnd_ != nullptr;
}

bool NativeTaskbar::Hide() {
    // Re-locate when we have nothing, or when the shell has replaced the window
    // under us. IsWindow is the cheap test; a restarted Explorer leaves a handle
    // that is no longer a window at all.
    if (!trayWnd_ || !IsWindow(trayWnd_)) {
        Locate();
    }

    if (!trayWnd_) return false;

    // Remember the original state only the first time, so a later call cannot
    // overwrite "was visible" with "we already hid it".
    if (!weHidIt_) {
        wasVisible_ = IsWindowVisible(trayWnd_) != FALSE;
    }

    if (wasVisible_ || weHidIt_) {
        ShowWindow(trayWnd_, SW_HIDE);
    }

    // Every secondary monitor's bar as well, otherwise the taskbar simply appears
    // to have moved to another screen.
    for (HWND h = FindWindowW(kSecondaryTrayClass, nullptr); h != nullptr;
         h = FindWindowExW(nullptr, h, kSecondaryTrayClass, nullptr)) {
        ShowWindow(h, SW_HIDE);
    }

    // The notification corner is a child of the primary bar and normally goes with
    // it; it is hidden explicitly because on some builds it is a separate
    // top-level window that survives its parent being hidden.
    if (trayNotify_) {
        ShowWindow(trayNotify_, SW_HIDE);
    }

    weHidIt_ = true;
    return IsWindowVisible(trayWnd_) == FALSE;
}

void NativeTaskbar::Restore() {
    // The strip goes back FIRST, before the taskbar is shown: showing the taskbar makes the shell
    // re-derive the work area from it, and this way the two answers are the same one.
    RestoreWorkArea();

    // Only undo what we did. If the taskbar was already hidden when the bar
    // started -- auto-hide, or the user's own choice -- putting it back would be
    // overriding them.
    if (!weHidIt_ || !wasVisible_) {
        weHidIt_ = false;
        return;
    }

    if (!trayWnd_ || !IsWindow(trayWnd_)) {
        trayWnd_ = FindWindowW(kTrayClass, nullptr);
    }

    if (trayWnd_) {
        ShowWindow(trayWnd_, SW_SHOW);
        if (trayNotify_) {
            ShowWindow(trayNotify_, SW_SHOW);
        }
    }

    for (HWND h = FindWindowW(kSecondaryTrayClass, nullptr); h != nullptr;
         h = FindWindowExW(nullptr, h, kSecondaryTrayClass, nullptr)) {
        ShowWindow(h, SW_SHOW);
    }

    weHidIt_ = false;
}

bool NativeTaskbar::EnsureHidden() {
    // NOTHING WAS EVER HIDDEN BY US. Two situations produce that, and they want opposite answers.
    //
    // The taskbar did not exist when this process started -- which is the ordinary case for a bar that
    // starts at LOGON, because the process and the shell are woken at the same moment and nothing orders
    // them. This used to be final: `Hide()` at startup returned false, the caller noted "not found", and
    // the periodic check was gated on that note, so the native taskbar stayed on screen under our bar for
    // the rest of the session with nothing in the log to say why. A bar that is installed to start at
    // logon is exactly the case that has to survive this, so the window is looked for again here.
    //
    // ... or the window is there and INVISIBLE, which is the user's own choice (auto-hide, or hidden by
    // hand). That is not ours to override, and it is the reason this is a visibility test rather than an
    // unconditional hide.
    if (!weHidIt_) {
        if (!trayWnd_ || !IsWindow(trayWnd_)) {
            if (!Locate()) return false;      // still nothing: the shell is not up yet either
        }
        if (!IsWindowVisible(trayWnd_)) return false;
        return Hide();                        // it is there and it is visible, and that is the job
    }

    // A stale handle means Explorer restarted and built a new taskbar, which
    // starts out visible. That is exactly the case worth catching: without this
    // the native bar reappears the first time the shell restarts.
    if (trayWnd_ && !IsWindow(trayWnd_)) {
        HandleExplorerRestart();
        return Hide();
    }

    if (!trayWnd_) {
        return Hide();
    }

    if (IsWindowVisible(trayWnd_)) {
        return Hide();
    }

    // The primary bar is hidden; check the secondary ones too, since a monitor
    // hot-plug creates a new visible one.
    for (HWND h = FindWindowW(kSecondaryTrayClass, nullptr); h != nullptr;
         h = FindWindowExW(nullptr, h, kSecondaryTrayClass, nullptr)) {
        if (IsWindowVisible(h)) {
            return Hide();
        }
    }

    return false;
}

bool NativeTaskbar::TaskbarMonitorBounds(RECT* out) const {
    const HWND reference = (trayWnd_ && IsWindow(trayWnd_)) ? trayWnd_ : GetDesktopWindow();
    const HMONITOR monitor = MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;

    *out = info.rcMonitor;
    return true;
}

NativeTaskbar::WorkAreaRelease NativeTaskbar::ReleaseWorkArea() {
    WorkAreaRelease result;

    // Only while WE are the reason the taskbar is hidden. With the taskbar on screen its reservation is
    // correct and taking it away would put maximized windows on top of a visible taskbar.
    if (!weHidIt_) return result;

    RECT screen = {};
    if (!TaskbarMonitorBounds(&screen)) return result;

    RECT before = {};
    if (!ReadWorkArea(&before)) return result;

    result.before = before;

    if (!haveWorkBefore_) {
        workBefore_ = before;
        haveWorkBefore_ = true;
    }

    if (SameRect(before, screen)) {
        // Nothing to give back: the work area is already the whole monitor (auto-hide, or an earlier
        // release in this session).
        releasedWorkArea_ = true;
        result.released = true;
        result.after = before;
        return result;
    }

    if (!WriteWorkArea(screen)) return result;

    // Verified rather than assumed, because the call above is the kind that reports success and does
    // nothing: with SPIF_SENDCHANGE it returns TRUE and the work area does not move at all.
    RECT after = {};
    if (!ReadWorkArea(&after)) return result;

    result.after = after;
    releasedWorkArea_ = SameRect(after, screen);
    result.released = releasedWorkArea_;
    result.changed = !SameRect(before, after);
    return result;
}

NativeTaskbar::WorkAreaRelease NativeTaskbar::EnsureWorkAreaReleased() {
    WorkAreaRelease result;
    if (!weHidIt_ || !releasedWorkArea_) return result;

    RECT screen = {};
    if (!TaskbarMonitorBounds(&screen)) return result;

    RECT now = {};
    if (!ReadWorkArea(&now)) return result;

    result.before = now;
    result.after = now;
    if (SameRect(now, screen)) {
        result.released = true;      // still ours: nothing to do, and nothing changed
        return result;
    }

    if (!WriteWorkArea(screen)) return result;

    RECT after = {};
    if (!ReadWorkArea(&after)) return result;

    result.after = after;
    releasedWorkArea_ = SameRect(after, screen);
    result.released = releasedWorkArea_;
    result.changed = !SameRect(now, after);
    return result;
}

void NativeTaskbar::RestoreWorkArea() {
    if (!haveWorkBefore_) return;

    // Put back what was FOUND, not what the shell would compute now. The shell re-derives the work area
    // from the taskbar as soon as it is shown again, so this is not the last word in the ordinary case --
    // it is the answer for the case where the shell does not, and it is the one that is always right.
    const bool written = WriteWorkArea(workBefore_);
    RECT after = {};
    const bool read = ReadWorkArea(&after);

    LogAndPrint("  work area: restored to %ld,%ld - %ld,%ld (%s)", workBefore_.left, workBefore_.top,
                workBefore_.right, workBefore_.bottom,
                written && read ? (SameRect(after, workBefore_) ? "confirmed" : "NOT CONFIRMED")
                                : "the write or the read failed");

    haveWorkBefore_ = false;
    releasedWorkArea_ = false;
}

bool NativeTaskbar::IsVisible() const {
    if (!trayWnd_ || !IsWindow(trayWnd_)) return false;
    return IsWindowVisible(trayWnd_) != FALSE;
}

namespace {

// The last sub-authority of a token's integrity SID: 0x1000 low, 0x2000 medium,
// 0x3000 high. Returns false when the value cannot be read.
bool IntegrityRid(HANDLE process, DWORD* rid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;

    BYTE buffer[256] = {};
    DWORD returned = 0;
    bool ok = false;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &returned)) {
        auto* mandatory = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        const DWORD count = *GetSidSubAuthorityCount(mandatory->Label.Sid);
        if (count > 0) {
            *rid = *GetSidSubAuthority(mandatory->Label.Sid, count - 1);
            ok = true;
        }
    }
    CloseHandle(token);
    return ok;
}

} // namespace

bool NativeTaskbar::IsBlockedByIntegrity() {
    if (!trayWnd_) return false;

    DWORD trayPid = 0;
    GetWindowThreadProcessId(trayWnd_, &trayPid);
    if (!trayPid) return false;

    DWORD ours = 0;
    DWORD theirs = 0;
    if (!IntegrityRid(GetCurrentProcess(), &ours)) return false;

    HANDLE trayProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, trayPid);
    if (!trayProcess) return false;
    const bool haveTheirs = IntegrityRid(trayProcess, &theirs);
    CloseHandle(trayProcess);
    if (!haveTheirs) return false;

    return ours < theirs;
}

void NativeTaskbar::HandleExplorerRestart() {
    // The shell owns the taskbar's visibility, and a restarted shell considers it
    // visible again. Claim ownership of the hide so EnsureHidden keeps enforcing
    // it, but keep wasVisible_ as "yes, it should be there" -- which is what
    // Restore needs in order to put it back.
    weHidIt_ = true;
    located_ = false;
}

} // namespace island
