// =====================================================================
//  GlassMaterial.cpp -- see the header for what belongs here and what does not.
// =====================================================================
#include "render/GlassMaterial.h"

#include "core/Log.h"

#include <algorithm>
#include <vector>

namespace island {
namespace {

using std::min;

constexpr int kNoiseSize = 256;
std::vector<unsigned char> g_noise;

// The grain, once. A 32-bit xorshift with a fixed seed: reproducible on every machine and every run, and
// nothing here needs a cryptographic source.
void BuildNoise() {
    if (!g_noise.empty()) return;
    g_noise.resize(static_cast<size_t>(kNoiseSize) * kNoiseSize * 4);

    unsigned int state = 0x9E3779B9u;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    unsigned long long sum = 0;
    const size_t pixels = static_cast<size_t>(kNoiseSize) * kNoiseSize;
    for (size_t i = 0; i < pixels; ++i) {
        // A narrow band around the middle rather than the full 0..255: at the alphas this is drawn at
        // (2-5%) a full-range grain is visible as speckle, and the point of it is to break up flatness.
        const unsigned char grey = static_cast<unsigned char>(112 + (next() % 32));
        // BGRA, opaque: a grey pixel with the strength applied later by the brush's own opacity.
        //
        // NOT an A8 alpha-only texture, and that was measured the hard way -- an A8 bitmap drawn into this
        // target made the WHOLE FRAME disappear (the island simply stopped being painted), which is what a
        // D2D format/compatibility error looks like from outside. The colour is in the pixels and the
        // strength in the brush, which is the arrangement with nothing left to go wrong.
        g_noise[i * 4 + 0] = grey;
        g_noise[i * 4 + 1] = grey;
        g_noise[i * 4 + 2] = grey;
        g_noise[i * 4 + 3] = 255;
        sum += grey;
    }
    LogLine(L"  glass: grain built, %dx%d, mean %llu (deterministic seed 0x9E3779B9)", kNoiseSize,
            kNoiseSize, sum / pixels);
}

// The grain as a D2D bitmap. Cached against the DEVICE it was made on: a device loss (a driver restart,
// a remote session) replaces it, and a bitmap from the dead device would fail every draw from then on.
ID2D1Bitmap1* g_noiseBitmap = nullptr;
ID2D1Device* g_noiseDevice = nullptr;

ID2D1Bitmap1* NoiseBitmap(ID2D1DeviceContext2* context) {
    if (!context) return nullptr;
    ID2D1Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return nullptr;

    if (g_noiseBitmap && g_noiseDevice == device) {
        device->Release();
        return g_noiseBitmap;
    }
    if (g_noiseBitmap) {
        g_noiseBitmap->Release();
        g_noiseBitmap = nullptr;
    }
    if (g_noiseDevice) {
        g_noiseDevice->Release();
        g_noiseDevice = nullptr;
    }

    BuildNoise();
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap1* bitmap = nullptr;
    const HRESULT hr = context->CreateBitmap(D2D1::SizeU(kNoiseSize, kNoiseSize), g_noise.data(),
                                             kNoiseSize * 4, properties, &bitmap);
    if (FAILED(hr) || !bitmap) {
        LogLine(L"  glass: the grain bitmap could not be created (hr 0x%08lX); the surfaces stay flat",
                static_cast<unsigned long>(hr));
        device->Release();
        return nullptr;
    }
    g_noiseBitmap = bitmap;
    g_noiseDevice = device;   // kept, to compare against next time
    return g_noiseBitmap;
}

} // namespace

int NoiseSize() { return kNoiseSize; }

const unsigned char* NoisePixels() {
    BuildNoise();
    return g_noise.data();
}

void DrawSoftShadow(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity,
                    float spreadDip) {
    if (!context || opacity <= 0.001f || spreadDip <= 0.01f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;

    // ONE BRUSH, RE-COLOURED per step, rather than six created and released.
    //
    // A solid brush can be re-coloured, which is cheaper than a device call per step and leaves nothing to
    // release in a loop that runs twice a frame (the pill and the card). The colours are all the same black
    // at different alphas, so `SetColor` is exactly the operation needed.
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &brush)) || !brush) {
        return;
    }

    // NESTED, WIDEST FIRST, each one fainter: six steps, each adding `spread/6`. Drawn outside-in so the
    // darker inner ones are not covered by the fainter outer ones.
    constexpr int kSteps = 6;
    const float stepDip = spreadDip / static_cast<float>(kSteps);
    for (int step = kSteps; step >= 1; --step) {
        const float grow = stepDip * static_cast<float>(step);
        // A quadratic falloff, so the outermost is a whisper and the innermost carries most of the weight:
        // a linear ramp reads as a hard edge with a soft halo, which is the "重" this is meant to avoid.
        const float t = static_cast<float>(step) / static_cast<float>(kSteps);
        const float alpha = opacity * (1.0f - t) * (1.0f - t) / static_cast<float>(kSteps) * 2.2f;
        if (alpha <= 0.001f) continue;

        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha));
        const D2D1_ROUNDED_RECT grown = {
            D2D1::RectF(shape.rect.left - grow, shape.rect.top - grow, shape.rect.right + grow,
                        shape.rect.bottom + grow),
            shape.radiusX + grow * 0.6f, shape.radiusY + grow * 0.6f};
        context->FillRoundedRectangle(&grown, brush);
    }
    brush->Release();
}

void DrawSurfaceLighting(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape,
                         const Theme& theme, float opacity) {
    if (!context || opacity <= 0.01f) return;
    const float height = shape.rect.bottom - shape.rect.top;
    const float width = shape.rect.right - shape.rect.left;
    if (height < 6.0f || width < 6.0f || theme.gloss <= 0.001f) return;

    // The gradient's own band, in DIP: a share of the shape but never the whole of a tall card, where a
    // wash over 45% of 280 DIP would read as a second surface rather than as light.
    const float topBand = min(height * 0.45f, 20.0f);
    const float bottomBand = min(height * 0.35f, 26.0f);

    const auto gradientFill = [&](const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float band,
                                  bool fromTop) {
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, from}, {1.0f, to}};
        ID2D1GradientStopCollection* collection = nullptr;
        if (FAILED(context->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2,
                                                         D2D1_EXTEND_MODE_CLAMP, &collection)) ||
            !collection) {
            return;
        }
        D2D1_ROUNDED_RECT band1 = shape;
        if (fromTop) {
            band1.rect.bottom = shape.rect.top + band;
        } else {
            band1.rect.top = shape.rect.bottom - band;
        }
        ID2D1LinearGradientBrush* brush = nullptr;
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES properties = {
            D2D1::Point2F(band1.rect.left, band1.rect.top),
            D2D1::Point2F(band1.rect.left, band1.rect.bottom)};
        if (SUCCEEDED(context->CreateLinearGradientBrush(properties, collection, &brush)) && brush) {
            // Filled over the SHAPE and not over the band: filling the band's own rectangle would draw its
            // straight bottom edge across the curve. The gradient's stops are what limit it in time; the
            // rectangle is what limits it in space, and the shape is the only rectangle that fits.
            context->FillRoundedRectangle(&shape, brush);
            brush->Release();
        }
        collection->Release();
    };

    // Light from above: the ink at the gloss strength, gone by the bottom of its band.
    gradientFill(ThemeInk(theme.gloss * 1.25f * opacity), ThemeInk(0.0f), topBand, /*fromTop=*/true);
    // ... and the shade under it, which is the same lighting seen from the other side: the surface is
    // darker where no light lands. Black rather than the ink, because the shading of a surface is the
    // absence of light and not a colour of its own.
    gradientFill(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                 D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.gloss * 0.6f * opacity), bottomBand,
                 /*fromTop=*/false);

    // The hairline just inside the edge -- the glass's own rim, which is what makes the edge read as a
    // surface seen through rather than as a shape drawn on the desktop. INSET by the stroke width so it
    // cannot spill outside the shape, which is why it needs no clipping layer.
    //
    // AND IT IS DIRECTIONAL: bright along the top, almost gone by the bottom, because that is where the
    // light is. A rim of one strength all the way round reads as a frame drawn inside the shape -- measured
    // on the screen, that is exactly what the first version of this looked like -- and it fights the shade
    // at the bottom, which is the one place the surface is supposed to be darker.
    const float inset = 1.0f;
    const D2D1_GRADIENT_STOP rimStops[2] = {
        {0.0f, ThemeInk(theme.gloss * 0.95f * opacity)},
        {1.0f, ThemeInk(theme.gloss * 0.10f * opacity)},
    };
    ID2D1GradientStopCollection* rimCollection = nullptr;
    ID2D1LinearGradientBrush* rim = nullptr;
    if (SUCCEEDED(context->CreateGradientStopCollection(rimStops, 2, D2D1_GAMMA_2_2,
                                                        D2D1_EXTEND_MODE_CLAMP, &rimCollection)) &&
        rimCollection) {
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES rimProperties = {
            D2D1::Point2F(shape.rect.left, shape.rect.top),
            D2D1::Point2F(shape.rect.left, shape.rect.bottom)};
        if (FAILED(context->CreateLinearGradientBrush(rimProperties, rimCollection, &rim))) {
            rim = nullptr;
        }
    }
    if (rim) {
        const D2D1_ROUNDED_RECT inner = {
            D2D1::RectF(shape.rect.left + inset, shape.rect.top + inset,
                        shape.rect.right - inset, shape.rect.bottom - inset),
            shape.radiusX > inset ? shape.radiusX - inset : 0.0f,
            shape.radiusY > inset ? shape.radiusY - inset : 0.0f};
        context->DrawRoundedRectangle(&inner, rim, 1.0f);
        rim->Release();
    }
    if (rimCollection) rimCollection->Release();
}

void DrawGrain(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity) {
    if (!context || opacity <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    ID2D1Bitmap1* noise = NoiseBitmap(context);
    if (!noise) return;

    // A BITMAP BRUSH FILLING THE SHAPE, which needs no layer and is clipped by the rounded rectangle the
    // fill is made of -- the grain cannot escape the pill, and there is no intermediate surface, no
    // layer push and nothing to balance.
    //
    // The first version pushed a layer with the rounded geometry as its mask and called DrawBitmap. It
    // looked right and killed the whole frame: with the grain on, the island stopped being painted at all
    // (measured by switching a user theme's `grain` between 0 and 0.05 with nothing else changed), which is
    // what a D2D error inside the draw looks like from outside -- no message, no island.
    const D2D1_BITMAP_BRUSH_PROPERTIES1 brushProperties = D2D1::BitmapBrushProperties1(
        D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_INTERPOLATION_MODE_LINEAR);
    ID2D1BitmapBrush1* brush = nullptr;
    if (FAILED(context->CreateBitmapBrush(noise, brushProperties, &brush)) || !brush) return;
    brush->SetOpacity(opacity);
    context->FillRoundedRectangle(&shape, brush);
    brush->Release();
}

} // namespace island
