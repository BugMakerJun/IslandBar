// =====================================================================
//  TrayUia.cpp -- see TrayUia.h for what this is for and what it costs
// =====================================================================
#include "shell/TrayUia.h"

#include <objbase.h>   // ... BEFORE uiautomation.h: it declares COM interfaces and will not compile without
#include <comdef.h>    // _variant_t: the UIA property conditions take a VARIANT
#include <uiautomation.h>   // the client headers, which bring the core ones and the property ids with them
#include <cstring>
#include <string>

#include "core/Log.h"

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace island {
namespace {

// The class names the shell's tray uses, measured on this machine. The buttons carry their own class as well
// (`SystemTray.NormalButton` for an application's icon, `SystemTray.AccentButton` for the shell's own power,
// network, volume and clock), and the chevron that opens the overflow is an AccentButton too.
// THE TREE HANGS OFF Shell_TrayWnd, not off the window called SystemTray_Main -- measured: a first version
// looked for SystemTray_Main and found nothing at all, not even the chevron, while walking Shell_TrayWnd's
// descendants finds every button (its XAML island, `Windows.UI.Composition.DesktopWindowContentBridge`, is a
// child of it). SystemTray_Main is tried second in case a future build moves them.
constexpr wchar_t kTrayWindowClass[] = L"SystemTray_Main";
constexpr wchar_t kTaskbarClass[] = L"Shell_TrayWnd";
// The overflow is its own top-level window once it is open (see the note in PressTrayIconWithUia about why the
// search used to start at the desktop).
constexpr wchar_t kOverflowClass[] = L"TopLevelWindowForOverflowXamlIsland";
constexpr wchar_t kChevronName[] = L"显示隐藏的图标";
constexpr wchar_t kAccentClass[] = L"SystemTray.AccentButton";
constexpr wchar_t kNormalClass[] = L"SystemTray.NormalButton";

// RAII for one COM pointer, because every early return in this file would otherwise have to remember to release
// it, and one of them would not.
template <typename T>
class ComPtr {
public:
    ~ComPtr() { if (value_) value_->Release(); }
    T** Put() { return &value_; }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void Own(T* value) { Reset(); value_ = value; }
    void Reset() { if (value_) { value_->Release(); value_ = nullptr; } }

private:
    T* value_ = nullptr;
};

std::wstring ElementName(IUIAutomationElement* element) {
    if (!element) return std::wstring();
    BSTR name = nullptr;
    if (FAILED(element->get_CurrentName(&name)) || !name) return std::wstring();
    std::wstring text(name, SysStringLen(name));
    SysFreeString(name);
    return text;
}

std::wstring ElementClass(IUIAutomationElement* element) {
    if (!element) return std::wstring();
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentClassName(&value)) || !value) return std::wstring();
    std::wstring text(value, SysStringLen(value));
    SysFreeString(value);
    return text;
}

// WHETHER AN ELEMENT IS THE ICON THE CALLER MEANS.
//
// The card's name for a cell comes from the shell's registry record (its tooltip); the element's name is the
// tooltip the shell is showing now. They usually agree and do not always: the shell's own network button is
// named "网络 网络\nInternet 访问" while the record says "网络", and a tooltip can carry a second line of state
// ("NVIDIA 设置", "Steam - 正在同步"). So the test is a containment test in both directions on the flattened
// text, not an equality -- and the FIRST element that matches wins, which is the leftmost icon of the tray.
bool NameMatches(const std::wstring& elementName, const wchar_t* wanted) {
    if (!wanted || !wanted[0] || elementName.empty()) return false;
    std::wstring flatName;
    for (wchar_t c : elementName) flatName.push_back((c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c);
    std::wstring flatWanted;
    for (const wchar_t* p = wanted; *p; ++p) flatWanted.push_back((*p == L'\n' || *p == L'\r') ? L' ' : *p);
    if (flatName.find(flatWanted) != std::wstring::npos) return true;
    if (flatWanted.find(flatName) != std::wstring::npos) return true;
    return false;
}

// The shell's tray buttons, depth first, as a flat list. Descendants rather than children because the XAML
// island nests its buttons a few levels down, and the count is small (a dozen on this machine).
struct TrayButtons {
    IUIAutomationElement* chevron = nullptr;
    IUIAutomationElement* match = nullptr;
    // How many tray buttons the walk SAW, and the names of the first few: "found nothing to match" and "the
    // walk saw no buttons at all" are different faults, and the first version of this reported neither.
    int seen = 0;
    std::wstring firstName;
};

void WalkForElements(IUIAutomation* automation, IUIAutomationElement* parent, const wchar_t* wanted,
                     TrayButtons* found) {
    if (!automation || !parent || !found) return;
    ComPtr<IUIAutomationCondition> buttons;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                   _variant_t(static_cast<long>(UIA_ButtonControlTypeId))
                                                       .Detach(),
                                                   buttons.Put()))) {
        return;
    }
    ComPtr<IUIAutomationElementArray> children;
    if (FAILED(parent->FindAll(TreeScope_Descendants, buttons.Get(), children.Put())) || !children) return;
    int count = 0;
    if (FAILED(children->get_Length(&count))) return;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(children->GetElement(i, element.Put())) || !element) continue;
        const std::wstring cls = ElementClass(element.Get());
        const bool isTrayButton = cls == kAccentClass || cls == kNormalClass;
        if (!isTrayButton) continue;
        const std::wstring name = ElementName(element.Get());
        ++found->seen;
        if (found->firstName.empty() && cls == kNormalClass) found->firstName = name;
        if (cls == kAccentClass && name.find(kChevronName) != std::wstring::npos) {
            if (!found->chevron) {
                element.Get()->AddRef();
                found->chevron = element.Get();
            }
            continue;
        }
        if (!found->match && NameMatches(name, wanted)) {
            element.Get()->AddRef();
            found->match = element.Get();
            // ... and keep walking: the chevron is needed too when the icon turns out to be in the overflow.
            continue;
        }
    }
}

IUIAutomationElement* FindTrayRoot(IUIAutomation* automation) {
    ComPtr<IUIAutomationElement> desktop;
    if (FAILED(automation->GetRootElement(desktop.Put())) || !desktop) return nullptr;
    // The taskbar first, then the tray window: the buttons are under the taskbar on this build, and asking for
    // the wrong one first costs a failed lookup rather than a wrong answer.
    for (const wchar_t* className : {kTaskbarClass, kTrayWindowClass}) {
        ComPtr<IUIAutomationCondition> byClass;
        if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId,
                                                       _variant_t(className).Detach(), byClass.Put()))) {
            continue;
        }
        for (TreeScope scope : {TreeScope_Children, TreeScope_Descendants}) {
            IUIAutomationElement* root = nullptr;
            if (SUCCEEDED(desktop->FindFirst(scope, byClass.Get(), &root)) && root) return root;
        }
    }
    return nullptr;
}

bool Invoke(IUIAutomationElement* element) {
    if (!element) return false;
    ComPtr<IUIAutomationInvokePattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                                            reinterpret_cast<void**>(pattern.Put())))) {
        return false;
    }
    return pattern && SUCCEEDED(pattern->Invoke());
}

// THE TASKBAR, MADE ALIVE FOR THE LENGTH OF A PRESS.
//
// `SW_SHOWNA` rather than `SW_SHOW`: the tray has to EXIST for UIA to see it, and it must not steal the
// foreground from the application this press is meant to bring forward. Nothing here touches the AppBar state,
// so the desktop's work area does not move and no maximized window is reflowed -- the cost is the taskbar
// appearing for a few hundred milliseconds and nothing else.
struct TaskbarReveal {
    HWND taskbar = nullptr;
    HWND tray = nullptr;
    bool hiddenTaskbar = false;
    bool hiddenTray = false;

    bool Show() {
        taskbar = FindWindowW(kTaskbarClass, nullptr);
        tray = FindWindowW(kTrayWindowClass, nullptr);
        hiddenTaskbar = taskbar && !IsWindowVisible(taskbar);
        hiddenTray = tray && !IsWindowVisible(tray);
        if (taskbar) ShowWindow(taskbar, SW_SHOWNA);
        if (tray) ShowWindow(tray, SW_SHOWNA);
        // The shell needs a moment to lay its XAML tray out before its buttons are in the tree: measured, a
        // lookup immediately after ShowWindow finds nothing while one 250 ms later finds every icon.
        Sleep(250);
        return true;
    }

    void Hide() {
        if (tray && hiddenTray) ShowWindow(tray, SW_HIDE);
        if (taskbar && hiddenTaskbar) ShowWindow(taskbar, SW_HIDE);
    }
};

}  // namespace

TrayUiaOutcome PressTrayIconWithUia(const wchar_t* iconName, const wchar_t* secondName) {
    TrayUiaOutcome outcome;
    if (!iconName || !iconName[0]) {
        std::swprintf(outcome.detail, 192, L"the cell has no name to look for");
        return outcome;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        outcome.refused = true;
        std::swprintf(outcome.detail, 192, L"COM could not be started (0x%08X)", static_cast<unsigned>(comHr));
        return outcome;
    }

    TaskbarReveal reveal;
    reveal.Show();

    ComPtr<IUIAutomation> automation;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  reinterpret_cast<void**>(automation.Put()));
    if (FAILED(hr) || !automation) {
        outcome.refused = true;
        std::swprintf(outcome.detail, 192, L"UI Automation is not available (0x%08X)",
                      static_cast<unsigned>(hr));
        reveal.Hide();
        if (comOwned) CoUninitialize();
        return outcome;
    }

    // WHERE THE SEARCH STARTS, AND WHY IT IS NOT THE DESKTOP.
    //
    // The tray's own buttons are under Shell_TrayWnd -- but the OVERFLOW is a separate top-level window
    // (`TopLevelWindowForOverflowXamlIsland`), so an icon the user has hidden appears in a different tree from
    // the one the chevron lives in. Measured: walking only the taskbar's subtree after opening the overflow
    // found nothing, every time.
    //
    // The next version walked the DESKTOP's descendants, which works and costs six seconds a press on a busy
    // desktop -- thousands of elements, each asked for its class name. The two windows above are the whole
    // answer, so those are the two trees this walks; the class filter inside the walk stays as the second
    // guard, not the only one.
    TrayButtons found;
    ComPtr<IUIAutomationElement> desktop;
    automation->GetRootElement(desktop.Put());
    if (desktop) {
        for (const wchar_t* className : {kTaskbarClass, kOverflowClass, kTrayWindowClass}) {
            ComPtr<IUIAutomationCondition> byClass;
            if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId,
                                                           _variant_t(className).Detach(), byClass.Put()))) {
                continue;
            }
            ComPtr<IUIAutomationElement> root;
            if (FAILED(desktop->FindFirst(TreeScope_Children, byClass.Get(), root.Put())) || !root) continue;
            WalkForElements(automation.Get(), root.Get(), iconName, &found);
            if (!found.match && secondName && secondName[0]) {
                WalkForElements(automation.Get(), root.Get(), secondName, &found);
            }
        }
    }
    // The overflow: an icon the user has hidden is only in the tree once the chevron has been invoked, and
    // invoking the chevron is itself a UIA Invoke -- no pointer involved.
    if (!found.match && found.chevron && Invoke(found.chevron)) {
        outcome.openedOverflow = true;
        Sleep(300);
        if (desktop) {
            TrayButtons again;
            ComPtr<IUIAutomationCondition> byClass;
            if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId,
                                                             _variant_t(kOverflowClass).Detach(),
                                                             byClass.Put()))) {
                ComPtr<IUIAutomationElement> root;
                if (SUCCEEDED(desktop->FindFirst(TreeScope_Children, byClass.Get(), root.Put())) && root) {
                    WalkForElements(automation.Get(), root.Get(), iconName, &again);
                    if (!again.match && secondName && secondName[0]) {
                        WalkForElements(automation.Get(), root.Get(), secondName, &again);
                    }
                }
            }
            found.match = again.match;
        }
    }

    if (found.match) {
        outcome.foundIcon = true;
        outcome.pressed = Invoke(found.match);
        std::swprintf(outcome.detail, 192, L"the shell's own tray icon '%ls'%ls was %ls",
                      ElementName(found.match).c_str(), outcome.openedOverflow ? L" (from the overflow)" : L"",
                      outcome.pressed ? L"invoked" : L"found but refused to be invoked");
    } else {
        std::swprintf(outcome.detail, 192,
                      L"no tray icon named '%ls'%ls in the shell's tree: %d button(s) seen, the first named '%ls'%ls",
                      iconName, (secondName && secondName[0]) ? L" (or its recorded tip)" : L"", found.seen, found.firstName.empty() ? L"-" : found.firstName.c_str(),
                      found.chevron ? L", and the overflow was opened" : L", and no chevron was found");
    }

    if (found.match) found.match->Release();
    if (found.chevron) found.chevron->Release();
    desktop.Reset();
    automation.Reset();
    reveal.Hide();
    if (comOwned) CoUninitialize();
    return outcome;
}

} // namespace island
