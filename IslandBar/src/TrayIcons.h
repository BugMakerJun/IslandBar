// =====================================================================
//  TrayIcons.h
//  The system tray's application icons, for the right-hand end of the bar.
//
//  WHERE THE ICONS COME FROM, and why not from the tray itself.
//
//  The classic way to read the notification area -- walk Shell_TrayWnd down to
//  ToolbarWindow32 and read the buttons out of Explorer's memory -- is dead on
//  Windows 11 (measured on build 26220: that subtree does not exist; the tray is
//  not a toolbar any more).
//
//  What IS available, measured with scripts/probe-tray-uia.ps1:
//
//    * the shell publishes the tray through UI Automation -- names, rectangles and
//      an Invoke pattern, so the icons can be SEEN and PRESSED -- but only while the
//      taskbar is visible, and this bar hides it. That makes UIA the way to press a
//      tray icon, and not the way to draw one.
//    * the icons themselves are in the registry, one subkey per icon, with a PNG of
//      the image:
//
//          HKCU\Control Panel\NotifyIconSettings\<hash>
//              ExecutablePath  the application that registered it
//              IconSnapshot    a PNG, 400-2300 bytes (magic 89 50 4E 47)
//              InitialTooltip  what the application calls it (often empty)
//              IsPromoted      1 = the shell shows it in the tray proper
//
//  So the images come from the registry, which needs no taskbar and no Explorer, and
//  the JSON is not involved at all.
//
//  WHICH ONES ARE SHOWN. The registry keeps an entry for every application that has
//  EVER put an icon in the tray, and most of them are not running: on this machine 27
//  entries, of which the 12 whose application is running are kept. That filter is the
//  difference between a tray and a graveyard of old installs.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace island {

// One icon, as the bar draws it.
struct TrayIcon {
    // What the application calls it, or its file name when it never said. Wide, because the tooltip is
    // whatever the application wrote and that is usually a sentence.
    wchar_t name[96] = {};
    // The PNG the shell stores for it: not owned by this struct's caller's frame, but held by the list
    // that produced it, which keeps it alive until the next refresh.
    const uint8_t* png = nullptr;
    size_t pngSize = 0;
    // 1 when the shell shows it in the tray proper rather than in the overflow.
    bool promoted = false;
};

// The live tray icons, newest registration first.
//
// Refreshed on a timer rather than every frame: it reads the registry, and the tray changes on the
// scale of applications starting and stopping.
class TrayIcons {
public:
    // Reads the registry and keeps the icons whose application is running. Returns how many.
    int Refresh();

    int Count() const { return static_cast<int>(entries_.size()); }
    // The CONTIGUOUS list, which is what the renderer walks: `icons[i]` has to be the i-th icon, and the
    // storage behind these has them interleaved with strings and vectors (see `list_`).
    const TrayIcon& At(int index) const { return list_[static_cast<size_t>(index)]; }

    // The executable behind one icon, for the log and for pressing it later.
    const wchar_t* ExePathAt(int index) const {
        return entries_[static_cast<size_t>(index)].exePath.c_str();
    }

    // Bumped when the set changes, so the frame loop can redraw once and the renderer can re-decode.
    unsigned long long Revision() const { return revision_; }

    // What the last refresh looked at, for the log: the entries the registry holds, and how many of
    // their applications were running. "No icons" has two very different causes -- an empty registry and
    // a failed process snapshot -- and these are what tell them apart.
    int Examined() const { return examined_; }
    int RunningApps() const { return runningApps_; }
    int ProcessesSeen() const { return processesSeen_; }

    // Where the icons live, so nothing else has to know the key name.
    static const wchar_t* SettingsKeyPath() { return L"Control Panel\\NotifyIconSettings"; }

private:
    struct Entry {
        TrayIcon icon;
        std::wstring exePath;
        std::vector<uint8_t> png;   // the storage `icon.png` points into
        long long writtenAt = 0;    // the subkey's last write, for the newest-first order
    };

    std::vector<Entry> entries_;
    // The same icons, contiguous, for the renderer.
    //
    // `At()` used to return `entries_[index].icon` and the frame handed the renderer `&At(0)` -- so the
    // renderer walked a pointer to the FIRST icon as if the icons were an array, while they are members of
    // `Entry` structs that also hold a wstring and a vector. `icons[1]` therefore read the middle of
    // entry 0: a size of 0xFEFEFEFEFEFEFEFE (the debug heap's fill for memory nobody wrote) and a picture
    // that would not decode. Measured, in the draw trace: "icon 1 has a 18374403900871474942 byte PNG".
    std::vector<TrayIcon> list_;
    unsigned long long revision_ = 1;
    // How many registry entries were looked at and how many were running, for the log.
    int examined_ = 0;
    int runningApps_ = 0;
    int processesSeen_ = 0;
};

} // namespace island
