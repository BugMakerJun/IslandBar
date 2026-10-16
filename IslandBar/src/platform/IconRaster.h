// platform/IconRaster.h -- an HICON turned into pixels, once
//
// WHY THIS IS A COMPONENT. Getting from an icon handle to a BGRA buffer is fifteen lines of GDI: a
// BITMAPV5HEADER whose four masks are what make the alpha channel mean anything, a DIBSection, SelectObject,
// DrawIconEx, GdiFlush, and three handles released in the order they were created -- a device context, a
// memory DC and a bitmap. Two callers wrote it out, in media/ (to re-encode an application's icon as a PNG
// for the notification rows) and in render/ (to upload the same kind of icon to the GPU), and the two copies
// differed in their variable names and in nothing else. The render side even said so in a comment ("one
// place that knows how to get from an icon to a bitmap rather than two") while being the second copy.
//
// THE TEARDOWN IS THE PART THAT MUST NOT DRIFT: a ReleaseDC that goes missing leaks a device context per
// icon, and both call sites build icons every time a track changes or the taskbar's row is refreshed.
//
// IT LIVES IN platform/ BECAUSE BOTH CALLERS SIT ABOVE IT (media/ and render/); anything higher would be the
// upward include the build refuses (see ARCHITECTURE.md section 3).

#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace island {

// The buffer is always this square. Named because the callers need it for their own strides: the PNG encoder
// takes the side, and a D2D bitmap takes side*4.
constexpr int kIconRasterSide = 32;

// Draws `icon` into `out` -- top-down 32-bit BGRA, `kIconRasterSide` square, resized to exactly that -- and
// says whether the pixels are there. The layout is the one both callers want: the PNG encoder reads it, and
// DXGI_FORMAT_B8G8R8A8_UNORM is what the GPU is handed, so nothing has to be flipped anywhere.
//
// `out` belongs to the caller and stays alive after the call; nothing here holds a pointer into it. What the
// icon's own handle does afterwards is also the caller's business -- one call site created it and destroys
// it, the other was handed one to borrow, and that difference is why destroying it is not in here.
bool RasterizeIcon32(HICON icon, std::vector<uint8_t>& out);

}  // namespace island
