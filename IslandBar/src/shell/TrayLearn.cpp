// =====================================================================
//  TrayLearn.cpp -- the driver behind TrayLearn.h
//
//  Four steps, and the third one is the only interesting one:
//
//      1. offer the hook module to every running tray application (one thread hook per thread);
//      2. ask every application to register its icon again -- TaskbarCreated;
//      3. read what the module wrote down, resolve each window's class, merge into the cache;
//      4. report coverage: how many of the card's icons can now be pressed without a real click.
//
//  THE REPORT IS A FILE, and that is a decision rather than a shortcut. The module runs inside someone
//  else's process, in one process per application, and a file append is the one channel that needs no
//  protocol, no lifetime agreement and no extra process -- and it survives the module being unable to ask us
//  anything. It is truncated at the start of every run, so it never grows without bound.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include "shell/TrayLearn.h"

#include "core/Log.h"

#include <psapi.h>   // GetProcessMemoryInfo: which of an application's processes carries its tray icon
#include <tlhelp32.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace island {
namespace {

const wchar_t* kCacheFile = L"tray_callbacks.txt";
const wchar_t* kReportFile = L"tray_learned.txt";
const wchar_t* kHookModule = L"island_tray_hook.dll";

// THE COPY OF THE MODULE THAT IS ACTUALLY OFFERED TO APPLICATIONS, NAMED AFTER ITS OWN CONTENTS.
//
// The module PINS itself inside every application it enters (see TrayHook.cpp), because the alternative
// shipped a crash: it used to be unloaded at the end of the learning window while the applications' import
// slots still pointed at it, and Explorer and wallpaper64.exe died of it on 2026-10-15. A pinned module --
// and, more simply, a loaded DLL file -- cannot be deleted or overwritten while any process has it, so the
// build's own `island_tray_hook.dll` must never be the file that gets loaded: replacing it would fail until
// every hooked application on the machine had restarted, which for Explorer means a reboot.
//
// So the bar stages a copy under a name derived from the module's CONTENT (`island_tray_hook-<fnv64>.dll`)
// and loads that. Rebuilding with a change produces a new name, which no lock can refuse; rebuilding
// without a change reuses the same file; and an older copy taken over by a newer one (the import walk
// accepts any `island_tray_hook*` pile, see TrayHook.cpp) stops being called even though it stays mapped
// in the applications that already had it.
//
// Copies that are no longer the current one are deleted here, and a copy that is still mapped somewhere
// simply refuses to be deleted -- which is the same information as "some application is still running the
// previous build", and costs nothing.
// WHETHER THIS PROCESS ALREADY HOLDS THE MODULE -- a different question from "did it load during this pass",
// and the one that matters now.
//
// The module PINS itself in every application it enters (see TrayHook.cpp), so an application reached by an
// earlier session, or by the startup pass ten seconds ago, keeps it for the rest of ITS life and keeps
// reporting every registration through it. The report file is truncated at the start of every pass, so such
// an application writes nothing during a later one -- and the log line then said "the module loaded in 0"
// about a machine where three applications were being watched the whole time. A number that reads like a
// failure while the mechanism is working is worse than no number at all.
bool ModuleAlreadyIn(DWORD pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsnicmp(entry.szModule, L"island_tray_hook", 16) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::wstring StageHookModule(const std::wstring& built) {
    const size_t slash = built.find_last_of(L'\\');
    const std::wstring directory = (slash == std::wstring::npos) ? std::wstring() : built.substr(0, slash + 1);

    unsigned long long hash = 1469598103934665603ull;   // FNV-1a, 64 bit
    {
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, built.c_str(), L"rb") != 0 || !stream) return built;
        unsigned char buffer[65536];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), stream)) > 0) {
            for (size_t i = 0; i < got; ++i) {
                hash ^= buffer[i];
                hash *= 1099511628211ull;
            }
        }
        fclose(stream);
    }

    wchar_t name[64] = {};
    _snwprintf_s(name, 64, _TRUNCATE, L"island_tray_hook-%016llX.dll", hash);
    const std::wstring staged = directory + name;

    if (GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(built.c_str(), staged.c_str(), TRUE)) {
        // A copy that cannot be made is not fatal: the built module still works. Only the upgrade path
        // suffers, and the bar says so in its log rather than pretending.
        LogAndPrint(L"  tray learn: could not stage %ls as %ls (%lu) -- loading it where it is\n",
                    kHookModule, name, GetLastError());
        return built;
    }

    WIN32_FIND_DATAW found = {};
    const std::wstring pattern = directory + L"island_tray_hook-*.dll";
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring candidate = directory + found.cFileName;
            if (_wcsicmp(candidate.c_str(), staged.c_str()) != 0) DeleteFileW(candidate.c_str());
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return staged;
}

std::wstring HomePath(const wchar_t* leaf) {
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    return std::wstring(home) + L"\\IslandBar\\" + leaf;
}

std::wstring BaseNameLower(const wchar_t* path) {
    if (!path || !*path) return std::wstring();
    const wchar_t* slash = wcsrchr(path, L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : std::wstring(path);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

// The processes running an image, by file name. Toolhelp rather than a snapshot of every process, for the
// same reason TrayPress uses it: the name is what the icon's registry entry gives us.
std::vector<DWORD> PidsForImage(const wchar_t* exePath) {
    std::vector<DWORD> pids;
    const std::wstring wanted = BaseNameLower(exePath);
    if (wanted.empty()) return pids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == wanted) pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

// ONE process per application, and the heaviest one. Measured: WeChat runs five processes and the tray icon
// lives in the largest; hooking the lightest one sees nothing at all.
DWORD HeaviestPid(const std::vector<DWORD>& pids) {
    DWORD best = 0;
    SIZE_T bestBytes = 0;
    for (DWORD pid : pids) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        PROCESS_MEMORY_COUNTERS counters = {};
        if (GetProcessMemoryInfo(process, &counters, sizeof(counters)) &&
            counters.WorkingSetSize > bestBytes) {
            bestBytes = counters.WorkingSetSize;
            best = pid;
        }
        CloseHandle(process);
    }
    return best;
}

// The value of ` key ` in a report line, as a bare token.
bool Field(const wchar_t* line, const wchar_t* key, wchar_t* out, size_t count) {
    wchar_t needle[32] = {};
    _snwprintf_s(needle, 32, _TRUNCATE, L" %ls ", key);
    const wchar_t* found = wcsstr(line, needle);
    if (!found) return false;
    found += wcslen(needle);
    size_t i = 0;
    while (found[i] && found[i] != L' ' && found[i] != L'"' && i + 1 < count) {
        out[i] = found[i];
        ++i;
    }
    out[i] = 0;
    return i > 0;
}

bool FieldNumber(const wchar_t* line, const wchar_t* key, unsigned* out, bool hex) {
    wchar_t text[32] = {};
    if (!Field(line, key, text, 32)) return false;
    wchar_t* end = nullptr;
    *out = static_cast<unsigned>(wcstoul(text, &end, hex ? 16 : 10));
    return end != text;
}

// One learned entry, ready for the cache file.
struct Learned {
    std::wstring exe;
    unsigned message = 0;
    std::wstring windowClass;
    unsigned uid = 0;
    unsigned version = 0;
};

// The lines already in the cache: what this run must not duplicate, and what it may have to correct (an
// application that changed its callback message).
std::vector<std::wstring> ReadCacheLines() {
    std::vector<std::wstring> lines;
    const std::wstring file = HomePath(kCacheFile);
    if (file.empty()) return lines;
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return lines;
    wchar_t buffer[1024] = {};
    while (fgetws(buffer, 1024, stream)) lines.push_back(buffer);
    fclose(stream);
    return lines;
}

// Whether a cache line belongs to (exe, uid). The file's own format: exe|message|window class|uid|...
bool LineIsFor(const std::wstring& line, const std::wstring& exe, unsigned uid) {
    if (line.empty() || line[0] == L'#') return false;
    const size_t first = line.find(L'|');
    const size_t second = first == std::wstring::npos ? std::wstring::npos : line.find(L'|', first + 1);
    const size_t third = second == std::wstring::npos ? std::wstring::npos : line.find(L'|', second + 1);
    const size_t fourth = third == std::wstring::npos ? std::wstring::npos : line.find(L'|', third + 1);
    if (fourth == std::wstring::npos) return false;
    std::wstring name = line.substr(0, first);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    if (name != exe) return false;
    const std::wstring uidText = line.substr(third + 1, fourth - third - 1);
    return static_cast<unsigned>(wcstoul(uidText.c_str(), nullptr, 10)) == uid;
}

// EVERYTHING THE MODULE HAS WRITTEN SO FAR, MERGED INTO THE CACHE THE PRESS PATH READS.
//
// Separate from the pass that installs the module, and that separation is what makes the module resident
// worth having: once an application has been reached, it keeps reporting (its own copy of the module stays
// mapped and its import slot stays redirected -- see TrayHook.cpp), so a registration that happens an hour
// later -- a new icon, an application restarted from its own menu -- arrives in this same file and is picked
// up by the next call of this function. The file is small, and merging twice is a no-op: a line that is
// already in the cache matches an existing entry and is skipped.
//
// Truncated by the installing pass (see LearnTrayCallbacks), so it never grows without bound.
void HarvestReport(TrayLearnResult* result) {
    if (!result) return;
    const std::wstring report = HomePath(kReportFile);
    std::vector<Learned> learned;
    if (!report.empty()) {
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, report.c_str(), L"r, ccs=UTF-8") == 0 && stream) {
            wchar_t line[1024] = {};
            std::vector<DWORD> attached;
            while (fgetws(line, 1024, stream)) {
                if (wcsstr(line, L"attached pid")) {
                    unsigned pid = 0;
                    if (FieldNumber(line, L"pid", &pid, false) &&
                        std::find(attached.begin(), attached.end(), pid) == attached.end()) {
                        attached.push_back(pid);
                    }
                    continue;
                }
                if (!wcsstr(line, L"call W NIM_ADD") && !wcsstr(line, L"call W NIM_MODIFY") &&
                    !wcsstr(line, L"call A NIM_ADD") && !wcsstr(line, L"call A NIM_MODIFY")) {
                    continue;
                }
                Learned item;
                wchar_t exe[260] = {};
                wchar_t hwndText[32] = {};
                if (!Field(line, L"exe", exe, 260) || !Field(line, L"hwnd", hwndText, 32)) continue;
                unsigned uid = 0;
                unsigned message = 0;
                unsigned version = 0;
                if (!FieldNumber(line, L"uid", &uid, false)) continue;
                if (!FieldNumber(line, L"msg", &message, true)) continue;
                if (!FieldNumber(line, L"version", &version, false)) version = 0;
                if (message == 0) continue;
                const HWND window = reinterpret_cast<HWND>(wcstoull(hwndText, nullptr, 16));
                if (!window || !IsWindow(window)) continue;
                wchar_t cls[128] = {};
                if (GetClassNameW(window, cls, 128) == 0 || !cls[0]) continue;
                item.exe = BaseNameLower(exe);
                item.message = message;
                item.windowClass = cls;
                item.uid = uid;
                item.version = version;
                ++result->captured;
                learned.push_back(item);
            }
            fclose(stream);
            result->appsReached = static_cast<int>(attached.size());
        }
    }

    std::vector<std::wstring> lines = ReadCacheLines();
    for (const Learned& item : learned) {
        bool replaced = false;
        bool duplicate = false;
        for (std::wstring& line : lines) {
            if (!LineIsFor(line, item.exe, item.uid)) continue;
            // A line that says `click` is a verdict from a press: this application ignores its own posted
            // callback. Learning must not talk the press path back into trying again every session.
            if (wcsstr(line.c_str(), L"|click") != nullptr) {
                duplicate = true;
                break;
            }
            if (wcsstr(line.c_str(), item.windowClass.c_str()) != nullptr) {
                duplicate = true;   // same application, same icon, same window class: nothing to say
                break;
            }
            wchar_t fresh[512] = {};
            _snwprintf_s(fresh, 512, _TRUNCATE, L"%ls|0x%04X|%ls|%u|both|%u\n", item.exe.c_str(),
                         item.message, item.windowClass.c_str(), item.uid, item.version);
            line = fresh;
            replaced = true;
            break;
        }
        if (duplicate) continue;
        if (!replaced) {
            wchar_t fresh[512] = {};
            _snwprintf_s(fresh, 512, _TRUNCATE, L"%ls|0x%04X|%ls|%u|both|%u\n", item.exe.c_str(),
                         item.message, item.windowClass.c_str(), item.uid, item.version);
            lines.push_back(fresh);
        }
        ++result->newEntries;
    }

    if (result->newEntries > 0) {
        const std::wstring file = HomePath(kCacheFile);
        FILE* stream = nullptr;
        if (!file.empty() && _wfopen_s(&stream, file.c_str(), L"w, ccs=UTF-8") == 0 && stream) {
            for (const std::wstring& line : lines) fputws(line.c_str(), stream);
            fclose(stream);
        }
    }
}

} // namespace

int CountCoveredIcons(const TrayIcons& icons) {
    const std::vector<std::wstring> lines = ReadCacheLines();
    int covered = 0;
    for (int i = 0; i < icons.Count(); ++i) {
        // AN ICON REGISTERED BY GUID IS NOT COUNTED, and that is a correction rather than a detail: its
        // `uid` is 0 because the registry entry carries no UID, so it used to match ITSELF against any cached
        // line for the same executable whose uid happened to be 0 -- on this machine, Explorer's Bluetooth
        // icon. The press path refuses such an icon anyway (the v4 callback layout needs a uid to put in
        // lParam), so counting it here only inflated the one number this project uses to decide whether the
        // automatic route is carrying its weight.
        if (!icons.At(i).hasUid) continue;
        const std::wstring exe = BaseNameLower(icons.ExePathAt(i));
        if (exe.empty()) continue;
        const TrayIcon& icon = icons.At(i);
        for (const std::wstring& line : lines) {
            if (LineIsFor(line, exe, icon.uid)) {
                ++covered;
                break;
            }
        }
    }
    return covered;
}

TrayLearnResult LearnTrayCallbacks(const TrayIcons& icons, bool broadcast) {
    TrayLearnResult result;
    result.icons = icons.Count();
    const ULONGLONG start = GetTickCount64();

    // --- where this bar and the module are ------------------------------------------------
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return result;
    std::wstring modulePath = self;
    const size_t slash = modulePath.find_last_of(L'\\');
    if (slash == std::wstring::npos) return result;
    modulePath = modulePath.substr(0, slash + 1) + kHookModule;
    // The staged copy, not the built file: see StageHookModule for why the file that gets loaded may not
    // be the file a later build replaces.
    modulePath = StageHookModule(modulePath);

    HMODULE hook = LoadLibraryW(modulePath.c_str());
    if (!hook) {
        LogAndPrint(L"  tray learn: no %ls next to the bar (%lu) -- nothing to learn with\n", kHookModule,
                    GetLastError());
        return result;
    }
    // THE ENTRY POINT MUST COME FROM THAT MODULE. A procedure address belonging to another binary is refused
    // for a cross-process thread hook, and refused silently: measured, the module loaded into the target and
    // the procedure was then never called once (HANDOFF 8.107).
    FARPROC procedure = GetProcAddress(hook, "IslandTrayHookProc");
    if (!procedure) {
        LogAndPrint(L"  tray learn: %ls does not export IslandTrayHookProc\n", kHookModule);
        FreeLibrary(hook);
        return result;
    }

    // --- the report starts empty, so everything in it belongs to this run -----------------
    const std::wstring report = HomePath(kReportFile);
    if (!report.empty()) DeleteFileW(report.c_str());

    // --- one attempt per application, and remember which threads we hooked -----------------
    std::vector<HHOOK> hooks;
    std::vector<DWORD> tried;
    for (int i = 0; i < icons.Count(); ++i) {
        if (!icons.At(i).hasUid) continue;   // see CountCoveredIcons: nothing can be learned for these
        const std::wstring exe = BaseNameLower(icons.ExePathAt(i));
        if (exe.empty()) continue;
        const DWORD pid = HeaviestPid(PidsForImage(icons.ExePathAt(i)));
        if (pid == 0 || std::find(tried.begin(), tried.end(), pid) != tried.end()) continue;
        tried.push_back(pid);
        ++result.appsTried;

        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        std::vector<DWORD> threads;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID == pid) threads.push_back(entry.th32ThreadID);
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        CloseHandle(process);

        for (DWORD thread : threads) {
            HHOOK installed = SetWindowsHookExW(WH_GETMESSAGE, reinterpret_cast<HOOKPROC>(procedure), hook,
                                                thread);
            if (installed) hooks.push_back(installed);
        }
    }

    // --- the signal that makes applications register again -------------------------------
    if (broadcast) {
        const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(HWND_BROADCAST, taskbarCreated, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000,
                            &ignored);
    }

    // The module is loaded the next time a hooked thread takes a message out of its queue, and the answers
    // arrive after that: two waits, not one, because "did the module arrive at all" and "did the application
    // register" are different questions and the log has to be able to tell them apart.
    Sleep(1200);
    Sleep(1500);

    for (HHOOK installed : hooks) UnhookWindowsHookEx(installed);
    // ... AND THIS PROCESS LETS GO OF THE MODULE, WHICH DOES NOT UNLOAD IT ANYWHERE.
    //
    // Measured, and it is the whole point of the pin: the applications keep it mapped (they pinned it
    // themselves -- see TrayHook.cpp), so the import slots this run redirected still point at live code and
    // the detour keeps reporting every registration from now on. FreeLibrary here drops only OUR reference.
    // Before the pin existed this same line, three seconds into the bar's life, was what left Explorer and
    // wallpaper64.exe with pointers into freed memory.
    FreeLibrary(hook);

    // ... and "already watched" counts as reached: see ModuleAlreadyIn.
    for (DWORD pid : tried) {
        if (ModuleAlreadyIn(pid)) ++result.appsAlreadyWatched;
    }

    HarvestReport(&result);

    result.covered = CountCoveredIcons(icons);
    result.ms = static_cast<long long>(GetTickCount64() - start);
    return result;
}

int HarvestLearnedCallbacks() {
    TrayLearnResult result;
    HarvestReport(&result);
    return result.newEntries;
}

std::wstring UncoveredApplications(const TrayIcons& icons) {
    const std::vector<std::wstring> lines = ReadCacheLines();
    std::vector<std::wstring> missing;
    for (int i = 0; i < icons.Count(); ++i) {
        const std::wstring exe = BaseNameLower(icons.ExePathAt(i));
        if (exe.empty()) continue;
        bool covered = false;
        for (const std::wstring& line : lines) {
            if (LineIsFor(line, exe, icons.At(i).uid)) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        if (std::find(missing.begin(), missing.end(), exe) == missing.end()) missing.push_back(exe);
    }
    std::sort(missing.begin(), missing.end());
    std::wstring joined;
    for (const std::wstring& exe : missing) {
        if (!joined.empty()) joined += L",";
        joined += exe;
    }
    return joined;
}

} // namespace island
