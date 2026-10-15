// =====================================================================
//  BarState.h -- the bar's own state, in one place instead of scattered through the frame loop
//
//  WHY THIS EXISTS. Until 2026-10-15 every one of these values was a local at the top of RunBar -- a function
//  about 5,000 lines long. Nothing said which locals were STATE (they survive from frame to frame and belong
//  to the bar as a whole) and which were merely computed inside one frame; a reader had to infer it from how
//  far apart the uses were, and a writer could not see the whole picture before changing one of them.
//
//  THIS IS A MIGRATION IN PROGRESS, NOT THE FINISHED THING. The groups move one named group at a time, each
//  with a full rebuild and the self-test, so that a mistake is always attributable to the group that just
//  moved. Slice 1 (the tray card and its menu) was removed with the tray itself on 2026-10-15; what stands
//  here now is the panel's own level state and the Start card's.
//
//  WHAT BELONGS HERE: state that outlives a frame and that more than one phase of the loop reads. What does
//  not: values computed inside a single frame (a rect, a flag that means "this frame drew something"), and
//  objects that own resources (the window, the renderer, the media session) -- those get their own named
//  group when their turn comes, because building them is a sequence rather than a set of fields.
// =====================================================================
#pragma once

#include <cstdint>   // uint8_t: the account picture the Start card's footer draws
#include <memory>
#include <vector>    // the Start card's lists, its icon pointers and its per-item springs

#include <windows.h>

#include "app/PlayerWindow.h"    // PlayerWindowInfo: the player that never publishes a session
#include "domain/CardLayout.h"   // PanelLevel: which level of the panel's card is showing
#include "domain/Motion.h"       // IslandState: the state whose name the slow line reports
#include "domain/TaskStripLayout.h"   // kMaxTaskButtons: one spring per icon in the row
#include "media/Lyrics.h"        // kLyricLineCapacity, TrackPosition
#include "media/MusicSession.h"  // kMusicTitleCapacity, kMusicArtistCapacity
#include "platform/WindowList.h" // kMaxTaskWindows, kTaskTitleCapacity: the row's titles are copied from it

namespace island {

// One spring per item of the Start card's three lists, so a highlight fades in and out instead of switching
// (see FrameState::pinWashes). Sized to the list, which is a handful of tiles, up to 121 picker rows and five
// power rows. It is declared here rather than nested inside BarState because the loops that step the springs
// name the type in a lambda signature (see main.cpp).
struct WashSlot {
    float value = 0.0f;
    float velocity = 0.0f;
};
    // --- the application row, the status line, and the hover preview (slice 6) --------------------------
    //
    // Moved 2026-10-15, comments and all. Three features share this block and they share it because they
    // drive EACH OTHER: the row decides how many icons there is room for, the status line moves beside the
    // deck when the pill's own line is taken (by a lyric, a message or a call), and the preview waits on the
    // pointer resting on an icon in that row. The titles are COPIED into this struct's array because the
    // window list refreshes between frames and a frame pointing into the list would read freed entries, and
    // the handles are frozen in the same way -- the click resolves through them and not through the live list.
struct BarState {

    // --- the Start card's own state (slice 5) --------------------------------------------------------
    //
    // Moved 2026-10-15, comments and all. The pins are the user's, from a text file beside the bar (see
    // StartApps); the account picture and name are read once, because they are the card's footer and change
    // about as often as the user changes his picture. The six wash vectors are one spring per item of the
    // card's three lists, plus the values they were drawn at, and the last three fields exist so a CHANGE in
    // where the pointer is can ask for a redraw (the card's highlight is not a state the loop already watches).
    bool startPageOpen = false;
    int startPageState = 0;        // 0 = the pins, 1 = the picker, 2 = the power list
    int startPickerScroll = 0;
    int startPowerConfirm = -1;
    std::vector<uint8_t> accountAvatarPng;
    wchar_t accountName[64] = {};
    // Rebuilt per frame for the frame's icon pointers (see below), and kept here so the storage outlives
    // the frame that points into it.
    std::vector<const uint8_t*> startIconPointers;
    std::vector<size_t> startIconSizes;
    // One spring per item of the Start card's three lists, so a highlight fades in and out instead of
    // switching: see FrameState::pinWashes. Sized to the list, which is a handful of tiles, up to 121
    // picker rows and five power rows.
    std::vector<WashSlot> startPinWashes;
    std::vector<WashSlot> startRowWashes;
    std::vector<WashSlot> startPowerWashes;
    std::vector<float> startPinWashValues;
    std::vector<float> startRowWashValues;
    std::vector<float> startPowerWashValues;
    bool startWashAnimating = false;
    int startPinCount = 0;
    int startAppCount = 0;
    // The pointer's position in the card, remembered so a CHANGE can ask for a redraw: the card's
    // highlight is not a state the loop already watches (see needDraw), and without this the highlight
    // moved only on the next clock tick -- up to a second late.
    int lastStartHitTarget = 0;
    int lastStartHitIndex = -2;
    int lastStartScrollDip = -1;
    // --- the system panel, and the 声音输出 page inside it (slice 2) ---------------------------------
    //
    // Moved 2026-10-15. These were locals at the top of RunBar with exactly these comments; what they
    // are for is written where they are now, and one thing is worth stating at the top: the panel is a
    // CARD that is on screen for the length of its own exit, so the LEVEL it was opened with is also
    // the level it leaves with (see PanelLevel).
    // Whether the system panel is open. It is a TOGGLE, like the shell flyout it mirrors.
    bool systemPanelOpen = false;   // RunBar sets it from its own argument (see main.cpp)
    // Which level of the panel's card is showing -- the tiles or the 声音输出 page. Held for the card's
    // whole life, like the owner: the card is on screen for the length of its own exit, so the level it was
    // opened with is also what it leaves with (see NextPanelLevel, and the places that read it: the
    // content, the page flag, and where the card stands).
    island::PanelLevel panelLevel = island::PanelLevel::Tiles;
    // Which page of tiles the panel is showing. The shell's flyout pages through them; a page is a window over
    // the whole set, and the arrows and dots are the controls for it.
    int systemPanelPage = 0;
    // Which page to start on, for looking at the second page without a pointer. Diagnostic only.
    int startPanelPage = 0;
    // The volume slider, while the pointer has the mouse button down on it.
    //
    // A press jumps the handle to where the click landed, and HOLDING it has to keep moving the handle --
    // a slider that only jumped once per click is not a slider, and the shell's own control follows the
    // pointer. The state lives out here because the click arrives through the message queue while the
    // movement is read from the pointer each pass.
    bool panelSliderDrag = false;
    // The fraction last actually pushed to the device, so a drag does not re-send the same value every frame.
    // The handle is drawn from the live reading either way, so nothing is lost by skipping the repeats.
    float lastAppliedVolumeFraction = -1.0f;
    float lastAppliedMixerFraction = -1.0f;
    float lastLoggedVolumeFraction = -1.0f;
    // --- the brightness row (DDC/CI) ---
    //
    // `brightnessAvailable` is only ever made true by a successful read, the same rule as the Bluetooth
    // probe: a monitor that does not answer must not leave a row on the panel claiming a level. It is
    // re-checked whenever the bar moves to another display, because a laptop panel answers and the monitor
    // beside it may not.
    bool brightnessAvailable = false;
    int brightnessPercent = 0;
    // Which slider a drag belongs to: 0 the volume, 1 the brightness (the same numbering as
    // SystemPanelHit::index, which is where a drag starts).
    int panelSliderDragRow = 0;
    // The same two clocks the volume keeps, for the same reasons: DDC/CI writes are not free (they go over
    // the video cable to the monitor), so they are pushed on a step and never more often than this.
    int lastAppliedBrightnessPercent = -1;
    int lastLoggedBrightnessPercent = -1;
    unsigned long long brightnessWriteNotBefore = 0;
    unsigned long long panelBrightnessEchoUntil = 0;
    // Set by a press that started a slider drag, taken by the release that ends it: see the Slider branch.
    bool sliderPressHandled = false;
    // The volume the sliders are DRAWN at: the live reading, chased over a tenth of a second.
    //
    // "没有动画过渡" -- the handle used to be drawn at whatever the reading was, so every change that did not
    // come from a drag (a click on the track, a keyboard volume key, the shell's own flyout) was a jump with
    // no movement in it. The chase is on the DRAWING only: a drag sets the device from the pointer's own
    // position and is drawn from the same place, so following the pointer stays 1:1 and the pointer never
    // feels like it is dragging something heavy.
    //
    // The music card's own bar was the other reader of this value; it was removed on request ("把音乐卡片里
    // 右上角的喇叭图标去掉吧"), and this stays because the SYSTEM PANEL's slider is drawn at it.
    float shownVolume = -1.0f;
    // How far each slider's handle has grown (0..1), chased towards "this row is the one in hand" for the
    // same reason the volume itself is chased: the size is an animation, and a handle that jumped 2 DIP in
    // one frame reads as a glitch rather than as the pointer arriving.
    float sliderHandleGrowth[2] = {0.0f, 0.0f};
    // Until when the volume's percentage stays on screen. Set for a moment after any change, so the number
    // is visible for a moment even when the slider was clicked rather than dragged.
    unsigned long long panelVolumeEchoUntil = 0;
    // The "声音输出" page: the shell's second level off the volume row, with the output devices, the spatial
    // sound row and the per-application mixer. A page INSIDE the card rather than a second card.
    bool soundPageOpen = false;
    float soundPageScroll = 0.0f;
    // The mixer's own sliders, while one of them has the mouse button down. -1 is none; row 0 is the device's
    // master volume and the rows after it are the applications. The PID is held rather than the row number
    // alone, because the rows are re-read once a second and a row can be a different application by the time
    // the pointer comes back to it -- the one the drag started on is the one it must keep moving.
    int mixerSliderDragRow = -1;
    unsigned long mixerSliderDragPid = 0;
    // Diagnostics only: hold the panel open, and the sound page with it, so a photograph of either is not at
    // the mercy of a click landing somewhere else on the machine. See ISLANDBAR_PANEL_LOCK.
    bool panelLocked = false;
    bool lockedSoundPage = false;

    // --- what is playing, its lyrics, and the channels that bring news in (slice 3) ------------------
    //
    // Moved 2026-10-15, comments and all. TWO KINDS OF THING ARE IN HERE and it is worth knowing which is
    // which: the MEDIA state (what the pill is showing, and the strings the renderer is given) and the
    // POLL CLOCKS for the three channels that have no event to listen to (the toast database, WeChat, and
    // the sound of a call). The objects that own those channels -- MusicSession, ToastReader, the lyric
    // provider -- stay in RunBar for now: building them is a sequence, and they get their own slice.
    unsigned long long lastToastPoll = 0;
    unsigned long long lastWeChatPoll = 0;
    // When the call strip's meter was last redrawn. See the redraw reasons below.
    unsigned long long lastCallMeterMs = 0;
    // The WeChat sound channel's own clock: 200 ms, because the edge it looks for lasts about a second.
    unsigned long long lastWeChatSoundPoll = 0;
    // >0: fire one play/pause on the media session after this long. The transport is otherwise reachable
    // only by clicking the card, and a script's click cannot reach it (the window's own synthetic click
    // lands wherever the REAL cursor is), so this is the only way to exercise the request path and the
    // promptness of the button's state without a person.
    DWORD autoTransportMs = 0;
    bool transportForced = false;
    // Seconds since the line on screen changed, for its entrance (see LyricLineRollFor). Starts past
    // the end of the entrance so nothing fades until a real line change resets it.
    float lyricLineAge = 1.0f;
    // The line on screen and the one it replaced, copied rather than pointed at.
    //
    // The cross-fade lasts several frames, so the line being replaced has to survive past the frame that
    // changed it, and the provider's own text cannot be borrowed: a track change replaces that whole store.
    char  shownLyricText[island::kLyricLineCapacity] = {};
    char  replacedLyricText[island::kLyricLineCapacity] = {};
    float replacedLyricScrollDip = 0.0f;
    // The TRACK the lyric state above belongs to. The session's revision is not that question -- it
    // moves for a position update as well -- so the track is compared by name (see the music block).
    char  drawnTrackTitle[island::kMusicTitleCapacity] = {};
    char  drawnTrackArtist[island::kMusicArtistCapacity] = {};
    // --- the window-title source, and which of the two the pill is showing ---------
    //
    // ONE source at a time: the media session if there is one, otherwise the player's own window title
    // (see PlayerWindow.h). `active*` is that decision, taken once per frame in the music block below
    // and read by everything downstream -- the lyric line, the marquee, the level meter's frames and
    // the pill's fallback line -- so no two of them can disagree about which track is up.
    island::PlayerWindowInfo playerWindowNow;
    unsigned long long lastPlayerWindowRevision = 0;
    // The stopwatch for a track whose position nobody reports: it runs only while the song plays, and
    // it is reset when the track changes. Advanced in the frame loop (see TrackPositionFor).
    double trackPlayedMs = 0.0;
    // This frame's distance into the track, and whether the session supplied it. ONE answer, decided
    // once per frame and read by the lyric line, the card's printed time and its bar, and the
    // once-a-second redraw -- so none of them can disagree about where the song is.
    island::TrackPosition track;
    bool activePresent = false;
    bool activeFromSession = false;
    bool activePlaying = false;
    const char* activeTitle = "";
    const char* activeArtist = "";

    // --- the diagnostics, the full-screen check, and what the loop reports about itself (slice 4) ---------
    //
    // Moved 2026-10-15. Read-once values that come from the environment do NOT live here: they are `const`
    // for the life of the process and are not state. ISLANDBAR_NO_FULLSCREEN is the one a reader would expect
    // to find below and it is deliberately not here -- it stays a local in RunBar, which is where the parsing
    // of a multi-line GetEnvironmentVariableW call belongs.
    //
    // What is here: the pointer edge as the LAST pass saw it (the edge is what matters -- a press that
    // starts outside the bar puts the panel away and a button already held does not), the one-shot
    // synthetic click a script needs because it cannot click this bar, the full-screen trio (polled four
    // times a second: GetForegroundWindow, GetWindowRect and a class name, see kFullscreenCheckMs), and
    // the two values that let the loop write a line when what it SHOWS has changed rather than one per
    // frame.
    // Whether the left button was down the last time the frame loop looked. The edge is what matters: a
    // press that starts outside the bar puts the panel away, and a button already held does not.
    bool leftWasDown = false;
    // Diagnostics: a one-shot synthetic click, so the outside-click dismissal can be exercised on a machine
    // whose integrity levels make a script's own input invisible to the bar. See ISLANDBAR_CLICK_AT.
    long injectClick_ = 0;
    long injectClickY_ = 0;
    unsigned long long injectClickAtMs_ = 0;
    // The full-screen state as of the last frame, so the transition is acted on once rather
    // than every frame.
    bool fullscreenLast = false;
    // ... and whether a full-screen APPLICATION is in front, measured by us rather than announced by the
    // shell (see FullScreenAppInFront). Polled: three cheap calls, four times a second.
    bool fullscreenAppInFront = false;
    unsigned long long lastFullscreenCheck = 0;
    int lastReportedButtons = -1;
    island::IslandState lastReportedState = island::IslandState::Collapsed;
    wchar_t taskTitles[island::kMaxTaskWindows][island::kTaskTitleCapacity] = {};
    // How far each icon button's growth has run, 0 to 1. Held across frames because it is an
    // animation, not a state: the button opens over several frames rather than snapping.
    //
    // ONE value, not two. An earlier version animated the width and the push-along separately, and
    // while the two disagreed the widened button's right edge ran past the next button's left edge
    // and painted over it -- which is the "the icons are covered / it did not make room" report that
    // survived two rounds of diagnosis. Both the width and the push now come from this value, so
    // they cannot disagree.
    float taskHoverAmount[island::kMaxTaskWindows] = {};
    // ... and how fast each of them is moving, which is what makes it a spring rather than a ramp: the
    // value alone cannot settle (see RowOpenStep).
    float taskHoverVelocity[island::kMaxTaskWindows] = {};
    // ... and the same for the two washes on the bar's own buttons (Start and the system panel's toggle).
    // They used to switch on in a single frame -- see FrameState::startWash.
    float controlWash[2] = {};        // 0 = Start, 1 = system toggle
    float controlWashVelocity[2] = {};
    // Set while any of those three is still moving, so the loop keeps drawing until they settle.
    bool controlWashAnimating = false;
    // True while any button is mid-transition, so the loop keeps drawing until it settles.
    bool iconAnimating = false;
// Whether any icon in the row is still arriving or leaving. Set inside the draw (where the row is
// updated) and read by the next frame's redraw decision, so the arrival animation keeps running.
bool rowAppearAnimating = false;

// --- the date / weekday / time / weather line -----------------------------
//
// How far the BESIDE-THE-DECK copy of it has faded in, 0 to 1, held across frames because it is an
// animation and not a state. It follows the one rule that decides where the line goes: while the
// pill's own line is free the line is INSIDE the pill (no fade -- the pill is always there), and
// while the pill's line is taken (the lyric, a message, a call) the same words stand beside the
// deck and fade in. See StatusPlacementFor for the rule and StatusStripFadeStep for the fade.
float statusStripOpacity = 0.0f;
// True while that fade is in flight, which is what asks for the next frame. Set inside the draw and
// read by the next pass, like the two arrival flags above.
bool statusStripAnimating = false;
// The weather reader's revision as of the last draw, so a new reading is a redraw reason of its own:
// the reading arrives on a worker, a few seconds after the bar has settled.
unsigned long long lastWeatherRevision = 0;
// Where the reading lives for the frame to point at: the word and the temperature, apart, because
// the line gives them their own sizes and a gap between them. Members of this scope rather than the
// reader's own storage, because a reading can be replaced mid-frame -- replace it and the frame
// would print half of one reading and half of the next.
wchar_t statusSkyText[32] = {};
wchar_t statusTemperatureText[32] = {};
// One entry per window the row is drawing or fading out: an icon's membership of the row and how far
// in it is (see TaskIconAppearStep).
struct RowSlot {
    HWND  handle = nullptr;
    float appear = 0.0f;
};
RowSlot rowSlots[island::kMaxTaskButtons] = {};
int rowSlotCount = 0;
    // How many application icons the row is currently showing. The click handler bounds
    // itself by this and not by the window list, which can hold windows the row had no room
    // for -- activating one of those would switch to a window with no icon on screen.
    int  taskWindowsShown = 0;
    // The window behind each drawn icon, frozen when the frame was built.
    //
    // The click resolves through THIS and not through the live list. The list is
    // re-enumerated every 500 ms and its order can differ from the one on screen, so
    // indexing into it at click time could activate a window the user did not point at.
    HWND taskHandles[island::kMaxTaskWindows] = {};
    // --- the hover preview's own state ---------------------------------------
    //
    // Which window the preview is FOR, and when the pointer arrived on its icon. The delay is the
    // whole reason this exists: the pointer crosses the row on its way somewhere, and a preview that
    // appeared on the first frame would flash for every icon it passed.
    HWND previewTarget = nullptr;
    unsigned long long previewSinceMs = 0;
    // Reported once each, because "the preview never appeared" and "the compositor refused this window"
    // are different answers and only the log can tell them apart.
    bool previewReported = false;
    int  previewLastRefusals = 0;
    // What was last written to the trace, so it reports changes rather than every frame.
    int   lastReportedHoveredIcon = -2;
    float lastReportedHoverAmount = -1.0f;
    int   lastReportedPointed = -2;
    bool  lastReportedAnimating = false;

    // The last music line written to the log, so that "what is playing" is reported when it CHANGES rather than
    // whenever the session tells the bar something -- which is about once a second. The session's revision moves
    // for every position update, so the reader that logs on a revision change was writing the same sentence
    // every second for as long as the bar ran; the identity below is what a reader actually wants to see move.
    char lastLoggedMusicApp[island::kMusicAppCapacity] = {};
    char lastLoggedMusicArtist[island::kMusicArtistCapacity] = {};
    char lastLoggedMusicTitle[island::kMusicTitleCapacity] = {};
    island::PlaybackState lastLoggedMusicState = island::PlaybackState::Closed;
    bool haveLoggedMusic = false;
    unsigned long long lastTaskRefresh = 0;
};

} // namespace island
