// =====================================================================
//  WindowList.h
//  Which top-level windows belong on the bar, and putting one in front.
//
//  This is the read side of replacing the taskbar: enumerate what is open,
//  decide what deserves a button, and activate one when it is clicked.
//
//  The enumeration calls are a handful of lines. The difficulty is entirely
//  in the DECISION, because `EnumWindows` hands back every top-level window
//  -- including the invisible, the cloaked, the tool palettes and the shell's
//  own furniture. A list built from the raw enumeration is hundreds of
//  entries long and mostly ghosts.
//
//  So the decision is a pure function over a plain description of a window
//  (`WindowTraits`), separated from the Win32 calls that fill that description
//  in. The rules are the part that is easy to get wrong and they are exactly
//  the part a test can exercise: the development sandbox has a desktop with no
//  windows on it, so anything that needs a real window to exist cannot be
//  checked there at all.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>

namespace island {

// How many buttons the bar will hold. Fixed, because the bar has a fixed
// window and a list that can grow without bound is a list that eventually
// draws outside it.
inline constexpr int kMaxTaskWindows = 32;

// Longest title kept, in wide characters. A title longer than this is clipped
// for display anyway, and a fixed field keeps the entry trivially copyable.
inline constexpr size_t kTaskTitleCapacity = 128;

// Everything about a window that the decision depends on.
//
// A plain struct rather than the HWND itself, so the rules can be tested
// without a desktop full of windows. Every field comes from one Win32 call,
// named in the comment.
struct WindowTraits {
    bool hasTitle = false;      // GetWindowTextLengthW > 0
    bool visible = false;       // IsWindowVisible
    bool iconic = false;        // IsIconic -- minimised
    bool toolWindow = false;    // GetWindowLongPtr(GWL_EXSTYLE) & WS_EX_TOOLWINDOW
    bool appWindow = false;     // GetWindowLongPtr(GWL_EXSTYLE) & WS_EX_APPWINDOW
    bool hasOwner = false;      // GetWindow(hwnd, GW_OWNER) != nullptr
    bool cloaked = false;       // DwmGetWindowAttribute(DWMWA_CLOAKED)
    bool hasSize = false;       // the window rect has a non-zero extent
    bool isShell = false;       // Shell_TrayWnd, Progman, WorkerW, ...
    bool isSelf = false;        // this process's own windows
};

// Whether a window earns a button on the bar.
//
// The rules, and why each one is needed:
//
//  * Not visible -> out. Obvious, and it removes most of the enumeration.
//  * Cloaked -> out. This is the important one and the least obvious: Windows
//    keeps a window for every suspended UWP application, and they are all
//    "visible" as far as IsWindowVisible is concerned. They are hidden by DWM
//    instead, and only DWMWA_CLOAKED reveals it. Without this check the bar is
//    full of invisible ghosts, which is exactly how a taskbar looks broken.
//  * No title and not marked as an application window -> out. A great many
//    helper windows are visible, titled with an empty string and exist only to
//    receive messages.
//  * Owned windows -> out, unless they claim WS_EX_APPWINDOW. An owned window
//    is a dialog or a palette belonging to another window; the shell's own
//    rule is the same, and WS_EX_APPWINDOW is how a window opts back in.
//  * WS_EX_TOOLWINDOW -> out, unless it claims WS_EX_APPWINDOW. Tool windows
//    are floating palettes and should not be listed.
//  * The shell's own furniture -> out. Shell_TrayWnd, Progman and WorkerW are
//    visible, titled and owned by Explorer; listing them puts phantom entries
//    on the bar and, worse, one of them is the desktop.
//  * This process's own windows -> out. The bar must not list itself.
//  * A window with no extent -> out. It cannot be seen or clicked.
inline bool ShouldListWindow(const WindowTraits& traits) {
    if (traits.isSelf || traits.isShell) return false;
    if (!traits.visible) return false;
    if (traits.cloaked) return false;
    if (!traits.hasSize) return false;

    // A window may be listed without a title only if it declares itself an
    // application window; that flag is a deliberate statement by the author.
    if (!traits.hasTitle && !traits.appWindow) return false;

    // WS_EX_APPWINDOW overrides both of the "belongs to something else" rules.
    if (traits.appWindow) return true;

    if (traits.hasOwner) return false;
    if (traits.toolWindow) return false;

    return true;
}

// One entry on the bar.
struct TaskEntry {
    HWND handle = nullptr;
    DWORD processId = 0;
    bool  iconic = false;    // minimised; activating it must restore it
    wchar_t title[kTaskTitleCapacity] = {};

    // The mark used to tell this process's windows from everything else.
    unsigned long long mark = 0;
};

// Chooses the next window to bring forward, given the current one.
//
// Only used to decide what the bar highlights and to restore the previous
// window when the bar itself is dismissed; the rules are here so they can be
// tested rather than scattered through the caller.
//
// Returns the index into `entries`, or -1 when there is nothing to choose.
inline int PickNextWindow(const TaskEntry* entries, int count, HWND current) {
    if (!entries || count <= 0) return -1;

    // Preference order: the first entry that is not minimised and is not the
    // current window, then anything that is not the current window, then the
    // current window itself.
    int fallback = -1;
    for (int i = 0; i < count; ++i) {
        if (entries[i].handle == current) continue;
        if (fallback < 0) fallback = i;
        if (!entries[i].iconic) return i;
    }
    if (fallback >= 0) return fallback;

    for (int i = 0; i < count; ++i) {
        if (entries[i].handle == current) return i;
    }
    return -1;
}

class WindowList {
public:
    WindowList() = default;

    // Refreshes from the desktop. Cheap enough to call on a timer: the
    // enumeration is a few hundred windows and no window is touched more than
    // a handful of times.
    //
    // Returns the number of entries.
    int Refresh();

    // Brings a window to the front, restoring it first when it is minimised.
    //
    // Returns true when the window ended up in front. Windows is allowed to
    // refuse -- a foreground lock protects whatever the user was doing -- and a
    // refusal is reported rather than hidden, because it looks exactly like a
    // click that did nothing.
    bool Activate(HWND handle);

    // Minimises a window, the way clicking the active window's button on the real taskbar does.
    //
    // The taskbar is a TOGGLE: the button of the window in front puts it away, and the button of any
    // other window brings that one forward. Without this the bar could only ever bring things forward,
    // which is half the gesture -- reported as "clicking an application on the desktop does not
    // minimise it".
    bool Minimize(HWND handle);

    int Count() const { return count_; }
    const TaskEntry& At(int index) const { return entries_[index]; }

    // The window in front when the list was last refreshed, so the bar can
    // highlight it.
    HWND ForegroundHandle() const { return foreground_; }

    // Why the last refresh kept the number of entries it did, for the log:
    // "12 of 337 windows listed, 291 cloaked, 22 untitled, ...".
    const wchar_t* LastSummary() const { return summary_; }

private:
    TaskEntry entries_[kMaxTaskWindows] = {};
    int  count_ = 0;
    HWND foreground_ = nullptr;
    int  totalSeen_ = 0;
    wchar_t summary_[192] = {};

    // The order the buttons were in last time, so a button never moves once it exists.
    //
    // EnumWindows walks in Z order, front to back, and using that as the button order is wrong for
    // the same reason it would be wrong on the real taskbar: ACTIVATING A WINDOW CHANGES ITS Z ORDER,
    // so clicking a button moved that button to the front. The report was exactly that -- "the icon
    // jumped to the first position".
    //
    // A button's position is something a person learns and reaches for. It has to be stable: new
    // windows append, closed windows leave a gap that closes up, and nothing else moves.
    HWND order_[kMaxTaskWindows] = {};
    int  orderCount_ = 0;
};

} // namespace island
