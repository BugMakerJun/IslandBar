#include "platform/IconRaster.h"

namespace island {

bool RasterizeIcon32(HICON icon, std::vector<uint8_t>& out) {
    out.clear();
    if (!icon) return false;

    const int side = kIconRasterSide;
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = side;
    header.bV5Height = -side;   // negative: top-down, which is the row order both callers read
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(memory, reinterpret_cast<BITMAPINFO*>(&header),
                                              DIB_RGB_COLORS, &bits, nullptr, 0)
                            : nullptr;

    bool drawn = false;
    if (bitmap && bits) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        // The DIB starts zeroed, so whatever the icon leaves alone stays transparent.
        DrawIconEx(memory, 0, 0, icon, side, side, 0, nullptr, DI_NORMAL);
        GdiFlush();
        SelectObject(memory, previous);

        const uint8_t* pixels = static_cast<const uint8_t*>(bits);
        out.assign(pixels, pixels + (side * side * 4));
        drawn = true;
    }

    // In the order they were created, and every one of them guarded: a failure above can leave any of the
    // three missing, and deleting what was never made is not a no-op.
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return drawn;
}

}  // namespace island
