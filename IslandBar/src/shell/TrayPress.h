// =====================================================================
//  TrayPress.h
//  Pressing one of the shell's tray icons for the user, from the bar's tray card.
//
//  The icons in the card are DRAWN from the registry (see TrayIcons). PRESSING one is the shell's
//  business: the menu or window it opens belongs to the application, and no amount of Windows API on
//  our side reproduces it -- the shell has to be the one that presses it.
//
//  WHY BY POSITION AND NOT BY NAME, which is the whole shape of this file.
//
//  The shell publishes its tray through UI Automation: the taskbar's own buttons, and -- once the
//  overflow flyout is up -- one `NotifyItemIcon` button per application icon, each with an Invoke
//  pattern. Those buttons are named after the APPLICATION, at runtime: " 微信", "QQ音乐 听我想听
//  Girls Just Want To Have Fun - ...", "Clash Verge 2.5.8 系统代理: on ...". The bar's names come from
//  the registry's `InitialTooltip`, which is what the application said when it first registered -- and
//  is very often empty, so the bar falls back to the executable's name: "Weixin.exe". The two lists
//  have six names in common out of sixteen, and the one the user will try first is not among them.
//
//  Matching them by picture was tried as well and cannot work: a screen grab of the overflow flyout
//  comes back black, because that panel is drawn by the compositor (the taskbar itself photographs
//  fine, which is what made the idea look plausible). Measured, in scripts/probe-tray-match.ps1.
//
//  What the shell CAN answer, per icon, is where the icon is:
//
//      Shell_NotifyIconGetRect(NOTIFYICONIDENTIFIER)
//
//  and its identifier is exactly what the registry entry carries. Measured on this machine: the
//  registry's `IconGuid` answers with the icon's rectangle (40x40 at the flyout's cell for that very
//  application -- ChatGPT.exe -> 2368,49, which is the flyout button named "ChatGPT"), and it answers
//  even while the taskbar is HIDDEN or made invisible. So the identity is the registry's, the position
//  is the shell's, and the press is a UI Automation Invoke on the button that sits on that position --
//  no names involved at all.
//
//  ... AND THE PRESS ITSELF IS NO LONGER A UI AUTOMATION INVOKE, which took a year's worth of rounds to
//  establish and one wasted session to see clearly (2026-10-15). The rectangle is still the shell's and the
//  button on it is still looked for -- that is how "there is really an icon here" is checked, and how the
//  shell's own name for it gets into the log -- but the BUTTON IS PRESSED WITH A REAL CLICK: the pointer is
//  moved onto it, the button goes down and up, and the pointer goes back where the user's hand was. Two
//  measurements forced that: the shell's tray buttons do not reliably carry an Invoke pattern at all, and
//  the application's response to a tray icon is driven by the shell forwarding MOUSE BEHAVIOUR, so an
//  Invoke that returns S_OK can leave the application completely untouched -- which is exactly what the
//  three presses of 2026-10-15 did, once the log was fixed enough to see them (see below). Every real-world
//  tool that does this by program (AutoHotkey's tray scripts, TrayStatus, TranslucentTB) injects input for
//  the same reason.
//
//  THE LOG LINE THAT REPORTED ALL OF THIS WAS ITSELF THE BUG FOR ONE ROUND. It was written with a NARROW
//  format string and a `%ls` for the shell's name of the icon, and that name is " 微信" on this machine: the
//  narrow CRT cannot convert a non-ASCII wide string to bytes, and MSVC's answer is to empty the WHOLE line
//  while still writing the newline. Every press therefore reported itself as a blank line, which reads
//  exactly like a press that never returned -- and the next session drew the natural but wrong conclusion
//  that the Invoke had hung. The line is wide now, and the facts it carries live in `TrayPressOutcome`.
//
//  ... AND ONLY WHEN THE SHELL HAS LAID THE TRAY OUT, which is the trap in the paragraph above. An icon
//  that lives in the overflow flyout has NO rectangle until that flyout has been opened: until then the
//  shell answers 2416,0 24x32 -- the chevron's slot -- for EVERY icon, hiding its ignorance behind a
//  plausible rectangle. Measured: all sixteen, identical, with the taskbar hidden for a while.
//
//  That is why the first version of this worked from the command line and not from the bar: every
//  command-line run followed a probe that had just opened the flyout to look inside it, so the rectangles
//  happened to be current. A measurement taken after somebody else has already done the thing being
//  measured is not a measurement. Hence TrayStage: the flyout is opened FIRST, invisibly, and only then is
//  the icon asked for -- see scripts/probe-tray-rect-after-flyout.ps1, which asks the same question both
//  ways.
//
//  THE TASKBAR IS NEVER SEEN. The shell only publishes its tray while the taskbar is VISIBLE, and this
//  bar exists to hide it, so the press shows it for the length of the operation with
//  `WS_EX_LAYERED + SetLayeredWindowAttributes(alpha 0)`: present and fully functional for UI
//  Automation, painted with nothing. Measured with scripts/probe-tray-hidden-invoke.ps1: six buttons
//  found in that state and the chevron pressed in 37 ms, with the band photographing as wallpaper.
//  The overflow flyout is made invisible the same way as soon as it appears, so the whole sequence --
//  taskbar, flyout, press -- happens off the user's screen and only the application's own window or
//  menu arrives.
//
//  WHAT IS NOT DONE HERE. An icon in the tray PROPER rather than the overflow is not a UI Automation
//  button at all (measured: the taskbar's own tree has the six system buttons and nothing else), so
//  there is nothing to Invoke; that case is reported rather than guessed at. On this machine every
//  application icon is in the overflow, so it has not come up.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>

namespace island {

// One press request: what the registry said about the icon, copied out of the list that produced it.
//
// A COPY, and that is not tidiness: the press runs on a worker thread while the tray list refreshes
// underneath it every five seconds, and the list's own storage (the PNG bytes, the strings) does not
// survive that.
struct TrayPressRequest {
    unsigned uid = 0;
    bool hasUid = false;
    bool hasGuid = false;
    GUID guid = {};
    wchar_t exePath[260] = {};
    wchar_t name[96] = {};
};

enum class TrayPressResult {
    Pressed,      // the shell took the press
    Found,        // the button is there and sits on the shell's rectangle; not pressed (a dry run)
    NotInShell,   // the shell has no rectangle for it: registered, but not on screen
    NoWindow,     // a UID icon whose owning window could not be found (see TrayPress.cpp)
    NoElement,    // the rectangle is known and no UI Automation button sits on it
    NoTaskbar,    // Shell_TrayWnd is not there at all
    Refused,      // UI Automation refused the Invoke
};

struct TrayPressOutcome {
    TrayPressResult result = TrayPressResult::Refused;
    RECT rect = {};                // where the shell says the icon is, when it answered
    wchar_t shellName[128] = {};   // what the SHELL calls it, when a button was found on that rectangle
    long long lookupMs = 0;        // the rect query, which needs no taskbar at all
    long long pressMs = 0;         // showing the taskbar invisibly, opening the flyout, pressing

    // ---- everything below is here because of ONE wasted round (2026-10-15). ------------------------
    //
    // The single line that was supposed to report all of the above was written with a NARROW format
    // string and a `%ls` for the shell's name, and the shell's name on this machine is " 微信": the
    // narrow CRT cannot convert that to bytes, so MSVC's answer is to empty the WHOLE line while still
    // writing the newline. Every press reported itself as a blank line, which reads exactly like a press
    // that never finished -- and the next session concluded that the Invoke had hung. The format string
    // is wide now (see main.cpp) and the facts live in the outcome instead of in a printf, so that a
    // future call site cannot lose them the same way.
    long long clickMs = 0;         // when the click left this process, from the start of the press
    // WHICH ROUTE RAN. There are two again (2026-10-15 late): "a posted callback" -- the application's own
    // tray message, sent straight to its window, which is what makes the press invisible -- and "a real
    // click", the pointer-moving route below, which is now the fallback. The field was deleted when there
    // was only one route; a switch that can hold one value is dead weight, two are not.
    const wchar_t* route = L"?";
    bool pointerMoved = false;     // whether the press had to move the user's pointer (a real click does)
    wchar_t target[160] = {};      // what was under the pointer: "class 'X' of pid N", and whose it is
    bool targetIsOurs = false;     // ... and whether that window is this bar's own
    bool madeClickThrough = false; // whether this bar had to be taken out of the hit test for the click
    long long panelShownMs = 0;    // when the overflow panel was made hit-testable for the click
    long long panelHiddenMs = 0;   // ... and when it was made transparent again (0 = never, or not needed)
    RECT menuRect = {};            // the application's menu, when one was seen (the right-click route)
    wchar_t menuClass[160] = {};   // what came up, in words (class, rectangle, whose window it is)
    long long menuWaitMs = -1;     // ms after the click it was seen; -1 = nothing appeared in the budget
    bool appAnswered = false;      // the application did something observable within 200 ms of the click
    wchar_t answer[192] = {};      // ... described: a new window, a new foreground window, or nothing
};

// Presses the icon `request` names. Safe to call from a worker thread that has called
// CoInitializeEx(COINIT_MULTITHREADED); needs no UI thread of its own.
//
// With `press` false the whole sequence runs -- the taskbar made invisible, the overflow flyout opened,
// the button found on the shell's rectangle -- and only the final Invoke is left out, which is what makes
// the route checkable without opening somebody's application. The result is then `Found`.
//
// `rightClick` asks for the application's own menu instead of its default action, which is what a right
// click means in the shell's tray -- and it is the only route to a menu item like "退出": UI Automation
// publishes no right-click pattern, so that half is a real click delivered at the icon (see TrayStage).
//
// ONE SWITCH, for a measurement that would otherwise need a rebuild (two others lived here until their
// questions were answered -- see the note where the switch is read):
//
//   ISLANDBAR_TRAY_PRESS_VISIBLE=1    the whole press runs with the shell's taskbar and overflow panel
//                                     actually on screen. Everything here exists to keep them off it, and
//                                     that is also what hides the answer to "does this press work at all"
//                                     behind "does it work while invisible": one switch, one experiment.
TrayPressOutcome PressTrayIcon(const TrayPressRequest& request, bool press, bool rightClick);

// Whether a press is in flight right now.
//
// The press needs the shell's taskbar to EXIST for about a fifth of a second, and this bar re-hides that
// taskbar on a five-second timer -- so one press in twenty-five would be interrupted by the bar's own
// watchdog and produce nothing, with no error anywhere. The frame loop asks this before re-hiding. A
// process-wide flag rather than a handshake, because the two are in different threads and the window is
// this short.
bool TrayPressInFlight();

// The cleanup a right click leaves behind, finished by the bar's own watchdog.
//
// A right click leaves the shell's panel open on purpose (its menu is attached to it -- see TrayPress.cpp),
// and the shell re-shows its taskbar while that menu is up. So the taskbar is left SHOWN BUT INVISIBLE for
// as long as that lasts, and this is how it gets put back: the frame loop asks this once per watchdog tick,
// and only then is the layering removed and the taskbar hidden again. Doing it at the end of the press
// instead was measured: the shell brought the taskbar back, visible, seconds after the menu opened.
bool TrayPressCleanupPending();
// Returns true when the TASKBAR was put away (the panel's own restore is separate and may need several
// rounds: it waits for the shell to close the panel the application's menu is attached to).
bool TrayPressFinishCleanup();

// THE SHELL'S TRAY, LAID OUT AND OPEN, INVISIBLY -- everything a press needs that is not the icon itself.
//
// RAII, because every path out has to put the desktop back: the taskbar hidden again, its extended style
// as it was, and the flyout gone with it. A press that returned early without this would leave the shell's
// taskbar over the bar, which is the one thing this project must never do.
//
// `Begin` shows the taskbar painted with nothing (`WS_EX_LAYERED` + alpha 0 -- present and fully functional
// for UI Automation, as measured), opens the overflow flyout (also made invisible the moment it appears,
// on a 5 ms poll) and waits for the panel to stop moving. `IconRect` may only be asked AFTER that: see the
// note at the top of this file about the chevron's slot.
//
// THE PANEL IS NEVER MOVED, and that is a correction rather than a description: this class used to put it
// back where it found it at the end of every press, through a field that was never assigned -- so "where it
// found it" was 0,0 and the shell's panel was teleported to the top-left corner of the screen on the way
// out, taking the application's own menu (which the shell anchors to that panel) with it. Measured
// 2026-10-15 from a user's screenshot and the log timestamps either side of the teleport.
class TrayStage {
public:
    TrayStage() = default;
    ~TrayStage();
    TrayStage(const TrayStage&) = delete;
    TrayStage& operator=(const TrayStage&) = delete;

    // False when the shell will not cooperate; `note` then says which part was missing.
    bool Begin(wchar_t* note, size_t noteCapacity);

    // The icon's rectangle as the shell reports it NOW. False means "registered but not displayed", which
    // is also what tells the card which of its cells has no shell icon behind it.
    bool IconRect(const TrayPressRequest& request, RECT* out) const;

    RECT FlyoutRect() const { return flyoutRect_; }

    // The panel made hit-testable for the length of one click, and transparent again afterwards.
    //
    // The icon being pressed lives in that panel, and a click is routed by position: the panel is the
    // window that has to receive it. Windows does not hit-test a layered window at alpha 0, which is the
    // whole reason these two calls exist -- and the alpha it comes back at is 1, so that the shell's icon
    // grid is never painted on the way past (see ShowPanelForClick). The panel is at alpha 0 for the rest
    // of the press, and these are the only two places that change it.
    void ShowPanelForClick();
    void HidePanelAfterClick();
    bool PanelStillShowing() const;

    // Presses the button that sits on `rect`, inside the flyout. With `press` false it is found and not
    // pressed (the dry run). `rightClick` asks for the application's own menu instead of its default
    // action -- a real click, because UI Automation has no pattern for one. `outcome` is filled in with
    // what happened, for the caller's log line. `request` is what the application is, which the press needs
    // to tell "it opened a window" apart from "the click landed on nothing".
    TrayPressResult PressOnRect(const TrayPressRequest& request, const RECT& rect, bool press,
                                bool rightClick, wchar_t* shellName, size_t shellNameCapacity,
                                TrayPressOutcome* outcome);

private:
    void Reset();

    // Milliseconds since Begin returned true, for the phase-by-phase log line of a press.
    long long SinceBegin() const;

    // One press delivered as a real click, both buttons. See the note above it in the .cpp: this is the
    // sequence the panel reveal, the hit-test check, the delivery, the wait for an answer and the panel's
    // removal all live in.
    TrayPressResult ClickOnRect(const TrayPressRequest& request, const RECT& rect, bool rightClick,
                                TrayPressOutcome* outcome);

    HWND taskbar_ = nullptr;
    bool wasVisible_ = false;
    bool layered_ = false;
    LONG_PTR style_ = 0;
    HWND flyoutWindow_ = nullptr;
    // The overflow panel is made invisible ONCE IT HAS SETTLED (see Begin), and these are what is needed to
    // put it back: its extended style, and whether this stage added the layered bit itself.
    LONG_PTR flyoutStyle_ = 0;
    bool flyoutLayered_ = false;
    // IUIAutomation / IUIAutomationElement, kept as void* so this header does not have to drag in the
    // UI Automation and COM headers for every file that asks what a press result is.
    void* automation_ = nullptr;
    void* chevron_ = nullptr;
    void* flyoutElement_ = nullptr;
    // Whether Begin had to initialize COM itself, and therefore has to put it back.
    bool comInitialized_ = false;
    // Whether the overflow panel is up because THIS stage opened it: it is a toggle, and pressing it again
    // is the only way to put it away -- hiding the taskbar does NOT close it (measured: the panel stayed on
    // the screen, still painted, after the taskbar was hidden and even after the layering was removed).
    bool weOpenedFlyout_ = false;
    // Whether the press was a right click, which is the one kind whose result (a menu) needs a moment
    // before the panel it came from is taken down. See Reset.
    bool rightClicked_ = false;
    // Whether this press is being run with the shell's tray really on screen (ISLANDBAR_TRAY_PRESS_VISIBLE).
    // Read once in Begin; nothing is hidden or revealed anywhere when it is set.
    bool visibleRun_ = false;
    // When Begin got as far as having the panel up, so every later log line can say "N ms in".
    ULONGLONG begunAt_ = 0;
    RECT flyoutRect_ = {};
};

// Which of `count` rectangles is the one the shell named, by exact position: the same rectangle within
// `tolerancePx` on every edge. This is the identity test that replaces matching names, and it is a plain
// function so the self-test can exercise it (see Diagnostics).
int IndexOfRectNear(const RECT* rects, int count, const RECT& wanted, int tolerancePx);

const wchar_t* TrayPressResultName(TrayPressResult result);

} // namespace island
