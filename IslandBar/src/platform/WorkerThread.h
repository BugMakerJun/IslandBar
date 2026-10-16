// platform/WorkerThread.h -- the stop event, the thread, and the rule that ends both
//
// WHY THIS IS A COMPONENT, and it is a measured reason rather than a taste: seven readers in this program
// start a background thread and must end it the same way -- set the stop event, wait for the thread with a
// TIMEOUT (a thread that never returns must not hang the bar), close the thread handle, close the event.
// Four of them had written that dance out identically, line for line, which is how the copy-paste was found
// (scripts/tmp-dup-scan.ps1, 2026-10-16), and the other three had written it almost identically.
//
// THE COPIES HAD ALREADY DRIFTED, WHICH IS THE LEAK. Three of the seven returned false out of Start()
// without ending what they had created: the stop event stayed open, the thread stayed running (with
// `running_` false, so Stop() was never called again), and the critical section stayed initialised -- and a
// second Start(), which the callers do perform, initialised that section a second time over the same
// storage. Nothing visible leaked in the steady state, which is exactly why it survived: the leak needs a
// start that fails and then gets retried.
//
// WHAT IS DELIBERATELY NOT HERE: the second event. Four of the readers also own a wake-up event
// (a write, a refresh, a toggle) whose auto-reset semantics and whose meaning belong to that reader's own
// protocol. The rule for ending a thread is shared; the protocol of waking it is not.
//
// THE OWNER CREATES THE THREAD, not this class: each reader's entry point is its own static member with its
// own name, and hiding that behind a `std::function` here would cost an allocation and a header to buy
// nothing. The order is Prepare(), then start the thread, then Attach(). Any failure between the first and
// the last is cleaned up by Stop(), which is idempotent -- so a reader that fails halfway does not have to
// write the unwind itself.

#pragma once

#include <windows.h>

namespace island {

class WorkerThread {
public:
    WorkerThread() = default;
    ~WorkerThread() { Stop(); }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    // Manual-reset, so every waiter sees it and the thread does not have to re-arm anything.
    //
    // It stops first, and that line is the fix for the leak the copies had: a Start() that gave up
    // half-way left the event (and often the thread) behind, and the retry -- the callers do retry --
    // overwrote the handle with a fresh one. Ending the previous attempt here makes a retry start from
    // nothing rather than from a corpse.
    bool Prepare() {
        Stop();
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return stopEvent_ != nullptr;
    }

    // Hands over a thread the caller has already started, after Prepare(). It does NOT stop anything: the
    // stop event Prepare() just created has to survive, and signalling it here is exactly the way to lose
    // it. Every caller pairs one Prepare() with one Attach() and calls Stop() in between if it is retrying.
    void Attach(HANDLE thread) { thread_ = thread; }

    // The handle the worker waits on beside its own wake-up event.
    HANDLE StopEvent() const { return stopEvent_; }

    bool Started() const { return thread_ != nullptr; }

    // True from the moment Prepare() succeeded until Stop() has finished: "there is a worker being run".
    //
    // This is the question the readers used to keep a flag of their own for (`lockReady_`), and the mapping
    // is deliberate rather than approximate -- Start() set that flag before creating the event and Stop()
    // cleared it after closing it, which is the same window this describes. It exists so the callers that
    // turn a request away when the worker is not there ("nothing can be written, so do nothing") keep
    // saying exactly what they said before.
    bool Prepared() const { return stopEvent_ != nullptr; }

    // Signals, waits with a timeout, closes what exists. Safe to call when nothing was ever prepared, safe
    // to call twice, and safe on a thread that has already ended: a thread that is gone is signalled by
    // WAIT_OBJECT_0 and a thread that ignores the stop event is given up on after `timeoutMs` rather than
    // hanging the caller. The handle is closed either way -- CloseHandle on a running thread detaches it,
    // which is the documented way to stop waiting without the thread deadlocking at exit.
    bool Stop(unsigned long timeoutMs = 3000) {
        bool ended = true;
        if (stopEvent_) SetEvent(stopEvent_);

        if (thread_) {
            ended = WaitForSingleObject(thread_, timeoutMs) == WAIT_OBJECT_0;
            CloseHandle(thread_);
            thread_ = nullptr;
        }

        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        return ended;
    }

private:
    HANDLE stopEvent_ = nullptr;
    HANDLE thread_ = nullptr;
};

}  // namespace island
