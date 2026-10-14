// =====================================================================
//  Log.h
//  A file log, because the console is not a reliable place to put output.
//
//  island_bar.exe is a Windows-subsystem binary, so it has no console of its
//  own. It borrows the parent's with AttachConsole, and that has produced three
//  separate "the program printed nothing" reports in a row -- from cmd, from
//  PowerShell, and from a redirected run. Each time the exit code was correct and
//  the text was simply gone, which is the worst combination: the tool looks
//  broken and gives no clue why.
//
//  A log file removes the dependency on console attachment entirely, and it also
//  survives a crash, which is the case that matters most for a program that hides
//  the user's taskbar.
//
//  The file lives beside the user's other application data rather than in the
//  repository, so running the bar never dirties the working tree.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>

namespace island {

// Where the log lives. Filled in on first use; never null afterwards.
const wchar_t* LogPath();

// Opens the log for this run, truncating any previous one. Safe to call more
// than once; the second call is a no-op.
void OpenLog();

// Closes the log.
void CloseLog();

// Writes one line, prefixed with a timestamp, and flushes immediately: a program
// that hides the taskbar must not lose the record of what it did if it is killed.
void LogLine(const wchar_t* format, ...);

// Writes one line to the log AND to stdout. Use for text a person asked for
// (the probe report, the self-test) so the file always has it even when the
// console does not show it.
void LogAndPrint(const wchar_t* format, ...);

// The same, for call sites whose text is plain ASCII. Kept as an overload rather
// than an extra name because diagnostic text is nearly all ASCII and having to
// pick between two spellings at every call site is noise.
//
// THE RULE, learned the hard way: "plain ASCII" means the FORMAT **and every argument that
// reaches it**, including anything passed to `%ls`. This overload widens byte by byte, so a wide
// argument holding a non-ASCII character is an invalid conversion -- and the CRT's response to
// that is to empty the whole line while still writing the newline. The result is a blank line in
// the log where the answer should have been, which reads exactly like "nothing happened". Three
// rounds were spent on a tile that looked inert while its own log line was being swallowed this
// way. If any argument can be Chinese, a device name, or a window title, use the wide overload.
void LogAndPrint(const char* format, ...);

// Whether stdout can be written to at all.
//
// This matters more than it looks. island_bar.exe is a Windows-subsystem binary,
// and when it is launched without a console the C runtime's stdout has an invalid
// file descriptor (-1). Writing to it does not fail politely: fputws and fputwc
// trip a debug assertion in lowio\isatty.cpp ("fh >= 0 && (unsigned)fh <
// (unsigned)_nhandle") and the process dies with a dialog box. That is the real
// reason several runs printed nothing.
//
// Every console write must go through this check. The log file is unaffected: it
// is always opened explicitly and never depends on stdout.
bool StdoutIsUsable();

// Writes one line to the console, but only if there is a console. Used internally
// by the LogAndPrint overloads and by any other code that wants to be safe.
void PrintLine(const wchar_t* text);

// The same for ASCII text.
void PrintLine(const char* text);

} // namespace island
