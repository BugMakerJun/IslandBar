// =====================================================================
//  GlassMaterial.h
//  The two layers of "glass" that are neither colour nor edge: the soft shadow under the shape and the
//  faint grain over it.
//
//  WHY A MODULE. The review brief this answers asks for a material built from layers -- blur, gradient,
//  noise, highlight, edge, shadow -- and the bar already has the gradient, the highlight and the edge
//  (they are what Theme.h tunes; the blur is measured as unavailable, see Theme.h and HANDOFF 8.91). The
//  two that were missing are here rather than inside the window or the renderer, because both are drawn
//  identically for the pill AND for every card, and one copy of "what a soft shadow is" is the same rule
//  the surfaces already follow.
//
//  WHAT IS DELIBERATELY NOT HERE: any kind of CPU blur, screenshot, or GDI path. The brief forbids those
//  and this project does too -- everything below is Direct2D primitives on the GPU.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>

namespace island {

// A soft shadow: nested rounded rectangles, each inflated a little and drawn fainter, which is a shadow
// with no intermediate surface, no effect graph and no allocation. `spreadDip` is how far it reaches.
//
// It is not a Gaussian and does not claim to be: a real one needs an offscreen bitmap and a blur pass,
// and at the size of these shapes (a 36 DIP pill, a 280 DIP card) the stepped version is what the eye
// sees anyway. What it must not be is a hard edge, so the steps overlap rather than tile.
void DrawSoftShadow(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity,
                    float spreadDip);

// The grain over a surface, so it does not read as flat plastic. Clipped to the shape by a layer mask.
void DrawGrain(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity);

// The grain itself: 256x256 BGRA pixels (opaque grey), generated once from a fixed seed. Four bytes per
// pixel and not an alpha-only texture -- see the note in the .cpp about what an A8 bitmap did to the frame.
//
// PURE and deterministic, which is what makes it testable: a texture that changed between runs would be
// a different material every launch, and one built from rand() would be a different material on every
// machine. Never null; the buffer belongs to the module.
int NoiseSize();
const unsigned char* NoisePixels();

} // namespace island
