// =====================================================================
//  TrayLedger.cpp -- see the header for what this is for
// =====================================================================
#include "domain/TrayLedger.h"

#include <cstring>
#include <cwchar>
#include <cwctype>

namespace island {
namespace {

// A copy that always leaves a terminator behind, and never reads past `capacity`: the strings come from
// another process's registration, so their length is that process's answer, not this one's.
void CopyText(wchar_t* out, size_t capacity, const wchar_t* text) {
    if (capacity == 0) return;
    size_t used = 0;
    if (text) {
        while (text[used] && used + 1 < capacity) {
            out[used] = text[used];
            ++used;
        }
    }
    out[used] = L'\0';
}

bool SameText(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return false;
    // Case insensitively: the registry spells a path however the installer wrote it, and the process table
    // spells the image name however the application was launched.
    while (*a && *b) {
        if (std::towlower(*a) != std::towlower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == L'\0' && *b == L'\0';
}

// The two ways a registration can be identified, in the order the shell uses them. The process is NOT one of
// them: a registration belongs to the window it names, and the window's owner is what the caller resolves.
bool SameRegistration(const TrayLiveRegistration& entry, unsigned long long window, unsigned uid,
                      bool hasGuid, const unsigned char* guid) {
    if (entry.hasGuid || hasGuid) {
        return entry.hasGuid && hasGuid && guid &&
               std::memcmp(entry.guid, guid, sizeof(entry.guid)) == 0;
    }
    return entry.window == window && entry.uid == uid;
}

// The weaker question Find() asks: a registry entry names a uid and an executable, never a window, so the
// window is not part of this comparison and the executable is (two applications register uid 3 here).
//
// ... EXCEPT WHEN ONE SIDE HAS NO NAME AT ALL, which happens for a registration whose window was already
// gone when the bar resolved its owner (MEASURED 2026-10-16: the probe that registers uid 7 arrives as
// `pid 0 exe -`). Refusing to match it would drop an icon the shell is holding, which is the very failure
// this ledger exists to prevent; the uid is then all there is to go on. What that accepts: two
// applications registering the same uid where one of them cannot be named. Nothing on this machine does.
bool NamesSameUid(const TrayLiveRegistration& entry, unsigned uid, const wchar_t* exeLeaf) {
    if (entry.hasGuid || entry.uid != uid) return false;
    if (entry.exe[0] == L'\0' || !exeLeaf || exeLeaf[0] == L'\0') return true;
    return SameText(entry.exe, exeLeaf);
}

} // namespace

void TrayLedger::Apply(TrayCopyCommand command, unsigned pid, unsigned long long window, unsigned uid,
                       bool hasGuid, const unsigned char* guid, const wchar_t* tip, const wchar_t* exe) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!SameRegistration(entries_[i], window, uid, hasGuid, guid)) continue;
        if (command == kTrayCopyDelete) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
        // The window a registration names can move (an application that recreates its tray window
        // re-registers), and the process is the one that is running now.
        entries_[i].pid = pid;
        if (exe && exe[0]) CopyText(entries_[i].exe, kTrayLedgerExeLength, exe);
        // A MODIFY that carries no tip must not erase the name that was already learned: NIM_SETVERSION and
        // plenty of refreshes send the same registration back with the tooltip field left empty.
        if (tip && tip[0]) CopyText(entries_[i].tip, kTrayLedgerTipLength, tip);
        return;
    }

    if (command == kTrayCopyDelete) return;   // nothing was holding it
    if (static_cast<int>(entries_.size()) >= kTrayLedgerCapacity) {
        ++refused_;
        return;
    }

    TrayLiveRegistration entry;
    entry.pid = pid;
    entry.window = window;
    entry.uid = uid;
    entry.hasGuid = hasGuid;
    if (hasGuid && guid) std::memcpy(entry.guid, guid, sizeof(entry.guid));
    CopyText(entry.exe, kTrayLedgerExeLength, exe);
    CopyText(entry.tip, kTrayLedgerTipLength, tip);
    entries_.push_back(entry);
}

void TrayLedger::ForgetWindow(unsigned long long window) {
    for (size_t i = 0; i < entries_.size();) {
        if (entries_[i].window == window) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void TrayLedger::ForgetProcess(unsigned pid) {
    for (size_t i = 0; i < entries_.size();) {
        if (entries_[i].pid == pid) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void TrayLedger::Reset() {
    entries_.clear();
    refused_ = 0;
}

const TrayLiveRegistration* TrayLedger::Find(const wchar_t* exeLeaf, unsigned uid, bool hasGuid,
                                             const unsigned char* guid) const {
    for (const TrayLiveRegistration& entry : entries_) {
        if (hasGuid) {
            // A guid is an identity on its own (see the header).
            if (entry.hasGuid && guid && std::memcmp(entry.guid, guid, sizeof(entry.guid)) == 0) return &entry;
            continue;
        }
        // A uid is not, so the executable has to agree as well.
        if (NamesSameUid(entry, uid, exeLeaf)) return &entry;
    }
    return nullptr;
}

} // namespace island
