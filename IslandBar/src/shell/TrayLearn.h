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

// How many of the card's icons have an entry in the callback cache. The number the startup line prints, and
// the one that says whether the automatic route is carrying its weight.
int CountCoveredIcons(const TrayIcons& icons);

} // namespace island
