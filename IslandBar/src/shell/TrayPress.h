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
// publishes no right-click pattern, so that half is a posted right button message (see TrayStage).
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

    // Presses the button that sits on `rect`, inside the flyout. With `press` false it is found and not
    // pressed (the dry run). `rightClick` asks for the application's own menu instead of its default
    // action -- a POSTED right button message, because UI Automation has no pattern for one.
    TrayPressResult PressOnRect(const RECT& rect, bool press, bool rightClick, wchar_t* shellName,
                               size_t shellNameCapacity);

private:
    void Reset();

    HWND taskbar_ = nullptr;
    bool wasVisible_ = false;
    bool layered_ = false;
    LONG_PTR style_ = 0;
    HWND flyoutWindow_ = nullptr;
    // The overflow panel is made invisible ONCE IT HAS SETTLED (see Begin), and these are what is needed to
    // put it back: its extended style, and whether this stage added the layered bit itself.
    LONG_PTR flyoutStyle_ = 0;
    bool flyoutLayered_ = false;
    // Where the shell had the panel before this stage moved it out of sight, so it can be put back.
    RECT flyoutHome_ = {};
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
    RECT flyoutRect_ = {};
};

// Which of `count` rectangles is the one the shell named, by exact position: the same rectangle within
// `tolerancePx` on every edge. This is the identity test that replaces matching names, and it is a plain
// function so the self-test can exercise it (see Diagnostics).
int IndexOfRectNear(const RECT* rects, int count, const RECT& wanted, int tolerancePx);

const wchar_t* TrayPressResultName(TrayPressResult result);

} // namespace island
