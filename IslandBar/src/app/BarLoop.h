// =====================================================================
//  BarLoop.h -- the bar itself: RunBar, and the only thing the entry point calls
//
//  WHY THIS FILE EXISTS. main.cpp held the bar, every phase of its frame loop and the
//  argument parsing all at once -- 6,563 lines, of which all but a few hundred were the
//  bar. The split is by RESPONSIBILITY and not by line count: what is left here is the
//  boundary between "the program decides what to do" (main.cpp) and "the bar runs"
//  (app/BarLoop.cpp).
//
//  The declaration is here rather than in a cpp so that the definition is checked
//  against it -- a signature that drifts on one side is a compile error, not a link
//  error six minutes later.
// =====================================================================
#pragma once

// `forcedState` pins the shape so it can be looked at, and `forcedStartHover` pins the Start button's
// hover. Debug only, and they exist because a shape can satisfy every assertion and still be wrong to
// look at: a screenshot of a named state settles in seconds what reasoning about geometry does not.
int RunBar(bool hideNativeTaskbar, bool claimAppBar, bool keepStrip, const wchar_t* forcedState,
           bool forcedStartHover, bool forcedSystemPanel, bool keepNativeTray);

