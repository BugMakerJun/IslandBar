// =====================================================================
//  ThumbnailPreview.h
//  A live thumbnail of a window, shown under the bar while the pointer rests
//  on that window's icon.
//
//  WHY A SECOND WINDOW. A DWM thumbnail is not a bitmap this program can
//  fetch: the compositor draws the source window INTO an HWND of ours, on its
//  own schedule. So the preview is a window of its own -- a topmost, never
//  activated, click-through popup placed under the bar -- and the bar's own
//  surface is not involved at all.
//
//  Click-through is deliberate. The preview is a reminder, not a control: the
//  icon above it is what opens the window, and the preview must not be able to
//  swallow a click meant for the desktop underneath or for the bar above.
//
//  Minimised windows have nothing to show (DWM renders them empty), which is
//  also what the shell's own taskbar does. Nothing here special-cases that.
// =====================================================================
#pragma once

#include <windows.h>

// HTHUMBNAIL, the handle a DWM thumbnail registration answers with. It is a member of this class, and
// the header needs the declaration for it -- leaving it out makes the compiler read the member's NAME as
// a specifier and complain about that instead ("C3646: 'thumbnail_': unknown override specifier"), which
// points nowhere near the missing include. The same lesson AppBar.h records for APPBARDATA.
#include <dwmapi.h>

namespace island {

// The largest a preview may be, in DIPs.
//
// A preview says WHICH window this is; it is not a second desktop. Past this size it covers the
// application the user is working in, which is the opposite of the point.
inline constexpr float kThumbnailMaxWidthDip  = 260.0f;
inline constexpr float kThumbnailMaxHeightDip = 160.0f;

// The smallest side a preview may have, in DIPs.
//
// A source window with an absurd shape (a 1 x 9000 strip, a 3000 x 40 toolbar) would otherwise produce
// a box that is a line: technically the right aspect ratio, and useless as a picture of anything.
inline constexpr float kMinThumbnailSideDip = 40.0f;

// How far below the bar's own underside the preview hangs.
inline constexpr float kThumbnailGapDip = 10.0f;

// Where a preview goes: a rectangle in SCREEN PIXELS.
//
// Pure, so its placement can be asserted without a compositor, a window list or a screenshot. The
// properties that matter are all statements about arithmetic: the source's aspect ratio is kept, the
// box is centred under the icon it belongs to, it hangs below the bar, it never leaves the monitor,
// and it is never degenerate.
//
// `sourceWidthPx`/`sourceHeightPx` are the source window's own size; a window that reports nothing
// usable gets the maximum box, because a preview of the wrong shape is worse than a large one.
struct ThumbnailPlacement {
    RECT box = {};       // screen pixels
    bool valid = false;
};

ThumbnailPlacement ThumbnailBoxFor(int sourceWidthPx, int sourceHeightPx, const RECT& iconBoxPx,
                                   const RECT& monitorPx, float scale);

// The preview window itself.
class ThumbnailPreview {
public:
    ThumbnailPreview() = default;
    ~ThumbnailPreview();

    ThumbnailPreview(const ThumbnailPreview&) = delete;
    ThumbnailPreview& operator=(const ThumbnailPreview&) = delete;

    // Registers the window class and creates the hidden popup, OWNED by `owner` (the bar's window).
    //
    // Ownership rather than a plain topmost popup, and that is the whole reason this takes a handle:
    // an owned window is always kept ABOVE its owner, so the bar can never end up covering its own
    // preview. Both being WS_EX_TOPMOST is not enough -- measured on this machine, a topmost popup that
    // was raised before the bar's topmost window stayed under it, and the preview was invisible while
    // every other answer (window exists, right rectangle, visible, thumbnail registered S_OK) said
    // otherwise (HANDOFF 8.51).
    bool Create(HWND owner);
    void Destroy();
    bool Ready() const { return hwnd_ != nullptr; }

    // Shows a live thumbnail of `source` in `boxPx` (screen pixels, from ThumbnailBoxFor).
    //
    // Returns false when the compositor refused the registration -- the one outcome worth logging,
    // because it is the difference between "the feature is off" and "the window cannot be previewed"
    // (an elevated window, a window on another desktop, a window that has gone).
    bool Show(HWND source, const RECT& boxPx);

    // Puts it away. Safe to call when it is already hidden, and called from the frame loop every frame
    // the pointer is not resting on an icon.
    void Hide();

    bool Visible() const { return visible_; }
    HWND Source() const { return source_; }
    HWND Handle() const { return hwnd_; }

    // The last thing the compositor said, for the log: the HRESULT of the update that put the thumbnail
    // on screen (or of the registration that was refused).
    long long LastResult() const { return lastResult_; }

    // What the compositor believes the source window measures, filled in by Show. Zero means it has
    // nothing to draw, which is a different fault from a refused registration and looks the same on
    // screen: a preview-shaped hole.
    SIZE SourceSize() const { return sourceSize_; }

    // How many times a registration has been refused since the program started, so a window that
    // cannot be previewed is reported once rather than every frame.
    int Refusals() const { return refusals_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HWND source_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
    SIZE sourceSize_ = {0, 0};
    bool visible_ = false;
    long long lastResult_ = 0;
    int refusals_ = 0;
};

} // namespace island
