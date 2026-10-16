// =====================================================================
//  Harness.cpp -- see Harness.h
// =====================================================================
#include "selftest/Harness.h"

#include "core/Log.h"

#include <cmath>
#include <cstdio>

namespace island {
namespace {

int g_failures = 0;

} // namespace

void Section(const wchar_t* title) {
    LogAndPrint(L"\n[%s]\n", title);
    std::fflush(stdout);
}

void Check(bool ok, const wchar_t* name, const wchar_t* detail) {
    LogAndPrint(L"  [%s] %-34s %s\n", ok ? L" OK " : L"FAIL", name, detail);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void Info(const wchar_t* name, const wchar_t* detail) {
    LogAndPrint(L"  [ -- ] %-34s %s\n", name, detail);
    std::fflush(stdout);
}

bool Nearly(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

int Failures() { return g_failures; }
void ResetFailures() { g_failures = 0; }

} // namespace island
