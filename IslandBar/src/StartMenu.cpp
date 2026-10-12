// =====================================================================
//  StartMenu.cpp
// =====================================================================
#include "StartMenu.h"

#include "Log.h"

#include <windows.h>
#include <shellapi.h>   // ShellExecuteW: the route that asks the shell to activate the Start menu itself

namespace island {
namespace {

constexpr WORD kCtrlKey = VK_CONTROL;
constexpr WORD kEscapeKey = VK_ESCAPE;
constexpr WORD kWinKey = VK_LWIN;

// How long the chord is held down.
//
// The probe script that first established "Ctrl+Esc opens Start in this session" held Escape down for
// 40 ms; the bar sent all four events back to back. Zero-length presses are the one difference between
// the attempt that worked and the attempt that did not.
constexpr DWORD kHoldMs = 40;

// The held chord is the DEFAULT now, and the reason is measured.
//
// Measured at the shell's own integrity level (the copy of the bar outside the project folder -- inside
// it the process is sandboxed down to low integrity, where nothing it sends to Explorer arrives at all;
// see HANDOFF 7.84): the four-events-back-to-back form produced no window at all, three times running,
// while the held form -- 40 ms between Escape down and up, which is what the probe script used -- brought
// a UWP host window (Windows.UI.Core.CoreWindow) to the foreground each time. A zero-length press is
// evidently not recognised as a chord.
//
// SendInput is kept rather than keybd_event for the reason the file already gives: it can report "refused"
// instead of "did nothing".
StartMenuRoute g_route = StartMenuRoute::SendInputHeld;

// One keystroke through keybd_event.
//
// The older call, kept because it enters the queue by a different path from SendInput -- and on this
// machine the difference is not academic: SendInput reports success and the shell ignores the chord.
// It cannot report refusal, so its result is not trustworthy as a diagnostic; the caller says which
// route it used instead.
void SendKeyLegacy(WORD vk, bool up) {
    keybd_event(static_cast<BYTE>(vk), 0, up ? KEYEVENTF_KEYUP : 0, 0);
}

// One keystroke, delivered through SendInput.
//
// SendInput rather than keybd_event, which is the older call: keybd_event is documented as superseded
// and it cannot report whether the input was accepted. The whole point here is being able to tell
// "delivered" from "refused".
bool SendKey(WORD vk, bool up) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    // Extended keys need the flag; the Windows key does not, but Control does not either -- setting it
    // for a key that is not extended is what produces a key the shell does not recognise.
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

} // namespace

const wchar_t* StartMenuResultName(StartMenuResult result) {
    switch (result) {
        case StartMenuResult::Sent:         return L"sent";
        case StartMenuResult::InputBlocked: return L"input blocked";
        case StartMenuResult::NoInput:      return L"input refused";
        case StartMenuResult::SentThroughLegacy: return L"sent (keybd_event)";
        case StartMenuResult::ActivatedApp: return L"asked the shell to activate it";
    }
    return L"?";
}

const wchar_t* StartMenuRouteName(StartMenuRoute route) {
    switch (route) {
        case StartMenuRoute::SendInputCtrlEsc:  return L"SendInput Ctrl+Esc";
        case StartMenuRoute::SendInputHeld:     return L"SendInput Ctrl+Esc held";
        case StartMenuRoute::KeybdEventCtrlEsc: return L"keybd_event Ctrl+Esc";
        case StartMenuRoute::KeybdEventWin:     return L"keybd_event Win";
        case StartMenuRoute::ActivateStartApp:  return L"activate StartMenuExperienceHost";
    }
    return L"?";
}

void SetStartMenuRoute(StartMenuRoute route) { g_route = route; }
StartMenuRoute CurrentStartMenuRoute() { return g_route; }

StartMenuResult ToggleStartMenu() {
    // What the keyboard looked like when the chord was asked for.
    //
    // A modifier held down at this moment changes what Ctrl+Esc MEANS -- it becomes some other shortcut,
    // or the chord is swallowed -- and that is invisible from the outside: the keys are sent, SendInput
    // reports success, and nothing happens. The probe script established that Ctrl+Esc works from a
    // plain shell in this same session, so the difference to look for is the state of the keyboard while
    // a mouse button is being released.
    const bool ctrlAlready = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftAlready = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool altAlready = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool winAlready = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
    const bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    // Anything already held is released first.
    //
    // The chord only means "open Start" as a bare Ctrl+Esc. With Shift held it is a different shortcut,
    // and with the mouse button still down Windows can treat the whole thing as a gesture rather than a
    // keystroke. Clearing them makes the attempt start from a known keyboard, which is the one thing the
    // probe had and the click does not.
    if (shiftAlready) SendKey(VK_SHIFT, true);
    if (altAlready) SendKey(VK_MENU, true);
    if (winAlready) SendKey(VK_LWIN, true);
    if (ctrlAlready) SendKey(VK_CONTROL, true);

    LogLine(L"  start menu: modifiers before -- ctrl=%d shift=%d alt=%d win=%d lmb=%d",
            ctrlAlready ? 1 : 0, shiftAlready ? 1 : 0, altAlready ? 1 : 0, winAlready ? 1 : 0,
            lmbDown ? 1 : 0);

    // Ctrl+Esc, NOT the Windows key.
    //
    // The Windows key was the obvious choice -- the Start menu is what that key does -- and it does not
    // work here. Measured, not guessed: the Windows key was synthesized and the foreground window stayed
    // where it was, while Ctrl+Esc brought the Start menu's own host window
    // (XamlExplorerHostIslandWindow) to the foreground. Both are documented Start shortcuts; only the
    // second one acts on synthetic input on this build.
    //
    // Ctrl+Esc is also the safer of the two for a reason that is not obvious: the Windows key's meaning
    // depends on whether a modifier is held -- Win+E opens Explorer, and a Win press while the user is
    // holding Ctrl does nothing at all rather than opening Start. Ctrl+Esc has no such second meaning,
    // so the case that made the Windows key unreliable is not a case here.
    bool ctrlDown = false, escDown = false, escUp = false, ctrlUp = false;
    switch (g_route) {
        case StartMenuRoute::SendInputCtrlEsc:
            ctrlDown = SendKey(kCtrlKey, false);
            escDown = SendKey(kEscapeKey, false);
            escUp = SendKey(kEscapeKey, true);
            ctrlUp = SendKey(kCtrlKey, true);
            break;
        case StartMenuRoute::SendInputHeld:
            // The same call, with the press held: the one difference from the probe's working attempt.
            ctrlDown = SendKey(kCtrlKey, false);
            escDown = SendKey(kEscapeKey, false);
            Sleep(kHoldMs);
            escUp = SendKey(kEscapeKey, true);
            ctrlUp = SendKey(kCtrlKey, true);
            break;
        case StartMenuRoute::KeybdEventCtrlEsc:
            SendKeyLegacy(kCtrlKey, false);
            SendKeyLegacy(kEscapeKey, false);
            Sleep(kHoldMs);
            SendKeyLegacy(kEscapeKey, true);
            SendKeyLegacy(kCtrlKey, true);
            // keybd_event cannot report anything, so the route reports "sent" and the caller is told
            // which route it was: a silent failure of this call is not distinguishable from success,
            // and pretending otherwise is what made the first three rounds of this look identical.
            return StartMenuResult::SentThroughLegacy;
        case StartMenuRoute::KeybdEventWin:
            SendKeyLegacy(kWinKey, false);
            Sleep(kHoldMs);
            SendKeyLegacy(kWinKey, true);
            return StartMenuResult::SentThroughLegacy;
        case StartMenuRoute::ActivateStartApp: {
            // Already in front? Then this is the half that puts it away.
            //
            // The control is a toggle, and activating an application that is already the foreground does
            // not put it away -- it does nothing. The Start menu's host window names itself, so the state
            // can be read instead of guessed, and Escape is what the key it replaces would do.
            const HWND foreground = GetForegroundWindow();
            wchar_t className[64] = {};
            if (foreground && GetClassNameW(foreground, className, 64) > 0 &&
                wcscmp(className, L"XamlExplorerHostIslandWindow") == 0) {
                SendKeyLegacy(kEscapeKey, false);
                Sleep(kHoldMs);
                SendKeyLegacy(kEscapeKey, true);
                LogLine(L"  start menu: it was in front, sent Escape");
                return StartMenuResult::SentThroughLegacy;
            }

            // `explorer.exe shell:AppsFolder\<AUMID>`, spelled as a ShellExecute so it goes through the
            // shell: the Start menu is a XAML application (StartMenuExperienceHost) hosted by Explorer,
            // and this is the documented way to ask for it. Result above 32 is success, the same
            // convention the panel's tiles use.
            static const wchar_t kStartApp[] =
                L"shell:AppsFolder\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy!App";
            const HINSTANCE opened =
                ShellExecuteW(nullptr, nullptr, kStartApp, nullptr, nullptr, SW_SHOWNORMAL);
            const INT_PTR code = reinterpret_cast<INT_PTR>(opened);
            LogLine(L"  start menu: activating the Start app -> %lld", static_cast<long long>(code));
            return (code > 32) ? StartMenuResult::ActivatedApp : StartMenuResult::InputBlocked;
        }
    }

    if (!ctrlDown && !escDown && !escUp && !ctrlUp) {
        // Nothing at all went in. UIPI refuses synthetic input from a lower integrity level, and a
        // session without an interactive desktop refuses everything.
        const DWORD err = GetLastError();
        LogLine(L"  start menu: SendInput refused every key (error %lu)", err);
        return StartMenuResult::InputBlocked;
    }
    if (!escDown || !escUp) {
        // The Escape half did not go, which for this chord means nothing was asked for. Reported
        // separately from a refusal because the causes differ: a refusal is a permission problem, while
        // a partial send is the queue dropping events under load.
        LogLine(L"  start menu: the Escape half was not delivered (down=%d up=%d)", escDown ? 1 : 0,
                escUp ? 1 : 0);
    }

    // The modifiers are released whatever happened above.
    //
    // A Control left logically down is worse than a menu that did not open: every subsequent keypress
    // the user makes becomes a shortcut. That is a defect that outlives the click that caused it.
    if (!ctrlUp) SendKey(kCtrlKey, true);
    if (!escUp) SendKey(kEscapeKey, true);

    if (!ctrlDown || !ctrlUp || !escDown || !escUp) {
        return StartMenuResult::NoInput;
    }

    LogLine(L"  start menu: Ctrl+Esc was sent");
    return StartMenuResult::Sent;
}

bool SendWindowsKeyChord(wchar_t letter) {
    if (letter == L'\0') return false;

    // Whatever is held is released first, so the chord means what it says. The same reason
    // ToggleStartMenu clears them: Win+E and a Win press with Control held are different shortcuts, and
    // neither is the one being asked for here.
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) SendKey(VK_SHIFT, true);
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) SendKey(VK_MENU, true);
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) SendKey(VK_CONTROL, true);

    const WORD key = static_cast<WORD>(VkKeyScanW(letter) & 0xFF);
    if (key == 0 || key == 0xFF) return false;

    // keybd_event, not SendInput, and with the key held briefly in between.
    //
    // Measured on this machine when the Start button was fixed: SendInput reported success for every
    // event and the shell ignored the chord, while the older call -- the one the probe used -- acted on
    // it. The same measurement applies here, and a chord that is reported as sent but does nothing is
    // the failure mode this project has already spent rounds on.
    SendKeyLegacy(VK_LWIN, false);
    SendKeyLegacy(key, false);
    Sleep(40);
    SendKeyLegacy(key, true);
    SendKeyLegacy(VK_LWIN, true);

    // Reported optimistically-but-honestly: keybd_event cannot say whether the shell accepted the
    // chord, so what is claimed here is only that the events were put into the queue. Whether the shell
    // acted on them is what the caller's log line is for.
    LogLine(L"  win+%c: chord sent (keybd_event)", letter);
    return true;
}

} // namespace island
