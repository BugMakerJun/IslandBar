// =====================================================================
//  FrameStats.h -- the frame clock every loop in this program reports through
//
//  It counts frames, total time, time actually spent drawing, the worst frame, and the hitches (intervals
//  above kHitchMs). Those numbers are what every performance decision in HANDOFF was made from, so they are
//  shared rather than private to the loop that happens to run them: the bar's frame loop and the
//  --music-preview command both time themselves with this, and both print the same lines.
//
//  It lives in core/ -- not in domain/ -- because a LARGE_INTEGER and a counter are how this program talks to
//  the clock, and the pure-logic layer is deliberately not allowed to know about either (see
//  domain/PurityCheck.cpp).
// =====================================================================
#pragma once

#include <windows.h>

namespace island {
struct FrameStats {
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER last = {};
    bool  started = false;

    // Intervals above this are counted as hitches.
    static constexpr double kHitchMs = 50.0;

    // ... and the ones that only deserve a line: there is no need to print a frame that ran at the
    // compositor's own rate, and no point printing a hitch without saying what it was doing.
    static constexpr double kSlowFrameMs = 40.0;

    long long count = 0;
    double totalMs = 0.0;
    double totalDrawnMs = 0.0;
    double worstMs = 0.0;
    int hitches = 0;
    // THE WAIT IS NOT A HITCH, and this is the measurement that showed it.
    //
    // When nothing is happening the loop BLOCKS on input, on purpose, with a 100 ms cap (see the pacing
    // at the bottom of the loop) -- so two frames out of every second or so arrive ~80-100 ms apart, and
    // every one of those used to be counted and reported as a hitch. The first version of this file's
    // slow-frame line said what those frames were doing, and the answer was "nothing, and it did not even
    // draw": the stalls being chased for three rounds were the idle wait.
    //
    // So the two are counted apart. `drew` is the caller telling this how the pass BEFORE the interval
    // spent itself -- which is the pass the interval measures -- and only passes that drew are evidence
    // about the loop's ability to keep up.
    double idleWorstMs = 0.0;
    long long idleCount = 0;
    // A coarse histogram, in milliseconds, for a rough percentile.
    int buckets[8] = {};   // <8, <16, <24, <33, <50, <100, <250, >=250

    void Start() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&last);
        started = true;
    }

    // Returns the interval it just recorded, so the caller can say something about the frame that took
    // it -- which is the whole point of the line this feeds.
    double Record(bool drew) {
        if (!started || frequency.QuadPart <= 0) return 0.0;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        const double ms = static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 /
                          static_cast<double>(frequency.QuadPart);
        last = now;

        ++count;
        totalMs += ms;
        if (drew) {
            totalDrawnMs += ms;
            if (ms > worstMs) worstMs = ms;
            if (ms > kHitchMs) ++hitches;

            if (ms < 8.0)        ++buckets[0];
            else if (ms < 16.0)  ++buckets[1];
            else if (ms < 24.0)  ++buckets[2];
            else if (ms < 33.0)  ++buckets[3];
            else if (ms < 50.0)  ++buckets[4];
            else if (ms < 100.0) ++buckets[5];
            else if (ms < 250.0) ++buckets[6];
            else                 ++buckets[7];
        } else {
            ++idleCount;
            if (ms > idleWorstMs) idleWorstMs = ms;
        }
        return ms;
    }

    void Reset() {
        count = 0;
        totalMs = 0.0;
        totalDrawnMs = 0.0;
        worstMs = 0.0;
        hitches = 0;
        idleWorstMs = 0.0;
        idleCount = 0;
        for (int& bucket : buckets) bucket = 0;
        Start();
    }

    void Describe(wchar_t* out, size_t capacity) const {
        if (!out || capacity == 0) return;
        // Both numbers are of the frames that DREW: an idle wait in the average would flatter exactly the
        // state where nobody is looking, and it would hide the frames that matter.
        const long long drawn = count - idleCount;
        const double mean = drawn > 0 ? totalDrawnMs / static_cast<double>(drawn) : 0.0;
        swprintf(out, capacity,
                 L"drawn %lld: avg %.1f ms, worst %.1f ms, hitches(>%.0f ms) %d | "
                 L"<8:%d <16:%d <24:%d <33:%d <50:%d <100:%d <250:%d 250+:%d | "
                 L"idle %lld pass(es), worst wait %.1f ms",
                 drawn, mean, worstMs, kHitchMs, hitches, buckets[0], buckets[1], buckets[2],
                 buckets[3], buckets[4], buckets[5], buckets[6], buckets[7], idleCount, idleWorstMs);
    }
};

} // namespace island
