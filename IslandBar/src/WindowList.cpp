// =====================================================================
//  WindowList.cpp
// =====================================================================
#include "WindowList.h"

#include "Log.h"

#include <dwmapi.h>

#include <cstdio>
#include <cwchar>
#include <cstring>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

namespace island {
namespace {

// Windows the shell owns that must never appear as buttons.
//
// They are visible, they have titles and they belong to Explorer, so every rule
// in ShouldListWindow would keep them. Progman IS the desktop: listing it puts
// an entry on the bar that, when clicked, tries to focus the wallpaper.
bool IsShellWindow(HWND hwnd) {
    struct ClassEntry {
        const wchar_t* name;
    };
    const wchar_t* shellClasses[] = {
        L"Shell_TrayWnd",          // the taskbar
        L"Shell_SecondaryTrayWnd", // taskbars on other monitors
        L"Progman",                // the desktop
        L"WorkerW",                // desktop layers
        L"Windows.UI.Core.CoreWindow",   // UWP hosts, cloaked anyway
        L"ApplicationManager_DesktopShellWindow",
        L"MultitaskingViewFrame",  // task view
        L"ForegroundStaging",
        L"XamlExplorerHostIslandWindow",
        L"TaskListThumbnailWnd",
    };

    wchar_t className[128] = {};
    if (GetClassNameW(hwnd, className, 128) == 0) return false;
    for (const wchar_t* candidate : shellClasses) {
        if (_wcsicmp(className, candidate) == 0) return true;
    }
    return false;
}

// Whether DWM is hiding this window.
//
// The single most important check in this file. Suspended UWP applications keep
// a top-level window that IsWindowVisible calls visible; DWM cloaks it. A list
// built without this is full of windows that cannot be seen.
bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

struct EnumContext {
    DWORD selfProcessId = 0;
    int   listed = 0;
    int   cloakedSkipped = 0;
    int   untitledSkipped = 0;
    int   invisibleSkipped = 0;
    int   ownedSkipped = 0;
    int   toolSkipped = 0;
    int   shellSkipped = 0;
    int   selfSkipped = 0;
    TaskEntry* entries = nullptr;
    int capacity = 0;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);

    // Cheapest and most selective test first: most top-level windows are not
    // visible.
    if (!IsWindowVisible(hwnd)) {
        ++context->invisibleSkipped;
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    WindowTraits traits;
    traits.visible = true;
    traits.cloaked = IsCloaked(hwnd);
    traits.isShell = IsShellWindow(hwnd);
    traits.isSelf = (pid == context->selfProcessId);
    traits.iconic = IsIconic(hwnd) != FALSE;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    traits.toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
    traits.appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
    traits.hasOwner = GetWindow(hwnd, GW_OWNER) != nullptr;

    wchar_t title[kTaskTitleCapacity] = {};
    const int titleLength = GetWindowTextW(hwnd, title, kTaskTitleCapacity);
    traits.hasTitle = titleLength > 0;

    RECT rect = {};
    if (GetWindowRect(hwnd, &rect)) {
        traits.hasSize = (rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0;
    }

    if (!ShouldListWindow(traits)) {
        // Counted by the reason, so the log explains the number instead of just
        // stating it. A taskbar holding two buttons out of three hundred windows
        // is either correct or a bug, and only the breakdown says which.
        if (traits.cloaked) ++context->cloakedSkipped;
        else if (traits.isShell) ++context->shellSkipped;
        else if (traits.isSelf) ++context->selfSkipped;
        else if (!traits.hasTitle && !traits.appWindow) ++context->untitledSkipped;
        else if (traits.hasOwner) ++context->ownedSkipped;
        else if (traits.toolWindow) ++context->toolSkipped;
        return TRUE;
    }

    if (context->listed >= context->capacity) return FALSE;   // full

    TaskEntry& entry = context->entries[context->listed];
    entry.handle = hwnd;
    entry.processId = pid;
    entry.iconic = traits.iconic;
    wcsncpy_s(entry.title, kTaskTitleCapacity, title, _TRUNCATE);
    // The process id is the mark: a window belonging to this process must never
    // be listed, and the pid is what identifies one.
    entry.mark = pid;
    ++context->listed;
    return TRUE;
}

} // namespace

int WindowList::Refresh() {
    EnumContext context;
    context.selfProcessId = GetCurrentProcessId();
    context.entries = entries_;
    context.capacity = kMaxTaskWindows;

    totalSeen_ = 0;
    foreground_ = GetForegroundWindow();

    // EnumWindows walks the top level in Z order, front to back. That order is the right INPUT -- it
    // says which windows are on the desktop -- but it is the wrong ORDER for the buttons, because
    // activating a window changes its Z order. Using it directly meant that clicking a button moved
    // that button to the front, which is what was reported: "the icon jumped to the first position".
    EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&context));

    const int listed = context.listed;

    // --- give the buttons their stable order ---------------------------------------
    //
    // A button's position is something a person learns and reaches for. So: keep every window that is
    // still listed in the position it already had, then append the ones that are new. Nothing else
    // moves. A closed window leaves a gap that closes up on its own, and the remaining buttons shift
    // -- which is unavoidable without leaving holes, and is what the real taskbar does too.
    {
        HWND next_[kMaxTaskWindows] = {};
        int nextCount = 0;

        // Existing windows, in their previous order.
        for (int i = 0; i < orderCount_ && nextCount < kMaxTaskWindows; ++i) {
            for (int j = 0; j < listed; ++j) {
                if (context.entries[j].handle == order_[i]) {
                    next_[nextCount++] = order_[i];
                    break;
                }
            }
        }

        // Windows that were not there last time, in the Z order the enumeration gave -- so a newly
        // opened window appears at the end, where there is room for it to appear.
        for (int j = 0; j < listed && nextCount < kMaxTaskWindows; ++j) {
            bool already = false;
            for (int k = 0; k < nextCount; ++k) {
                if (next_[k] == context.entries[j].handle) {
                    already = true;
                    break;
                }
            }
            if (!already) next_[nextCount++] = context.entries[j].handle;
        }

        for (int i = 0; i < nextCount; ++i) order_[i] = next_[i];
        orderCount_ = nextCount;

        // The entries array is reordered to match, so At(i) is the i-th button.
        TaskEntry stable[kMaxTaskWindows] = {};
        for (int i = 0; i < nextCount; ++i) {
            for (int j = 0; j < listed; ++j) {
                if (context.entries[j].handle == order_[i]) {
                    stable[i] = context.entries[j];
                    break;
                }
            }
        }
        for (int i = 0; i < nextCount; ++i) entries_[i] = stable[i];
    }

    count_ = orderCount_ > listed ? listed : orderCount_;

    const int skipped = context.cloakedSkipped + context.untitledSkipped +
                        context.ownedSkipped + context.toolSkipped + context.shellSkipped +
                        context.selfSkipped + context.invisibleSkipped;
    totalSeen_ = listed + skipped;

    std::swprintf(summary_, 192,
                  L"%d listed of %d: %d invisible, %d cloaked, %d untitled, %d owned, "
                  L"%d tool, %d shell, %d self",
                  count_, totalSeen_, context.invisibleSkipped, context.cloakedSkipped,
                  context.untitledSkipped, context.ownedSkipped, context.toolSkipped,
                  context.shellSkipped, context.selfSkipped);
    return count_;
}

bool WindowList::Activate(HWND handle) {
    if (!handle || !IsWindow(handle)) {
        // Reported because "the icon did nothing" and "the icon was never given a window" are
        // different faults, and the second one is silent otherwise.
        LogLine(L"  activate: no window (%p)", reinterpret_cast<void*>(handle));
        return false;
    }

    // Reported because a minimised window is the case where "it did nothing" has several distinct
    // causes and they are indistinguishable from outside the process: the restore may not have been
    // requested, it may have been requested and not yet applied, or it may have been applied and the
    // raise refused. Only the values say which.
    const bool wasIconic = IsIconic(handle) != FALSE;
    LogLine(L"  activate: entering, wasIconic=%d, foreground before=%d", wasIconic ? 1 : 0,
            GetForegroundWindow() == handle ? 1 : 0);

    // A minimised window has to be restored before it can be brought forward.
    //
    // ShowWindow ALONE DOES NOT DO IT ACROSS PROCESSES. That was the bug, and the log said so plainly:
    // eight clicks on minimised windows, every one of them reporting
    //     entering, wasIconic=1 ... nowIconic=1
    // -- the foreground changed and the window stayed in the taskbar. ShowWindow posts to the target
    // thread and the restore is not carried out; its return value is no help, because it only reports
    // that the request was queued.
    //
    // Three documented routes are attempted in order, and EACH ONE'S RESULT IS LOGGED. That is not
    // decoration: which of them works is a property of the target application, and the answer cannot be
    // reasoned out from the documentation -- it has to be observed. Reporting only the final state would
    // leave "none of them worked" indistinguishable from "the first one needed a moment".
    const auto requestRestore = [](HWND target) {
        // 1. WM_SYSCOMMAND with SC_RESTORE -- the command the window's own system menu sends.
        //    Delivered with a timeout so a hung target cannot stall the bar's frame loop.
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(target, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 200, &ignored);
        const bool afterCommand = IsIconic(target) != FALSE;
        LogLine(L"  activate:   after SC_RESTORE: iconic=%d", afterCommand ? 1 : 0);
        if (!afterCommand) return;

        // 2. SetWindowPlacement with SW_SHOWNORMAL -- sets the state rather than requesting a
        //    transition, and is the route recommended for another process's window.
        WINDOWPLACEMENT placement = {};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(target, &placement)) {
            placement.showCmd = SW_SHOWNORMAL;
            SetWindowPlacement(target, &placement);
        }
        const bool afterPlacement = IsIconic(target) != FALSE;
        LogLine(L"  activate:   after SetWindowPlacement: iconic=%d", afterPlacement ? 1 : 0);
        if (!afterPlacement) return;

        // 3. ShowWindow last, because it does work for some windows and costs nothing to try.
        ShowWindow(target, SW_RESTORE);
        LogLine(L"  activate:   after ShowWindow: iconic=%d", IsIconic(target) ? 1 : 0);
    };

    if (wasIconic) {
        requestRestore(handle);
    }

    // SetForegroundWindow can legitimately fail.
    //
    // Windows only lets a process take the foreground under a set of conditions
    // designed to stop programs stealing focus. A taskbar click is one of the
    // cases that SHOULD qualify -- the user just clicked -- but the rule is
    // enforced against the process, and the bar deliberately never activates
    // itself (WS_EX_NOACTIVATE), so it is not the foreground process and cannot
    // always assert that right. When the call is refused it returns FALSE and no
    // error is set; there is no other symptom.
    //
    // Rather than pretend, the result is returned. The caller logs it, because
    // "the click did nothing" and "Windows refused to raise the window" look
    // identical from the outside and need different explanations.
    const BOOL raised = SetForegroundWindow(handle);

    // The early return is kept for the common case -- an already-visible window that came forward --
    // but NOT when the window was minimised: there the restore is the whole point, and returning on the
    // foreground result alone would report success while the window was still minimised.
    if (raised && !wasIconic) {
        LogLine(L"  activate: raised, was not minimised");
        return true;
    }

    // A second documented route, tried before giving up.
    //
    // `AllowSetForegroundWindow(ASFW_ANY)` is the documented way to say "any process may take the
    // foreground from me". It is normally used to hand permission to a child, and it is listed
    // among the conditions SetForegroundWindow accepts, so it is worth one attempt. It cannot
    // help a process that has no foreground rights at all -- which is the case inside a sandbox --
    // and when it does not help, the refusal is reported rather than papered over.
    AllowSetForegroundWindow(ASFW_ANY);
    if (SetForegroundWindow(handle)) {
        LogLine(L"  activate: raised after AllowSetForegroundWindow, nowIconic=%d",
                IsIconic(handle) ? 1 : 0);
        return !IsIconic(handle);
    }

    // One documented fallback: attaching the input queue to the target's thread
    // makes this process eligible to set the foreground. It is used only after
    // the direct call fails, because it changes the calling thread's input
    // attachment and must be undone.
    DWORD targetPid = 0;
    const DWORD targetThread = GetWindowThreadProcessId(handle, &targetPid);
    const DWORD selfThread = GetCurrentThreadId();
    if (targetThread == 0 || targetThread == selfThread) return false;

    bool attached = false;
    if (AttachThreadInput(selfThread, targetThread, TRUE)) {
        attached = true;
        // Brought forward directly as well: attaching alone does not raise it.
        BringWindowToTop(handle);
        if (IsIconic(handle)) requestRestore(handle);
        SetForegroundWindow(handle);
        AttachThreadInput(selfThread, targetThread, FALSE);
    }

    // A window that was minimised is only "activated" once it is actually restored.
    //
    // This is the difference between reporting what was ASKED for and what HAPPENED. ShowWindow on
    // another process's window is posted, not performed, so immediately after the call the window may
    // still be minimised -- and the previous version returned the foreground result alone, which was
    // true, and the caller logged "activated" while the window stayed in the taskbar. The user's report
    // was precisely that: the click said it worked and nothing appeared.
    const bool nowIconic = IsIconic(handle) != FALSE;

    LogLine(L"  activate: wasIconic=%d, nowIconic=%d, foreground=%d, attached=%d",
            wasIconic ? 1 : 0, nowIconic ? 1 : 0, GetForegroundWindow() == handle ? 1 : 0,
            attached ? 1 : 0);

    return GetForegroundWindow() == handle && !nowIconic;
}

bool WindowList::Minimize(HWND handle) {
    if (!handle || !IsWindow(handle)) return false;
    if (IsIconic(handle)) return true;   // already away; the gesture is satisfied

    // SC_MINIMIZE through the system-command channel, the same route that turned out to be the one
    // that actually restores a minimised window across processes. ShowWindow(SW_MINIMIZE) is the
    // obvious call and it is deliberately not used: it does not run the window's own handling of the
    // command, and for a window that customises minimising -- which several do -- that is a visible
    // difference.
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(handle, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 200, &ignored);

    const bool nowIconic = IsIconic(handle) != FALSE;
    LogLine(L"  minimize: %p nowIconic=%d", reinterpret_cast<void*>(handle), nowIconic ? 1 : 0);
    return nowIconic;
}

} // namespace island
