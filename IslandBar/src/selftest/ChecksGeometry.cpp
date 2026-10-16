// =====================================================================
//  ChecksGeometry.cpp -- see selftest/Checks.h for why the self-test is split like this
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

void TestGeometryPredicate(float originX, float originY, float monitorWidthDip, float workTopDip) {
    Section(L"hit-test geometry (pure)");

    const RoundedBox pill = PillBox(originX, originY, monitorWidthDip, workTopDip);
    const float cx = (pill.rect.left + pill.rect.right) * 0.5f;
    const float cy = (pill.rect.top + pill.rect.bottom) * 0.5f;
    const float r  = pill.radiusX;

    Check(Contains(pill, cx, cy), L"centre is inside");
    Check(!Contains(pill, pill.rect.left - 1.0f, cy), L"left of the rect is outside");
    Check(!Contains(pill, pill.rect.right + 1.0f, cy), L"right of the rect is outside");
    Check(!Contains(pill, cx, pill.rect.top - 1.0f), L"above the rect is outside");
    Check(!Contains(pill, cx, pill.rect.bottom + 1.0f), L"below the rect is outside");

    // The top-left corner of the bounding box lies outside the rounded shape:
    // that single point is the difference between a pill and a rectangle.
    Check(!Contains(pill, pill.rect.left, pill.rect.top),
          L"bounding-box corner is cut away");

    // The corner is where a pill differs from a rectangle, so it gets an exact
    // boundary test rather than a sampled one.
    //
    // The point on the corner circle at 45 degrees is (centre - r/sqrt(2)) in
    // both axes. Testing it is the only way to prove the radius is applied:
    // the bounding-box corner is trivially outside, and the circle's own centre
    // is trivially inside. An earlier version of this check used
    // (centre.x, centre.y - r), which shares x with the centre and therefore
    // reports dx = 0 -- it passed no matter what the radius was.
    const float diagonal = r / std::sqrt(2.0f);
    const float eps = 0.05f;

    const float cxL = pill.rect.left + r;   // corner circle centres
    const float cxR = pill.rect.right - r;
    const float cyT = pill.rect.top + r;
    const float cyB = pill.rect.bottom - r;

    Check(Contains(pill, cxL - diagonal + eps, cyT - diagonal + eps),
          L"inside the corner arc");
    Check(!Contains(pill, cxL - diagonal - eps, cyT - diagonal - eps),
          L"outside the corner arc");
    Check(Contains(pill, cxR + diagonal - eps, cyT - diagonal + eps),
          L"arc respected at top-right");
    Check(Contains(pill, cxL - diagonal + eps, cyB + diagonal - eps),
          L"arc respected at bottom-left");
    Check(!Contains(pill, cxR + diagonal + eps, cyB + diagonal + eps),
          L"arc respected at bottom-right");

    // Tolerance must widen the region without moving the painted shape.
    const float justOutside = pill.rect.left - 1.0f;
    Check(!Contains(pill, justOutside, cy), L"tolerance off: 1px out is a miss");
    Check(ContainsWithTolerance(pill, justOutside, cy, kHitToleranceDip),
          L"tolerance on: 1px out is a hit");
    Check(!ContainsWithTolerance(pill, pill.rect.left - kHitToleranceDip - 2.0f, cy,
                                 kHitToleranceDip),
          L"tolerance band is not unbounded");

    // A square-cornered box must behave like a rectangle, including the middle
    // bands where the corner term drops out.
    RoundedBox square;
    square.rect = RectF{0.0f, 0.0f, 100.0f, 50.0f};
    square.radiusX = 0.0f;
    square.radiusY = 0.0f;
    Check(Contains(square, 50.0f, 25.0f), L"square box contains its centre");
    Check(Contains(square, 0.5f, 0.5f), L"square box has square corners");
    Check(!Contains(square, 100.5f, 25.0f), L"square box still rejects outside");
}

} // namespace island
