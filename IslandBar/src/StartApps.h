// =====================================================================
//  StartApps.h
//  The applications a self-drawn Start card can offer, and the ones the user pinned himself.
//
//  WHERE THE LIST COMES FROM. Not from the Start menu's own storage: the pinned order of the shell's
//  Start lives in `%LOCALAPPDATA%\Packages\Microsoft.Windows.StartMenuExperienceHost_*\LocalState\start2.bin`,
//  and that file is opaque -- measured: 13,276 bytes, and a UTF-16 scan of the whole thing finds no
//  application id at all (every run of "text" is noise). So the shell's pinned order is NOT readable.
//
//  What IS readable is the list underneath it, and it is the same list the shell builds its "all apps"
//  view from: the virtual folder `shell:AppsFolder`. Measured on this machine: 121 items -- 89 Win32
//  shortcuts and 32 packaged applications -- with LOCALISED names (微信, 记事本, 便签) and a "path" in one
//  of three shapes:
//
//      C:\SOFTWARE\QQMusic\QQMusic.exe                            a file
//      {1AC14E77-...}\cleanmgr.exe                                a shell folder item
//      Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe!App           a packaged application
//
//  and all three launch through one call, `ShellExecute(L"shell:AppsFolder\\" + target)`.
//
//  Reading the whole list costs about 600 ms, which is why it happens off the UI thread and only when it
//  is asked for.
//
//  THE PINS ARE OURS. Since the shell's pinned list cannot be read, this card keeps its own, in a text
//  file beside the bar (`start_pins.txt`), one target per line -- which also means it can be edited by
//  hand, and that it survives the card being rewritten.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace island {

// One application, as the shell's own list describes it.
struct StartApp {
    // What the shell calls it, in the user's language.
    wchar_t name[112] = {};
    // What launches it: the AppsFolder path, in whichever of the three shapes it came in.
    wchar_t target[512] = {};
};

class StartApps {
public:
    // Reads `shell:AppsFolder` into the list. Takes a few hundred milliseconds; call it off the UI thread.
    // Returns how many applications were found (0 also means "could not be read").
    int RefreshList();

    // Reloads the pins from the text file. Cheap -- a file read -- so it belongs on the UI thread.
    void LoadPins();

    int ListCount() const { return static_cast<int>(list_.size()); }
    const StartApp& ListAt(int index) const { return list_[static_cast<size_t>(index)]; }

    int PinCount() const { return static_cast<int>(pins_.size()); }
    const StartApp& PinAt(int index) const { return pins_[static_cast<size_t>(index)]; }

    // Pins an application by its target (the caller has one because it came from the list). Also writes
    // the file. Returns false when it is already pinned or the target is not in the shell's list.
    bool Pin(const wchar_t* target);
    bool Unpin(int index);

    // The pinned application's icon as a PNG, or null while it is still being resolved. Resolved on a
    // worker when the pin is added, because getting a shell item's image takes a few milliseconds and the
    // frame loop cannot spend them.
    const uint8_t* PinIconPng(int index, size_t* sizeOut) const;

    // Bumped whenever the list or the pins change, so the renderer can re-decode what it has cached.
    unsigned long long Revision() const { return revision_; }
    // Bumped when an icon arrives, which happens separately from a pin being added.
    unsigned long long IconRevision() const { return iconRevision_; }

    // Where the pins live, so nothing else has to know.
    static void PinsPath(wchar_t* out, size_t capacity);

private:
    void SavePins() const;
    void ResolveIconAsync(const wchar_t* target);

    std::vector<StartApp> list_;
    // Two vectors rather than a vector of structs, and that is not an accident: the renderer is handed a
    // POINTER to the first entry and walks them by index, which only works if the entries are contiguous.
    // The tray taught this the hard way -- it handed over `&entries_[0].icon` from a struct that also held
    // a wstring, and the renderer read the middle of the first entry as the second icon.
    std::vector<StartApp> pins_;
    std::vector<std::vector<uint8_t>> pinIcons_;   // one PNG per pin, in the same order
    unsigned long long revision_ = 1;
    unsigned long long iconRevision_ = 1;
};

// One AppsFolder item's icon as a PNG of `side` x `side` pixels. False when the item has no image.
//
// `shell:AppsFolder\<target>` is parsed for every shape of target, so a packaged application and a file
// are the same call here.
bool StartAppIconPng(const wchar_t* target, int side, std::vector<uint8_t>* png);

// Launches one AppsFolder item. One call for all three shapes of target.
bool LaunchStartApp(const wchar_t* target);

} // namespace island
