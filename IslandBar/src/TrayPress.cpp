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

bool ShellRectForTrayIcon(const TrayPressRequest& request, RECT* out) {
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

TrayPressOutcome PressTrayIcon(const TrayPressRequest& request, bool press) {
    TrayPressOutcome outcome;
    // Before the taskbar is touched: the bar's watchdog must not hide it in the middle of this.
    PressInFlight inFlight;

    // 1. WHERE THE ICON IS, which needs no taskbar and no window of ours -- measured with the taskbar
    // hidden, and with it shown but invisible.
    const ULONGLONG lookupStart = GetTickCount64();
    const bool haveRect = ShellRectForTrayIcon(request, &outcome.rect);
    outcome.lookupMs = MillisSince(lookupStart);
    if (!haveRect) {
        // A registered icon the shell is not displaying: four of the sixteen on this machine answer
        // 0x80004005 for this reason. There is nothing to press, and that is a fact to report rather
        // than to work around.
        outcome.result = (request.hasGuid || request.hasUid) ? TrayPressResult::NotInShell
                                                            : TrayPressResult::NoWindow;
        return outcome;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        outcome.result = TrayPressResult::NoTaskbar;
        return outcome;
    }

    const ULONGLONG pressStart = GetTickCount64();
    VisibleButInvisible taskbarState(taskbar);
    InvisibleWindow flyoutState;

    IUIAutomation* automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation))) ||
        !automation) {
        outcome.result = TrayPressResult::Refused;
        return outcome;
    }
    // The tray's own subtree takes a moment to be published after the window is shown; measured at about
    // 300 ms, which is why this is a poll and not a sleep.
    IUIAutomationElement* taskbarElement = nullptr;
    IUIAutomationElement* chevron = nullptr;
    RECT chevronRect = {};
    for (int attempt = 0; attempt < 25 && !chevron; ++attempt) {
        if (!taskbarElement) automation->ElementFromHandle(taskbar, &taskbarElement);
        if (taskbarElement) chevron = FindOverflowChevron(automation, taskbarElement, &chevronRect);
        if (!chevron) Sleep(40);
    }

    // The icon's rectangle tells us which of the shell's two trays holds it: the taskbar band (y under
    // the bar) is the tray proper, below it is the overflow flyout. Only the flyout's icons are
    // published as buttons.
    RECT taskbarRect = {};
    GetWindowRect(taskbar, &taskbarRect);
    const bool inOverflow = outcome.rect.top >= taskbarRect.bottom - 2;

    if (!chevron) {
        outcome.result = TrayPressResult::NoElement;
    } else if (!inOverflow) {
        // The tray PROPER: the shell publishes its six system buttons there and nothing else, so an
        // application icon in it cannot be pressed through UI Automation. Reported, not guessed at.
        outcome.result = TrayPressResult::NoElement;
    } else {
        // The flyout has to be UP for its buttons to exist at all, and the chevron is a toggle -- so it is
        // only pressed when the flyout is not already showing.
        IUIAutomationElement* flyout = FindOverflowFlyout(automation);
        if (!flyout) {
            InvokeElement(chevron);
            // 5 ms steps, not 40: the flyout is on the screen from the moment the shell paints it until
            // this loop catches it and makes it invisible, and that interval is the only part of this
            // whole sequence a user could see. Measured at 40 ms steps: the panel's own opening animation
            // was still being caught late enough to be noticeable.
            for (int attempt = 0; attempt < 200 && !flyout; ++attempt) {
                Sleep(5);
                flyout = FindOverflowFlyout(automation);
            }
        }
        if (!flyout) {
            outcome.result = TrayPressResult::NoElement;
        } else {
            // Invisible BEFORE anything is pressed in it: the user asked for an icon to be pressed, not to
            // watch the shell's panel open on the way there.
            UIA_HWND native = nullptr;
            if (SUCCEEDED(flyout->get_CurrentNativeWindowHandle(&native)) && native) {
                flyoutState.Apply(reinterpret_cast<HWND>(native));
            }
            IUIAutomationElement* button = ElementOnRect(
                automation, flyout, outcome.rect, 4, true, outcome.shellName,
                sizeof(outcome.shellName) / sizeof(wchar_t), nullptr);
            if (button) {
                if (!press) {
                    outcome.result = TrayPressResult::Found;
                } else {
                    outcome.result =
                        InvokeElement(button) ? TrayPressResult::Pressed : TrayPressResult::Refused;
                }
                button->Release();
            } else {
                outcome.result = TrayPressResult::NoElement;
            }
            flyout->Release();
            // The flyout and the taskbar go away together when this function returns: the taskbar state
            // hides it again, and the application's own window or menu is not a child of the shell's
            // (measured: the overflow panel is a top-level window of the shell's, and what a press opens
            // belongs to the application).
        }
    }

    if (chevron) chevron->Release();
    if (taskbarElement) taskbarElement->Release();
    automation->Release();
    outcome.pressMs = MillisSince(pressStart);
    return outcome;
}

} // namespace island
