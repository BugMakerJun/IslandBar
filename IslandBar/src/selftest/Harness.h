// =====================================================================
//  Harness.h -- the lines a check can print, and the counter behind them
//
//  WHY IT IS ITS OWN FILE (2026-10-16): the self-test used to be one 8,968-line translation unit whose
//  helpers sat in an anonymous namespace, so the checks could not be split into files whose subjects can
//  be read one at a time. The helpers moved here and the checks moved to Checks*.cpp; this is what both
//  halves agree on.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

// SetTimer installs this id; posting WM_TIMER for it runs the pointer poll
// synchronously, so the test never has to sleep and hope. Shared, because the check that uses it and the
// window it is installed on now live in different files.
inline constexpr UINT_PTR kPointerTimerId = 1;

// One section heading in the report.
void Section(const wchar_t* title);
// One assertion. `detail` is the measurement the line carries when it is worth carrying.
void Check(bool ok, const wchar_t* name, const wchar_t* detail = L"");
// A line that reports a measurement without asserting anything ([ -- ] in the report).
void Info(const wchar_t* name, const wchar_t* detail);
bool Nearly(float a, float b, float tolerance = 0.01f);

// The failure count, in one place: Check increments it, the entry points read and reset it.
int Failures();
void ResetFailures();

} // namespace island
