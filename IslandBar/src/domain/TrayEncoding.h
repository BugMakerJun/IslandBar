// =====================================================================
//  domain/TrayEncoding.h
//  THE ONE PLACE THAT KNOWS HOW A TRAY CLICK IS ADDRESSED TO AN APPLICATION.
//
//  WHY IT IS ONE PLACE. The shell and the application agree on a protocol through the uVersion the
//  application registered with, and the two protocols put the SAME click in DIFFERENT places. Until
//  2026-10-16 that knowledge lived in a comment inside a probe, with the version-4 shape written down
//  backwards, and the cost was measured: WeChat (uVersion 4) crashed twice and came back at its login
//  screen, and a "10.4-second modal menu loop" reported the round before was really its own crash dialog.
//  HANDOFF 8.142 has the byte-level evidence -- the shell was caught, on a real right click, sending
//      wParam = MAKELONG(pointer x, pointer y),  lParam = MAKELONG(event, uid)
//  to a version-4 application, with the pointer exactly on the icon the user had clicked.
//
//  SO: every encoding decision lives here, in a header that cannot see <windows.h> (enforced by
//  domain/PurityCheck.cpp), and every sender -- the island, a tool, a test -- calls PlanTrayClick.
//  A third protocol means changing this file and its assertions, not hunting for every place that
//  happens to build a message.
//
//  WHAT THIS FILE DOES NOT SAY. Nothing here decides WHETHER a click worked. A window procedure that
//  ignores a message answers exactly what an acting one answers, and an application that opens a menu
//  may hold the call for as long as the menu is open. kTrayClickTimeoutMs is the upper bound on ONE
//  synchronous delivery; it is never a verdict about the menu (HANDOFF 8.142, review round 41).
// =====================================================================
#pragma once

namespace island {

// Windows' own values, repeated here so this header stays free of <windows.h>. They are frozen ABI.
inline constexpr unsigned kTrayMouseLeftDown    = 0x0201;   // WM_LBUTTONDOWN
inline constexpr unsigned kTrayMouseLeftUp      = 0x0202;   // WM_LBUTTONUP
inline constexpr unsigned kTrayMouseRightDown   = 0x0204;   // WM_RBUTTONDOWN
inline constexpr unsigned kTrayMouseRightUp     = 0x0205;   // WM_RBUTTONUP
inline constexpr unsigned kTrayMouseContextMenu = 0x007B;   // WM_CONTEXTMENU
inline constexpr unsigned kTraySelect           = 0x0400;   // NIN_SELECT = WM_USER + 0

// The upper bound on one synchronous delivery -- see the note at the top. It is not a menu criterion.
inline constexpr unsigned kTrayClickTimeoutMs = 250;

// LOWORD/HIWORD packing without the Win32 macro.
constexpr unsigned TrayPack(unsigned low, unsigned high) {
    return (low & 0xFFFFu) | ((high & 0xFFFFu) << 16);
}
constexpr unsigned TrayLow(unsigned packed) { return packed & 0xFFFFu; }
constexpr unsigned TrayHigh(unsigned packed) { return (packed >> 16) & 0xFFFFu; }

struct TrayMessage {
    unsigned wParam = 0;
    unsigned lParam = 0;
};

// One click as the application expects to receive it.
//
//   version 0/3:  a PAIR of messages -- down, then up -- with the uid in wParam and the mouse message
//                 in lParam.
//   version 4:    a SINGLE message whose wParam is the POINTER POSITION and whose lParam carries the
//                 event in its low word and the uid in its high word.
struct TrayClickPlan {
    TrayMessage first;
    bool hasSecond = false;
    TrayMessage second;
    // Which protocol went out, so the log can tell "a message was sent" from "the right message was sent".
    bool version4 = false;
};

// One message, encoded. This is the form the press code needs, because a press is allowed to TRY more than
// one spelling for an application whose protocol was never observed (see TrayPress.cpp): it chooses the
// packing and the event, and the placement of the two words still comes from here and nowhere else.
constexpr TrayMessage EncodeTrayMessage(bool version4Packing, unsigned uid, unsigned event, int x, int y) {
    TrayMessage message;
    if (version4Packing) {
        message.wParam = TrayPack(static_cast<unsigned>(x), static_cast<unsigned>(y));
        message.lParam = TrayPack(event, uid);
    } else {
        message.wParam = uid;
        message.lParam = event;
    }
    return message;
}

constexpr TrayClickPlan PlanTrayClick(unsigned version, unsigned uid, bool rightClick, int x, int y) {
    TrayClickPlan plan;
    plan.version4 = version >= 4;
    if (plan.version4) {
        const unsigned event = rightClick ? kTrayMouseContextMenu : kTraySelect;
        plan.first = EncodeTrayMessage(true, uid, event, x, y);
    } else {
        plan.first = EncodeTrayMessage(false, uid, rightClick ? kTrayMouseRightDown : kTrayMouseLeftDown, x, y);
        plan.hasSecond = true;
        plan.second = EncodeTrayMessage(false, uid, rightClick ? kTrayMouseRightUp : kTrayMouseLeftUp, x, y);
    }
    return plan;
}

}  // namespace island
