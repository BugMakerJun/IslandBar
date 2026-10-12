// =====================================================================
//  TextRenderer.h
//  DirectWrite text for the island.
//
//  Deliberately small: it formats a time and a date and draws them into
//  whatever D2D context it is handed. It does not own the context or the target
//  bitmap, so the same object serves the pill and the expanded card.
//
//  Formatting is cached. GetLocalTime costs microseconds, but the formats and
//  the date string are rebuilt only when they actually change -- the time
//  changes once a second, the date once a day.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <cstddef>

#include "Geometry.h"      // RoundedBox: the text is laid out inside the pill's box
#include "MusicSession.h"  // the now-playing field capacities
#include "CardLayout.h"    // where the card's controls go

namespace island {

enum class ClockStyle {
    Compact,   // hh:mm for the pill
    Card,      // hh:mm plus the date, for the expanded card
};

// One window button on the taskbar.
//
// At namespace scope rather than nested inside TextRenderer, so the frame state can hold an array
// of them without a circular-looking dependency and the renderer can be handed one plain block.
struct TaskButton {
    const wchar_t* title = nullptr;   // already UTF-16; the window's own text
    ID2D1Bitmap1* icon = nullptr;     // the owning process's icon, or null
    bool active = false;              // this is the window in front
    bool minimised = false;

    // The button's background COLOUR, taken from the island's own fill.
    //
    // The COLOUR IS PASSED IN rather than chosen here, and that is the point: it is the same value
    // the island's pill is filled with. A button that picked its own shade would drift away from the
    // island the first time either changed, and the two read as one object.
    //
    // The RECTANGLE is not passed in. It is derived inside the draw from the same layout call that
    // positions the button, so the background and the button cannot disagree -- passing it in from
    // the caller meant computing the layout twice, and the two would part company the moment one of
    // them used a different hovered index.
    //
    // Before this existed the icons sat straight on the desktop. An icon on a bright photograph is
    // hard to read and a title on one is worse, which is exactly what was reported.
    D2D1_COLOR_F fill = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);

    // How far this button's opening has run, 0 to 1. Animated by the caller so the button OPENS
    // instead of snapping -- a width that jumps in one frame reads as a stutter, which is what was
    // reported.
    //
    // One value decides the button's width AND how far the buttons after it are pushed. A second
    // animated value for the push used to exist, disagreed with this one mid-transition, and let the
    // open button paint over its neighbour.
    float hoverAmount = 0.0f;
};


class TextRenderer {
public:
    // The call timer, as mm:ss (or h:mm:ss once it runs past an hour).
    //
    // A free function rather than something the drawing code does inline, because it is the part that is
    // WRONG in a way nobody notices for ten minutes: 61 seconds printed as "1:1" or 3661 as "61:01" both look
    // like a timer until you check. Asserted in the self-test at the boundaries.
    static void CallTimerText(int seconds, wchar_t* out, size_t capacity);

    // How tall bar `index` of a `count`-bar meter is, for a level of 0..1, as a fraction of the tallest bar.
    //
    // The bars are a fixed pattern scaled by the level, not a per-bar random number: a level meter whose bars
    // jump independently reads as noise, while one that keeps its shape and breathes reads as sound. Pure so
    // the shape can be asserted (it must never be flat, and it must never exceed 1).
    static float CallBarFraction(int index, int count, float level);
    TextRenderer() = default;
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Creates the DirectWrite factory and the text formats. Safe to call twice;
    // the second call is a no-op.
    bool Initialize();

    void Shutdown();

    bool IsReady() const { return factory_ != nullptr; }

    // Draws the current local time centred in `box`, using window-local DIPs.
    //
    // `opacity` scales the text colour so it can follow the pill's own fade
    // instead of popping in at full strength.
    void DrawClock(ID2D1DeviceContext2* context, const RoundedBox& box,
                   ClockStyle style, float opacity);

    // --- the status strip: what stands in for the clock while the island is busy ----
    //
    // The date, the weekday, the time and -- when there is a reading -- the weather, in the
    // band to the left of the system button. See StatusStripFor for why it exists and where
    // it stands.

    // The strip's one line, assembled from the three parts.
    //
    // A free function over its inputs, and a STATIC one, because the self-test has to be able
    // to measure the longest line the strip will ever print against the strip's own box -- and
    // that measurement must not need a window, a clock or a network.
    static void ComposeStatusLine(const wchar_t* time, const wchar_t* date, const wchar_t* weather,
                                  wchar_t* out, size_t capacity);

    // Draws the strip on the island's own line: an island-coloured capsule, so the words are
    // legible over any wallpaper, with the line centred inside it.
    //
    // `opacity` is the strip's own fade (0 -> 1 as the island becomes occupied), and `fill` is
    // the island's fill colour -- passed in rather than chosen here, for the reason TaskButton
    // gives: a surface that picks its own shade drifts away from the bar it belongs to.
    void DrawStatusStrip(ID2D1DeviceContext2* context, const RoundedBox& island, float opacity,
                         const wchar_t* weather, const D2D1_COLOR_F& fill);

    // The drawn width of a composed status line, in DIPs. Needs Initialize(). Exists so the
    // strip's fixed box can be shown to hold the longest line it will ever print (see the
    // assertion in the self-test) rather than being sized by eye.
    float StatusLineWidth(const wchar_t* line) const;

    // Draws a notification's sender, title and body in the lower part of `box`.
    //
    // Empty fields are skipped rather than drawn blank, so a notification with only
    // a title does not leave a hole where the body would be.
    void DrawNotification(ID2D1DeviceContext2* context, const RoundedBox& box,
                          const char* app, const char* title, const char* body,
                          const uint8_t* iconPixels, int iconWidth, int iconHeight,
                          unsigned long long iconRevision, float opacity);

    // Everything the music card says, in one struct so the call site cannot pass the
    // fields in the wrong order.
    struct MusicCardText {
        const char* title = nullptr;
        const char* artist = nullptr;
        const char* album = nullptr;
        const char* app = nullptr;
        bool  playing = false;
        long long positionMs = 0;
        long long durationMs = 0;
        // 0..1, or <= 0 when the duration is unknown (a live stream), in which case the
        // bar is drawn but never filled.
        float progressFraction = 0.0f;
    };

    // Draws the now-playing card, laid out like the Windows 11 media flyout: blurred
    // album art as the background, the app's name and state along the top, the cover,
    // title and artist, the elapsed and total times with a progress bar between them,
    // and previous / play-pause / next along the bottom.
    //
    // `backgroundPixels` is the blurred artwork (see WicImage's
    // MakeBlurredBackgroundPng); passing null simply omits the wash.
    // `artwork` and `background` are GPU bitmaps the CALLER owns and caches; passing raw
    // pixels here instead would mean this function rebuilt them on every frame, which
    // copies the pixels to the GPU each time -- see Renderer::CachedBitmap.
    void DrawMusicCard(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const MusicCardText& text, ID2D1Bitmap1* artwork, int artworkWidth,
                       int artworkHeight, ID2D1Bitmap1* background, ID2D1Bitmap1* icon,
                       float opacity);

    // Draws a cached bitmap into a rectangle. Shared by the card's background and its
    // album cover so both go through one draw path.
    // The filter is a parameter because the two users differ: the card's background is a
    // smooth blur that linear resampling cannot visibly change, while the album cover is
    // small and detailed and is worth the cubic filter.
    void DrawBitmapRect(ID2D1DeviceContext2* context, ID2D1Bitmap1* bitmap,
                        const RectF& target, float opacity,
                        D2D1_INTERPOLATION_MODE mode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    // Raw-pixel variant, used once per assertion by the diagnostic artwork drawing.
    void DrawBitmapRect(ID2D1DeviceContext2* context, const uint8_t* pixels, int width,
                        int height, const RectF& target, float opacity);

    // How long the last music-card draw spent on each expensive layer, in microseconds.
    //
    // Reported because "the card is laggy" has to be attributed to a layer before it can
    // be fixed: the first guess was the per-frame bitmap upload, and measuring showed that
    // was worth nothing, so the cost is one of these blits.
    double LastCardBackgroundMicroseconds() const { return lastCardBackgroundUs_; }
    double LastCardArtworkMicroseconds() const { return lastCardArtworkUs_; }
    double LastCardMicroseconds() const { return lastCardUs_; }

    // Draws decoded album art into the layout's artwork square. `pixels` is BGRA
    // premultiplied, as WicImage produces.
    //
    // The bitmap is created from the pixel data on every call and released again;
    // artwork changes rarely and the image is at most 128x128, so caching it would
    // save very little while adding a lifetime to get wrong.
    void DrawArtwork(ID2D1DeviceContext2* context, const RoundedBox& box,
                     const uint8_t* pixels, int width, int height);

    // Draws one line of lyrics in the resting pill, marquee-style.
    //
    // `scrollDip` shifts the text left as it plays, so a line longer than the pill
    // scrolls instead of being cut off. The text is clipped to the pill's rounded
    // shape, so it can never appear outside the island.
    // How one line replaces another: the roll. `progress` runs 0 -> 1 (see LyricLineRollFor) and `previous`
    // is the line that is leaving, drawn above the arriving one by one line height. For the length of the
    // roll the pill therefore shows two half rows -- one climbing out, one rising in -- and that is the
    // shape the user asked for, twice.
    struct LineRoll {
        const char* previous = nullptr;
        float       progress = 1.0f;
        float       previousScrollDip = 0.0f;
    };

    //
    // `roll` is how the line arrived: at progress 1 it is settled and nothing is leaving, which is
    // what every caller that is not changing lines passes (the default).
    void DrawLyricLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const char* utf8Line, float scrollDip, float opacity,
                       const LineRoll& roll = {});

    // Draws one line of the track's title in the resting pill. Used when there are
    // no lyrics to show.
    void DrawTrackLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const char* utf8Line, float scrollDip, float opacity,
                       const LineRoll& roll = {});

    // The pill's BACKGROUND audio wave.
    //
    // A full-width, full-height wave rather than a row of vertical bars: "电平条有没有别的什么方式 现在着用
    // 竖条看着有点丑 然后能不能占胶囊全部高度 宽度也一样 颜色跟随专辑获取歌曲变". The ribbon's
    // half-height at each x is a smooth envelope times the peak reading (AudioVolume::Peak, sampled by the
    // worker about thirty times a second), so it fills the pill when the music is loud and thins to a line
    // when it is quiet; `phase` drifts with time so the shape moves even while the level sits still.
    // `accent` is the colour taken from the cover (see AccentColourFromArtwork).
    void DrawAudioWave(ID2D1DeviceContext2* context, const RoundedBox& box, float phase, float level,
                       const D2D1_COLOR_F& accent, float opacity);

    // The wave's half-height at `step` of `steps`, 0..1, before the level scales it.
    //
    // A pure function for the same reason CallBarFraction is one: "the wave does not move" and "the wave
    // is not drawn" look identical on screen, and this separates the two.
    static float AudioWaveEnvelope(int step, int steps, float phase);

    // One window button on the taskbar.
    //
    // The struct itself is at namespace scope (see above) so the frame state can hold an array of
    // them; this alias keeps the name available inside the class where it reads better.
    using TaskButton = island::TaskButton;

    // Draws the row of application icons beside the island.
    //
    // `hovered` widens each button so it can show its window's title; at rest they are plain
    // squares. The island's own box is passed because the row is positioned FROM it -- the island is
    // the fixed point and the row grows to its right.
    //
    // `buttons` is read up to `count`, and the count is trusted rather than silently adjusted: a bar
    // that draws a different number of buttons than the click handler hit-tests is a bar whose
    // clicks land on the wrong window.
    // `rowAmounts` and `rowCount` describe the row, and are the same count as the buttons.
    //
    // `cardLeftDip` and `cardWidthDip` locate the system panel's card when one is open, so the row can
    // start past it instead of underneath it: the card hangs from the system button and spans rightwards
    // over where the row would otherwise begin. Zero means "no card", which is every other state.
    void DrawTaskIcons(ID2D1DeviceContext2* context, const RoundedBox& island,
                       const TaskButton* buttons, int count, int hoveredIndex, float opacity,
                       const float* rowAmounts = nullptr, int rowCount = 0,
                       float cardLeftDip = 0.0f, float cardWidthDip = 0.0f,
                       // Each icon's arrival, 0..1 (see TaskIconAppearStep); null means all settled.
                       const float* appearAmounts = nullptr);

    // The Start button, on the left of the island.
    //
    // Drawn from the SAME island box the icon row is drawn from, so the two ends of the bar cannot
    // disagree about where the middle is. `hovered` is whether the pointer is on it, and `fill` is the
    // island's own colour, for the reason the icon buttons take it: a button that chose its own shade
    // would drift away from the island the first time either changed.
    void DrawStartButton(ID2D1DeviceContext2* context, const RoundedBox& island, bool hovered,
                         const D2D1_COLOR_F& fill, float opacity);

    // Which system indicator a row slot — or a panel tile — holds.
    //
    // ONE enum for both, because they show the same things: the tile that explains Wi-Fi must draw the
    // same mark as the indicator that summarises it, and two enums would let them drift apart.
    enum class IndicatorIcon : int {
        InputMethod = 0,
        Network = 1,
        Bluetooth = 2,
        Volume = 3,
        Battery = 4,
        Airplane = 5,
        Settings = 6,
        Accessibility = 7,
        BatterySaver = 8,
        Captions = 9,
        // The second page's tiles. Codes verified by drawing them, like the rest of this table -- a glyph that
        // does not exist comes out as a box, which is why they are looked at rather than trusted.
        Moon = 10,
        Hotspot = 11,
        Share = 12,
        // The output-device button at the right of the volume row, and the icon of a sound device in the
        // "声音输出" page. It is the speaker glyph with the arrow that says "this is where the sound goes",
        // which is the mark the shell's own flyout puts at the end of the volume row.
        OutputDevice = 13,
        // The two tiles on the shell's second page that open a LIST of display modes, and the two device
        // marks the "声音输出" page needs for a headset and for audio that comes out of a screen.
        WiredDisplay = 14,
        Projection = 15,
        Headphones = 16,
        DisplayAudio = 17,
    };

    using SystemPanelIcon = IndicatorIcon;

    // The Segoe Fluent Icons codepoint for a mark, at a given reading.
    //
    // A function rather than a switch buried in the drawing code, because the codepoints are the part
    // that went wrong: U+E0CE looked like the Bluetooth codepoint and drew an empty box, and A MISSING
    // GLYPH DOES NOT REPORT AN ERROR. Keeping the mapping in one place, with the values written down,
    // means the next one can be checked against this table instead of guessed at again.
    //
    // `value` is the reading the mark depends on -- signal strength for the network, 0-100 for the
    // battery, 0 for muted on the volume -- and is ignored by the marks that do not vary.
    static wchar_t IndicatorGlyph(IndicatorIcon icon, int value);

    // DrawIndicator used to live here: one system indicator, at a rectangle the caller had laid out.
    //
    // It is gone with the indicator row itself. The glyph table above stays, because the system panel's
    // TILES are drawn from it -- a tile that says "Bluetooth" and an indicator that said the same thing
    // had to show the same mark, and that is still true of the tiles.

    // The system panel's button: a chevron on the left deck, beside the Start button.
    //
    // `open` points it up when the panel is showing, which is the one piece of state the button has to
    // communicate by itself: whether pressing it again will put away what is already there.
    void DrawSystemButton(ID2D1DeviceContext2* context, const RectF& box, bool hovered, bool open,
                          const D2D1_COLOR_F& fill, float opacity);

    // What pressing a panel tile does.
    //
    // An enum rather than a callback, so a tile is plain data: the frame is built in one place and
    // consumed in another, and the click handler has to be able to ask "what does this tile want"
    // without the tile carrying a function pointer into a lifetime it does not own.
    enum class PanelAction : int {
        None = 0,        // nothing yet -- the tile reports state and does not change it
        OpenSettings,    // launch the `uri` page
        ToggleMute,      // the speaker tile's own action
        // Open the "声音输出" page inside the card -- the shell's own second level off the volume row --
        // rather than launching a settings page outside it.
        OpenSoundPage,
        // 投影: the shell's own tile sends Win+P, which is what brings up the projection-mode list. The
        // page it opens is the shell's, not a settings URI, so it cannot be expressed as OpenSettings.
        ProjectionModes,
        // A tile that switches something here and now, rather than taking the user to a page. The one that
        // exists is 飞行模式, which switches the Wi-Fi radio through the documented WLAN opcode -- see
        // island::SetWifiRadioOn for why it is that radio and not the whole radio set.
        ToggleRadio,
    };

    // One tile of the system panel, as the caller has decided it.
    //
    // The caller supplies the finished words rather than the readings, because turning a reading into
    // a label ("Wi-Fi", "Airplane mode") and deciding whether it is ON is a judgement about the
    // machine, and that belongs with the code that has the reading.
    struct SystemPanelTile {
        const wchar_t* label = nullptr;   // "Wi-Fi", "Bluetooth", ...
        // What the CAPTION under the box says, when that is not the name: the state ("开"/"关"), which is what
        // the shell's own flyout puts there (its Wi-Fi tile is captioned 可用 rather than "Wi-Fi" -- the glyph
        // above it already says which control it is). Null means "caption the label".
        const wchar_t* stateText = nullptr;
        SystemPanelIcon icon = SystemPanelIcon::Network;
        int iconValue = 0;                // the icon's own reading, e.g. signal strength
        bool on = false;                  // the tile's ON appearance
        // Whether the tile shows a chevron, i.e. whether it leads somewhere rather than toggling in place.
        //
        // The shell's own flyout draws this arrow on the tiles that open a LIST -- Wi-Fi and Bluetooth open
        // their flyouts -- and not on the plain ones (飞行模式, 辅助功能, 节能模式, 实时字幕 have none). The
        // first version of this drew the arrow on every tile that had any action, which is not the same
        // question and does not look like the flyout it is meant to sit beside.
        bool chevron = false;

        // What pressing it does.
        //
        // Carried on the tile rather than looked up from the label, because the label is user-facing
        // text and the action is not: renaming a tile must not silently change what it does, and a
        // lookup by label would also tie the two together in the one direction that cannot be tested.
        PanelAction action = PanelAction::None;
        const wchar_t* uri = nullptr;     // the settings page, for PanelAction::OpenSettings
    };

    struct SystemPanelText {
        SystemPanelTile tiles[kMaxSystemTiles] = {};
        int tileCount = 0;
        // Which page of tiles this is, and how many there are. The tiles above are always the CURRENT page's
        // (the caller assembles the slice), so the layout only needs these two for the paging dots.
        int pageIndex = 0;
        int pageCount = 1;
        float volume = 0.0f;      // 0..1, for the slider
        bool muted = false;
        // Whether to print the volume as a percentage beside the slider. The shell shows the number while
        // the slider is being used and hides it again, and the number is the only exact reading on the
        // panel -- a handle at 40% of its travel says nothing about whether that is 40 or 45.
        bool showVolumePercent = false;
    };

    // The system panel, drawn into the card.
    //
    // One call for the whole panel, like the music card: the tile washes, glyphs, labels, the slider
    // and the settings button are drawn in one place so their layers cannot end up in the wrong order.
    void DrawSystemPanel(ID2D1DeviceContext2* context, const RoundedBox& box,
                         const SystemPanelText& text, float opacity);

    // What the "声音输出" page says: the device list, the spatial-sound rows and the mixer.
    //
    // The strings and the readings are supplied by the caller -- they come from the audio stack, which this
    // class knows nothing about -- and the geometry comes from SoundPageFor, which the hit test also calls.
    struct SoundPageText {
        // Rows the mixer can hold, including the output device's own row at the top: the shell's mixer lists
        // the device first and the applications under it.
        static constexpr int kMaxMixRows = kMaxSoundSessions + 1;

        // The page's own words. Chinese, because this is the shell's own page and it is what the reference
        // flyout says; the caller supplies them so this class holds no user-facing strings of its own.
        const wchar_t* title = nullptr;         // 声音输出
        const wchar_t* deviceLabel = nullptr;   // 输出设备
        const wchar_t* spatialLabel = nullptr;  // 空间音效
        const wchar_t* mixerLabel = nullptr;    // 音量合成器
        const wchar_t* footer = nullptr;        // 更多音量设置

        struct DeviceRow {
            const wchar_t* name = nullptr;
            int formFactor = 0;      // EndpointFormFactor; the glyph is chosen from it
            bool isDefault = false;
        };
        DeviceRow devices[kMaxSoundDevices] = {};
        int deviceCount = 0;

        struct SpatialRow {
            const wchar_t* label = nullptr;
            bool selected = false;
        };
        SpatialRow spatial[kSoundSpatialRows] = {};

        struct MixRow {
            const wchar_t* label = nullptr;     // for the log, and for a row with no icon
            float volume = 1.0f;
            bool muted = false;
            // The application's icon exactly as the sound worker encoded it: a small PNG, decoded once per
            // revision and kept as a bitmap here. Null falls back to a speaker glyph.
            const unsigned char* iconPng = nullptr;
            size_t iconSize = 0;
            unsigned long long iconRevision = 0;
        };
        MixRow mix[kMaxMixRows] = {};
        int mixCount = 0;

        float scroll = 0.0f;
    };

    // The 声音输出 page, drawn into the card. One call, like the panel, so its layers cannot be drawn out of
    // order -- and clipped to its own viewport, so a scrolled row cannot paint over the header.
    void DrawSoundPage(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const SoundPageText& text, float opacity);

    // The CALL strip: the island, widened, showing a call in progress.
    //
    // The reference the user handed over is the iPhone's Dynamic Island during a call -- a round avatar, a
    // green running timer, and a row of level bars -- and every part of it here is a real reading rather than
    // a decoration: the timer is the call's elapsed time, and the bars are the OUTPUT's peak level, so they
    // move when somebody speaks and sit still when nobody does.
    struct CallStripText {
        int seconds = 0;                 // elapsed, for the green timer
        float level = 0.0f;              // 0..1 peak, for the bars
        bool  levelKnown = false;        // false draws an idle meter rather than one stuck at zero
        // The caller's picture is not readable from outside WeChat, so the strip shows the APPLICATION's icon
        // in that place -- the honest stand-in, and the same icon the notification rows use.
        const uint8_t* iconPixels = nullptr;
        int iconWidth = 0;
        int iconHeight = 0;
        unsigned long long iconRevision = 0;
    };
    void DrawCallStrip(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const CallStripText& text, float opacity);

    // Height, in DIPs, that a taskbar button's title needs at the current face size. Reported so
    // the layout can be checked against the strip it has to fit in.
    float TaskTitleLineHeight() const;

    // Width of a text run in DIPs, for deciding whether it needs to scroll. Returns
    // 0 when it cannot be measured.
    float TextWidth(const wchar_t* text, bool titleStyle) const;

    // Width of one line of the PILL's own text, measured with the format that DRAWS it.
    //
    // `TextWidth` measures with appFormat_ (11 DIP, NORMAL), which is right for most callers but not for the
    // pill: the strip's line is drawn with pillLineFormat_ (11 DIP, BOLD), and a bold advance is wider for
    // Latin text. Sizing the marquee's rectangle from the narrower number is what let a long English line be
    // cut instead of scrolled -- and, with word wrap still on, what let it wrap into two rows. Both the fit
    // decision and the drawn rectangle go through here now, so the two cannot disagree.
    float TrackLineWidth(const wchar_t* text) const;

    // True when the notification text formats were created. The card still draws
    // without them, so a failure here degrades rather than breaks.
    bool HasNotificationFormats() const { return titleFormat_ != nullptr; }

    // Reports the line height each format actually needs, in DIPs.
    //
    // The card lays text out in hand-sized rectangles, and a rectangle shorter than
    // the font's natural line height clips the glyphs. That is exactly what happened:
    // the title drew as a horizontal sliver because its 20 DIP rect was shorter than
    // the face's line height. A CJK face's line height is well above its nominal
    // point size, so the rectangle has to be sized from the real metrics.
    struct TextMetrics {
        float titleLineHeight = 0.0f;
        float secondaryLineHeight = 0.0f;
        // The pill's line (the lyric / track line), which is drawn in its own heavier format.
        float pillLineHeight = 0.0f;
    };
    TextMetrics Metrics() const;

    // The second the clock currently shows. The frame loop uses this to decide
    // whether a redraw is needed while the island is sitting still: everything
    // else about a resting pill is constant.
    void CurrentSecond(int* second);

    // The text this would currently draw, for assertions and window titles.
    // Writes at most `capacity` wide characters.
    void CurrentTimeText(wchar_t* out, size_t capacity) const;

private:
    // Refreshes the cached formatted strings if the second or the day rolled
    // over. Caching by day-key rather than by value avoids both a strcmp per
    // frame and a stale date at midnight.
    void Refresh();

    IDWriteFactory3*     factory_     = nullptr;
    IDWriteTextFormat*   timeFormat_  = nullptr;

    // The last title-box width written to the log, so the trace reports changes rather than frames.
    // Owned by the text renderer because that is what computes and draws the box.
    float lastTitleWidth_ = -1.0f;

    // What the last icon draw was asked for, so a change is reported once.
    int   lastDrawnCount_ = -1;
    int   lastIslandLeft_ = -9999;
    // The smallest arrival reported so far, so an icon on its way in logs one line per step rather than
    // one per frame while it is settling.
    float lastLeastAppear_ = 1.0f;
    bool  startTraced_ = false;
    int   startTraceLeft_ = -9999;
    float startTraceAlpha_ = -1.0f;
    float startTraceOpacity_ = -1.0f;

    // The mixer's application icons, one per row, kept until the sound worker's revision for that row
    // changes. Decoding a PNG on every frame would be the most expensive thing on the page by a wide
    // margin, and the icon only changes when a different application takes the row.
    struct SoundIconSlot {
        ID2D1Bitmap1* bitmap = nullptr;
        unsigned long long revision = 0;
    };
    SoundIconSlot soundIcons_[SoundPageText::kMaxMixRows];

    // The bitmap for one mixer row's icon. The PNG is decoded and uploaded only when that row's revision
    // changes, so a steady page costs nothing per frame.
    ID2D1Bitmap1* SoundIconAt(ID2D1DeviceContext2* context, int slot, const unsigned char* png,
                              size_t size, unsigned long long revision);

    // The notification sender's icon, uploaded once per revision -- same arrangement as the mixer rows.
    struct CachedBitmapSlot {
        ID2D1Bitmap1* bitmap = nullptr;
        unsigned long long revision = 0;
    };
    CachedBitmapSlot notificationIcon_;
    ID2D1Bitmap1* NotificationIconBitmap(ID2D1DeviceContext2* context, const uint8_t* pixels,
                                        int width, int height, unsigned long long revision);
    // The call strip's icon, cached for the same reason (one decode per call, not one per frame).
    CachedBitmapSlot callIcon_;
    ID2D1Bitmap1* CallIconBitmap(ID2D1DeviceContext2* context, const uint8_t* pixels, int width,
                                 int height, unsigned long long revision);

    IDWriteTextFormat*   dateFormat_  = nullptr;
    IDWriteTextFormat*   appFormat_   = nullptr;
    // THE STATUS STRIP'S OWN LINE, and its own size for its own reason.
    //
    // 12.5 DIP: a size between the card's title and the Windows clock, because this line stands
    // next to the deck's 30 DIP buttons rather than inside a card -- at the pill's 15 it read as a
    // second island, and at the card's 11 the date was unreadable at a glance, which is the one
    // thing this line is for. Semibold, matching the pill's clock: it is a reading, not a label.
    IDWriteTextFormat*   statusStripFormat_ = nullptr;
    // THE PILL'S OWN LINE, one weight heavier than appFormat_.
    //
    // A separate format rather than a heavier appFormat_, because appFormat_ is also what the task
    // buttons, the card's app line and the track times are drawn with -- bolding it would bold all of
    // them, which is not what "字体加粗" was about. Asked for as "现在可以了 美化美化就行了 比如字体加粗".
    IDWriteTextFormat*   pillLineFormat_ = nullptr;
    // The music card's top row, which needs a different size and alignment from the notification
    // header that appFormat_ serves: the source name is left aligned beside the icon, and the
    // volume readout is right aligned at the far end of the same row.
    IDWriteTextFormat*   appRowFormat_  = nullptr;
    IDWriteTextFormat*   volumeFormat_  = nullptr;
    IDWriteTextFormat*   titleFormat_ = nullptr;
    IDWriteTextFormat*   bodyFormat_  = nullptr;

    // The music card's own formats. They are separate from the notification ones
    // because the card is left aligned while every notification line is centred, and
    // because its title is larger. See the comment in Initialize().
    IDWriteTextFormat*   musicTitleFormat_    = nullptr;
    IDWriteTextFormat*   musicSubtitleFormat_ = nullptr;
    // The ♪ that marks a cover-less track's empty cover slot. Its own format rather than the title's: the
    // glyph has to read as a placeholder at a glance, and 20 DIP left it a speck in a 92 DIP square.
    IDWriteTextFormat*   coverNoteFormat_     = nullptr;
    IDWriteTextFormat*   glyphFormat_         = nullptr;
    IDWriteTextFormat*   inputMethodFormat_   = nullptr;
    IDWriteTextFormat*   gearGlyphFormat_ = nullptr;

    SYSTEMTIME time_{};
    wchar_t timeText_[16]  = {};   // L"hh:mm"
    wchar_t dateText_[32]  = {};   // L"<weekday>, <month> <day>"
    wchar_t dateKey_[16]   = {};   // L"yyyy-mm-dd", to detect a day rollover
    bool haveTime_ = false;

    // Last music-card layer timings, for the preview report.
    double lastCardUs_ = 0.0;
    double lastCardBackgroundUs_ = 0.0;
    double lastCardArtworkUs_ = 0.0;
};

} // namespace island
