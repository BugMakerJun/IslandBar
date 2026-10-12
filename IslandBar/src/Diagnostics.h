// =====================================================================
//  Diagnostics.h
//  The phase 0-d self-test.
//
//  The foundation is verified by running it, not by reading it: the test
//  creates the real window and the real composition surface, then interrogates
//  the window that Win32 actually produced -- extended styles, z-order, DPI,
//  hit-test returns -- instead of trusting what was asked for.
// =====================================================================
#pragma once

namespace island {

// Returns the number of failed checks (0 == pass).
int RunSelfTest();

// Probes whether this process is actually allowed to hide the shell's taskbar,
// and reports the outcome of each method. Restores the taskbar before returning.
//
// This exists because "ShowWindow(SW_HIDE) did nothing" has several possible
// causes -- privilege, a policy, a sandbox, or the wrong window -- and they need
// distinguishing before any of them is designed around.
int RunTaskbarProbe();

} // namespace island
