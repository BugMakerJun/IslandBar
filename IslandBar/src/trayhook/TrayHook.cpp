// =====================================================================
//  TrayHook.cpp -- INSIDE THE APPLICATION, WHEN IT TELLS THE SHELL ABOUT ITS TRAY ICON
//
//  WHAT THIS IS FOR. Pressing a tray cell without moving the pointer means POSTING the application's own
//  callback message to its own tray window, and that needs three things about the icon: the window, the
//  message number and the uid. Until now the message number was learned by hand, one application at a time,
//  with a Win32k trace and a third-party program in the room (HANDOFF 8.104) -- which the user called out,
//  rightly, as a chore rather than a feature.
//
//  The way out is to be present when the application REGISTERS its icon, because the structure it passes to
//  the documented API carries every field needed. This module is that presence: the bar loads it into each
//  running tray application with SetWindowsHookEx(WH_GETMESSAGE) on that application's threads, and once it
//  is inside, it points the application's OWN import slots for Shell_NotifyIconW/A at the detour below. The
//  application then writes down its own answer the next time it registers -- which the bar asks for with the
//  standard TaskbarCreated broadcast, the same signal the shell sends when Explorer restarts.
//
//  WHY IMPORT TABLES AND NOT CODE PATCHING. Overwriting the first bytes of the shell's function would catch
//  one more case -- an application that resolves the address itself and calls through its own pointer, which
//  is what WeChat's Qt tray does (measured: 0 import slots, so it is not caught here) -- but it means copying
//  an unknown number of instruction bytes and putting them back somewhere, in a process that is not ours. An
//  import slot is a pointer in a table; if this module is wrong about it, the call still reaches the real
//  function. That is the trade this file makes deliberately, and the missed case is written down in
//  HANDOFF 8.107 rather than hidden.
//
//  WHAT IT WRITES: %USERPROFILE%\IslandBar\tray_learned.txt, one line per call. The bar reads that file,
//  resolves each window's class and turns the lines into its callback cache.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATA and friends: WIN32_LEAN_AND_MEAN leaves them out of windows.h
#include <delayimp.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

namespace {

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
volatile LONG g_state = 0;   // 0 = not tried yet
volatile LONG g_hooked = 0;  // how many import slots were redirected
void* g_realW = nullptr;
void* g_realA = nullptr;

// Declared here so the first-call check inside LogCall can name them: they are defined below, and that check
// needs their addresses to ask the import tables "does anything point at us yet".
BOOL WINAPI DetourShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data);
BOOL WINAPI DetourShellNotifyIconA(DWORD message, PNOTIFYICONDATAA data);

void ReportPath(wchar_t* out, size_t count) {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) {
        wcscpy_s(out, count, L"C:\\island_bar_tray_learned.txt");
        return;
    }
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\IslandBar\\tray_learned.txt", home);
}

void ReportLine(const wchar_t* format, ...) {
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    wchar_t path[MAX_PATH] = {};
    ReportPath(path, MAX_PATH);
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path, L"a, ccs=UTF-8") == 0 && stream) {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fwprintf(stream, L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        va_list args;
        va_start(args, format);
        vfwprintf(stream, format, args);
        va_end(args);
        fputwc(L'\n', stream);
        fclose(stream);
    }
    LeaveCriticalSection(&g_lock);
}

// What one walk found: how many slots were redirected, and how many ALREADY pointed at the detour. The
// second number is the one that answers "is this application being watched", and it is not the same question
// as the first: measured, a process reported 0 redirects and then captured four calls anyway, which sent the
// first version of this log down the wrong path. Whatever install time says, the count that matters is the
// one taken at the moment of a call.
struct PatchCount {
    int patched = 0;
    int alreadyOurs = 0;
};

// The module a code address belongs to, and its file name.
HMODULE ModuleOfAddress(void* address, const wchar_t** leafOut) {
    if (leafOut) *leafOut = nullptr;
    if (!address) return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        return nullptr;
    }
    static wchar_t path[MAX_PATH];   // one walk at a time, and this is only used for its file name
    path[0] = 0;
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return module;
    const wchar_t* leaf = wcsrchr(path, L'\\');
    if (leafOut) *leafOut = leaf ? leaf + 1 : path;
    return module;
}

bool IsOurs(const wchar_t* leaf) {
    if (!leaf) return false;
    return _wcsicmp(leaf, L"island_tray_hook.dll") == 0 ||
           _wcsicmp(leaf, L"probe-tray-notifyicon-hook.dll") == 0;
}

// MAY THIS IMPORT SLOT BE REDIRECTED? The slot's NAME has already said it is Shell_NotifyIcon; this decides
// whether the value sitting in it may be overwritten. Three states may be taken over, and everything else is
// left exactly as it is:
//
//   * shell32's own function, which is the normal case;
//   * an EARLIER COPY OF OURS: measured, an application hooked by a previous build (or by the probe that came
//     before this module) keeps its slot pointing at that older module. The first version of this walk only
//     replaced slots holding shell32's address, so those applications reported "0 slots" and stayed unwatched
//     until somebody restarted them -- which is exactly what the user hit;
//   * the linker's delay-load stub, which lives in the module being walked.
//
// Somebody else's hook (MyDockFinder's, another tray tool's) is not ours to replace. Neither is an address
// that cannot be placed in any module at all: an earlier version of this treated that as "a stub, take it",
// and the walk then redirected junk -- 816 slots in Explorer, 288 in Razer, 78 in WeChat, all of them
// unrelated APIs pointed at Shell_NotifyIconW. Conservative by default is the correction.
bool MayTakeOver(void* address, HMODULE ownModule) {
    if (!address) return false;
    const wchar_t* leaf = nullptr;
    HMODULE owner = ModuleOfAddress(address, &leaf);
    if (!owner) return false;                   // not in any loaded module: not ours to touch
    if (owner == ownModule) return true;        // the linker's delay-load stub
    if (IsOurs(leaf)) return true;              // an older copy of ours
    return leaf && _wcsicmp(leaf, L"shell32.dll") == 0;
}

// Does this import slot NAME Shell_NotifyIcon? The name is the criterion, and it is the one the first version
// of this walk lacked: without it, "the slot belongs to shell32" was mistaken for "the slot is the function we
// want", and every one of an application's shell32 imports was redirected to this module's detour.
bool SlotNames(IMAGE_THUNK_DATA* slot, IMAGE_THUNK_DATA* byName, BYTE* base, const char* wanted) {
    if (!slot || !byName || !wanted) return false;
    if (byName->u1.Ordinal & IMAGE_ORDINAL_FLAG) return false;
    const IMAGE_IMPORT_BY_NAME* hint = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + byName->u1.AddressOfData);
    return hint && strcmp(reinterpret_cast<const char*>(hint->Name), wanted) == 0;
}

// How many slots one process may legitimately have redirected at once. An application imports
// Shell_NotifyIcon once, sometimes twice (A and W), across a handful of its own modules -- so a walk that
// wants to redirect dozens has gone wrong, and the honest answer is to stop rather than to keep pointing
// somebody else's imports at our function.
const int kMaxRedirectsPerProcess = 8;

// One import slot by address: an ordinary import already holds the address being replaced.
//
// `patch` false means COUNT ONLY, which is how the watch checks below ask "what points at the detour right
// now" without touching anything. The first version of that check passed a null target instead and got its
// early return: it reported 0 while the detour was demonstrably being called.
void PatchThunkArray(IMAGE_THUNK_DATA* iat, IMAGE_THUNK_DATA* byName, BYTE* base, void* targetW, void* detourW,
                     void* targetA, void* detourA, PatchCount* count, bool patch, HMODULE ownModule) {
    if (!iat || !detourW || !detourA || !count) return;
    for (; iat->u1.Function; ++iat, byName = byName ? byName + 1 : nullptr) {
        void* current = reinterpret_cast<void*>(iat->u1.Function);
        if (current == detourW || current == detourA) {
            ++count->alreadyOurs;
            continue;
        }
        if (!patch) continue;
        const bool wantsW = (current == targetW) || SlotNames(iat, byName, base, "Shell_NotifyIconW");
        const bool wantsA = (current == targetA) || SlotNames(iat, byName, base, "Shell_NotifyIconA");
        if (!wantsW && !wantsA) continue;
        if (!MayTakeOver(current, ownModule)) continue;
        if (count->patched >= kMaxRedirectsPerProcess) continue;   // see kMaxRedirectsPerProcess
        DWORD old = 0;
        if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
        iat->u1.Function = reinterpret_cast<ULONG_PTR>(wantsW ? detourW : detourA);
        DWORD ignored = 0;
        VirtualProtect(&iat->u1.Function, sizeof(void*), old, &ignored);
        ++count->patched;
    }
}

// One delay-loaded slot, by NAME: its slot may still hold the linker's stub rather than the function, so an
// address comparison would miss it.
void PatchDelaySlot(IMAGE_THUNK_DATA* slot, IMAGE_THUNK_DATA* byName, const char* wanted, void* detour,
                    PatchCount* count, bool patch, HMODULE ownModule) {
    if (!slot || !byName || !wanted || !detour || !count) return;
    if (byName->u1.Ordinal & IMAGE_ORDINAL_FLAG) return;
    const IMAGE_IMPORT_BY_NAME* hint = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(byName->u1.AddressOfData);
    if (!hint || strcmp(reinterpret_cast<const char*>(hint->Name), wanted) != 0) return;
    if (reinterpret_cast<void*>(slot->u1.Function) == detour) {
        ++count->alreadyOurs;
        return;
    }
    if (!patch) return;
    if (!MayTakeOver(reinterpret_cast<void*>(slot->u1.Function), ownModule)) return;
    DWORD old = 0;
    if (!VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return;
    slot->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
    DWORD ignored = 0;
    VirtualProtect(&slot->u1.Function, sizeof(void*), old, &ignored);
    ++count->patched;
}

// Every offset is checked against the module's SizeOfImage before it is followed: measured, an unchecked walk
// of this kind killed the target process on its third module.
void PatchModuleUnchecked(HMODULE module, size_t imageSize, void* targetW, void* detourW, void* targetA,
                          void* detourA, PatchCount* count, bool patch) {
    if (!module || imageSize < sizeof(IMAGE_DOS_HEADER) || !count) return;
    BYTE* base = reinterpret_cast<BYTE*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > imageSize)
        return;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress && imports.Size && imports.VirtualAddress < imageSize) {
        const IMAGE_IMPORT_DESCRIPTOR* descriptor =
            reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (; descriptor->Name && descriptor->Name < imageSize; ++descriptor) {
            if (descriptor->FirstThunk == 0 || descriptor->FirstThunk >= imageSize) continue;
            IMAGE_THUNK_DATA* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            IMAGE_THUNK_DATA* byName =
                descriptor->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                                                     base + descriptor->OriginalFirstThunk)
                                               : nullptr;
            PatchThunkArray(iat, byName, base, targetW, detourW, targetA, detourA, count, patch, module);
        }
    }

    const IMAGE_DATA_DIRECTORY& delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress && delayed.Size && delayed.VirtualAddress < imageSize) {
        const ImgDelayDescr* descriptor = reinterpret_cast<const ImgDelayDescr*>(base + delayed.VirtualAddress);
        for (; descriptor->rvaDLLName && descriptor->rvaDLLName < imageSize; ++descriptor) {
            if ((descriptor->grAttrs & dlattrRva) == 0) continue;   // only the RVA form is understood here
            if (descriptor->rvaIAT == 0 || descriptor->rvaIAT >= imageSize) continue;
            if (descriptor->rvaINT == 0 || descriptor->rvaINT >= imageSize) continue;
            IMAGE_THUNK_DATA* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->rvaIAT);
            IMAGE_THUNK_DATA* byName = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->rvaINT);
            int slots = 0;
            for (; iat->u1.Function && slots < 4096; ++iat, ++byName, ++slots) {
                PatchDelaySlot(iat, byName, "Shell_NotifyIconW", detourW, count, patch, module);
                PatchDelaySlot(iat, byName, "Shell_NotifyIconA", detourA, count, patch, module);
            }
        }
    }
}

PatchCount WalkProcessImports(void* targetW, void* detourW, void* targetA, void* detourA, bool patch) {
    PatchCount count;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                __try {
                    PatchModuleUnchecked(entry.hModule, entry.modBaseSize, targetW, detourW, targetA, detourA, &count, patch);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // A module whose headers do not walk cleanly is skipped: measured, an unguarded walk
                    // killed the target process on its third module.
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return count;
}

void* RealAddress(void* volatile* slot, const char* name) {
    if (*slot) return *slot;
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    if (!shell) return nullptr;
    PVOID address = reinterpret_cast<PVOID>(GetProcAddress(shell, name));
    InterlockedCompareExchangePointer(slot, address, nullptr);
    return *slot;
}

const char* MessageName(DWORD message) {
    switch (message) {
        case NIM_ADD: return "NIM_ADD";
        case NIM_MODIFY: return "NIM_MODIFY";
        case NIM_DELETE: return "NIM_DELETE";
        case NIM_SETFOCUS: return "NIM_SETFOCUS";
        case NIM_SETVERSION: return "NIM_SETVERSION";
        default: return "NIM_?";
    }
}

void LogCall(const char* which, DWORD message, const void* data, void* returnAddress) {
    // ASK THE IMPORT TABLES AGAIN, on the first call of this process. "install time said 0 slots" and "four
    // calls arrived anyway" were both true at once, and that contradiction is worth a line of its own rather
    // than a guess: nothing is redirected here (no target given), so what this counts is exactly the number of
    // slots that already point at the detour.
    static volatile LONG firstCall = 0;
    if (InterlockedCompareExchange(&firstCall, 1, 0) == 0) {
        const PatchCount now = WalkProcessImports(nullptr, reinterpret_cast<void*>(&DetourShellNotifyIconW),
                                                 nullptr, reinterpret_cast<void*>(&DetourShellNotifyIconA),
                                                 false);
        ReportLine(L"watch check pid %lu: %d slot(s) point at the detour at the moment of the first call "
                   L"(install time said %ld)",
                   GetCurrentProcessId(), now.alreadyOurs, g_hooked);
    }
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    const wchar_t* exe = leaf ? leaf + 1 : self;
    wchar_t caller[64] = {};
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(returnAddress), &owner) &&
        owner) {
        wchar_t full[MAX_PATH] = {};
        GetModuleFileNameW(owner, full, MAX_PATH);
        const wchar_t* callerLeaf = wcsrchr(full, L'\\');
        wcsncpy_s(caller, callerLeaf ? callerLeaf + 1 : full, _TRUNCATE);
    } else {
        wcscpy_s(caller, L"?");
    }
    if (!data) {
        ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd NULL", which, MessageName(message),
                   GetCurrentProcessId(), exe, caller);
        return;
    }
    HWND window = nullptr;
    UINT uid = 0;
    UINT callback = 0;
    UINT version = 0;
    UINT flags = 0;
    wchar_t tip[128] = {};
    if (strcmp(which, "W") == 0) {
        const NOTIFYICONDATAW* nid = reinterpret_cast<const NOTIFYICONDATAW*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        wcsncpy_s(tip, nid->szTip, _TRUNCATE);
    } else {
        const NOTIFYICONDATAA* nid = reinterpret_cast<const NOTIFYICONDATAA*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        MultiByteToWideChar(CP_ACP, 0, nid->szTip, -1, tip, 128);
    }
    // A tooltip may be a whole sentence with line breaks, and one call has to stay one line: the bar reads
    // this file line by line.
    for (wchar_t* p = tip; *p; ++p) {
        if (*p == L'\n' || *p == L'\r' || *p == L'"') *p = L' ';
    }
    // The hook count is repeated here on purpose: measured, the "hooked ... 0 slot(s)" line was written in
    // one process that then captured calls anyway, and a future session reading only that line would
    // conclude the mechanism cannot work. Whatever the reading at install time says, this line is the count
    // in force at the moment of the call.
    ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd %p uid %u msg 0x%04X version %u flags 0x%X "
               L"tip \"%ls\" (slots %ld)",
               which, MessageName(message), GetCurrentProcessId(), exe, caller, window, uid, callback,
               version, flags, tip, g_hooked);
}

typedef BOOL(WINAPI* ShellNotifyIconW_t)(DWORD, PNOTIFYICONDATAW);
typedef BOOL(WINAPI* ShellNotifyIconA_t)(DWORD, PNOTIFYICONDATAA);

BOOL WINAPI DetourShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data) {
    LogCall("W", message, data, _ReturnAddress());
    ShellNotifyIconW_t real = reinterpret_cast<ShellNotifyIconW_t>(RealAddress(&g_realW, "Shell_NotifyIconW"));
    if (!real) return FALSE;
    return real(message, data);
}

BOOL WINAPI DetourShellNotifyIconA(DWORD message, PNOTIFYICONDATAA data) {
    LogCall("A", message, data, _ReturnAddress());
    ShellNotifyIconA_t real = reinterpret_cast<ShellNotifyIconA_t>(RealAddress(&g_realA, "Shell_NotifyIconA"));
    if (!real) return FALSE;
    return real(message, data);
}

void EnsureHooked() {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    void* targetW = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconW")) : nullptr;
    void* targetA = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconA")) : nullptr;
    g_realW = targetW;
    g_realA = targetA;

    const PatchCount count =
        WalkProcessImports(targetW, reinterpret_cast<void*>(&DetourShellNotifyIconW), targetA,
                           reinterpret_cast<void*>(&DetourShellNotifyIconA), true);
    InterlockedExchange(&g_hooked, count.patched + count.alreadyOurs);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    ReportLine(L"hooked pid %lu exe %ls: %d slot(s) redirected, %d already pointing here; watching=%ls",
               GetCurrentProcessId(), leaf ? leaf + 1 : self, count.patched, count.alreadyOurs,
               (count.patched + count.alreadyOurs) ? L"yes"
                                                   : L"no -- it does not import Shell_NotifyIcon, so its tray "
                                                     L"code resolves the function by itself and this module "
                                                     L"cannot see it");
}

} // namespace

// The hook procedure the bar installs. It is inert: it looks at no message and changes none. Its presence is
// what gets this module loaded into the application at all, and the first call is what does the hooking.
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam, LPARAM lparam) {
    EnsureHooked();
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const wchar_t* leaf = wcsrchr(self, L'\\');
        ReportLine(L"attached pid %lu exe %ls", GetCurrentProcessId(), leaf ? leaf + 1 : self);
    }
    return TRUE;
}
