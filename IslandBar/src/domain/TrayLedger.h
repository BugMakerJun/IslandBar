// =====================================================================
//  TrayLedger.h -- WHICH TRAY REGISTRATIONS THE SHELL IS HOLDING RIGHT NOW
//
//  WHY THIS EXISTS. The card used to be built from the registry alone: HKCU\Control Panel\NotifyIconSettings
//  keeps a subkey for every icon an application has EVER registered, and the filter on top of it was "is the
//  executable that subkey names running". That is not the question the card is answering, and the difference
//  is visible on screen: measured 2026-10-16, the shell's tray held nine application icons while the card drew
//  thirteen -- three NVIDIA entries (NVDisplay.Container.exe is a process that runs all day and registered
//  nothing in that session at all) and one dead explorer GUID whose stored picture is a blank 210-byte PNG.
//
//  WHAT IT IS. A ledger of the registrations the shell has received, fed from the module's ring
//  (shell/TrayLearn.cpp drains it, one record per Shell_NotifyIcon call) and read by the card
//  (shell/TrayIcons.cpp): a registry entry is drawn only while this ledger holds a registration naming it.
//  ADD and MODIFY put an entry in -- MODIFY too, because an application that registered before this bar
//  started only ever sends MODIFY -- and DELETE, a dead window, or a dead process takes it out.
//
//  WHAT IT DOES NOT DECIDE. It knows nothing about processes, windows or the shell. The caller resolves the
//  owner of the tray window and hands the executable's name in; the caller also walks the entries and drops
//  the ones whose window is gone. That is what keeps this file in domain/, and it is why the rule can be
//  asserted in --self-test with no shell, no tray and no application in the room (see Diagnostics.cpp).
//
//  THE NAME IS PART OF IT, not a detail of drawing. What an application calls its icon lives in the registry
//  as InitialTooltip, and that field is often EMPTY -- which is how the shell's own Bluetooth icon came to be
//  drawn on this card as "explorer.exe": explorer registers it on bthprops.cpl's behalf and never wrote a
//  tooltip there. Every registration, on the other hand, carries the tip the application sends the shell, so
//  the ledger has the name the tray actually shows.
// =====================================================================
#pragma once

#include <cstddef>
#include <vector>

#include "domain/TrayRegistration.h"   // TrayCopyCommand: the commands, named once for both ends

namespace island {

// How many live registrations are remembered. The machine this was measured on holds nine; an application
// that re-registers in a loop (Task Manager does, once per refresh) is the reason this is a ceiling and not
// an assumption: past it, the new registration is refused and counted rather than growing without bound.
inline constexpr int kTrayLedgerCapacity = 128;
inline constexpr size_t kTrayLedgerExeLength = 64;
inline constexpr size_t kTrayLedgerTipLength = 128;

// One registration the shell is holding: the identity a press needs, plus the two names the card draws.
//
// `window` is an HWND as a NUMBER. This layer does not know what a window is, and it has no business
// knowing -- it only has to tell two registrations of the same application apart.
struct TrayLiveRegistration {
    unsigned pid = 0;                  // the process that owns the tray window
    unsigned long long window = 0;
    unsigned uid = 0;                  // meaningful only when `hasGuid` is false
    bool hasGuid = false;
    unsigned char guid[16] = {};
    wchar_t exe[kTrayLedgerExeLength] = {};   // lower case, as the cache and the registry both spell it
    wchar_t tip[kTrayLedgerTipLength] = {};   // what the application calls its icon, or empty
};

// The live registrations, oldest arrival first. Not thread safe on purpose: the shell layer that owns it
// (shell/TrayLearn.cpp) holds the lock, so a reader here is never a second reader's problem.
class TrayLedger {
public:
    // One Shell_NotifyIcon call, as the module decoded it. `guid` is read only when `hasGuid` is set.
    //
    // DELETE removes the registration the identity names. Everything else adds it or refreshes it in place:
    // a MODIFY for a registration that was never seen (an application that registered before this bar
    // started) is how that application first appears here.
    void Apply(TrayCopyCommand command, unsigned pid, unsigned long long window, unsigned uid, bool hasGuid,
               const unsigned char* guid, const wchar_t* tip, const wchar_t* exe);

    // The application is gone, or its window is: the caller decides which, and tells the ledger.
    void ForgetWindow(unsigned long long window);
    void ForgetProcess(unsigned pid);
    // Nothing is known any more -- the shell itself was replaced, so nothing it held is registered there.
    void Reset();

    int Count() const { return static_cast<int>(entries_.size()); }
    // Registrations refused past kTrayLedgerCapacity, so a full ledger is visible rather than silent.
    int Refused() const { return refused_; }
    const TrayLiveRegistration& At(int index) const { return entries_[static_cast<size_t>(index)]; }

    // The registration a registry entry names, or null when the shell is not holding one.
    //
    // A guid is an identity on its own. A uid is NOT: ChatGPT and RazerAppEngine both register with uid 3 on
    // the machine this was measured on, so a uid counts only together with the executable that used it.
    const TrayLiveRegistration* Find(const wchar_t* exeLeaf, unsigned uid, bool hasGuid,
                                     const unsigned char* guid) const;

private:
    std::vector<TrayLiveRegistration> entries_;
    int refused_ = 0;
};

} // namespace island
