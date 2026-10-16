// =====================================================================
//  ChecksInput.cpp -- see selftest/Checks.h for why the self-test is split like this
//
//  The includes are the ones the single file carried; pruning them per file is a smaller,
//  separate exercise and is deliberately not mixed into the move.
// =====================================================================
// =====================================================================
//  Diagnostics.cpp
//  The runtime self-test and the native-taskbar probe.
//
//  Every assertion below is deliberately phrased as a question about what the
//  system actually did, not about what was requested. The interesting failures
//  in this area of Windows are the silent ones -- a per-monitor-v2 awareness
//  that never took effect, a z-order that quietly dropped out of topmost, a
//  swap chain that presented black outside the pill -- and none of them raise
//  an HRESULT.
//
//  The probe's report goes to the log file as well as to stdout, because
//  stdout has been lost on this machine three separate times.
// =====================================================================
#include "selftest/Diagnostics.h"

#include <windows.h>
#include <shellapi.h>   // APPBARDATA / ABM_* for the native-taskbar probe

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>   // strtoul: unpacking the hex of a captured registration in the tray checks
#include <vector>
#include <cwchar>
#include <string>

#include "domain/Geometry.h"
#include "core/Log.h"
#include "domain/MiniJson.h"
#include "domain/TrayRegistration.h"
#include "domain/TrayLedger.h"     // which registrations the shell is holding (see the tray checks)
#include "platform/TrayRing.h"     // the channel the module publishes registrations on
#include "domain/CardLayout.h"
#include "domain/StatePolicy.h"
#include "domain/TrayEncoding.h"   // the only place a tray click is encoded (see its header)
#include "media/Lyrics.h"
#include "app/AppBar.h"
#include "domain/TaskStripLayout.h"
#include "platform/WindowList.h"
#include "media/MusicSession.h"
#include "app/PlayerWindow.h"
#include "media/WicImage.h"
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/ToastStore.h"
#include "services/WeChatWatch.h"
#include "shell/Weather.h"
#include "platform/ThumbnailPreview.h"
#include "app/IslandWindow.h"
#include "platform/Monitor.h"
#include "platform/NativeTaskbar.h"   // the three answers to "what happens to the shell's taskbar"
#include "shell/SystemToggles.h"
#include "render/Theme.h"
#include "render/GlassMaterial.h"
#include "shell/Weather.h"   // SkyMarkForCode: the word and the mark come from the same WMO code
#include "domain/Motion.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"
#include "shell/TrayLearn.h"   // CountCoveredIcons: how much of the tray a press can reach (see the tray checks)

#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

void TestHoverSequence() {
    Section(L"hover sequence (frame-loop policy)");

    IslandWindow window;
    const MonitorGeometry monitor = QueryMonitor(nullptr);
    if (!window.Create(monitor, /*show=*/false)) {
        Check(false, L"window created for the hover sequence");
        return;
    }

    const IslandState resting = IslandState::MusicStrip;

    // Settle into the lyric strip, as the loop does once music is playing.
    window.SetState(resting);
    for (int i = 0; i < 60; ++i) window.Tick(0.05f);
    Check(!window.IsAnimating(), L"the resting strip settles");

    // A pointer entry asks for the CARD, with no intermediate shape.
    //
    // It used to ask for Hover, which meant the island spread sideways to 720 DIP and then
    // contracted into the 400 DIP card -- an out-and-back the user reported as a stutter.
    // The card's width is narrower than the hover shape's, so the detour was not even in
    // the direction of travel.
    window.SetState(IslandState::Expanded);
    Check(window.State() == IslandState::Expanded,
          L"a pointer entry selects the card directly");

    for (int i = 0; i < 60; ++i) window.Tick(0.05f);

    wchar_t detail[200];
    std::swprintf(detail, 200, L"strip -> %ls (width %.0f DIP)", StateName(window.State()),
                  window.CurrentBox().rect.right - window.CurrentBox().rect.left);
    Check(window.State() == IslandState::Expanded && !window.IsAnimating(),
          L"the card is what one pointer entry produces", detail);

    // Leaving returns to the resting strip, not the plain pill: the lyrics are what the
    // user asked to see while music plays.
    window.SetState(resting);
    for (int i = 0; i < 60; ++i) window.Tick(0.05f);
    Check(window.State() == resting, L"leaving returns to the lyric strip");

    // And the same path from the plain pill, so both resting shapes reach the card.
    {
        IslandWindow second;
        if (second.Create(monitor, /*show=*/false)) {
            second.SetState(IslandState::Collapsed);
            for (int i = 0; i < 60; ++i) second.Tick(0.05f);
            second.SetState(IslandState::Expanded);
            for (int i = 0; i < 60; ++i) second.Tick(0.05f);
            std::swprintf(detail, 200, L"pill -> %ls", StateName(second.State()));
            Check(second.State() == IslandState::Expanded,
                  L"the card is what one pointer entry produces from the plain pill too",
                  detail);
            second.Destroy();
        } else {
            Check(false, L"second window for the pointer path");
        }
    }

    window.Destroy();
}

void TestPointerStateCycle() {
    Section(L"pointer state cycle (settled-shape policy)");

    IslandWindow window;
    const MonitorGeometry monitor = QueryMonitor(nullptr);
    if (!window.Create(monitor, /*show=*/false)) {
        Check(false, L"window created for the pointer cycle");
        return;
    }

    const auto settle = [&]() {
        for (int i = 0; i < 80; ++i) window.Tick(0.05f);
        return !window.IsAnimating();
    };

    // The frame loop's policy, called -- NOT copied.
    //
    // The first version of this test reproduced the conditions inline, and it then passed
    // against a build with the bug still in place, because the test's copy was correct
    // while main.cpp's was not. A test that restates a rule instead of calling it proves
    // nothing about the rule. This calls island::NextShape, the same function main.cpp
    // calls, so a regression cannot hide behind a stale copy.
    const auto applyPolicy = [&](bool pointerInside, IslandState resting, bool active) {
        const IslandState next = NextShape(window.State(), resting, pointerInside, active);
        if (next != window.State()) window.SetState(next);
    };

    // The rule itself, exercised as a pure function over every combination.
    //
    // The expectations changed when the hover state stopped being an empty waypoint and became the
    // TASKBAR. Previously the pointer opening the card was the whole story; now a hover shows the
    // list of open windows, and only a notification opens the card. The table is the specification,
    // so it is rewritten here rather than left describing behaviour that no longer exists -- a test
    // that disagrees with the code is worse than no test, because it makes the failing build look
    // like the correct one.
    {
        struct Case {
            const wchar_t* what;
            IslandState from;
            IslandState resting;
            bool pointerInside;
            bool notificationActive;
            IslandState expected;
        };
        const Case cases[] = {
            // The pointer inside shows the TASKBAR from either resting shape.
            {L"pointer in from the plain pill", IslandState::Collapsed, IslandState::Collapsed,
             true, false, IslandState::Hover},
            // ... and the CARD while something is playing: the strip rests there exactly while a session
            // reports Playing, and the pointer on the bar is the gesture that asks for the details.
            {L"pointer in from the lyric strip", IslandState::MusicStrip, IslandState::MusicStrip,
             true, false, IslandState::Expanded},
            {L"pointer still in on the taskbar while playing", IslandState::Hover,
             IslandState::MusicStrip, true, false, IslandState::Expanded},
            {L"adopt the card from the strip", IslandState::MusicStrip, IslandState::MusicStrip, true,
             false, IslandState::Expanded},

            // Leaving returns to whichever resting shape applies.
            {L"leaving the taskbar", IslandState::Hover, IslandState::MusicStrip, false, false,
             IslandState::MusicStrip},
            {L"leaving the taskbar to the pill", IslandState::Hover, IslandState::Collapsed, false,
             false, IslandState::Collapsed},
            {L"leaving the card", IslandState::Expanded, IslandState::MusicStrip, false, false,
             IslandState::MusicStrip},

            // A notification outranks the pointer, and it is the STRIP it asks for rather than the card:
            // the island widens and shows the message inside it, from wherever the pointer happens to be.
            // (The card is one hover away, which is the case that follows.)
            {L"notification from the pill", IslandState::Collapsed, IslandState::Collapsed, false,
             true, IslandState::NoticeStrip},
            // THE STRIP WITH THE POINTER INSIDE TOO. These two expected the details card, which was
            // removed on request ("这个卡片不需要"): the strip carries the line, and hovering is not a
            // second way to read a message.
            {L"notification while the pointer is in", IslandState::Hover, IslandState::MusicStrip,
             true, true, IslandState::NoticeStrip},
            {L"notification replaces the taskbar", IslandState::Hover, IslandState::Collapsed, true,
             true, IslandState::NoticeStrip},
            // ... and with the pointer AWAY the strip is what shows, which is the shape a message takes when
            // nobody is looking at the bar.
            {L"notification with the pointer away", IslandState::Expanded, IslandState::MusicStrip,
             false, true, IslandState::NoticeStrip},

            // A resting shape adopts a new rest (playback started or stopped).
            {L"playback started", IslandState::Collapsed, IslandState::MusicStrip, false, false,
             IslandState::MusicStrip},
            {L"playback stopped", IslandState::MusicStrip, IslandState::Collapsed, false, false,
             IslandState::Collapsed},
            // Already at rest: nothing to do.
            {L"already at rest", IslandState::MusicStrip, IslandState::MusicStrip, false, false,
             IslandState::MusicStrip},

            // A card whose reason has gone lets go of the state, pointer on the bar or not.
            //
            // Added for the report "the first press is smooth; pressing again to put it away and to
            // open it again has no animation". With the request withdrawn and the pointer sitting on
            // the control it was pressed with, no rule matched Expanded: the state stayed on a card
            // that nothing wanted, the panel vanished as a flag (nothing to animate), and the next
            // press had no transition either because the state had never moved.
            {L"the card's reason has gone", IslandState::Expanded, IslandState::Collapsed, true,
             false, IslandState::Hover},
            // ... but NOT while something is playing: there the card is what the pointer is asking for, so
            // it stays up and becomes the media card rather than collapsing and reopening.
            {L"the card's reason has gone, but music is playing", IslandState::Expanded,
             IslandState::MusicStrip, true, false, IslandState::Expanded},
        };

        int mismatches = 0;
        const wchar_t* firstWrong = L"";
        for (const Case& c : cases) {
            if (NextShape(c.from, c.resting, c.pointerInside, c.notificationActive) !=
                c.expected) {
                if (mismatches == 0) firstWrong = c.what;
                ++mismatches;
            }
        }
        wchar_t detail[192];
        std::swprintf(detail, 192, L"%d of %d wrong, first: %ls", mismatches,
                      static_cast<int>(sizeof(cases) / sizeof(cases[0])), firstWrong);
        Check(mismatches == 0, L"the state policy gives the expected shape for every case",
              detail);
    }

    // The same rule with the panel in it, which the five-argument form cannot express.
    {
        PointerContext c;
        c.shape = IslandState::Expanded;
        c.resting = IslandState::Collapsed;
        c.pointerInside = true;
        c.iconCount = 8;
        c.systemPanelOpen = true;
        Check(NextShape(c) == IslandState::Expanded,
              L"the open panel holds the card against the pointer");
        c.systemPanelOpen = false;
        // The panel's toggle is on the DECK, so the pointer pressing it is off the island: the card is given
        // up by the same rule that handles an icon or the empty stretch of bar (7.91/7.92), and it returns to
        // the resting shape rather than to the hover shape. The two are the same rectangle, so this is about
        // the state, not about the picture -- and the resting shape is the one that keeps the lyric strip.
        Check(NextShape(c) == IslandState::Collapsed,
              L"the card lets go once the panel is asked to close");
        c.systemPanelOpen = true;
        c.notificationActive = true;
        // A notification no longer asks for the card at all -- it is the strip (see the policy table).
        Check(NextShape(c) == IslandState::Expanded,
              L"the panel still holds the card while a notification is up");

        // The pointer on an ICON asks about that window, not about the island: the icon widens and the
        // shape stays. With music playing this was the case that opened the media card from the row --
        // reported as "the card should come up on this area [the pill], not on the icons".
        {
            PointerContext icon;
            icon.shape = IslandState::MusicStrip;
            icon.resting = IslandState::MusicStrip;      // a playing session rests as the strip
            // ... and a playing session is a SESSION. The card asks for that, not for the strip: the
            // strip is also what a player read from its own window title rests as, and that source has
            // nothing to put on a card (see PlayerWindow.h). This frame is the session case, so it says
            // so -- the contract changed with the rule, and the test frame has to change with it.
            icon.musicPresent = true;
            icon.pointerInside = true;
            icon.pointerOnIsland = false;                // an icon is on the bar, not on the island
            icon.iconCount = 8;
            icon.hoveredIcon = 3;
            Check(NextShape(icon) != IslandState::Expanded,
                  L"the pointer on an icon does not open the card");
            icon.hoveredIcon = -1;
            icon.pointerOnIsland = true;                 // ... and the pill itself does
            Check(NextShape(icon) == IslandState::Expanded,
                  L"... but the same pointer off the icons does");
            // A message still wins over the row: it must not be hidden by where the pointer happens to be.
            icon.hoveredIcon = 3;
            icon.notificationActive = true;
            Check(NextShape(icon) == IslandState::NoticeStrip,
                  L"a notification shows the strip whatever the pointer is on");

            // And a card that is ALREADY up is given away when the pointer moves off the pill -- onto an
            // icon, or onto one of the deck's buttons. The card belongs to the pill.
            {
                PointerContext up;
                up.shape = IslandState::Expanded;
                up.resting = IslandState::MusicStrip;
                // A session, for the same reason as the frame above.
                up.musicPresent = true;
                up.pointerInside = true;
                up.pointerOnIsland = false;          // the pointer has left the pill
                up.iconCount = 8;
                up.hoveredIcon = 3;
                Check(NextShape(up) == IslandState::MusicStrip,
                      L"a card is given up when the pointer moves onto an icon");
                up.hoveredIcon = -1;
                up.pointerOnDeck = true;
                Check(NextShape(up) == IslandState::MusicStrip,
                      L"a card is given up when the pointer moves onto a deck button");
                up.pointerOnDeck = false;
                // The empty stretch past the icon row is the case the user reported: on the bar, not on the
                // island, and it used to open the media card.
                up.iconCount = 0;
                Check(NextShape(up) == IslandState::MusicStrip,
                      L"a card is given up when the pointer moves onto empty bar");
                up.pointerOnIsland = true;
                Check(NextShape(up) == IslandState::Expanded,
                      L"... and on the pill itself the card stays");
                // On the CARD is the pill's territory too: the pointer has to be able to WALK from the pill
                // down onto the card, or its transport buttons cannot be pressed. Taking "not on the island"
                // literally was the mistake -- reported as "the card disappears when I move the mouse onto
                // it, so its buttons cannot be used".
                up.pointerOnIsland = false;
                up.pointerInCardColumn = true;
                Check(NextShape(up) == IslandState::Expanded,
                      L"the card keeps itself while the pointer is on the card");
                up.pointerInCardColumn = false;
                // AND A TRACK THAT CAME FROM A WINDOW TITLE IS NOT A CARD, one field different from the
                // frame above: the line is up (`resting` is the strip) but there is no session, and
                // every field the card is made of -- cover, position, three transport keys -- belongs to
                // the session. Asked in these terms so that the two sources cannot be confused again.
                {
                    wchar_t detail[224];
                    PointerContext lyricOnly;
                    lyricOnly.shape = IslandState::MusicStrip;
                    lyricOnly.resting = IslandState::MusicStrip;
                    lyricOnly.pointerInside = true;
                    lyricOnly.pointerOnIsland = true;
                    lyricOnly.musicPresent = false;
                    std::swprintf(detail, 224,
                                  L"a track from a window title, pointer on the pill -> %ls",
                                  StateName(NextShape(lyricOnly)));
                    Check(NextShape(lyricOnly) != IslandState::Expanded,
                          L"a track read from a player's window title does not offer the media card",
                          detail);
                    lyricOnly.musicPresent = true;
                    Check(NextShape(lyricOnly) == IslandState::Expanded,
                          L"... while the same track WITH a session still does", detail);
                }
                // The two explicit requests keep it: a message must not be hidden by the pointer, and the
                // panel was opened deliberately.
                up.pointerOnIsland = false;
                up.pointerOnDeck = true;
                up.notificationActive = true;
                // The strip, not the card: a deck button is not the island, so the message shows in the
                // island rather than taking over the screen -- and the deck is pushed clear of it anyway.
                Check(NextShape(up) == IslandState::NoticeStrip,
                      L"a notification shows in the island even with the pointer on a deck button");
                up.notificationActive = false;
                up.systemPanelOpen = true;
                Check(NextShape(up) == IslandState::Expanded,
                      L"the open panel keeps its card with the pointer on a deck button");
                // ... and off the card, a deck button asks for nothing at all: the two rules that used to
                // fight (give the card up / open it again) must agree, or the bar flaps between the card and
                // the strip. Measured: six changes a second.
                up.systemPanelOpen = false;
                up.shape = IslandState::MusicStrip;
                up.pointerOnDeck = true;
                up.pointerOnIsland = false;
                Check(NextShape(up) == IslandState::MusicStrip,
                      L"a deck button under the pointer leaves the bar at rest");
                up.hoveredIcon = 3;
                Check(NextShape(up) == IslandState::MusicStrip,
                      L"... and so does an icon under the pointer");
            }
        }
    }

    // Start settled at rest with music playing.
    window.SetState(IslandState::MusicStrip);
    Check(settle(), L"the strip settles before the cycle");

    // The pointer arrives: the loop's edge handler selects Hover, which IS the taskbar.
    window.SetState(IslandState::Hover);
    Check(settle(), L"hover settles after the pointer arrives");

    // With music PLAYING the settled-shape policy turns that hover into the CARD.
    //
    // The resting shape here is the lyric strip, which is what a *playing* session rests as, and the
    // detail view is one hover away -- the behaviour the README has promised since the first phase. (The
    // table above covers the same rule for every combination; this goes through the window, so it also
    // covers the motion being asked for the card.)
    applyPolicy(/*pointerInside=*/true, IslandState::MusicStrip, /*active=*/false);
    Check(window.State() == IslandState::Expanded,
          L"a settled hover while music plays opens the media card");
    Check(settle(), L"the media card settles");

    // A notification keeps it there: a message must not be displaced by the pointer.
    applyPolicy(/*pointerInside=*/true, IslandState::MusicStrip, /*active=*/true);
    Check(window.State() == IslandState::NoticeStrip,
          L"a notification is the strip, not the card, even while the media card was open");

    // Leaving closes it back to the lyric strip.
    applyPolicy(/*pointerInside=*/false, IslandState::MusicStrip, /*active=*/false);
    Check(window.State() == IslandState::MusicStrip,
          L"leaving the card returns to the lyric strip");
    Check(settle(), L"the strip settles again");

    // A notification holds the card open against the pointer leaving, which is the whole
    // point of the `active` flag.
    window.SetState(IslandState::Expanded);
    Check(settle(), L"the notification card settles");
    applyPolicy(/*pointerInside=*/false, IslandState::MusicStrip, /*active=*/true);
    // The STRIP, not the card: with the pointer away a message widens the island and shows itself in it.
    // The card is what the pointer asks for by going to the island (the case just above).
    Check(window.State() == IslandState::NoticeStrip,
          L"an active notification puts the island into its notice strip while the pointer is away");
    Check(settle(), L"the notice strip settles");
    applyPolicy(/*pointerInside=*/false, IslandState::MusicStrip, /*active=*/false);
    Check(window.State() == IslandState::MusicStrip,
          L"once the notification expires the pointer position decides again");

    // The plain pill follows the same cycle, so both resting shapes reach the card.
    window.SetState(IslandState::Collapsed);
    Check(settle(), L"the plain pill settles");
    window.SetState(IslandState::Hover);
    Check(settle(), L"hover settles from the plain pill");
    // The plain pill promotes to the taskbar, not to the card. The card is for notifications.
    applyPolicy(/*pointerInside=*/true, IslandState::Collapsed, /*active=*/false);
    Check(window.State() == IslandState::Hover,
          L"the plain pill promotes to the taskbar, not the card");

    // A PAUSED SESSION STILL OPENS THE CARD, which is what makes the play button reachable.
    //
    // The promotion used to need `resting == MusicStrip`, and that is true only WHILE PLAYING: pressing
    // pause retracted the card in the same breath, so the button vanished and the press that would have
    // resumed could never be made. Reported as "暂停按钮点击没有变化".
    {
        island::PointerContext paused;
        paused.shape = IslandState::Collapsed;
        paused.resting = IslandState::Collapsed;   // a paused session rests as the plain pill
        paused.pointerInside = true;
        paused.pointerOnIsland = true;
        paused.musicPresent = true;
        Check(NextShape(paused) == IslandState::Expanded,
              L"a paused session still opens the media card", L"paused, pointer on the island");

        paused.musicPresent = false;
        Check(NextShape(paused) == IslandState::Hover,
              L"... and with no session at all the same pointer opens nothing");

        // AND THE CARD STAYS OPEN OVER A PAUSED PLAYER, which is the flicker that came with opening it:
        // with only `resting == MusicStrip` holding it, the pointer rule pulled the shape back to hover on
        // the very next frame and the two swapped for as long as the pointer was there. Reported as
        // "音乐卡片会出现消失的来回".
        paused.musicPresent = true;
        paused.shape = IslandState::Expanded;
        Check(NextShape(paused) == IslandState::Expanded,
              L"the card stays up over a paused session");
    }

    applyPolicy(/*pointerInside=*/true, IslandState::Collapsed, /*active=*/true);
    Check(window.State() == IslandState::NoticeStrip,
          L"and a notification from the plain pill is the strip, not the card");

    window.Destroy();
}

} // namespace island
