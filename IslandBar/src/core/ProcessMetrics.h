// =====================================================================
//  ProcessMetrics.h
//  One line about what this process is holding, so "it grows" becomes a curve
//  instead of an impression.
//
//  WHY IT EXISTS. The bar is a resident component: it is expected to sit on the
//  desktop for days. "It uses little memory" is therefore a claim about a 24-hour
//  curve, and a curve needs samples. The spread measured on this machine is 20MB
//  wide -- 89.6MB private bytes at startup, 107-109MB after 45 minutes of use --
//  and the two explanations for it, a bounded cache filling up and a leak, are
//  told apart by exactly one thing: whether the counters BELOW rise together with
//  the bytes above.
//
//  So one line carries both halves:
//
//      mem[reason]: private=..MB ws=..MB vram=local/nonlocalMB handles=..
//                   threads=.. cover=..B lyrics=.. tray=.. winicons=..
//                   bitmaps=.. notify=.. tasks=..
//
//  READING IT. Private bytes rising while every count after it stays flat is a
//  leak. Private bytes rising WITH a count is that thing's cache working, and the
//  question becomes whether it has a ceiling. That is the whole reason the counts
//  are on the same line as the bytes rather than in a second report.
//
//  Note which numbers are deliberately NOT here: a Direct2D bitmap count and a COM
//  reference count are not readable through any public API, so they would have to
//  be guesses. The caches this program owns are counted instead, and the GPU side
//  is asked of the adapter (see Renderer::VideoMemory), which does answer.
// =====================================================================
#pragma once

namespace island {

// What this process currently holds, as the operating system reports it.
struct ProcessMemorySample {
    unsigned long long privateBytes      = 0;
    unsigned long long workingSetBytes   = 0;
    // Filled in by the caller from the renderer, which owns the DXGI adapter: LOCAL is the GPU's own
    // segment (VRAM on a discrete card, the same system memory on an integrated one), NON_LOCAL is
    // what the driver reserved elsewhere. A GPU allocation with no ceiling shows up here and nowhere
    // else, which is why the field exists at all.
    unsigned long long vramLocalBytes    = 0;
    unsigned long long vramNonLocalBytes = 0;
    unsigned long handles                = 0;   // kernel handles this process holds
    unsigned long threads                = 0;   // threads this process holds, including other people's
    // GDI and USER objects. The cheapest way to see a leak that lives in the window manager rather
    // than in the heap: those two are a FIXED per-process budget, so a bar that leaks one object per
    // redraw dies at ten thousand rather than when memory runs out -- and it dies as a drawing failure
    // everywhere at once, which reads as a hundred unrelated bugs.
    unsigned long gdiObjects             = 0;
    unsigned long userObjects            = 0;
    bool vramValid                       = false;
};

// What the bar is holding on to. Every field counts something this program owns.
struct ProcessCacheCounts {
    unsigned long long coverBytes = 0;   // the current album cover as the media reader holds it
    int lyricLines                = 0;   // parsed lyric lines for the current track
    int trayIcons                 = 0;   // tray icons mirrored from the shell
    int windowIcons               = 0;   // window-button icons the renderer has cached
    int bitmaps                   = 0;   // decoded images the renderer has cached
    int notifications             = 0;   // notifications in the store
    int taskButtons               = 0;   // windows listed on the bar
};

// Fills `out` from this process and the operating system. A field that cannot be read is left at
// zero rather than failing the call: a missing number in a diagnostic line must not cost the line.
// The two VRAM fields are NOT filled here -- the renderer owns the adapter; see the note above.
void SampleProcessMemory(ProcessMemorySample* out);

// One line, at a fixed shape, so two runs can be diffed and a day can be plotted.
//
// `reason` is short and pure ASCII: "5min", "card-closed", "track", "tray-refresh". It is what turns
// a wall of samples into "this line came 30 seconds after a card closed".
void LogProcessMemory(const char* reason, const ProcessMemorySample& sample,
                      const ProcessCacheCounts& counts);

} // namespace island
