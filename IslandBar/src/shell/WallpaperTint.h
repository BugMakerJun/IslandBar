// =====================================================================
//  WallpaperTint.h -- the wallpaper's own average colour, read once
//
//  WHY THIS EXISTS (round 53, the user's "苹果那种液态玻璃"). The one thing Apple's material does that
//  this window cannot is REFRACT what is behind it: three APIs were measured and none of them honours the
//  bar's shape (HANDOFF 8.86/8.91/8.163 -- acrylic blurs but ignores the window region, the system backdrop
//  does nothing on this per-pixel-alpha DComp window). What CAN be had for nothing is the wallpaper's
//  COLOUR: the file is on disk, it is read ONCE at startup, and the pill's own surface is tinted toward it
//  so the bar sits on the wallpaper instead of on a fixed grey. No capture, no per-frame cost, and nothing
//  is sampled from the screen.
//
//  WHAT IT DOES NOT DO, said plainly: it does not know where on the screen the wallpaper is drawn. The
//  WallpaperStyle (fill / fit / tile / span) and the per-monitor picture are not modelled. The sample is the
//  TOP BAND OF THE IMAGE ITSELF -- the top 20% of its height, the middle 60% of its width -- which is the
//  part of a wallpaper a bar at the top of the screen is most likely standing on. A photograph whose top is
//  sky and whose bottom is grass therefore tints the bar like the sky, and that approximation is the whole
//  difference between this and a real sample.
// =====================================================================
#pragma once

#include <d2d1_3.h>

namespace island {

struct WallpaperTintResult {
    D2D1_COLOR_F colour = D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f);
    // False when there is no wallpaper, the file could not be read, or the decode failed. The colour is
    // then the neutral above, and the caller draws exactly what it drew before this existed.
    bool fromWallpaper = false;
    // What the average was actually taken from, so the log line can be checked against the file.
    int sampledWidth = 0;
    int sampledHeight = 0;
};

// Reads it, logs one line, and never throws: a wallpaper that cannot be read is a bar with no tint, not a
// bar that fails to start.
WallpaperTintResult ReadWallpaperTint();

}  // namespace island
