// =====================================================================
//  NativeTaskbar.cpp
// =====================================================================
#include "NativeTaskbar.h"

namespace island {
namespace {

// The shell's taskbar windows. Shell_TrayWnd is the primary bar; there is one
// Shell_SecondaryTrayWnd per additional monitor. Both must go, or hiding the
// primary one just looks like the bar moved to the other screen.
constexpr const wchar_t* kTrayClass          = L"Shell_TrayWnd";
constexpr const wchar_t* kSecondaryTrayClass = L"Shell_SecondaryTrayWnd";
constexpr const wchar_t* kTrayNotifyClass    = L"TrayNotifyWnd";

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
    if (!weHidIt_) return false;

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
