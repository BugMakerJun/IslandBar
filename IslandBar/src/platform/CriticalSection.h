// platform/CriticalSection.h -- a lock that exists from construction to destruction
//
// WHY THIS IS A COMPONENT. Seven readers (the audio level, the media session, the sound-output page,
// the lyric provider, the weather reader, the notification pipe, the player-window watch) each own one of
// these, and each of them wrote the same three steps by hand: InitializeCriticalSection in Start(), an
// `lockReady_` flag that every reader of the data had to test before touching the lock, and
// DeleteCriticalSection in Stop(). The flag is where the copies drifted: a Start() that failed before it got
// as far as creating the section left `lockReady_` false, and a second Start() called
// InitializeCriticalSection on the same storage again -- leaking the first section and leaving two of them
// describing one object.
//
// HOW THIS IS DIFFERENT. There is no flag, because there is no window in which the object exists without
// its lock: the constructor initialises it and the destructor deletes it. A caller that used to ask "is it
// ready?" now just locks.
//
// CRITICAL_SECTION rather than std::mutex: these locks are taken by a worker thread and by the frame loop
// several times a second and never held across a wait, which is the case the Win32 one is built for, and
// this project links no C++ runtime beyond the static CRT it already uses.

#pragma once

#include <windows.h>

namespace island {

class CriticalSection {
public:
    CriticalSection() { InitializeCriticalSection(&section_); }
    ~CriticalSection() { DeleteCriticalSection(&section_); }

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;

    void Enter() { EnterCriticalSection(&section_); }
    void Leave() { LeaveCriticalSection(&section_); }

    // RAII, for the paths that must not leave the lock held when they return early. The readers here take
    // the lock, copy a struct out and release it; every one of them is written with this.
    class Guard {
    public:
        explicit Guard(CriticalSection& section) : section_(&section) { section_->Enter(); }
        ~Guard() { section_->Leave(); }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        CriticalSection* section_;
    };

private:
    CRITICAL_SECTION section_{};
};

}  // namespace island
