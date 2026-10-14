// =====================================================================
//  TrayLearn.h
//  Learning every application's tray callback, automatically, by being inside it when it registers.
//
//  WHY THIS EXISTS. A press is invisible only when it is the application's own callback posted to its own
//  tray window, and that needs the window, the message number and the uid. The message number cannot be
//  looked up (the shell's registry entry has uid, path, tooltip and picture, and no callback message) and it
//  cannot be observed from outside (the shell delivers it without a queue anyone else can read), so it used
//  to be learned by hand: a Win32k trace, an administrator prompt, and a third-party program clicking the
//  icon as the reference (HANDOFF 8.104). The user's verdict on that was correct: one application at a time
//  is not a mechanism.
//
//  What is a mechanism is being present when the application calls Shell_NotifyIcon: the structure it passes
//  carries the window, the uid, the callback message and the version. src/trayhook/TrayHook.cpp is that
//  presence -- a small module loaded into each running tray application with a thread hook -- and this file
//  is the driver that loads it, asks every application to register again (the standard TaskbarCreated
//  broadcast), reads what came back and merges it into the cache TrayPress already reads:
//
//      %USERPROFILE%\IslandBar\tray_callbacks.txt
//
//  AND THE MODULE STAYS BEHIND. It pins itself in each application it enters (see TrayHook.cpp) and its
//  import slot stays redirected, so the report keeps growing with every registration for the rest of the
//  session -- an application restarted from its own menu, an icon that only appears while a game runs. That
//  is what `HarvestLearnedCallbacks` is for: the bar calls it on its tray-refresh timer and picks those up
//  without installing anything again. The installing pass is only needed for applications this bar has not
//  reached yet, which is what `UncoveredApplications` reports.
//
//  WHAT IT CANNOT REACH, measured on this machine rather than assumed (HANDOFF 8.107): applications that
//  resolve Shell_NotifyIcon themselves and call through their own pointer (WeChat's Qt tray: 0 import
//  slots), applications that refuse an injected module outright (QQ Music), elevated ones (Task Manager is
//  High integrity) and packaged ones (ChatGPT is an AppContainer, where SetWindowsHookEx is refused with
//  error 87). Those keep whatever they had -- a hand-learned line, or the real click.
// =====================================================================
#pragma once

#include "shell/TrayIcons.h"

namespace island {

struct TrayLearnResult {
    int icons = 0;         // icons on the card at the time of the run
    int appsTried = 0;     // running tray applications the module was offered to
    int appsReached = 0;   // ... in which the module actually loaded
    // ... plus the ones that were ALREADY holding it, which is the normal case after the first session: the
    // module pins itself where it lands and stays for the life of that process (see TrayHook.cpp), so a pass
    // that loads into nobody can still be a machine where every application is watched. Without this number
    // the log reads "the module loaded in 0" on a machine that is fully covered.
    int appsAlreadyWatched = 0;
    int captured = 0;      // registrations seen (NIM_ADD / NIM_MODIFY), duplicates included
    int newEntries = 0;    // lines added to the callback cache
    int covered = 0;       // icons of the card that have an entry afterwards
    long long ms = 0;      // how long the whole thing took
};

// Learns what every running tray application's callback is, and writes the cache.
//
// `broadcast` sends TaskbarCreated first, which is what makes applications register again: without it, one
// that registered hours ago has no reason to call Shell_NotifyIcon while this runs. Blocking, for a worker
// thread; it takes about three seconds.
TrayLearnResult LearnTrayCallbacks(const TrayIcons& icons, bool broadcast);

// Reads whatever the resident module has written since the last call and merges it into the cache.
// Returns how many NEW cache entries appeared (0 when nothing has registered since). Cheap: one small file
// read, no injection, no waiting -- safe to call from the frame loop's tray-refresh tick.
int HarvestLearnedCallbacks();

// The applications of the card's icons that still have NO callback entry, as one comma-joined, sorted
// string. The frame loop compares it with the string from its previous attempt and only tries again when it
// CHANGED: retrying the same four unreachable applications every few seconds would spend the whole session
// installing a module that they refuse for the same reason each time.
std::wstring UncoveredApplications(const TrayIcons& icons);

// How many of the card's icons have an entry in the callback cache. The number the startup line prints, and
// the one that says whether the automatic route is carrying its weight.
int CountCoveredIcons(const TrayIcons& icons);

} // namespace island
