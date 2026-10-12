// =====================================================================
//  TrayPress.cpp -- see the header for the route and for what it is built on.
// =====================================================================
#include "TrayPress.h"

#include "Log.h"

#include <shellapi.h>
#include <tlhelp32.h>
// objbase.h before the UI Automation headers: with WIN32_LEAN_AND_MEAN, windows.h does not pull in the
// COM declarations, and the SDK's UIA headers are written in terms of them -- every interface in them
// fails to compile without this ("missing type specifier ... IRawElementProviderSimple"), which is what
// the first two attempts at this file produced.
#include <objbase.h>
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

// Marks the press for the frame loop, and clears it whatever way the function returns.
class PressInFlight {
public:
    PressInFlight() { g_pressInFlight.store(true); }
    ~PressInFlight() { g_pressInFlight.store(false); }
};

// The taskbar, shown but painted with nothing, for as long as the press needs it to exist.
//
// RAII because every path out of the sequence has to put the desktop back: a return that forgot this
// would leave the shell's taskbar visible over the bar, which is the one thing this whole project must
// not do. The rule is the same one the probe scripts follow, and the layering is removed rather than
// left in place: an extra extended style on somebody else's window is not ours to keep.
class VisibleButInvisible {
public:
    explicit VisibleButInvisible(HWND taskbar) : taskbar_(taskbar) {
        if (!taskbar_) return;
        style_ = GetWindowLongPtrW(taskbar_, GWL_EXSTYLE);
        wasVisible_ = IsWindowVisible(taskbar_) != FALSE;
        if ((style_ & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(taskbar_, GWL_EXSTYLE, style_ | WS_EX_LAYERED);
            addedLayered_ = true;
        }
        SetLayeredWindowAttributes(taskbar_, 0, 0, LWA_ALPHA);
        // SW_SHOWNA: shown, not activated. Activating another process's window would take the focus
        // away from whatever the user is typing in.
        ShowWindow(taskbar_, SW_SHOWNA);
    }

    ~VisibleButInvisible() {
        if (!taskbar_) return;
        // Hidden again first: the bar owns that state, and it re-hides on its own timer anyway.
        if (!wasVisible_) ShowWindow(taskbar_, SW_HIDE);
        if (addedLayered_) SetWindowLongPtrW(taskbar_, GWL_EXSTYLE, style_);
        SetLayeredWindowAttributes(taskbar_, 0, 255, LWA_ALPHA);
    }

private:
    HWND taskbar_ = nullptr;
    LONG_PTR style_ = 0;
    bool addedLayered_ = false;
    bool wasVisible_ = false;
};

// The same treatment for the overflow flyout, which is a window of the shell's in its own right: it is
// what the icons are pressed in, and the user has no reason to watch it appear.
class InvisibleWindow {
public:
    ~InvisibleWindow() {
        if (!hwnd_) return;
        if (addedLayered_) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style_);
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    }

    void Apply(HWND hwnd) {
        if (!hwnd || hwnd == hwnd_) return;
        hwnd_ = hwnd;
        style_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if ((style_ & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style_ | WS_EX_LAYERED);
            addedLayered_ = true;
        }
        SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
    }

private:
    HWND hwnd_ = nullptr;
    LONG_PTR style_ = 0;
    bool addedLayered_ = false;
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
            const HRESULT hr = element->GetCurrentPatternAs(UIA_InvokePatternId,
                                                            IID_PPV_ARGS(&invoke));
            invokable = SUCCEEDED(hr) && invoke != nullptr;
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

// How many buttons of the shell's overflow flyout are inside `element`. Zero for the flyout's window when
// it is closed -- and that window EXISTS while closed (measured: it is in the desktop's window list,
// hidden, with the same rectangle it has when open), so "a window of that class is present" is not the
// question. "Does it hold icons" is.
int OverflowIconCount(IUIAutomation* automation, IUIAutomationElement* element) {
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

// The shell's overflow flyout: the top-level window it opens the hidden icons in, ready to be pressed in
// (shown, and holding buttons). Null when there is nothing to press in yet.
IUIAutomationElement* FindOverflowFlyout(IUIAutomation* automation) {
    if (!automation) return nullptr;
    IUIAutomationElement* desktop = nullptr;
    if (FAILED(automation->GetRootElement(&desktop)) || !desktop) return nullptr;

    VARIANT className = {};
    className.vt = VT_BSTR;
    className.bstrVal = SysAllocString(L"TopLevelWindowForOverflowXamlIsland");
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElement* flyout = nullptr;
    if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, className,
                                                     &condition)) &&
        condition) {
        IUIAutomationElementArray* tops = nullptr;
        if (SUCCEEDED(desktop->FindAll(TreeScope_Children, condition, &tops)) && tops) {
            int count = 0;
            tops->get_Length(&count);
            for (int i = 0; i < count && !flyout; ++i) {
                IUIAutomationElement* candidate = nullptr;
                if (FAILED(tops->GetElement(i, &candidate)) || !candidate) continue;
                UIA_HWND native = nullptr;
                const bool shown = SUCCEEDED(candidate->get_CurrentNativeWindowHandle(&native)) &&
                                   native && IsWindowVisible(reinterpret_cast<HWND>(native));
                if (shown && OverflowIconCount(automation, candidate) > 0) {
                    candidate->AddRef();
                    flyout = candidate;
                }
                candidate->Release();
            }
            tops->Release();
        }
        condition->Release();
    }
    SysFreeString(className.bstrVal);
    desktop->Release();
    return flyout;
}

} // namespace

bool TrayPressInFlight() { return g_pressInFlight.load(); }

bool TrayPressCleanupPending() { return g_cleanupTaskbar.load() != nullptr; }

void TrayPressFinishCleanup() {
    const HWND taskbar = g_cleanupTaskbar.exchange(nullptr);
    if (!taskbar) return;
    const LONG_PTR style = g_cleanupStyle.load();
    ShowWindow(taskbar, SW_HIDE);
    SetWindowLongPtrW(taskbar, GWL_EXSTYLE, style);
    SetLayeredWindowAttributes(taskbar, 0, 255, LWA_ALPHA);
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
        // A moment for what the press opened to be up first -- longer for a menu, which the application
        // creates just after the click is delivered and which is dismissed if the panel it was opened from
        // disappears underneath it; a window does not care.
        Sleep(50);
        InvokeElement(static_cast<IUIAutomationElement*>(chevron_));
        Sleep(120);
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
    if (flyoutWindow_) {
        // Back where the shell had it, so the panel is where the shell thinks it is the next time somebody
        // opens it by hand.
        SetWindowPos(flyoutWindow_, nullptr, flyoutHome_.left, flyoutHome_.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        flyoutWindow_ = nullptr;
    }
    if (taskbar_) {
        if (!wasVisible_) ShowWindow(taskbar_, SW_HIDE);
        if (rightClicked_ && weOpenedFlyout_) {
            // LEFT ALPHA-0 UNTIL THE BAR'S WATCHDOG RUNS, and the reason is measured: while the shell's
            // panel and its menu are up, the SHELL re-shows its own taskbar -- and removing the layering
            // here put it on the user's screen, opaque, for the seconds until the bar hid it again. So the
            // style is handed over instead of restored (TrayPressFinishCleanup), and a re-show in the
            // meantime is invisible.
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

    taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar_) {
        say(L"no taskbar");
        return false;
    }

    // Shown painted with nothing, for the whole of the stage: UI Automation needs the shell's tray to
    // exist, and the user must not see it. See the header for the measurement behind alpha 0.
    style_ = GetWindowLongPtrW(taskbar_, GWL_EXSTYLE);
    wasVisible_ = IsWindowVisible(taskbar_) != FALSE;
    if ((style_ & WS_EX_LAYERED) == 0) {
        SetWindowLongPtrW(taskbar_, GWL_EXSTYLE, style_ | WS_EX_LAYERED);
        layered_ = true;
    }
    SetLayeredWindowAttributes(taskbar_, 0, 0, LWA_ALPHA);
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
    IUIAutomationElement* flyout = FindOverflowFlyout(automation);
    if (!flyout) {
        InvokeElement(chevron);
        weOpenedFlyout_ = true;
        // 5 ms steps: the flyout is visible from the moment the shell paints it until this loop catches it
        // and makes it invisible, and that interval is the only part of the sequence a user could see.
        for (int attempt = 0; attempt < 300 && !flyout; ++attempt) {
            Sleep(5);
            flyout = FindOverflowFlyout(automation);
        }
    }
    if (!flyout) {
        say(L"the overflow flyout did not open");
        return false;
    }
    flyoutElement_ = flyout;

    // THE PANEL ITSELF CANNOT BE HIDDEN, and that is measured rather than assumed:
    //
    //   * `WS_EX_LAYERED` + alpha 0 works on the TASKBAR (its band photographs as wallpaper, twice) and
    //     does nothing to this window -- it stays fully painted, because it is a XAML island drawn through
    //     DirectComposition, which ignores the layered attributes;
    //   * moving its window moves the ICONS' rectangles with it (-1632,89 for the WeChat icon, 4000 px to
    //     the left) and leaves the pixels on the screen where they were -- so the panel is painted by some
    //     other window of the shell's, which probe-tray-panel-windows.ps1 went looking for among every
    //     window overlapping that rectangle.
    //
    // So the panel is visible for the length of a press -- a few hundred milliseconds, and the press is
    // the user's own action -- and what this stage does is keep that as short as the shell allows: the
    // panel is opened, the button is pressed, and the chevron is pressed again to put it away (see Reset,
    // which is also the only thing that takes it down at all).
    UIA_HWND native = nullptr;
    if (SUCCEEDED(flyout->get_CurrentNativeWindowHandle(&native)) && native) {
        flyoutWindow_ = reinterpret_cast<HWND>(native);
    }
    LogAndPrint("  tray stage: taskbar %p shown invisibly, overflow %p (%ls)\n",
                reinterpret_cast<void*>(taskbar_), reinterpret_cast<void*>(flyoutWindow_),
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
    // Already the panel's NEW place: it was moved off the screen before this loop started, and the icons
    // move with their window.
    flyoutRect_ = current;
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

TrayPressResult TrayStage::PressOnRect(const RECT& rect, bool press, bool rightClick, wchar_t* shellName,
                                      size_t shellNameCapacity) {
    IUIAutomation* automation = static_cast<IUIAutomation*>(automation_);
    IUIAutomationElement* flyout = static_cast<IUIAutomationElement*>(flyoutElement_);
    if (!automation || !flyout) return TrayPressResult::NoElement;

    IUIAutomationElement* button =
        ElementOnRect(automation, flyout, rect, 4, !rightClick, shellName, shellNameCapacity, nullptr);
    if (!button) {
        // The right-click path does not need an Invoke pattern, so the element is looked for without one
        // and its absence is then the honest answer.
        return TrayPressResult::NoElement;
    }

    TrayPressResult result = TrayPressResult::Found;
    rightClicked_ = press && rightClick;
    if (press && !rightClick) {
        result = InvokeElement(button) ? TrayPressResult::Pressed : TrayPressResult::Refused;
    } else if (press) {
        // THE APPLICATION'S OWN MENU -- the menu with 退出 in it -- and this is the one place the bar
        // synthesises real input.
        //
        // Three routes were measured. UI Automation has no right-click pattern at all. A POSTED right
        // button message leaves the application silent: the panel's icons are not driven from window
        // messages, so nothing opens and nothing complains. What does work is a real right click AT THE
        // ICON'S OWN POSITION, which is what a user's right click is -- so the pointer is moved there,
        // clicked, and put straight back where it was found, and the only thing that stays on screen is the
        // menu that was asked for.
        POINT saved = {};
        if (!GetCursorPos(&saved)) {
            result = TrayPressResult::Refused;
        } else {
            const POINT centre = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
            SetCursorPos(centre.x, centre.y);
            // No click without the pointer actually being there, and no click without a moment for the
            // move to land: a right click is routed by position, so a click that arrives before the move
            // is a click on whatever the pointer used to be over.
            Sleep(30);
            mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
            Sleep(30);
            mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            // Back at once: the menu belongs to the application and does not follow the pointer, so this
            // does not close it -- and the user's hand is where they left it.
            Sleep(60);
            SetCursorPos(saved.x, saved.y);
            result = TrayPressResult::Pressed;
        }
    }
    button->Release();
    return result;
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
    outcome.result = stage.PressOnRect(outcome.rect, press, rightClick, outcome.shellName,
                                       sizeof(outcome.shellName) / sizeof(wchar_t));
    outcome.pressMs = MillisSince(pressStart);
    return outcome;
}

} // namespace island
