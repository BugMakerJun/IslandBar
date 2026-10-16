#include "shell/WallpaperTint.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "core/Log.h"
#include "media/WicImage.h"

namespace island {
namespace {

// The sample is taken through the PNG encoder for one boring reason: the wallpaper is 4K, and decoding it
// at full size would allocate ~33 MB to average a colour. ReencodeToPng asks WIC to scale it to 96 px on
// its long side first, so the pixels this averages are 96x54 -- a few kilobytes, and the same code path the
// album art already uses.
constexpr int kSampleLongSide = 96;
constexpr size_t kSmallPngCapacity = 256 * 1024;

bool ReadWholeFile(const wchar_t* path, std::vector<uint8_t>* out) {
    const HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(handle, &size) != FALSE && size.QuadPart > 0 &&
              size.QuadPart <= (64LL * 1024 * 1024);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) != FALSE &&
             read == out->size();
    }
    CloseHandle(handle);
    if (!ok) out->clear();
    return ok;
}

}  // namespace

WallpaperTintResult ReadWallpaperTint() {
    WallpaperTintResult result;

    wchar_t path[MAX_PATH] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) == FALSE) {
        LogLine(L"  wallpaper: no path from SPI_GETDESKWALLPAPER -- the pill keeps its own colour");
        return result;
    }
    if (path[0] == L'\0') {
        LogLine(L"  wallpaper: none set -- the pill keeps its own colour");
        return result;
    }

    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, &file)) {
        LogLine(L"  wallpaper: '%ls' could not be read -- the pill keeps its own colour", path);
        return result;
    }

    // NOT called `small`: windows.h brings in rpcndr.h, which has `#define small char` for MIDL's sake, and
    // a variable of that name is silently replaced by the type -- "vector<uint8_t> char(...)".
    std::vector<uint8_t> scaled(kSmallPngCapacity);
    const size_t scaledSize = ReencodeToPng(file.data(), file.size(), kSampleLongSide, scaled.data(),
                                           scaled.size());
    if (scaledSize == 0) {
        LogLine(L"  wallpaper: '%ls' is not an image this decoder can read -- the pill keeps its own colour",
                path);
        return result;
    }

    DecodedImage decoded;
    if (!DecodeImage(scaled.data(), scaledSize, &decoded, DecodeSlot::Other) || !decoded.pixels ||
        decoded.width <= 0 || decoded.height <= 0) {
        LogLine(L"  wallpaper: '%ls' decoded to nothing -- the pill keeps its own colour", path);
        return result;
    }

    // The top band, middle 60%: see the note in the header about what this does and does not model.
    const int bandHeight = decoded.height / 5 > 0 ? decoded.height / 5 : 1;
    const int x0 = decoded.width * 2 / 10;
    const int x1 = decoded.width - x0;
    const int y0 = 0;
    const int y1 = bandHeight;

    // Averaged in straight (non-premultiplied) terms: the pixel path is premultiplied BGRA, and a wallpaper
    // is opaque, so dividing by alpha would be dividing by 255 -- this is that division folded away.
    double sum[3] = {0.0, 0.0, 0.0};
    long long count = 0;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* row = decoded.pixels + static_cast<size_t>(y) * decoded.width * 4;
        for (int x = x0; x < x1; ++x) {
            const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
            sum[0] += pixel[2];   // R
            sum[1] += pixel[1];   // G
            sum[2] += pixel[0];   // B
            ++count;
        }
    }
    if (count == 0) {
        LogLine(L"  wallpaper: '%ls' had nothing to average -- the pill keeps its own colour", path);
        return result;
    }

    result.colour = D2D1::ColorF(static_cast<float>(sum[0] / count / 255.0),
                                 static_cast<float>(sum[1] / count / 255.0),
                                 static_cast<float>(sum[2] / count / 255.0), 1.0f);
    result.fromWallpaper = true;
    result.sampledWidth = x1 - x0;
    result.sampledHeight = y1 - y0;
    LogLine(L"  wallpaper: tint #%02X%02X%02X from the top band of '%ls' (%dx%d sample)",
            static_cast<unsigned>(result.colour.r * 255.0f + 0.5f),
            static_cast<unsigned>(result.colour.g * 255.0f + 0.5f),
            static_cast<unsigned>(result.colour.b * 255.0f + 0.5f), path, result.sampledWidth,
            result.sampledHeight);
    return result;
}

}  // namespace island
