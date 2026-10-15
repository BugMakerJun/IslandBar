// =====================================================================
//  Cli.h -- the commands that run INSTEAD of the bar
//
//  WHAT THESE ARE. Every one of them is a question that cannot be answered by looking at the island: what did
//  the shell record about the tray, is there anything to send to that icon, did the media session arrive, can
//  the toast database be read at all, does the icon of this application decode. They exist because "nothing
//  happened" and "the thing was never there" look identical on a bar, and because a script cannot click a
//  cell on this machine (see AGENTS.md: anything whose image lives under the project tree runs at LOW
//  integrity, and UIPI then discards every message aimed at Explorer).
//
//  THEY ARE DIAGNOSTICS, SO THEY TOUCH EVERYTHING. A command that asks about the tray still has to stand up a
//  window and a renderer to answer -- that is not a layering mistake, it is what "run the bar and ask it one
//  question" means. What they must NOT do is grow into a second program: each one returns an exit code and
//  prints through core/Log.h, and the ones that matter to the workflow are named in ARCHITECTURE.md.
//
//  The argument PARSING stays in main.cpp: main knows the flags, these know the work.
// =====================================================================
#pragma once

namespace island {

// UTF-8 to wide, for a command-line argument that is already UTF-8 (see main's own use for --state).
// It is command-line plumbing, so it lives here rather than in a subsystem header.
bool ArgumentAsUtf8ToWide(const char* utf8, wchar_t* out, size_t capacity);

// --tray-dump: what the shell's own record holds, one line per icon.
int RunTrayDump();

// --tray-surface: per cell, whether its callback is known and whether its window is there -- the number
// README and HANDOFF quote as coverage.
int RunTraySurface();

// --tray-learn: offer the module to the shell and ask every application to register again.
int RunTrayLearn();

// --tray-press N [--fire] [--right]: press one cell exactly as a click on it would.
int RunTrayPress(int index, bool fire, bool rightClick);

// --icon-dump ID: decode one application's icon and report the PNG size, which is how "the icon is blank"
// gets told apart from "the icon was never found".
int RunIconDump(const char* appId);

// --window-list [--activate]: the windows the bar's own task list holds.
int RunWindowList(bool activate);

// --toast-dump [N]: the newest toasts out of the shell's database.
int RunToastDump(int limit);

// --volume-set N / --volume-test: the volume path, without a window on screen.
int RunVolumeSet(int percent);
int RunVolumeTest();

// --notify: push a notification through the pipe the bar reads.
int RunNotify(int argc, char** argv);

// --music-preview / --lyrics-test: the media card and the lyrics sources, one at a time.
int RunMusicPreview();
int RunLyricsTest(const char* title, const char* artist, bool verbose);

} // namespace island
