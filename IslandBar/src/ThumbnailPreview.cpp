// =====================================================================
//  ThumbnailPreview.cpp
// =====================================================================
#include "ThumbnailPreview.h"

#include "Geometry.h"
#include "Log.h"

#include <dwmapi.h>

#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace island {
namespace {

constexpr wchar_t kThumbnailClassName[] = L"IslandBarThumbnail";

} // namespace

ThumbnailPlacement ThumbnailBoxFor(int sourceWidthPx, int sourceHeightPx, const RECT& iconBoxPx,
                                   const RECT& monitorPx, float scale) {
    ThumbnailPlacement placement;
    if (scale <= 0.0f) scale = 1.0f;

    const float maxWidth = kThumbnailMaxWidthDip * scale;
    const float maxHeight = kThumbnailMaxHeightDip * scale;
    const float minSide = kMinThumbnailSideDip * scale;

    // The source's shape, when it is usable: aspect ratio kept, fitted inside the maximum box.
    float width = maxWidth;
    float height = maxHeight;
    if (sourceWidthPx > 0 && sourceHeightPx > 0) {
        const float aspect = static_cast<float>(sourceWidthPx) / static_cast<float>(sourceHeightPx);
        width = maxWidth;
        height = width / aspect;
        if (height > maxHeight) {
            height = maxHeight;
            width = height * aspect;
        }
        if (width < minSide) width = minSide;
        if (height < minSide) height = minSide;
    }

    // Centred under the icon, and hanging below it by the gap -- from the icon's BOTTOM edge, because
    // that is the edge the pointer came in over.
    const float centreX = (static_cast<float>(iconBoxPx.left) + static_cast<float>(iconBoxPx.right)) * 0.5f;
    float left = centreX - width * 0.5f;
    float top = static_cast<float>(iconBoxPx.bottom) + kThumbnailGapDip * scale;

    // Inside the monitor, on both axes. A preview that hung off the edge would be clipped by the
    // display, which reads as a rendering fault rather than as a window that is too near the corner.
    const float monitorLeft = static_cast<float>(monitorPx.left);
    const float monitorRight = static_cast<float>(monitorPx.right);
    const float monitorTop = static_cast<float>(monitorPx.top);
    const float monitorBottom = static_cast<float>(monitorPx.bottom);

    if (left + width > monitorRight) left = monitorRight - width;
    if (left < monitorLeft) left = monitorLeft;
    if (top + height > monitorBottom) top = monitorBottom - height;
    if (top < monitorTop) top = monitorTop;

    placement.box.left = static_cast<LONG>(left);
    placement.box.top = static_cast<LONG>(top);
    placement.box.right = static_cast<LONG>(left + width);
    placement.box.bottom = static_cast<LONG>(top + height);
    placement.valid = (placement.box.right > placement.box.left) &&
                      (placement.box.bottom > placement.box.top);
    return placement;
}

ThumbnailPreview::~ThumbnailPreview() {
    Destroy();
}

bool ThumbnailPreview::Create(HWND owner) {
    if (hwnd_) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ThumbnailPreview::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // the compositor paints the thumbnail
    wc.lpszClassName = kThumbnailClassName;

    // A second registration of the same class fails with ERROR_CLASS_ALREADY_EXISTS, which is not an
    // error: it is process-wide.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // NO WS_EX_LAYERED, and that is not a detail: DWM refuses to register a thumbnail whose destination
    // is a layered window. WS_EX_NOACTIVATE keeps it from taking focus -- the bar is not allowed to steal
    // the foreground, and neither is anything it owns.
    //
    // WS_EX_TRANSPARENT IS DELIBERATELY ABSENT, though it is the usual way to make a window
    // click-through: measured on this machine, a thumbnail is registered successfully into such a window
    // (DwmRegisterThumbnail returns S_OK, the window is visible at the right place and the source is an
    // ordinary visible window) and NOTHING IS DRAWN INTO IT. Click-through is done by answering
    // HTTRANSPARENT to WM_NCHITTEST instead, which is the same trick the bar itself uses and leaves the
    // window ordinary in every way the compositor cares about.
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

    hwnd_ = CreateWindowExW(exStyle, kThumbnailClassName, L"IslandBar preview", WS_POPUP, 0, 0, 1, 1,
                            owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    return true;
}

void ThumbnailPreview::Destroy() {
    if (thumbnail_) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    source_ = nullptr;
    visible_ = false;
}

bool ThumbnailPreview::Show(HWND source, const RECT& boxPx) {
    if (!hwnd_ || !source || !IsWindow(source)) return false;
    if (boxPx.right <= boxPx.left || boxPx.bottom <= boxPx.top) return false;

    // SHOWN BEFORE IT IS REGISTERED, which is the order that matters.
    //
    // The window is created 1x1 and hidden, and the first version registered the thumbnail against it
    // there and then -- which the compositor accepts (S_OK) and then draws nothing into, for ever: the
    // window is moved and shown afterwards, and the thumbnail never appears. Measured on this machine as
    // "the preview window is visible at the right rectangle with the right source and not one pixel of it
    // is drawn" (HANDOFF 8.51). Making it visible at its final size FIRST, and registering afterwards,
    // is what the compositor actually needs.
    SetWindowPos(hwnd_, HWND_TOPMOST, boxPx.left, boxPx.top, boxPx.right - boxPx.left,
                 boxPx.bottom - boxPx.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    if (source_ != source || !thumbnail_) {
        if (thumbnail_) {
            DwmUnregisterThumbnail(thumbnail_);
            thumbnail_ = nullptr;
        }

        const HRESULT registered = DwmRegisterThumbnail(hwnd_, source, &thumbnail_);
        lastResult_ = static_cast<long long>(registered);
        if (FAILED(registered) || !thumbnail_) {
            thumbnail_ = nullptr;
            source_ = nullptr;
            if (visible_) {
                ShowWindow(hwnd_, SW_HIDE);
                visible_ = false;
            }
            ++refusals_;
            return false;
        }
        source_ = source;
    }

    // What the COMPOSITOR believes the source is: this is the number it will draw from, and it is not
    // necessarily the window's own rectangle (a window 1226x701 read back as 0x0 is a thumbnail that
    // cannot be drawn, however well-formed the registration was).
    sourceSize_ = {0, 0};
    DwmQueryThumbnailSourceSize(thumbnail_, &sourceSize_);

    // The whole source window, into the whole client area: the aspect ratio and the size were decided
    // by ThumbnailBoxFor, so the two must not disagree here as well.
    //
    // rcSource IS STATED EXPLICITLY, and that is the whole of a defect that cost several hours: an EMPTY
    // rcSource is documented as "the whole source window", and on this machine a thumbnail registered
    // with one draws NOTHING -- registration returns S_OK, the update returns S_OK,
    // DwmQueryThumbnailSourceSize answers 626x473, the destination window is visible at the right
    // rectangle, and not one pixel appears (tools/thumbprobe measured both ways: empty source -> the
    // window underneath shows through, explicit source -> the window itself). The rectangle the
    // compositor reports is what it will draw, so that is what is handed back to it.
    DWM_THUMBNAIL_PROPERTIES properties = {};
    properties.dwFlags =
        DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    properties.rcDestination = {0, 0, boxPx.right - boxPx.left, boxPx.bottom - boxPx.top};
    properties.rcSource = {0, 0, sourceSize_.cx, sourceSize_.cy};
    properties.opacity = 255;
    properties.fVisible = TRUE;
    // DWM_TNP_SOURCECLIENTAREAONLY is deliberately NOT set: the source rectangle above is the
    // compositor's own answer for the whole window, frame included, which is what a preview is meant to
    // look like.
    const HRESULT updated = DwmUpdateThumbnailProperties(thumbnail_, &properties);
    lastResult_ = static_cast<long long>(updated);
    if (FAILED(updated)) {
        // The registration was granted and the properties were refused, which is a different fault from
        // "the compositor will not preview this window" and would otherwise look identical: a window on
        // screen with nothing in it.
        ++refusals_;
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        return false;
    }

    return true;
}

void ThumbnailPreview::Hide() {
    if (!hwnd_ || !visible_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

LRESULT CALLBACK ThumbnailPreview::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam) {
    switch (message) {
        // Nothing to erase: the compositor owns this window's pixels, and painting into it would fight
        // the thumbnail for the same rectangle.
        case WM_ERASEBKGND:
            return 1;
        // Click-through by message as well as by style, so a click that lands on the preview during the
        // frame the style is being applied still reaches what is underneath.
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace island
