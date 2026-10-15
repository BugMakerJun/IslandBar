// =====================================================================
//  BarState.h -- the bar's own state, in one place instead of scattered through the frame loop
//
//  WHY THIS EXISTS. Until 2026-10-15 every one of these values was a local at the top of RunBar -- a function
//  about 5,000 lines long. Nothing said which locals were STATE (they survive from frame to frame and belong
//  to the bar as a whole) and which were merely computed inside one frame; a reader had to infer it from how
//  far apart the uses were, and a writer could not see the whole picture before changing one of them.
//
//  THIS IS SLICE 1 OF THAT MIGRATION, NOT THE FINISHED THING. The tray card and its menu are here because
//  they are the area with the sharpest edges -- a menu that must not offer a row it cannot carry out, an armed
//  "end the application" entry, a learning pass that must not start twice -- and because one piece of that
//  state had already turned out to be owned by the wrong module (trayMenuRequestCell, see shell/TrayMenu.h).
//  The rest follows one named group at a time, each moved with a full rebuild and the self-test, so that a
//  mistake is always attributable to the group that just moved.
//
//  WHAT BELONGS HERE: state that outlives a frame and that more than one phase of the loop reads. What does
//  not: values computed inside a single frame (a rect, a flag that means "this frame drew something"), and
//  objects that own resources (the window, the renderer, the media session) -- those get their own named
//  group when their turn comes, because building them is a sequence rather than a set of fields.
// =====================================================================
#pragma once

#include <atomic>
#include <memory>

#include <windows.h>

#include "domain/CardLayout.h"   // TrayMenuAction, kMaxTrayMenuEntries

namespace island {

struct BarState {
    // --- the system panel's card, and the tray's level of it --------------------------------
    //
    // The tray page and the panel's tiles are two levels of ONE card, which is why their flags are read
    // together by the click handler and by the draw.
    bool trayPageOpen = false;

    // --- THE TRAY'S OWN MENU (2026-10-14, HANDOFF 8.80) -------------------------------------
    //
    // Which cell it belongs to and what its rows say. The GEOMETRY lives in the window -- it owns the card and
    // answers the hover test -- and this is the MEANING: the same split as every other control on this bar.
    int trayMenuCell = -1;
    TrayMenuAction trayMenuActions[kMaxTrayMenuEntries] = {};
    int trayMenuActionCount = 0;

    // Whether 结束应用 has been asked for once and is waiting for the press that carries it out. Cleared
    // whenever the menu opens or closes: an armed entry must not survive the menu it was armed in.
    bool trayQuitArmed = false;

    // --- re-reading the shell's tray, and learning what it holds -----------------------------
    //
    // The tray is re-read on its own slow cadence: it touches the registry and a process snapshot, and what it
    // describes -- which applications are running -- changes on that scale.
    unsigned long long lastTrayRefresh = 0;

    // -1 rather than a count: the first refresh in the loop then reports the tray's size once, at startup,
    // which is the line to look for when the tray is empty.
    int lastTrayCount = -1;

    // Which process the tray's registrations are read in (see TrayShellProcessId). Watched, because the module
    // is pinned for the life of the process it lands in: an Explorer that restarts has to be given it again.
    // Nothing else needs a pass any more, which is the point of reading registrations in the shell.
    //
    // Seeded with the shell that exists right now, so the startup pass is not immediately repeated by the
    // first tick (measured: it was, three times over, on the first run of this code).
    DWORD lastShellPid = 0;

    // Shared with the worker and on the heap: the worker is detached, and a detached thread writing to a local
    // of a function that has already returned is a crash waiting for the user to close the bar.
    std::shared_ptr<std::atomic<bool>> learnInFlight = std::make_shared<std::atomic<bool>>(false);
    unsigned long long lastLearnAttempt = 0;
};

} // namespace island
