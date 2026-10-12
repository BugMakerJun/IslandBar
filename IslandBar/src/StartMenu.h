// =====================================================================
//  StartMenu.h
//  Opens the Start menu.
//
//  WHY THIS IS NOT A ONE-LINE CALL
//
//  There is no supported API for it. Windows 11 hosts the Start menu in a XAML process, so the route
//  that worked on Windows 7 -- WM_SYSCOMMAND with SC_TASKLIST -- does nothing at all, and the "Start"
//  child window of Shell_TrayWnd that older code looks for does not exist on this build either (probed
//  and confirmed absent). Third-party bars open it the way the keyboard does: by synthesizing the
//  Windows key, because the Start menu IS that key's action.
//
//  That is what this does, and it is written so the OUTCOME can be told apart from the ATTEMPT. Sending
//  the keystroke can fail for reasons that are invisible afterwards -- the input was blocked, the
//  session was not interactive, another key was held down and turned it into a shortcut -- and "the
//  button did nothing" is the same symptom for all of them. Every call therefore reports whether the
//  input was actually delivered.
// =====================================================================
#pragma once

namespace island {

// What happened when the Start menu was asked for.
enum class StartMenuResult {
    Sent,          // the keystroke was delivered to the input queue
    InputBlocked,  // SendInput refused it (UIPI, or no interactive session)
    NoInput,       // SendInput returned a short count: part of the sequence did not go

    // keybd_event cannot say whether the input was accepted, so this is not the same claim as `Sent`:
    // it means "the events went in through the older call", which is all that call can report.
    SentThroughLegacy,

    // The shell was asked to activate the application the Start menu IS, and accepted the request.
    ActivatedApp,
};

const wchar_t* StartMenuResultName(StartMenuResult result);

// How the chord is put into the input queue. Reported as a name in the log, because "the key was sent
// and nothing happened" has now been measured to depend on WHICH call sent it.
enum class StartMenuRoute {
    SendInputCtrlEsc,   // SendInput, the four events back to back -- the original
    SendInputHeld,      // SendInput, with the documented-feeling 40 ms between Escape down and up
    KeybdEventCtrlEsc,  // keybd_event, the older call, held 40 ms -- what the probe script used
    KeybdEventWin,      // keybd_event with the Windows key, which is what the key itself does

    // Ask the shell to activate StartMenuExperienceHost -- the process the Start menu IS.
    //
    // Added because the four keyboard routes above were measured NOT to work on this build: from a
    // medium-integrity process, with every modifier clear, all four reported "sent" and the foreground
    // window never left the browser. The header's own first paragraph had the fallback written down
    // ("look at shell:AppsFolder or activating the Start AppX") -- see HANDOFF 7.84.
    ActivateStartApp,
};

const wchar_t* StartMenuRouteName(StartMenuRoute route);
void SetStartMenuRoute(StartMenuRoute route);
StartMenuRoute CurrentStartMenuRoute();

// Asks the shell to open (or close) the Start menu.
//
// A toggle, like the key itself: called while the menu is open it closes it. On the keyboard routes that
// is the shell's business -- the keystroke is the same either way. The AppX route has to ask, because
// activating an application that is already in front does not put it away: it looks at the foreground
// window (the Start menu's own host class) and sends Escape in that case.
StartMenuResult ToggleStartMenu();

// Sends the Windows-key chord Win+<letter>, for the shell shortcuts that have no API and no page of
// their own -- Win+P (the projection-mode list) is the one the system panel's 投影 tile asks for.
//
// A chord rather than a scan code because the shell's own shortcuts are what is being asked for, and the
// shell only sees them as chords. The modifiers are cleared first, for the reason ToggleStartMenu clears
// them: a key already held turns the chord into a different shortcut, silently. Reported with the same
// caveat as the Start button -- synthetic input is dropped by UIPI when this process sits below the shell.
bool SendWindowsKeyChord(wchar_t letter);

} // namespace island
