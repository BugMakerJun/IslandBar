// =====================================================================
//  TrayPress.cpp -- see the header for the route and for what it is built on.
// =====================================================================
#include "shell/TrayPress.h"

#include "core/Log.h"

#include <shellapi.h>
#include <tlhelp32.h>
// objbase.h before the UI Automation headers: with WIN32_LEAN_AND_MEAN, windows.h does not pull in the
// COM declarations, and the SDK's UIA headers are written in terms of them -- every interface in them
// fails to compile without this ("missing type specifier ... IRawElementProviderSimple"), which is what
// the first two attempts at this file produced.
#include <objbase.h>
// The SafeArray calls below (`GetRuntimeId` hands back one of those), which oleauto.h declares: with
// WIN32_LEAN_AND_MEAN it does not arrive with windows.h, and objbase.h does not pull it in either.
#include <oleauto.h>
// uiautomation.h first: the client header forward-declares interfaces that the core one defines, and on
// its own it does not compile (measured -- C4430/C2146 for every interface in it).
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace island {
namespace {

// See TrayPressInFlight. Set for the whole of PressTrayIcon, by whichever thread is running it.
std::atomic<bool> g_pressInFlight{false};

// The taskbar a right click left shown-but-invisible, and the extended style it had before: see
// TrayPressCleanupPending. Written by the press thread, read and cleared by the frame loop.
std::atomic<HWND> g_cleanupTaskbar{nullptr};
std::atomic<LONG_PTR> g_cleanupStyle{0};

// The same handover for the SHELL'S OVERFLOW PANEL, which a right click leaves invisible on purpose.
//
// The panel is left at alpha 0 because the application's menu is on the screen for as long as the user
// keeps it there, and the panel's own surface is the icon grid: restoring its alpha at the end of the press
// would paint that grid over the menu the user asked for. Its style and alpha are put back by the frame
// loop once the SHELL has closed the panel -- see TrayPressFinishCleanup, which refuses to do it while the
// panel is still up (measured: the shell closes the panel by hiding it when the menu goes away).
std::atomic<HWND> g_cleanupPanel{nullptr};
std::atomic<LONG_PTR> g_cleanupPanelStyle{0};
std::atomic<bool> g_cleanupPanelUnlayer{false};

// The one measurement switch left. It is read once per press so that a run cannot change its mind halfway.
//
// TWO OTHERS WERE HERE AND ARE GONE (2026-10-15): `..._NO_REVEAL` proved that without the panel being put
// back on screen neither button does anything, and `..._ROUTE=invoke` reproduced the route this file used
// to take -- which is how it was measured that the route does nothing. Both questions are answered, and a
// switch that reproduces an answered question is a second code path nobody will ever run again.
bool SwitchOn(const wchar_t* name) {
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, 32);
    return length > 0 && length < 32 && value[0] != L'0';
}

// ISLANDBAR_TRAY_PRESS_VISIBLE: nothing is hidden or revealed, so somebody can watch what a press does.
bool VisibleRunWanted() { return SwitchOn(L"ISLANDBAR_TRAY_PRESS_VISIBLE"); }

// Marks the press for the frame loop, and clears it whatever way the function returns.
class PressInFlight {
public:
    PressInFlight() { g_pressInFlight.store(true); }
    ~PressInFlight() { g_pressInFlight.store(false); }
};

long long MillisSince(const ULONGLONG start) { return static_cast<long long>(GetTickCount64() - start); }

// The pids of every process whose image is `exePath`'s file name, case-insensitively.
//
// Toolhelp and a name comparison rather than OpenProcess per pid: the same choice TrayIcons makes, for
// the same reason -- this is a handful of milliseconds on a machine with hundreds of processes.
std::vector<DWORD> PidsForImage(const wchar_t* exePath) {
    std::vector<DWORD> pids;
    if (!exePath || !*exePath) return pids;
    const wchar_t* slash = std::wcsrchr(exePath, L'\\');
    std::wstring wanted = slash ? std::wstring(slash + 1) : std::wstring(exePath);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::towlower);

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == wanted) pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

// The windows of the processes in `pids`: every top-level one and every message-only one.
//
// Both, because a tray icon's owner is usually a window nobody ever sees: WeChat's is
// `Qt51514WxTrayIconMessageWindowClass`, QQ Music's is a `TXGuiFoundation` window, and others are
// message-only windows, which EnumWindows does not report at all (they are children of HWND_MESSAGE).
struct WindowSearch {
    const std::vector<DWORD>* pids = nullptr;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
    WindowSearch* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (std::find(search->pids->begin(), search->pids->end(), pid) != search->pids->end()) {
        search->windows.push_back(hwnd);
    }
    return TRUE;
}

std::vector<HWND> WindowsOf(const std::vector<DWORD>& pids) {
    std::vector<HWND> windows;
    if (pids.empty()) return windows;
    WindowSearch search;
    search.pids = &pids;
    EnumWindows(&CollectWindow, reinterpret_cast<LPARAM>(&search));
    // Message-only windows live under a special parent handle, and are reached from it rather than from
    // the desktop: FindWindowExW(HWND_MESSAGE, ...) walks that separate list.
    HWND child = nullptr;
    while ((child = FindWindowExW(HWND_MESSAGE, child, nullptr, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(child, &pid);
        if (std::find(pids.begin(), pids.end(), pid) != pids.end()) search.windows.push_back(child);
    }
    return search.windows;
}

bool QueryRect(const NOTIFYICONIDENTIFIER& identifier, RECT* out) {
    RECT rect = {};
    const HRESULT hr = Shell_NotifyIconGetRect(&identifier, &rect);
    if (FAILED(hr) || hr == S_FALSE) return false;
    *out = rect;
    return true;
}

bool RectIsEmpty(const RECT& rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

// The UI Automation element that sits exactly on `wanted`, from the descendants of `root`.
//
// By POSITION, not by name: the shell's name for the icon and the bar's are different strings for the
// same icon (see the header), while the rectangle is the same rectangle -- the card's cell and the
// shell's button are both 40x40 at the same place, one drawn by us and one by the shell.
IUIAutomationElement* ElementOnRect(IUIAutomation* automation, IUIAutomationElement* root,
                                   const RECT& wanted, int tolerancePx, bool requireInvoke,
                                   wchar_t* nameOut, size_t nameCapacity, RECT* rectOut) {
    if (!automation || !root) return nullptr;
    IUIAutomationCondition* everything = nullptr;
    if (FAILED(automation->CreateTrueCondition(&everything)) || !everything) return nullptr;
    IUIAutomationElementArray* found = nullptr;
    const HRESULT hr = root->FindAll(TreeScope_Descendants, everything, &found);
    everything->Release();
    if (FAILED(hr) || !found) return nullptr;

    IUIAutomationElement* best = nullptr;
    int count = 0;
    found->get_Length(&count);
    std::vector<RECT> rects;
    std::vector<IUIAutomationElement*> elements;
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(found->GetElement(i, &element)) || !element) continue;
        RECT rect = {};
        if (FAILED(element->get_CurrentBoundingRectangle(&rect)) || RectIsEmpty(rect)) {
            element->Release();
            continue;
        }
        rects.push_back(rect);
        elements.push_back(element);
    }
    found->Release();

    const int hit = IndexOfRectNear(rects.data(), static_cast<int>(rects.size()), wanted, tolerancePx);
    if (hit >= 0) {
        IUIAutomationElement* element = elements[static_cast<size_t>(hit)];
        bool invokable = true;
        if (requireInvoke) {
            IUIAutomationInvokePattern* invoke = nullptr;
            const HRESULT patternHr = element->GetCurrentPatternAs(UIA_InvokePatternId,
                                                                   IID_PPV_ARGS(&invoke));
            invokable = SUCCEEDED(patternHr) && invoke != nullptr;
            if (invoke) invoke->Release();
        }
        if (invokable) {
            if (nameOut && nameCapacity > 0) {
                BSTR name = nullptr;
                if (SUCCEEDED(element->get_CurrentName(&name)) && name) {
                    wcsncpy_s(nameOut, nameCapacity, name, _TRUNCATE);
                    SysFreeString(name);
                }
            }
            if (rectOut) *rectOut = rects[static_cast<size_t>(hit)];
            element->AddRef();
            best = element;
        }
    }
    for (IUIAutomationElement* element : elements) element->Release();
    return best;
}

// The overflow chevron: the leftmost button the taskbar publishes as a system tray icon. Leftmost
// because the chevron is the first of them and the others (network, volume, clock, show-desktop) are to
// its right -- measured, and stable across the tray's own reordering.
IUIAutomationElement* FindOverflowChevron(IUIAutomation* automation, IUIAutomationElement* taskbar,
                                         RECT* rectOut) {
    if (!automation || !taskbar) return nullptr;
    IUIAutomationCondition* condition = nullptr;
    VARIANT id = {};
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"SystemTrayIcon");
    HRESULT hr = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition);
    SysFreeString(id.bstrVal);
    if (FAILED(hr) || !condition) return nullptr;

    IUIAutomationElementArray* buttons = nullptr;
    hr = taskbar->FindAll(TreeScope_Descendants, condition, &buttons);
    condition->Release();
    if (FAILED(hr) || !buttons) return nullptr;

    IUIAutomationElement* chevron = nullptr;
    RECT chevronRect = {};
    int count = 0;
    buttons->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(buttons->GetElement(i, &element)) || !element) continue;
        RECT rect = {};
        if (SUCCEEDED(element->get_CurrentBoundingRectangle(&rect)) && !RectIsEmpty(rect)) {
            if (!chevron || rect.left < chevronRect.left) {
                if (chevron) chevron->Release();
                element->AddRef();
                chevron = element;
                chevronRect = rect;
            }
        }
        element->Release();
    }
    buttons->Release();
    if (chevron && rectOut) *rectOut = chevronRect;
    return chevron;
}

bool InvokeElement(IUIAutomationElement* element) {
    if (!element) return false;
    IUIAutomationInvokePattern* invoke = nullptr;
    if (FAILED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) || !invoke) {
        return false;
    }
    const HRESULT hr = invoke->Invoke();
    invoke->Release();
    return SUCCEEDED(hr);
}

// ---- the press itself, which is a REAL CLICK -----------------------------------------------------------
//
// The route changed on 2026-10-15, and this is the research behind the change (HANDOFF 8.79/8.80, and the
// review thread's 15th to 17th rounds, which were asked with this project's own measurements):
//
//   * the buttons the shell publishes for its tray icons do not reliably carry an Invoke pattern at all,
//     and where they do (`GetCurrentPatternAs` succeeding) the Invoke can answer S_OK while the
//     application never hears about it -- the "tray" half of a tray application listens for the SHELL'S
//     forwarded mouse behaviour, not for an automation call on a button;
//   * every real-world tool that does this programmatically (AutoHotkey's tray scripts, TrayStatus,
//     TranslucentTB) reproduces exactly that: find the icon's screen rectangle, put the POINTER on it, and
//     send the button down and up;
//   * our own bar is a TOPMOST window over the same band, and an alpha-0 layered window still takes part in
//     hit testing -- alpha hides the pixels, not the window -- so what is under the point has to be
//     MEASURED before the click, and the bar has to step out of the way if it turns out to be us.
//
// THE PRESS IS ONE `SendInput` QUEUE (2026-10-15, on the review's advice and its own reason): the move is
// still a `SetCursorPos` -- what matters is not how the pointer gets there but that the press and its
// release cannot be separated -- and the pair goes out as one item in the input queue instead of two calls
// with a sleep between them. The pointer genuinely moves, which is a trade the user accepted in exchange
// for the press working at all; it is put back straight afterwards.

// What would receive a click at `point`, in words, and whether that window is this bar's own.
void DescribeWindowAt(POINT point, wchar_t* out, size_t capacity, bool* isOurs) {
    if (out && capacity > 0) out[0] = L'\0';
    if (isOurs) *isOurs = false;
    const HWND under = WindowFromPoint(point);
    if (!under) {
        if (out && capacity > 0) wcsncpy_s(out, capacity, L"nothing (no window is there)", _TRUNCATE);
        return;
    }
    wchar_t cls[96] = {};
    GetClassNameW(under, cls, 96);
    DWORD pid = 0;
    GetWindowThreadProcessId(under, &pid);
    const bool ours = pid == GetCurrentProcessId();
    if (isOurs) *isOurs = ours;
    if (out && capacity > 0) {
        std::swprintf(out, capacity, L"class '%ls' of pid %lu%ls", cls, pid,
                      ours ? L" -- THIS BAR" : L"");
    }
}

// Takes this bar out of the hit test for as long as a click is being delivered, and only when the window
// under the click really is this bar's.
//
// `WS_EX_TRANSPARENT` and not an emptied window region: the region is also what the bar's own hit testing
// and its input contract are built on, and zeroing it for a moment would flicker the bar itself. The bit is
// put back when this object dies, which is after the click has been delivered AND the answer waited for --
// input is queued, so a bit removed immediately after the click is sent could be gone before it is routed.
class ClickThroughGuard {
public:
    explicit ClickThroughGuard(HWND window) : window_(window) {
        if (!window_) return;
        style_ = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        if ((style_ & WS_EX_TRANSPARENT) != 0) {
            window_ = nullptr;   // it was already click-through: nothing of ours to put back
            return;
        }
        SetWindowLongPtrW(window_, GWL_EXSTYLE, style_ | WS_EX_TRANSPARENT);
        applied_ = true;
    }
    ~ClickThroughGuard() {
        if (applied_) SetWindowLongPtrW(window_, GWL_EXSTYLE, style_);
    }
    ClickThroughGuard(const ClickThroughGuard&) = delete;
    ClickThroughGuard& operator=(const ClickThroughGuard&) = delete;

    bool applied() const { return applied_; }

private:
    HWND window_ = nullptr;
    LONG_PTR style_ = 0;
    bool applied_ = false;
};

// What the press knows about the desktop before it clicks, and what it measures the click against.
//
// MEASURED, 2026-10-15, because the plan this replaced was built on a wrong assumption about what a tray
// menu IS on this machine: right-clicking WeGame's icon brings up a window of WeGame's OWN process (class
// `TWINCONTROL`, 250x280 at 2136,100 -- the click was at 2394,109, and the menu's top-right corner sits
// ten pixels from it). It is NOT a `#32768` Win32 popup menu; it is NOT in the UI Automation tree (asked
// the whole desktop for ControlType=Menu: zero, while the menu was painted on the screen); and it does not
// answer `WindowFromPoint` either (asked at the middle of it while it was painted: the desktop's
// `SysListView32`). So a search by class name, by automation, or by hit testing cannot see a tray menu
// here -- the only thing that can is the APPLICATION'S OWN WINDOW LIST, asked twice.
struct WindowWatch {
    std::vector<DWORD> pids;          // the application's processes
    std::vector<HWND> appWindows;     // every window of theirs, message-only ones included
    std::vector<HWND> appVisible;     // ... and which of those were already on screen
};

WindowWatch TakeWindowWatch(const TrayPressRequest& request) {
    WindowWatch watch;
    watch.pids = PidsForImage(request.exePath);
    watch.appWindows = WindowsOf(watch.pids);
    for (HWND window : watch.appWindows) {
        if (IsWindowVisible(window)) watch.appVisible.push_back(window);
    }
    return watch;
}

// One look: has anything of the application's come up since the snapshot? The window, its class and its
// rectangle are filled in when it has, and `out` describes it in words for the log.
//
// ... AND ONLY THE APPLICATION'S OWN WINDOWS COUNT. A first version also accepted "any new top-level window
// of another process, or one wearing the popup-menu class `#32768`" -- and that version was measured
// reporting a shell TOOLTIP as the application's answer: a left press on WeGame's icon produced
// `Xaml_WindowedPopupClass ... 130x41`, which is the icon's own tooltip, while WeGame itself did nothing.
// A log line that says the application answered when it did not is worse than one that says nothing, so the
// question is now asked of the application and of nobody else. If a future shell draws a tray menu on the
// application's behalf, this will say "nothing came up" about a menu that IS up, and that is the known and
// documented limit of the criterion.
bool AnyWindowCameUp(const WindowWatch& watch, wchar_t* out, size_t capacity, HWND* handle, RECT* rectOut) {
    const auto describe = [out, capacity](const wchar_t* cls, const RECT& rect, const wchar_t* whose) {
        if (out && capacity > 0) {
            std::swprintf(out, capacity, L"its own '%ls' at %ld,%ld %ldx%ld (%ls)", cls, rect.left,
                          rect.top, rect.right - rect.left, rect.bottom - rect.top, whose);
        }
    };
    for (HWND window : WindowsOf(watch.pids)) {
        if (!IsWindowVisible(window)) continue;
        if (std::find(watch.appVisible.begin(), watch.appVisible.end(), window) != watch.appVisible.end()) {
            continue;
        }
        RECT rect = {};
        if (!GetWindowRect(window, &rect)) continue;
        if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
        wchar_t cls[96] = {};
        GetClassNameW(window, cls, 96);
        describe(cls, rect, L"a window of its own that was not on screen before");
        if (handle) *handle = window;
        if (rectOut) *rectOut = rect;
        return true;
    }
    return false;
}

// Watches for `budgetMs` for something to come up, and reports how long it took (-1 when nothing did).
long long WaitForSomethingToComeUp(const WindowWatch& watch, long long budgetMs, wchar_t* describeOut,
                                   size_t describeCapacity, HWND* windowOut, RECT* rectOut) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (AnyWindowCameUp(watch, describeOut, describeCapacity, windowOut, rectOut)) {
            return MillisSince(start);
        }
        if (MillisSince(start) >= budgetMs) return -1;
        Sleep(30);
    }
}

// Whether the window the menu was seen in is still on screen, which is the question that decides whether
// taking the panel away cost the user their menu (see PressOnRect).
bool WindowIsStillUp(HWND window) {
    return window && IsWindow(window) && IsWindowVisible(window);
}

// Did the application do anything? The rule this exists for is the review's: a clean HRESULT is not an
// answer -- "the click never arrived" and "the click arrived and was ignored" look identical from here, and
// only the application's own windows can tell them apart.
//
// TWO QUESTIONS, both observable from outside any application and neither fakeable by a press that
// "succeeded": a window of the application's that came up, or the foreground moving to one of the
// application's windows. The budget is 1.5 s rather than the 200 ms the plan asked for, and the reason is
// measured: WeGame is a launcher, and the first run of this watch said "nothing" 200 ms after a click that
// a longer look would have seen -- the pointer is already back where the user left it by then, so the wait
// costs nobody anything.
bool WatchApplicationForAnswer(const WindowWatch& watch, HWND foregroundBefore, wchar_t* out,
                               size_t capacity) {
    const auto say = [out, capacity](const wchar_t* text) {
        if (out && capacity > 0) wcsncpy_s(out, capacity, text, _TRUNCATE);
    };
    say(L"nothing of its own appeared");
    if (watch.pids.empty()) {
        say(L"none of its processes is running");
        return false;
    }
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (AnyWindowCameUp(watch, out, capacity, nullptr, nullptr)) return true;
        const HWND foreground = GetForegroundWindow();
        if (foreground && foreground != foregroundBefore) {
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            if (std::find(watch.pids.begin(), watch.pids.end(), pid) != watch.pids.end()) {
                wchar_t cls[96] = {};
                GetClassNameW(foreground, cls, 96);
                if (out && capacity > 0) {
                    std::swprintf(out, capacity, L"the foreground window is now its '%ls'", cls);
                }
                return true;
            }
        }
        if (MillisSince(start) >= 1500) {
            say(L"nothing of its own appeared, and the foreground did not move to it, in 1500 ms");
            return false;
        }
        Sleep(30);
    }
}

// Every fact the automation tree has about the button the shell publishes on the press's rectangle.
//
// ASKED FOR BY NAME in the review (round 15/16), because the previous round could not tell "nothing was
// pressed" from "something was pressed and ignored": the element's own identity, whether it is on screen
// and enabled, and which patterns it answers -- with the raw HRESULT of each -- are what a future session
// needs to see, and none of them are in the log otherwise.
void LogElementFacts(IUIAutomationElement* element, const wchar_t* prefix) {
    if (!element) return;
    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name) {
        LogAndPrint(L"  %ls: the shell's button is named '%ls'\n", prefix, name);
        SysFreeString(name);
    } else {
        LogAndPrint(L"  %ls: the shell's button has no name\n", prefix);
    }
    CONTROLTYPEID type = 0;
    element->get_CurrentControlType(&type);
    RECT rect = {};
    element->get_CurrentBoundingRectangle(&rect);
    BOOL offscreen = FALSE;
    BOOL enabled = FALSE;
    element->get_CurrentIsOffscreen(&offscreen);
    element->get_CurrentIsEnabled(&enabled);
    wchar_t runtimeId[128] = {};
    SAFEARRAY* ids = nullptr;
    if (SUCCEEDED(element->GetRuntimeId(&ids)) && ids) {
        long lower = 0;
        long upper = -1;
        SafeArrayGetLBound(ids, 1, &lower);
        SafeArrayGetUBound(ids, 1, &upper);
        int written = 0;
        for (long index = lower; index <= upper && written < 8; ++index) {
            int value = 0;
            if (FAILED(SafeArrayGetElement(ids, &index, &value))) break;
            written += std::swprintf(runtimeId + std::wcslen(runtimeId), 128 - std::wcslen(runtimeId),
                                     L"%d ", value);
        }
        SafeArrayDestroy(ids);
    }
    LogAndPrint(L"  %ls: control type %d, at %ld,%ld %ldx%ld, offscreen=%d enabled=%d, runtime id "
                L"[%ls]\n",
                prefix, static_cast<int>(type), rect.left, rect.top, rect.right - rect.left,
                rect.bottom - rect.top, offscreen ? 1 : 0, enabled ? 1 : 0, runtimeId);
    // WHICH PATTERNS IT ANSWERS, with the HRESULT of each one: `Invoke` alone was the mistake this project
    // paid for -- the pattern that is missing is as informative as the one that is there.
    struct PatternName {
        const wchar_t* label;
        PATTERNID id;
    };
    const PatternName patterns[] = {
        {L"Invoke", UIA_InvokePatternId},
        {L"Toggle", UIA_TogglePatternId},
        {L"LegacyIAccessible", UIA_LegacyIAccessiblePatternId},
        {L"SelectionItem", UIA_SelectionItemPatternId},
        {L"ExpandCollapse", UIA_ExpandCollapsePatternId},
        {L"Value", UIA_ValuePatternId},
    };
    wchar_t yes[160] = {};
    wchar_t no[160] = {};
    size_t yesAt = 0;
    size_t noAt = 0;
    for (const PatternName& pattern : patterns) {
        IUnknown* answered = nullptr;
        const HRESULT hr = element->GetCurrentPattern(pattern.id, &answered);
        const bool available = SUCCEEDED(hr) && answered != nullptr;
        if (answered) answered->Release();
        wchar_t* target = available ? yes : no;
        size_t* at = available ? &yesAt : &noAt;
        const size_t room = sizeof(yes) / sizeof(wchar_t) - *at;
        if (room <= 4) break;
        const int written = std::swprintf(target + *at, room, L"%ls ", pattern.label);
        if (written > 0) {
            *at += static_cast<size_t>(written);
        }
    }
    LogAndPrint(L"  %ls: patterns available [%ls], not available [%ls]\n", prefix, yes, no);
}

// How many buttons of the shell's overflow flyout are inside `element`. Zero for the flyout's window when
// the panel is not really up -- and that is the question, not "does the window exist" (it does while the
// panel is closed) and not "is it shown" (measured 2026-10-15: the shell leaves it shown in states where
// nothing in it is laid out, and a press that trusted the window being visible skipped the chevron press,
// clicked nothing and reported "no menu" four times in a row). "Does it hold icons" is the question, and it
// is asked of the window's own element -- one handle lookup -- rather than of a walk of the desktop's
// children, which is what made the old version of this cost a second per press.
int PanelIconCount(IUIAutomation* automation, IUIAutomationElement* element) {
    if (!automation || !element) return 0;
    VARIANT id = {};
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"NotifyItemIcon");
    IUIAutomationCondition* condition = nullptr;
    if (FAILED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition)) ||
        !condition) {
        SysFreeString(id.bstrVal);
        return 0;
    }
    SysFreeString(id.bstrVal);
    IUIAutomationElementArray* found = nullptr;
    int count = 0;
    if (SUCCEEDED(element->FindAll(TreeScope_Descendants, condition, &found)) && found) {
        found->get_Length(&count);
        found->Release();
    }
    condition->Release();
    return count;
}

} // namespace

bool TrayPressInFlight() { return g_pressInFlight.load(); }

// ---- the posted-callback route, learned once per application (2026-10-15 late) ------------------------
//
// MEASURED, and this is the whole reason it exists: an application's tray icon can be pressed WITHOUT
// touching the pointer, by posting the application's own callback message to its own tray window. On WeChat
// that put its tray menu -- 退出微信 and all -- at the coordinates this bar passed, with the pointer sitting
// still, and nothing injected anywhere (HANDOFF 8.104; the screenshot is captures/wx-A1.png).
//
// THE LAYOUT IS THE PART THAT COSTS (NOTIFYICONDATA.uCallbackMessage):
//
//   v4 (modern):  LOWORD(lParam) = the notification, HIWORD(lParam) = the icon's uid,
//                 and the ANCHOR POINT is in wParam (low word x, high word y).
//   v3 (older):   wParam = uid, lParam = the mouse message (WM_RBUTTONUP and friends).
//
// The first two attempts used the v3-looking spelling with coordinates bolted into lParam's high word; both
// were delivered, both were retrieved by the application, and both were ignored. This is not guessable.
//
// WHAT IS CACHED, AND WHY A FILE. Per application: the message number, the class of the window it has to be
// posted to, and the icon's uid. The message number cannot be looked up anywhere (the shell's registry entry
// carries UID, IconGuid, path, tooltip and icon, and no callback message) and cannot be observed from this
// side either (the shell delivers it without it passing through any queue this process can see), so it is
// LEARNED once with a short Win32k trace -- scripts/probe-tray-callback-etw.ps1 -- and written here:
//
//     %USERPROFILE%\IslandBar\tray_callbacks.txt
//     # exe|message|window class|uid
//     Weixin.exe|0x8385|Qt51514WxTrayIconMessageWindowClass|0
//
// AN APPLICATION WITH NO ENTRY IS PRESSED THE OLD WAY (a real click), and so is one whose entry does not
// lead to a window: the file is the switch, and deleting a line is the way back.
struct TrayCallback {
    wchar_t exe[64] = {};
    unsigned message = 0;
    wchar_t windowClass[128] = {};
    unsigned uid = 0;
    // WHICH PRESS THIS ENTRY IS ALLOWED TO CARRY, learned the same way it was learned at all: by trying it.
    // A right click works (measured on WeChat: the menu came up at the anchor). A LEFT click does not, and
    // the reason is the foreground lock: the application's own handler calls SetForegroundWindow, and a
    // background process is refused, so a window that is merely BEHIND does not come forward. Five spellings
    // of the left-click notification were tried and none of them activated WeChat, while MyDockFinder's own
    // left press does activate it -- its engine imports SendInput, so that route is more than a post.
    // Until that is built here, a left press keeps the real click (which does satisfy the foreground rules).
    bool rightClickOnly = false;
};

std::wstring BaseNameLower(const wchar_t* path) {
    if (!path || !*path) return std::wstring();
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : std::wstring(path);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

// The learned entries, read once per press. A missing file is not an error: it means "nothing learned yet",
// which is the state every application starts in.
std::vector<TrayCallback> ReadTrayCallbacks() {
    std::vector<TrayCallback> entries;
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return entries;
    std::wstring file = std::wstring(home) + L"\\IslandBar\\tray_callbacks.txt";

    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return entries;
    wchar_t line[512] = {};
    while (std::fgetws(line, 512, stream)) {
        std::wstring text = line;
        const size_t comment = text.find(L'#');
        if (comment != std::wstring::npos) text = text.substr(0, comment);
        std::vector<std::wstring> fields;
        size_t start = 0;
        for (;;) {
            const size_t bar = text.find(L'|', start);
            std::wstring field = text.substr(start, bar == std::wstring::npos ? std::wstring::npos
                                                                              : bar - start);
            while (!field.empty() && (field.back() == L'\r' || field.back() == L'\n' ||
                                      field.back() == L' ' || field.back() == L'\t')) {
                field.pop_back();
            }
            while (!field.empty() && (field.front() == L' ' || field.front() == L'\t')) field.erase(0, 1);
            fields.push_back(field);
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
        if (fields.size() < 3 || fields[0].empty() || fields[1].empty()) continue;
        TrayCallback entry;
        std::transform(fields[0].begin(), fields[0].end(), entry.exe, ::towlower);
        const bool hex = fields[1].size() > 2 && fields[1][0] == L'0' &&
                         (fields[1][1] == L'x' || fields[1][1] == L'X');
        entry.message = static_cast<unsigned>(
            std::wcstoul(fields[1].c_str() + (hex ? 2 : 0), nullptr, hex ? 16 : 10));
        wcsncpy_s(entry.windowClass, fields[2].c_str(), _TRUNCATE);
        if (fields.size() > 3) entry.uid = static_cast<unsigned>(std::wcstoul(fields[3].c_str(), nullptr, 10));
        if (fields.size() > 4) entry.rightClickOnly = _wcsicmp(fields[4].c_str(), L"right") == 0;
        if (entry.message) entries.push_back(entry);
    }
    std::fclose(stream);
    return entries;
}

// The application's window of the class the entry names. False when it is not running, has no such window
// yet (a tray icon that has not registered), or when nothing is known about it.
bool CallbackWindowFor(const TrayPressRequest& request, const TrayCallback& entry, HWND* out) {
    const std::vector<DWORD> pids = PidsForImage(request.exePath);
    if (pids.empty()) return false;
    for (HWND window : WindowsOf(pids)) {
        wchar_t cls[128] = {};
        if (!GetClassNameW(window, cls, 128)) continue;
        if (_wcsicmp(cls, entry.windowClass) == 0) {
            *out = window;
            return true;
        }
    }
    return false;
}

// Posts that callback and waits for the application to answer. False means "this route was not taken" --
// nothing learned, no such window, or the post itself failed -- and the caller is expected to fall back.
//
// THE ANCHOR IS WHERE THE POINTER ALREADY IS. The user pressed a cell in this bar's tray card, so the pointer
// is on that cell and has not been moved: passing it through is what puts the menu under the cell the user
// clicked rather than at the application's own icon in the screen's corner. (On WeChat this is measured: the
// menu came up at exactly the coordinates passed in.)
bool SendTrayCallback(const TrayPressRequest& request, bool rightClick, TrayPressOutcome* outcome) {
    const std::vector<TrayCallback> entries = ReadTrayCallbacks();
    const std::wstring exe = BaseNameLower(request.exePath);
    const TrayCallback* entry = nullptr;
    for (const TrayCallback& candidate : entries) {
        if (exe == candidate.exe) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) return false;
    if (entry->rightClickOnly && !rightClick) return false;   // see the note on rightClickOnly: fall back

    HWND window = nullptr;
    if (!CallbackWindowFor(request, *entry, &window)) return false;
    POINT anchor = {};
    if (!GetCursorPos(&anchor)) return false;

    const WindowWatch watch = TakeWindowWatch(request);
    const HWND foregroundBefore = GetForegroundWindow();

    // v4: the anchor in wParam (low word x, high word y), the notification and the icon's uid in lParam.
    const WPARAM wparam = static_cast<WPARAM>(MAKELONG(anchor.x, anchor.y));
    const LPARAM lparam = static_cast<LPARAM>(
        MAKELONG(rightClick ? WM_CONTEXTMENU : 0x0400 /* NIN_SELECT */, entry->uid));
    const BOOL posted = PostMessageW(window, entry->message, wparam, lparam);
    if (!posted) return false;

    wchar_t target[160] = {};
    wchar_t cls[128] = {};
    GetClassNameW(window, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::swprintf(target, 160, L"class '%ls' of pid %lu (posted, not clicked)", cls, pid);

    const bool answered = WatchApplicationForAnswer(watch, foregroundBefore, outcome ? outcome->answer : nullptr,
                                                    outcome ? sizeof(outcome->answer) / sizeof(wchar_t) : 0);
    if (outcome) {
        outcome->route = L"a posted callback";
        outcome->pointerMoved = false;
        outcome->result = TrayPressResult::Pressed;
        outcome->appAnswered = answered;
        wcsncpy_s(outcome->target, sizeof(outcome->target) / sizeof(wchar_t), target, _TRUNCATE);
    }
    LogAndPrint(L"  tray press: posted 0x%04X to %ls, the pointer was NOT moved (anchor %ld,%ld) -- the "
                L"application %ls\n",
                entry->message, target, anchor.x, anchor.y,
                answered ? L"answered" : L"has not been seen to answer yet, and nothing else was done about "
                                       L"it (pressing twice would be worse)");
    return true;
}

bool TrayPressCleanupPending() {
    return g_cleanupTaskbar.load() != nullptr || g_cleanupPanel.load() != nullptr;
}
bool TrayPressFinishCleanup() {
    const HWND taskbar = g_cleanupTaskbar.exchange(nullptr);
    if (taskbar) {
        const LONG_PTR style = g_cleanupStyle.load();
        ShowWindow(taskbar, SW_HIDE);
        SetWindowLongPtrW(taskbar, GWL_EXSTYLE, style);
        SetLayeredWindowAttributes(taskbar, 0, 255, LWA_ALPHA);
    }
    // THE PANEL ONLY GOES BACK ONCE THE SHELL HAS CLOSED IT. While the application's menu is up the panel
    // must stay invisible -- its surface is the icon grid, and restoring the alpha would paint that grid
    // over the menu -- so this part of the cleanup keeps asking, once per watchdog tick, until the shell
    // hides the panel (measured: it hides it when the user dismisses the menu; it is not destroyed).
    const HWND panel = g_cleanupPanel.load();
    if (panel) {
        if (!IsWindow(panel) || !IsWindowVisible(panel)) {
            if (g_cleanupPanelUnlayer.load()) {
                SetWindowLongPtrW(panel, GWL_EXSTYLE, g_cleanupPanelStyle.load());
            }
            SetLayeredWindowAttributes(panel, 0, 255, LWA_ALPHA);
            g_cleanupPanel.store(nullptr);
        }
    }
    return taskbar != nullptr;
}

TrayStage::~TrayStage() { Reset(); }

void TrayStage::Reset() {
    // THE PANEL FIRST, because hiding the taskbar does not take it down: measured, the overflow panel was
    // still on the screen -- still painted -- after the taskbar had been hidden, so the next act of this
    // function (removing the layering, which restores the alpha it was given) put it back in front of the
    // user, and a screenshot of a right click showed the shell's whole icon grid where the application's
    // menu should have been. The chevron is a toggle, and it is only pressed when this stage is the one
    // that opened the panel.
    //
    // ... AND NOT AFTER A RIGHT CLICK. The menu that a right click opens is the SHELL's own (Win11 offers
    // 退出 <application> there, which is the item the user asked for), it is attached to this panel, and
    // pressing the chevron takes it down with it -- measured: the menu was up and vanished the instant the
    // panel closed. So after a right click the panel is left to the shell, which closes it when the user
    // picks an item or clicks away, and only the taskbar is put back here.
    if (weOpenedFlyout_ && chevron_ && !rightClicked_) {
        // A moment for what the press opened to be up first.
        Sleep(50);
        // ... AND ONLY IF THE SHELL HAS NOT ALREADY CLOSED IT ITSELF. The chevron is a TOGGLE, and the
        // shell closes the panel by itself when a tray icon in it is clicked -- so pressing the chevron
        // without asking first re-OPENS the panel we came to close, which is a state the next hand-opened
        // menu would pay for. Measured 2026-10-15: which of the two it is varies, so it is asked.
        if (PanelStillShowing()) {
            InvokeElement(static_cast<IUIAutomationElement*>(chevron_));
            // THE VERDICT IS WAITED FOR, not read once: the shell animates the panel away, so a single
            // look a tenth of a second later says "still showing" about a panel that is on its way out.
            // Measured 2026-10-15: this line used to read "it is STILL showing" on every press.
            for (int attempt = 0; attempt < 20 && PanelStillShowing(); ++attempt) Sleep(30);
            LogAndPrint("  tray stage: the panel was still up, so the chevron was pressed to close it "
                        "(%ls)\n", PanelStillShowing() ? L"it is STILL showing" : L"closed");
        } else {
            LogAndPrint("  tray stage: the shell had already closed the panel itself\n");
        }
    }
    if (chevron_) static_cast<IUIAutomationElement*>(chevron_)->Release();
    if (flyoutElement_) static_cast<IUIAutomationElement*>(flyoutElement_)->Release();
    if (automation_) static_cast<IUIAutomation*>(automation_)->Release();
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    chevron_ = nullptr;
    flyoutElement_ = nullptr;
    automation_ = nullptr;
    // ONE QUESTION, ASKED ONCE, DECIDES WHAT MAY BE PUT BACK HERE -- and it is asked now, after the chevron
    // has had its chance to close the panel, because the chevron press is the thing that usually changes
    // the answer. While the shell still has the panel up (its menu is on it, or a previous press left one
    // there) it also re-shows its own taskbar, so BOTH the panel's alpha and the taskbar's style have to be
    // handed to the frame loop rather than put back here. Measured 2026-10-15 from a user's screen: a left
    // press that ran while an earlier right-click menu was still up restored the alpha, and the shell's icon
    // grid was painted under that menu and left there.
    const bool panelStillUp = PanelStillShowing();
    if (flyoutWindow_) {
        // THE PANEL IS NOT MOVED. It used to be "put back where the shell had it" through a field that was
        // never assigned, so the shell's overflow panel ended up at 0,0 on every press -- and the
        // application's own menu, which the shell anchors to that panel, went to the top-left corner of the
        // screen with it, grid and all (the user's screenshot of 2026-10-15). Nothing here may move it: the
        // icon rectangles a press is aimed with are screen coordinates inside it, and the menu's place is
        // derived from it.
        if (panelStillUp) {
            // THE SHELL STILL HAS IT UP, so the style and alpha are handed to the frame loop, which puts them
            // back once the shell has closed the panel -- see TrayPressFinishCleanup.
            LogAndPrint("  tray stage: the shell still has the panel up, so its alpha and the taskbar's are "
                        "left to the bar's watchdog\n");
            g_cleanupPanelStyle.store(flyoutStyle_);
            g_cleanupPanelUnlayer.store(flyoutLayered_);
            g_cleanupPanel.store(flyoutWindow_);
        } else {
            // The panel is closed: putting its alpha and its extended style back cannot be seen by anybody,
            // and leaving an alpha of 0 on somebody else's window is not ours to keep.
            SetLayeredWindowAttributes(flyoutWindow_, 0, 255, LWA_ALPHA);
            if (flyoutLayered_) {
                SetWindowLongPtrW(flyoutWindow_, GWL_EXSTYLE, flyoutStyle_);
            }
        }
        flyoutLayered_ = false;
        flyoutWindow_ = nullptr;
    }
    if (taskbar_) {
        if (!wasVisible_) ShowWindow(taskbar_, SW_HIDE);
        if (panelStillUp) {
            // LEFT ALPHA-0 UNTIL THE BAR'S WATCHDOG RUNS, for the same reason as the panel above: the shell
            // re-shows its own taskbar while its panel is up, and removing the layering here put it on the
            // user's screen, opaque, for the seconds until the bar hid it again. The style is handed over
            // instead of restored (TrayPressFinishCleanup), and a re-show in the meantime is invisible.
            g_cleanupStyle.store(style_);
            g_cleanupTaskbar.store(taskbar_);
        } else {
            // Hidden again first: the bar owns that state, and it re-hides on its own timer anyway.
            if (layered_) SetWindowLongPtrW(taskbar_, GWL_EXSTYLE, style_);
            SetLayeredWindowAttributes(taskbar_, 0, 255, LWA_ALPHA);
        }
        taskbar_ = nullptr;
        layered_ = false;
    }
    flyoutRect_ = {};
}

bool TrayStage::Begin(wchar_t* note, size_t noteCapacity) {
    const auto say = [note, noteCapacity](const wchar_t* text) {
        if (note && noteCapacity > 0) wcsncpy_s(note, noteCapacity, text, _TRUNCATE);
    };
    say(L"");

    visibleRun_ = VisibleRunWanted();

    // Makes the overflow panel invisible. Called for the window the instant its class name answers, and
    // again every time UI Automation sees the element while it slides, and once more after it has settled:
    // see the note where it is first used for why the repetition is the whole trick.
    // ... AND IT IS ALSO WHERE THE PANEL'S WINDOW IS REMEMBERED, which has to happen in every mode: Reset
    // needs the handle and the style it had, whether or not this run ever changed the alpha.
    const auto stagePanelWindow = [this](HWND window) {
        if (!window) return;
        if (window != flyoutWindow_) {
            flyoutWindow_ = window;
            flyoutStyle_ = GetWindowLongPtrW(window, GWL_EXSTYLE);
            if (!visibleRun_ && (flyoutStyle_ & WS_EX_LAYERED) == 0) {
                SetWindowLongPtrW(window, GWL_EXSTYLE, flyoutStyle_ | WS_EX_LAYERED);
                flyoutLayered_ = true;
            }
        }
        if (visibleRun_) return;   // the measurement mode: the shell's panel is left exactly as it is
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
    };
    taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar_) {
        say(L"no taskbar");
        return false;
    }

    // Shown painted with nothing, for the whole of the stage: UI Automation needs the shell's tray to
    // exist, and the user must not see it. See the header for the measurement behind alpha 0.
    style_ = GetWindowLongPtrW(taskbar_, GWL_EXSTYLE);
    wasVisible_ = IsWindowVisible(taskbar_) != FALSE;
    if (!visibleRun_ && (style_ & WS_EX_LAYERED) == 0) {
        SetWindowLongPtrW(taskbar_, GWL_EXSTYLE, style_ | WS_EX_LAYERED);
        layered_ = true;
    }
    if (!visibleRun_) SetLayeredWindowAttributes(taskbar_, 0, 0, LWA_ALPHA);
    // SW_SHOWNA: shown, not activated. Activating another process's window would take the focus away from
    // whatever the user is typing in.
    ShowWindow(taskbar_, SW_SHOWNA);

    IUIAutomation* automation = nullptr;
    // COM, because UI Automation is a COM object and this may be a thread that never asked for an
    // apartment: `--tray-surface` runs on a bare main thread, and without this the whole stage reports
    // "UI Automation could not be created" (which is exactly what it did the first time it was asked).
    // RPC_E_CHANGED_MODE means somebody else already initialized this thread into another apartment, which
    // is fine -- COM is up either way, and it is not ours to uninitialize.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool weInitializedCom = SUCCEEDED(comHr);
    comInitialized_ = weInitializedCom;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation))) ||
        !automation) {
        if (weInitializedCom) CoUninitialize();
        say(L"UI Automation could not be created");
        return false;
    }
    automation_ = automation;

    // The tray's own subtree takes a moment to be published after the window is shown; measured at about
    // 300 ms, which is why this is a poll and not a sleep.
    IUIAutomationElement* taskbarElement = nullptr;
    IUIAutomationElement* chevron = nullptr;
    RECT chevronRect = {};
    for (int attempt = 0; attempt < 60 && !chevron; ++attempt) {
        if (!taskbarElement) automation->ElementFromHandle(taskbar_, &taskbarElement);
        if (taskbarElement) chevron = FindOverflowChevron(automation, taskbarElement, &chevronRect);
        if (!chevron) Sleep(40);
    }
    if (taskbarElement) taskbarElement->Release();
    if (!chevron) {
        say(L"the tray never published its buttons");
        return false;
    }
    chevron_ = chevron;

    // THE FLYOUT FIRST, because the icons have no rectangles until the shell has laid them out -- until
    // then every query answers the chevron's own slot (see the header). It is a toggle, so it is only
    // pressed when it is not already showing.
    //
    // ASKED OF THE WINDOW, NOT OF THE DESKTOP'S AUTOMATION TREE. This used to be a UI Automation search --
    // every top-level window of the desktop, filtered by class name, with each candidate's subtree counted
    // to see whether it held icons -- and it was called every few milliseconds while the panel slid in.
    // Measured on the user's own press (2026-10-15): 1.10 s between "tray press" and the next log line, all
    // of it in here. The question itself is unchanged -- does the panel hold icons -- and it is still the
    // right one; it is now asked of the window's OWN element (one handle lookup) instead of the desktop's
    // children, which is where the second went.
    HWND panelWindow = FindWindowExW(nullptr, nullptr, L"TopLevelWindowForOverflowXamlIsland", nullptr);
    IUIAutomationElement* flyout = nullptr;
    if (panelWindow) automation->ElementFromHandle(panelWindow, &flyout);
    if (!flyout || PanelIconCount(automation, flyout) == 0) {
        if (flyout) {
            flyout->Release();
            flyout = nullptr;
        }
        InvokeElement(chevron);
        weOpenedFlyout_ = true;
        // The window is hidden the moment it appears (every 1 ms) and again after the settle, because the
        // shell RE-PAINTS and re-shows it while it slides in -- which is what undid the first attempt at
        // this, written down at the time as "alpha 0 does nothing to this window". Applied repeatedly it
        // holds; see scripts/probe-tray-panel-child-windows.ps1.
        for (int attempt = 0; attempt < 400; ++attempt) {
            Sleep(1);
            panelWindow = FindWindowExW(nullptr, nullptr, L"TopLevelWindowForOverflowXamlIsland", nullptr);
            if (!panelWindow) continue;
            stagePanelWindow(panelWindow);
            // ... AND IT IS UP WHEN IT HOLDS ICONS, not when it is merely shown.
            if (FAILED(automation->ElementFromHandle(panelWindow, &flyout)) || !flyout) continue;
            if (PanelIconCount(automation, flyout) > 0) break;
            flyout->Release();
            flyout = nullptr;
        }
    }
    if (!flyout) {
        say(L"the overflow flyout did not open");
        return false;
    }
    flyoutElement_ = flyout;
    flyoutWindow_ = panelWindow;

    // THE PANEL IS MADE INVISIBLE, and the history of this is worth keeping because the conclusion drawn
    // from it the first time was wrong:
    //
    //   * `WS_EX_LAYERED` + alpha 0 applied ONCE, as soon as the window appeared, did nothing -- and the
    //     reason is that the shell RE-PAINTS and re-shows the panel while it slides into place, so the
    //     attribute was overwritten before it could be seen. What was written down then ("it stays fully
    //     painted, because it is a XAML island drawn through DirectComposition, which ignores the layered
    //     attributes") described the measurement correctly and the cause wrongly;
    //   * applied AGAIN on every pass, and once more after the panel has settled, it HOLDS. Measured with
    //     scripts/probe-tray-panel-child-windows.ps1: the panel's own rectangle reads 70 (dark, painted)
    //     while it is open, 70 again 800 ms later with nothing done to it, and 193 -- the wallpaper -- the
    //     moment the alpha goes on, with the shell still answering UI Automation for the panel in that
    //     state ("panel alive: True"), which is what the press needs.
    //
    // The child window that paints the island (`Windows.UI.Composition.DesktopWindowContentBridge`, listed
    // by that probe) is not touched: the parent's alpha is what does it.
    LogAndPrint("  tray stage: taskbar %p %ls, overflow %p (%ls)\n",
                reinterpret_cast<void*>(taskbar_),
                visibleRun_ ? L"left ON SCREEN (ISLANDBAR_TRAY_PRESS_VISIBLE)"
                            : L"shown invisibly",
                reinterpret_cast<void*>(flyoutWindow_),
                flyoutWindow_ ? L"found" : L"not found by UI Automation");

    // ... AND LET IT STOP MOVING. The panel slides into place, and a rectangle read while it is moving is
    // a rectangle the button is about to leave: measured, the first row's icons were at y 33, 38, 40, 42
    // and 44 within one pass, because each query caught the panel a little further down.
    const ULONGLONG flyoutSeen = GetTickCount64();
    RECT previous = {};
    RECT current = {};
    int stableReads = 0;
    for (int attempt = 0; attempt < 60 && stableReads < 3; ++attempt) {
        if (FAILED(flyout->get_CurrentBoundingRectangle(&current))) break;
        const bool same = current.left == previous.left && current.top == previous.top &&
                          current.right == previous.right && current.bottom == previous.bottom;
        stableReads = same ? stableReads + 1 : 0;
        previous = current;
        // THREE reads and a minimum age, not two: the slide's last frames move about a pixel per 25 ms,
        // which two reads in a row can easily agree on by accident -- measured, the first row of icons came
        // back at y 43, 45, 47, 48 with the panel nominally "settled", and a rectangle read mid-slide does
        // not match the button that ends up there.
        if (stableReads < 3 || GetTickCount64() - flyoutSeen < 150) Sleep(25);
    }
    // THE PANEL'S OWN PLACE, and it stays that way for the whole press: the icon rectangles below are
    // screen coordinates inside it, and the application's menu -- which the shell anchors to this panel --
    // is placed from it. (An earlier version moved the panel off the screen before this loop and wrote
    // this comment about "its NEW place"; the move is gone and so is the reason for it.)
    flyoutRect_ = current;

    // ... AND NOW THE PANEL CAN BE MADE INVISIBLE -- after it has settled, which is the whole difference.
    //
    // This was tried as soon as the window appeared and then written down as impossible: "WS_EX_LAYERED +
    // alpha 0 ... does nothing to this window -- it stays fully painted, because it is a XAML island drawn
    // through DirectComposition, which ignores the layered attributes". The measurement was right and the
    // conclusion was wrong: the shell RE-PAINTS and re-shows the panel while it slides into place, so an
    // alpha of 0 applied during the slide is overwritten before anybody could see its effect. Applied after
    // the settle -- the same three stable reads the rectangle below needs -- it holds.
    //
    // Measured with scripts/probe-tray-panel-child-windows.ps1, which opens the panel itself, lets it
    // settle, and reads the panel's own rectangle out of a screen grab: 70 (the panel, painted) while it is
    // open and 70 again 800 ms later with nothing done to it, then 193 -- the wallpaper -- the moment the
    // alpha goes on, while the shell still answers UI Automation for it ("panel alive: True"), which is
    // exactly what the press needs. The child window that paints the island is listed by that probe too
    // (`Windows.UI.Composition.DesktopWindowContentBridge`); the parent's alpha is what does it.
    if (flyoutWindow_) {
        // Once more, now that the shell has stopped touching it: this is the one that is guaranteed to
        // survive, and the ones during the slide only shorten the part a user could have seen.
        stagePanelWindow(flyoutWindow_);
        LogAndPrint("  tray stage: %ls (%p, %ls added)\n",
                    visibleRun_ ? L"the overflow panel is ON SCREEN for this run, untouched"
                                : L"the overflow panel was made invisible after it settled",
                    reinterpret_cast<void*>(flyoutWindow_), flyoutLayered_ ? L"layered bit" : L"no");
    }
    // From here on every log line of this press can say how far into it it happened.
    begunAt_ = GetTickCount64();
    return true;
}

bool TrayStage::IconRect(const TrayPressRequest& request, RECT* out) const {
    if (!out) return false;
    if (request.hasGuid) {
        NOTIFYICONIDENTIFIER identifier = {};
        identifier.cbSize = sizeof(identifier);
        identifier.guidItem = request.guid;
        return QueryRect(identifier, out);
    }
    if (!request.hasUid) return false;

    // A UID belongs to a WINDOW of the application, and the registry does not say which: it is looked up
    // by asking the shell about each window the application owns until one of them answers. Wrong pairs
    // simply fail, which is why every window of every one of its processes is tried.
    const std::vector<DWORD> pids = PidsForImage(request.exePath);
    const std::vector<HWND> windows = WindowsOf(pids);
    for (HWND window : windows) {
        NOTIFYICONIDENTIFIER identifier = {};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = window;
        identifier.uID = request.uid;
        if (QueryRect(identifier, out)) return true;
    }

    // ... AND WHEN THE REGISTRY'S UID ANSWERS NOWHERE, a small range is tried instead: one entry on this
    // machine carries 0xFFFFFFFF (the Task Manager's icon, which the shell does display), so the recorded
    // UID is not always the UID the application registered with. Only a UNIQUE answer is accepted -- if
    // two of the application's icons answer, there is no way to tell which one the card's cell is, and
    // pressing the wrong application's menu is worse than pressing nothing.
    RECT unique = {};
    int matches = 0;
    for (unsigned uid = 0; uid <= 8 && matches < 2; ++uid) {
        for (HWND window : windows) {
            NOTIFYICONIDENTIFIER identifier = {};
            identifier.cbSize = sizeof(identifier);
            identifier.hWnd = window;
            identifier.uID = uid;
            RECT rect = {};
            if (!QueryRect(identifier, &rect)) continue;
            const bool same = matches > 0 && rect.left == unique.left && rect.top == unique.top &&
                              rect.right == unique.right && rect.bottom == unique.bottom;
            if (same) continue;   // the same icon found through another window or a repeated query
            unique = rect;
            ++matches;
            break;
        }
    }
    if (matches == 1) {
        *out = unique;
        return true;
    }
    return false;
}

long long TrayStage::SinceBegin() const { return begunAt_ ? MillisSince(begunAt_) : 0; }

void TrayStage::ShowPanelForClick() {
    if (!flyoutWindow_ || visibleRun_) return;
    // ALPHA 1, NOT 255. The panel has to stop being TRANSPARENT, because Windows does not hit-test a
    // layered window whose alpha is 0 -- that is the rule the click depends on, and it is measured twice
    // over in this file (with the panel at alpha 0 `WindowFromPoint` over the icon answers the desktop, and
    // a press that never reveals the panel is a press that does nothing at all). Nothing else about the
    // reveal needs to be visible, so it is not: at 1 of 255 the panel paints 0.4% of its content over
    // whatever is behind it, which measures as the wallpaper (see scripts/measure-tray-panel.ps1, which
    // reads the panel's own rectangle through a whole press).
    SetLayeredWindowAttributes(flyoutWindow_, 0, 1, LWA_ALPHA);
    LogAndPrint("  tray press: the panel was made hit-testable for the click (alpha 1, %lld ms in)\n",
                SinceBegin());
}

void TrayStage::HidePanelAfterClick() {
    if (!flyoutWindow_ || visibleRun_) return;
    SetLayeredWindowAttributes(flyoutWindow_, 0, 0, LWA_ALPHA);
    // ... AND AGAIN, A FEW TIMES, because the shell re-paints and re-shows this panel whenever it is
    // animating it -- the same behaviour Begin fights while the panel slides in. Measured with
    // scripts/measure-tray-panel.ps1 (which samples the panel's own pixels through a whole press): after
    // this call the panel's area keeps reading the wallpaper (mean 42 of 255) for the rest of the press,
    // and the only painted samples in a whole press are the 25-40 ms of the shell's own slide-in.
    //
    // SO THIS DOES NOT BUY THE SILENCE IT WAS ADDED FOR -- the slide-in is painted before anybody gets
    // here. It is kept because the alternative was measured and is worse in the other direction: without
    // it a press that leaves the panel up (a right click, whose menu the shell keeps) can have the shell
    // re-show the icon grid after this function returns, and re-applying the alpha is the only lever this
    // file has ever found on that window.
    const HWND window = flyoutWindow_;
    for (int attempt = 0; attempt < 4; ++attempt) {
        Sleep(45);
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
    }
    LogAndPrint("  tray press: the panel was taken off the screen again (%lld ms in)\n", SinceBegin());
}

bool TrayStage::PanelStillShowing() const {
    return flyoutWindow_ != nullptr && IsWindowVisible(flyoutWindow_) != FALSE;
}

TrayPressResult TrayStage::PressOnRect(const TrayPressRequest& request, const RECT& rect, bool press,
                                       bool rightClick, wchar_t* shellName, size_t shellNameCapacity,
                                       TrayPressOutcome* outcome) {
    IUIAutomation* automation = static_cast<IUIAutomation*>(automation_);
    IUIAutomationElement* flyout = static_cast<IUIAutomationElement*>(flyoutElement_);
    if (!automation || !flyout) return TrayPressResult::NoElement;

    // THE BUTTON ON THAT RECTANGLE IS STILL LOOKED FOR, and it is the only thing UI Automation is used for
    // now: not for its Invoke (see the note at the top of the helpers above), but because "there is a
    // button of the
    // shell's exactly here" is what says the rectangle is real rather than the chevron's slot handed out to
    // every icon, and because the shell's own name for the icon is the one fact about it that the registry
    // copy does not have. A rectangle with nothing on it is reported rather than clicked at.
    IUIAutomationElement* button =
        ElementOnRect(automation, flyout, rect, 4, false, shellName, shellNameCapacity, nullptr);
    if (!button) {
        return TrayPressResult::NoElement;
    }
    if (press && outcome) LogElementFacts(button, L"tray press");

    TrayPressResult result = TrayPressResult::Found;
    rightClicked_ = press && rightClick;
    if (press) {
        result = ClickOnRect(request, rect, rightClick, outcome);
    }
    button->Release();
    return result;
}

// ONE PRESS, DELIVERED AS A REAL CLICK -- both buttons, since 2026-10-15.
//
// The sequence, in the order the plan of that round fixed it:
//
//   1. the panel is made hit-testable -- alpha 1, not 0 and not 255 -- because that is the one thing the
//      shell's side of this needs, and anything more paints the icon grid on the user's screen. Measured
//      both ways: with the panel left transparent the application's menu does not appear at all and the
//      left press does nothing, and with it made opaque the grid is visible for 15-38 ms in every press;
//      see ShowPanelForClick;
//   2. what is UNDER the point is measured before anything is clicked. This bar is TOPMOST and covers the
//      same band as the shell's tray; an alpha-0 layered window is still hit-tested, so if the answer is
//      "this bar" the bar steps out of the way for the length of the delivery (ClickThroughGuard);
//   3. the pointer is moved to the icon's own centre, the button goes down and up -- a real click, which is
//      what a tray application is listening for (see the note at the top of the helpers above);
//   4. LEFT: the panel is made transparent again at once, and the application is watched for 1.5 s for a
//      window of its own or a foreground change, because a clean return is not an answer;
//      RIGHT: the panel is LEFT ALONE until the application's menu has actually appeared (measured: an
//      ownerless menu is worse than anything this costs), and only then is it made transparent again;
//   5. the pointer is put back where the user's hand was, since the menu belongs to the application and
//      does not follow the pointer.
TrayPressResult TrayStage::ClickOnRect(const TrayPressRequest& request, const RECT& rect, bool rightClick,
                                       TrayPressOutcome* outcome) {
    POINT saved = {};
    if (!GetCursorPos(&saved)) return TrayPressResult::Refused;
    const POINT centre = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};

    if (outcome) {
        outcome->route = L"a real click";
        outcome->pointerMoved = true;
        outcome->panelShownMs = 0;
    }
    if (visibleRun_) {
        LogAndPrint("  tray press: nothing was hidden in this run, so nothing has to be revealed\n");
    } else {
        ShowPanelForClick();
        if (outcome) outcome->panelShownMs = SinceBegin();
    }

    // Measured, not assumed: what would receive this click.
    wchar_t target[160] = {};
    bool ours = false;
    DescribeWindowAt(centre, target, sizeof(target) / sizeof(wchar_t), &ours);
    const HWND under = ours ? WindowFromPoint(centre) : nullptr;
    LogAndPrint(L"  tray press: the click at %ld,%ld is aimed at %ls\n", centre.x, centre.y, target);
    if (outcome) {
        wcsncpy_s(outcome->target, sizeof(outcome->target) / sizeof(wchar_t), target, _TRUNCATE);
        outcome->targetIsOurs = ours;
    }
    {
        // The guard lives for the whole delivery AND the wait: input is queued, so a bit taken off straight
        // after the click is sent could be gone before that click is routed.
        ClickThroughGuard guard(under);
        if (guard.applied()) {
            wchar_t after[160] = {};
            bool stillOurs = false;
            DescribeWindowAt(centre, after, sizeof(after) / sizeof(wchar_t), &stillOurs);
            LogAndPrint(L"  tray press: this bar was under the click, so it was made click-through; the "
                        L"point now answers %ls%s\n",
                        after, stillOurs ? L" -- the bit did NOT take effect" : L"");
        }
        if (outcome) outcome->madeClickThrough = guard.applied();

        // The desktop as it was before the click: what "the menu appeared" and "the application answered"
        // are both measured against. See WindowWatch for why this, and not a search for a menu window.
        const WindowWatch watch = TakeWindowWatch(request);
        const HWND foregroundBefore = GetForegroundWindow();

        // THE POINTER FIRST, AND CONFIRMED RATHER THAN SLEPT THROUGH. The click is routed by position, so a
        // click that arrives before the move has landed lands on whatever the pointer used to be over. The
        // previous version guessed that 30 ms was enough; this asks, and gives up after 100 ms. Two pixels
        // of tolerance because the coordinates the shell reports are not always the ones the pointer can
        // hold (the review's warning about rounding, and the reason the centre of the rectangle is used
        // rather than an edge).
        SetCursorPos(centre.x, centre.y);
        for (int attempt = 0; attempt < 20; ++attempt) {
            POINT now = {};
            GetCursorPos(&now);
            if (std::abs(now.x - centre.x) <= 2 && std::abs(now.y - centre.y) <= 2) break;
            Sleep(5);
        }
        // ... AND THE PRESS GOES OUT AS ONE QUEUE. Down and up used to be two calls with a 30 ms sleep
        // between them, which is two chances for something else to happen between a press and its release;
        // in one SendInput they are one item in the input queue.
        INPUT events[2] = {};
        events[0].type = INPUT_MOUSE;
        events[0].mi.dwFlags = rightClick ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        events[1].type = INPUT_MOUSE;
        events[1].mi.dwFlags = rightClick ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        SendInput(2, events, sizeof(INPUT));
        if (outcome) outcome->clickMs = SinceBegin();

        if (rightClick) {
            // THE MENU FIRST, THE PANEL AFTERWARDS. Six hundred milliseconds is the review's budget, and the
            // measured ~245 ms for a menu to reach the screen says it is generous.
            wchar_t menuClass[160] = {};
            RECT menuRect = {};
            HWND menuWindow = nullptr;
            const long long waited = WaitForSomethingToComeUp(watch, 600, menuClass,
                                                              sizeof(menuClass) / sizeof(wchar_t),
                                                              &menuWindow, &menuRect);
            if (outcome) {
                outcome->menuWaitMs = waited;
                wcsncpy_s(outcome->menuClass, sizeof(outcome->menuClass) / sizeof(wchar_t), menuClass,
                          _TRUNCATE);
                outcome->menuRect = menuRect;
            }
            if (waited < 0) {
                LogAndPrint("  tray press: nothing of the application's came up within 600 ms of the "
                            "click, so no menu was seen\n");
            } else {
                LogAndPrint(L"  tray press: something came up %lld ms after the click: %ls -- the icon's "
                            L"own rectangle is %ld,%ld %ldx%ld and the click was at %ld,%ld\n",
                            waited, menuClass, rect.left, rect.top, rect.right - rect.left,
                            rect.bottom - rect.top, centre.x, centre.y);
            }
            HidePanelAfterClick();
            if (outcome && !visibleRun_) outcome->panelHiddenMs = SinceBegin();
            // ... AND IF THE MENU WENT WITH THE PANEL, PUT THE PANEL BACK. The earlier measurement of this
            // route said the menu is drawn inside the panel's own surface; the later one said a menu of the
            // shell's (`#32768`) stays on screen while the panel is invisible. Both cannot be true of the
            // same window, so the question is asked instead of assumed -- and a menu that vanished would be
            // worse than the flash this is trying to remove.
            if (waited >= 0 && !WindowIsStillUp(menuWindow)) {
                LogAndPrint("  tray press: the menu went away with the panel -- it is drawn inside it, so "
                            "the panel stays on screen until the user is done with it\n");
                ShowPanelForClick();
                if (outcome) outcome->panelHiddenMs = 0;
            }
        } else {
            HidePanelAfterClick();
            if (outcome && !visibleRun_) outcome->panelHiddenMs = SinceBegin();
        }
        Sleep(60);
        SetCursorPos(saved.x, saved.y);
        // ... AND ONLY NOW, with the pointer back where the user's hand was, is the application asked
        // whether it noticed. See WatchApplicationForAnswer for why the wait is not 200 ms.
        if (!rightClick && outcome) {
            outcome->appAnswered = WatchApplicationForAnswer(
                watch, foregroundBefore, outcome->answer, sizeof(outcome->answer) / sizeof(wchar_t));
        }
    }
    return TrayPressResult::Pressed;
}

int IndexOfRectNear(const RECT* rects, int count, const RECT& wanted, int tolerancePx) {
    if (!rects) return -1;
    for (int i = 0; i < count; ++i) {
        const RECT& other = rects[i];
        if (std::abs(other.left - wanted.left) <= tolerancePx &&
            std::abs(other.top - wanted.top) <= tolerancePx &&
            std::abs(other.right - wanted.right) <= tolerancePx &&
            std::abs(other.bottom - wanted.bottom) <= tolerancePx) {
            return i;
        }
    }
    return -1;
}

const wchar_t* TrayPressResultName(TrayPressResult result) {
    switch (result) {
        case TrayPressResult::Pressed: return L"pressed";
        case TrayPressResult::Found: return L"found it, and was asked not to press";
        case TrayPressResult::NotInShell: return L"the shell has no rectangle for it";
        case TrayPressResult::NoWindow: return L"no window of that application answered";
        case TrayPressResult::NoElement: return L"nothing sits on that rectangle";
        case TrayPressResult::NoTaskbar: return L"no taskbar";
        case TrayPressResult::Refused: return L"refused";
    }
    return L"?";
}

// Presses one icon: the stage first (the shell's tray laid out and its flyout open, invisibly), then the
// shell's own answer for where the icon is, then the button that sits there.
TrayPressOutcome PressTrayIcon(const TrayPressRequest& request, bool press, bool rightClick) {
    TrayPressOutcome outcome;
    // Before the taskbar is touched: the bar's watchdog must not hide it in the middle of this.
    PressInFlight inFlight;

    // THE POSTED CALLBACK FIRST, when this application has one learned (see the note above TrayCallback). It
    // does not move the pointer, does not show the shell's taskbar or its overflow panel, and takes about
    // ten milliseconds -- and it is how the press looks identical to MyDockFinder's. Everything below is the
    // route that moves the pointer, kept as the fallback for every application without an entry.
    if (press && SendTrayCallback(request, rightClick, &outcome)) return outcome;

    // ... AND THE POINTER-MOVING ROUTE CAN BE SWITCHED OFF, which is what ISLANDBAR_TRAY_NO_CLICK=1 does.
    //
    // The user's position is that the real click is not the answer -- the jump is the one thing they will not
    // have (2026-10-15: "把真实点击这个去掉不要 … 我就想要 MyDockFinder 一样的效果"). It is not deleted yet,
    // and the reason is measured rather than cautious: only applications whose callback has been learned can
    // be pressed without it, which today is one, so switching it off for good would leave every other
    // application unpressed -- a state that looks like a broken button rather than like MyDockFinder. The way
    // out is the shell-side route being probed now (scripts/probe-tray-in-explorer.ps1), which covers every
    // application at once, including new ones; when that lands, this whole block goes. Until then this
    // switch is how the no-click state is looked at deliberately.
    if (press && SwitchOn(L"ISLANDBAR_TRAY_NO_CLICK")) {
        LogAndPrint(L"  tray press: this application has no learned callback and the real click is switched "
                    L"off (ISLANDBAR_TRAY_NO_CLICK), so nothing was pressed\n");
        outcome.result = TrayPressResult::NotInShell;
        outcome.route = L"nothing (the real click is switched off)";
        outcome.pointerMoved = false;
        return outcome;
    }

    TrayStage stage;
    wchar_t note[160] = {};
    if (!stage.Begin(note, 160)) {
        outcome.result = (std::wcscmp(note, L"no taskbar") == 0) ? TrayPressResult::NoTaskbar
                                                                : TrayPressResult::NoElement;
        return outcome;
    }

    const ULONGLONG lookupStart = GetTickCount64();
    const bool haveRect = stage.IconRect(request, &outcome.rect);
    outcome.lookupMs = MillisSince(lookupStart);
    if (!haveRect) {
        // A registered icon the shell is not displaying: four of the sixteen on this machine answer
        // 0x80004005 for this reason. There is nothing to press, and that is a fact to report rather
        // than to work around.
        outcome.result = (request.hasGuid || request.hasUid) ? TrayPressResult::NotInShell
                                                            : TrayPressResult::NoWindow;
        return outcome;
    }

    const ULONGLONG pressStart = GetTickCount64();
    // No separate "is it in the band or the flyout" test: a button either sits on the rectangle or it does
    // not, and that is the same question asked with the better answer. An icon in the tray PROPER is not a
    // UI Automation button at all (the taskbar's own tree holds its six system buttons and nothing else),
    // so it comes back as "nothing sits on that rectangle" -- which is the truth about it.
    outcome.result = stage.PressOnRect(request, outcome.rect, press, rightClick, outcome.shellName,
                                       sizeof(outcome.shellName) / sizeof(wchar_t), &outcome);
    outcome.pressMs = MillisSince(pressStart);
    return outcome;
}

} // namespace island
