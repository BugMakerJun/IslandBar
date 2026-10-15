// =====================================================================
//  TrayMenu.h -- the tray card's behaviour: what a cell does, what its menu offers
//
//  WHAT IS IN HERE. Nine functions, and every one of them was in main.cpp's anonymous namespace until
//  2026-10-15. They are the whole of "a tray cell": turning an icon into a press request, flattening a
//  tooltip for a log line, comparing an executable's name with a window's process, finding the window behind
//  an icon, deciding which menu rows are possible, carrying one out, and starting the press on a worker
//  thread.
//
//  WHY IT IS ITS OWN HEADER. main.cpp is where the program is assembled; a reader looking for the frame loop
//  had to walk past 1,300 lines of command-line handlers and 230 lines of tray rules. The tray's rules have
//  two callers -- the frame loop (a click on a cell) and the --tray-* commands (a press from a script) -- and
//  neither of them is main.cpp's business.
//
//  THE ONE RULE WORTH KNOWING BEFORE READING THE CODE: an entry is offered only if it can actually do
//  something. `TrayMenuFacts::hasCallback` is what keeps 打开 and 应用自己的菜单… out of the menu for an
//  icon whose callback was never learned -- those two entries ARE the application's own action, and there is
//  no message to send without a learned callback. A row that silently does nothing is worse than no row.
// =====================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>

#include "domain/CardLayout.h"   // TrayMenuFacts, TrayMenuAction
#include "platform/WindowList.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"

namespace island {

// The cell a press could not deal with, left for the frame loop to open the bar's own menu over.
//
// IT LIVES HERE, not in main.cpp, and that is the point: a press runs on a worker thread, and the thread that
// owns the window is the one that may open a menu. The worker therefore records WHICH cell it could not press,
// and the next frame reads it. That hand-over is part of the tray's behaviour, so the variable that carries it
// belongs to the tray -- main.cpp used to own it only because the code that wrote it lived there.
extern std::atomic<int> trayMenuRequestCell;

// One icon's press, copied out of the list: the press runs on a worker thread and the list refreshes under it.
TrayPressRequest MakeTrayPressRequest(const TrayIcon& icon, const wchar_t* exePath);

// An icon's name with its line breaks flattened, for a log line. Five of the names on this machine are
// multi-line tooltips, and printing one raw puts the rest of the log line on a line of its own -- which reads
// exactly like a missing answer.
const wchar_t* FlatName(const wchar_t* name, wchar_t* buffer, size_t capacity);

// The bare executable name out of a path ("Weixin.exe" from a full path).
const wchar_t* BaseNameOf(const wchar_t* path);

// Whether two bare names are the same image (case-insensitive: the registry records what the application
// wrote, and Windows does not care about the case).
bool SameImage(const wchar_t* a, const wchar_t* b);

// Ends every process of that image and returns how many were ended. The forceful path, offered only behind a
// second press (see TrayMenuArmedLabel): a tray application's window closes INTO the tray rather than
// quitting, so "ask it to close" cannot deliver "the application is gone".
int TerminateProcessesOfImage(const wchar_t* image);

// The first window of that image in the bar's own task list -- the same list the icon row draws, so 显示窗口
// acts on the window whose icon the user would have clicked.
HWND WindowOfImageInList(WindowList& windows, const wchar_t* image);

// What the icon can actually do, so that the menu never offers a row that does nothing.
void TrayMenuFactsFor(const TrayIcons& icons, WindowList& windows, int cell, TrayMenuFacts* out);

// Carries out one entry of that menu.
void RunTrayMenuAction(const TrayIcons& icons, WindowList& windows, int cell, TrayMenuAction action);

// Starts a press on a worker thread (it sends a message and then watches the application for an answer, which
// the bar's own frame loop has no time for).
void StartTrayPress(const TrayIcons& icons, int cell, bool rightClick);

} // namespace island
