// =====================================================================
//  ProcessMetrics.cpp
//  See ProcessMetrics.h for why the line exists and how it is read.
// =====================================================================
#include "core/ProcessMetrics.h"

#include "core/Log.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "psapi.lib")   // GetProcessMemoryInfo

namespace island {
namespace {

// The thread count is ASKED OF THE SYSTEM rather than tracked by this program, and that is the whole
// point of it: the interesting threads are the ones this program did not create -- the Windows Runtime,
// WASAPI, the D3D driver and the thread pool. A counter of our own would not see a single one of them,
// and those are exactly the ones that explain "60 threads for a taskbar".
unsigned long CountThreadsOfThisProcess() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    unsigned long count = 0;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == self) ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

const double kBytesPerMegabyte = 1024.0 * 1024.0;

} // namespace

void SampleProcessMemory(ProcessMemorySample* out) {
    if (!out) return;
    *out = ProcessMemorySample{};

    // PROCESS_MEMORY_COUNTERS_EX rather than the Process object a script would read: PrivateUsage is
    // the number that matters here (Windows' own Task Manager "Memory" column), and it is the one the
    // 24-hour curve is about.
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        out->privateBytes    = static_cast<unsigned long long>(counters.PrivateUsage);
        out->workingSetBytes = static_cast<unsigned long long>(counters.WorkingSetSize);
    }

    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        out->handles = handles;
    }

    // GetGuiResources rather than a count of our own brushes and windows: it sees what the process
    // actually holds in the window manager, which includes objects created by the shell's own dialogs
    // and by anything else in this address space.
    out->gdiObjects  = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    out->userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

    out->threads = CountThreadsOfThisProcess();
}

void LogProcessMemory(const char* reason, const ProcessMemorySample& sample,
                      const ProcessCacheCounts& counts) {
    const char* why = reason ? reason : "?";

    // Two spellings rather than one with a placeholder: a VRAM figure that could not be read must not
    // print as "0.0MB", which reads as "the GPU is using nothing" -- the opposite of "not known".
    if (sample.vramValid) {
        LogAndPrint("  mem[%s]: private=%.1fMB ws=%.1fMB vram=%.1f/%.1fMB handles=%lu threads=%lu "
                    "gdi=%lu user=%lu cover=%lluB lyrics=%d tray=%d winicons=%d bitmaps=%d notify=%d "
                    "tasks=%d",
                    why,
                    static_cast<double>(sample.privateBytes) / kBytesPerMegabyte,
                    static_cast<double>(sample.workingSetBytes) / kBytesPerMegabyte,
                    static_cast<double>(sample.vramLocalBytes) / kBytesPerMegabyte,
                    static_cast<double>(sample.vramNonLocalBytes) / kBytesPerMegabyte,
                    sample.handles, sample.threads, sample.gdiObjects, sample.userObjects,
                    counts.coverBytes, counts.lyricLines, counts.trayIcons, counts.windowIcons,
                    counts.bitmaps, counts.notifications, counts.taskButtons);
    } else {
        LogAndPrint("  mem[%s]: private=%.1fMB ws=%.1fMB vram=n/a handles=%lu threads=%lu "
                    "gdi=%lu user=%lu cover=%lluB lyrics=%d tray=%d winicons=%d bitmaps=%d notify=%d "
                    "tasks=%d",
                    why,
                    static_cast<double>(sample.privateBytes) / kBytesPerMegabyte,
                    static_cast<double>(sample.workingSetBytes) / kBytesPerMegabyte,
                    sample.handles, sample.threads, sample.gdiObjects, sample.userObjects,
                    counts.coverBytes, counts.lyricLines, counts.trayIcons, counts.windowIcons,
                    counts.bitmaps, counts.notifications, counts.taskButtons);
    }
}

} // namespace island
