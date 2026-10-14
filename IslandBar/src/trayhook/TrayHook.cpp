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

// Declared HERE, at file scope, rather than inside the anonymous namespace below: the pin needs an address
// that is inside this module, and pinning is what stops the loader from ever unloading it. Inside the
// namespace the definition below would be a second, differently linked function of the same name.
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam, LPARAM lparam);

namespace {

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
volatile LONG g_state = 0;   // 0 = not tried yet
volatile LONG g_hooked = 0;  // how many import slots were redirected
void* g_realW = nullptr;
void* g_realA = nullptr;
void* g_realGetProcAddress = nullptr;

// Declared first: everything below uses them, and the detour of GetProcAddress hands out their addresses.
BOOL WINAPI DetourShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data);
BOOL WINAPI DetourShellNotifyIconA(DWORD message, PNOTIFYICONDATAA data);
typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
FARPROC WINAPI DetourGetProcAddress(HMODULE module, LPCSTR name);

// THE APPLICATION'S OWN WAY OF FINDING Shell_NotifyIcon, which the import walk cannot see.
//
// Measured on this machine (HANDOFF 8.107): WeChat's Qt tray resolves Shell_NotifyIcon at run time and calls
// it through its own pointer -- its import tables hold NO slot for either spelling, so patching the import
// table covers WeChat, QQ Music's launcher and the rest of the frameworks that do it this way: nothing to
// patch. The application still has to ASK for the address, though, and that question goes through
// `GetProcAddress` -- which IS an import, in every module that needs it.
//
// So the walk redirects those slots too, and this answers the application with our detour when the name it
// asks for is one of the two. Everything else is forwarded unchanged, through a pointer captured before any
// slot was touched, so this cannot recurse and cannot change any other behaviour of the process.
bool IsShell32Handle(HMODULE module) {
    if (!module) return false;
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    return shell && module == shell;
}

FARPROC WINAPI DetourGetProcAddress(HMODULE module, LPCSTR name) {
    GetProcAddress_t real = reinterpret_cast<GetProcAddress_t>(g_realGetProcAddress);
    if (!real) return nullptr;
    if (name && IsShell32Handle(module)) {
        // Two string comparisons on a short literal, in a function called very often: uppercase first
        // character is enough to dismiss every other name in the process.
        if (name[0] == 'S' || name[0] == 's') {
            if (_stricmp(name, "Shell_NotifyIconW") == 0) {
                return reinterpret_cast<FARPROC>(&DetourShellNotifyIconW);
            }
            if (_stricmp(name, "Shell_NotifyIconA") == 0) {
                return reinterpret_cast<FARPROC>(&DetourShellNotifyIconA);
            }
        }
    }
    return real(module, name);
}

// Is this process the bar itself? The global hook reaches every process on the desktop, its own included, and
// the bar must not patch its own import tables or report itself as a tray application.
bool IsOurOwnProcess() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;
    const wchar_t* leaf = wcsrchr(self, L'\\');
    return leaf && _wcsicmp(leaf + 1, L"island_bar.exe") == 0;
}

// A GUID as the registry and the cache file spell it: braces, upper-case hex. Written by hand rather than
// with StringFromGUID2 because that one lives in ole32, and this module deliberately links nothing but
// user32 and shell32 -- it is loaded into every GUI process on the desktop.
void GuidText(const GUID& guid, wchar_t* out, size_t count) {
    _snwprintf_s(out, count, _TRUNCATE, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                 static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                 guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
                 guid.Data4[7]);
}

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

// =====================================================================
//  TEMPORARY MEASUREMENT -- WHAT THE SHELL ACTUALLY SENDS TO THE TRAY WINDOW
//
//  DELETE THIS WHOLE BLOCK once the answer is written down in HANDOFF (and the marker file it needs).
//
//  The question it answers: pressing clash-verge's tray icon with a posted message does nothing, in EIGHT
//  shapes (v3 and v4 packing, WM_LBUTTONUP / NIN_SELECT / WM_LBUTTONDOWN+UP / WM_LBUTTONDBLCLK), while the
//  RIGHT button of the same icon opens its menu from a single post and a REAL click on the left does restore
//  its window. So the shell sends something this module does not know how to spell, and the only place that
//  can be seen is inside the application: this module is already there, pinned.
//
//  Two channels, because "sent" and "posted" are different Windows mechanisms and the project has already
//  measured that the shell's own notification does not appear in a message queue trace:
//
//    * WH_CALLWNDPROC, installed on this process's threads from inside      -> messages SENT to the window
//    * this module's existing WH_GETMESSAGE procedure (see below)           -> messages POSTED to it
//
//  Off unless the marker file exists, so it costs nothing and logs nothing in a normal session.
// =====================================================================
volatile LONG g_callbackMessage = 0;   // the callback message the application registered (from NIM_ADD)
void* volatile g_trayWindow = nullptr; // ... and the window it registered
bool g_watch = false;
HHOOK g_sentHooks[96] = {};
int g_sentHookCount = 0;

bool MarkerPresent() {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return false;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\IslandBar\\tray_watch.txt", home);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void ReportObserved(const wchar_t* channel, HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    wchar_t cls[96] = {};
    if (window) GetClassNameW(window, cls, 96);
    ReportLine(L"observed %ls pid %lu hwnd %p class \"%ls\" msg 0x%04X wparam 0x%08X lparam 0x%08X",
               channel, GetCurrentProcessId(), window, cls, message, static_cast<unsigned>(wparam),
               static_cast<unsigned>(lparam));
}

// Everything at or above WM_USER is the application's own business: a window procedure is sent no end of
// WM_* bookkeeping, and only the private range can be the tray callback.
bool WorthObserving(UINT message) {
    return message >= WM_USER || message == (UINT)g_callbackMessage;
}

LRESULT CALLBACK SentProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam && g_watch) {
        const CWPSTRUCT* call = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (call && WorthObserving(call->message)) {
            ReportObserved(L"sent", call->hwnd, call->message, call->wParam, call->lParam);
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

// One CALLWNDPROC hook per thread of this process. hMod is NULL because the procedure lives in this very
// process (documented for exactly this case), and the hooks are deliberately never removed: this module is
// pinned and the process is about to be measured, not lived in.
void InstallSentWatch() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (g_sentHookCount >= 96) break;
            const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, SentProc, nullptr, entry.th32ThreadID);
            if (hook) g_sentHooks[g_sentHookCount++] = hook;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

// What one walk found: how many slots were redirected, and how many ALREADY pointed at the detour. The
// second number is the one that answers "is this application being watched", and it is not the same question
// as the first: measured, a process reported 0 redirects and then captured four calls anyway, which sent the
// first version of this log down the wrong path. Whatever install time says, the count that matters is the
// one taken at the moment of a call.
struct PatchCount {
    int patched = 0;
    int alreadyOurs = 0;
    // How many of `patched` held an address that was in NO loaded module -- i.e. a slot this walk REPAIRED
    // rather than redirected. See OwnerOfSlot: a copy of this module that was unloaded leaves exactly that,
    // and it is what crashed Explorer and wallpaper64.exe on this machine on 2026-10-15.
    int healed = 0;
    // ... and the same three numbers for the GetProcAddress slots of this process.
    int patchedGetProcAddress = 0;
    int alreadyOursGetProcAddress = 0;
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
    // ANY VERSION OF THIS MODULE, by prefix rather than by exact name.
    //
    // The module is deployed under a content-stamped file name (see TrayLearn.cpp: the copy in the target
    // process is PINNED and therefore locked, so a new build must arrive as a new file). An application can
    // therefore hold slots pointing at `island_tray_hook-3f2a....dll` while this copy is
    // `island_tray_hook-91bc....dll`, and the newer copy is the one that has to take those slots over.
    if (_wcsnicmp(leaf, L"island_tray_hook", 16) == 0) return true;
    return _wcsicmp(leaf, L"probe-tray-notifyicon-hook.dll") == 0;
}

// WHAT A SLOT HOLDS RIGHT NOW, which is the question "may this be overwritten" is really asking.
//
// This used to be a yes/no over three cases, and the case it refused is the one that shipped a crash: an
// address that belongs to NO loaded module. Measured on this machine (2026-10-15): the earlier build
// unhooked and unloaded itself at the end of the learning window while applications' import slots still
// pointed at its detour, so Explorer crashed six times, wallpaper64.exe four times (Application log:
// module `island_tray_hook.dll_unloaded`, exception 0xc0000005 at offset 0x10a0 -- the detour's entry).
//
// A slot NAMED Shell_NotifyIcon (the name is checked before this, see PatchThunkArray) whose value is in no
// module at all is either that dangling pointer or a stub of a module that has been unloaded: either way the
// value is unusable, and pointing it at a live function is the repair rather than a risk. Every OTHER case
// is unchanged, including the one that stays out of bounds: a real module that is not ours, not shell32 and
// not the importing module itself (MyDockFinder's own hook, for one) is never touched.
enum class SlotOwner {
    NotOurs,   // inside some other module of the application's
    Shell,     // shell32's own function: the normal case
    Ours,      // an earlier copy of this module, or of the probe that came before it
    Stub,      // the importing module's own delay-load stub
    Orphan,    // in no loaded module: a dangling pointer left by a copy of ours that was unloaded
};

SlotOwner OwnerOfSlot(void* address, HMODULE ownModule) {
    if (!address) return SlotOwner::NotOurs;
    const wchar_t* leaf = nullptr;
    HMODULE owner = ModuleOfAddress(address, &leaf);
    if (!owner) return SlotOwner::Orphan;
    if (owner == ownModule) return SlotOwner::Stub;
    if (IsOurs(leaf)) return SlotOwner::Ours;
    if (leaf && _wcsicmp(leaf, L"shell32.dll") == 0) return SlotOwner::Shell;
    return SlotOwner::NotOurs;
}

bool MayTakeOver(SlotOwner owner) { return owner != SlotOwner::NotOurs; }

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

// ... AND A SECOND CAP, for the GetProcAddress slots (see DetourGetProcAddress). Those are expected in the
// dozens -- every module that resolves anything dynamically imports it -- so the limit only exists to stop a
// walk that has gone wrong, and it is deliberately well above what a real process carries.
const int kMaxGetProcAddressRedirects = 96;

// One import slot by address: an ordinary import already holds the address being replaced.
//
// `patch` false means COUNT ONLY, which is how the watch checks below ask "what points at the detour right
// now" without touching anything. The first version of that check passed a null target instead and got its
// early return: it reported 0 while the detour was demonstrably being called.
void PatchThunkArray(IMAGE_THUNK_DATA* iat, IMAGE_THUNK_DATA* byName, BYTE* base, void* targetW, void* detourW,
                     void* targetA, void* detourA, void* targetG, void* detourG, PatchCount* count, bool patch,
                     HMODULE ownModule) {
    if (!iat || !detourW || !detourA || !count) return;
    for (; iat->u1.Function; ++iat, byName = byName ? byName + 1 : nullptr) {
        void* current = reinterpret_cast<void*>(iat->u1.Function);
        if (current == detourW || current == detourA || (detourG && current == detourG)) {
            ++count->alreadyOurs;
            continue;
        }
        if (!patch) continue;
        // The application's own way of looking Shell_NotifyIcon up (see DetourGetProcAddress): patched by
        // name as well, because a slot holding GetProcAddress is what a dynamic framework calls.
        if (detourG && targetG &&
            ((current == targetG) || SlotNames(iat, byName, base, "GetProcAddress"))) {
            const SlotOwner owner = OwnerOfSlot(current, ownModule);
            // THE SLOT IS NAMED GetProcAddress, and the only value it can legitimately hold is the system's
            // own function -- which lives in kernelbase.dll (kernel32 forwards to it since Windows 7), a
            // module MayTakeOver deliberately refuses. The name check is what makes allowing it safe here:
            // this branch is only reached for a slot the import table itself calls GetProcAddress.
            const wchar_t* ownerLeaf = nullptr;
            ModuleOfAddress(current, &ownerLeaf);
            const bool isSystem = ownerLeaf && (_wcsicmp(ownerLeaf, L"kernel32.dll") == 0 ||
                                                _wcsicmp(ownerLeaf, L"kernelbase.dll") == 0);
            if (!MayTakeOver(owner) && !isSystem) continue;
            if (count->patchedGetProcAddress >= kMaxGetProcAddressRedirects) continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(detourG);
            DWORD ignored = 0;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &ignored);
            ++count->patchedGetProcAddress;
            continue;
        }
        const bool wantsW = (current == targetW) || SlotNames(iat, byName, base, "Shell_NotifyIconW");
        const bool wantsA = (current == targetA) || SlotNames(iat, byName, base, "Shell_NotifyIconA");
        if (!wantsW && !wantsA) continue;
        const SlotOwner owner = OwnerOfSlot(current, ownModule);
        if (!MayTakeOver(owner)) continue;
        if (count->patched >= kMaxRedirectsPerProcess) continue;   // see kMaxRedirectsPerProcess
        DWORD old = 0;
        if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
        iat->u1.Function = reinterpret_cast<ULONG_PTR>(wantsW ? detourW : detourA);
        DWORD ignored = 0;
        VirtualProtect(&iat->u1.Function, sizeof(void*), old, &ignored);
        ++count->patched;
        if (owner == SlotOwner::Orphan) ++count->healed;
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
    const SlotOwner owner = OwnerOfSlot(reinterpret_cast<void*>(slot->u1.Function), ownModule);
    if (!MayTakeOver(owner)) return;
    DWORD old = 0;
    if (!VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return;
    slot->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
    DWORD ignored = 0;
    VirtualProtect(&slot->u1.Function, sizeof(void*), old, &ignored);
    ++count->patched;
    if (owner == SlotOwner::Orphan) ++count->healed;
}

// Every offset is checked against the module's SizeOfImage before it is followed: measured, an unchecked walk
// of this kind killed the target process on its third module.
void PatchModuleUnchecked(HMODULE module, size_t imageSize, void* targetW, void* detourW, void* targetA,
                          void* detourA, void* targetG, void* detourG, PatchCount* count, bool patch) {
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
            PatchThunkArray(iat, byName, base, targetW, detourW, targetA, detourA, targetG, detourG, count,
                            patch, module);
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

PatchCount WalkProcessImports(void* targetW, void* detourW, void* targetA, void* detourA, void* targetG,
                              void* detourG, bool patch) {
    PatchCount count;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                __try {
                    PatchModuleUnchecked(entry.hModule, entry.modBaseSize, targetW, detourW, targetA, detourA,
                                         targetG, detourG, &count, patch);
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
                                                 nullptr, reinterpret_cast<void*>(&DetourGetProcAddress), false);
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
    // THE GUID, for the applications that name their icon with one instead of a uid -- which is the modern
    // way and, on this machine, what ChatGPT, the shell's own network and volume icons and the security
    // center all use. The press needs it because a uid of 0 is not an identity: several icons of one
    // application share it, and a press that matched on it would deliver one icon's callback to another.
    wchar_t guidText[64] = {};
    GUID registration = {};
    if (strcmp(which, "W") == 0) {
        const NOTIFYICONDATAW* nid = reinterpret_cast<const NOTIFYICONDATAW*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        registration = nid->guidItem;
        wcsncpy_s(tip, nid->szTip, _TRUNCATE);
    } else {
        const NOTIFYICONDATAA* nid = reinterpret_cast<const NOTIFYICONDATAA*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        registration = nid->guidItem;
        MultiByteToWideChar(CP_ACP, 0, nid->szTip, -1, tip, 128);
    }
    {
        GUID none = {};
        if (memcmp(&registration, &none, sizeof(GUID)) != 0) {
            GuidText(registration, guidText, 64);
        }
    }
    // A tooltip may be a whole sentence with line breaks, and one call has to stay one line: the bar reads
    // this file line by line.
    for (wchar_t* p = tip; *p; ++p) {
        if (*p == L'\n' || *p == L'\r' || *p == L'"') *p = L' ';
    }
    // TEMPORARY (see the block above): remember what this application registered, so the two observation
    // channels know what to look for. Removed with the rest of that block.
    if (message == NIM_ADD && callback) {
        g_callbackMessage = static_cast<LONG>(callback);
        g_trayWindow = window;
    }
    // The hook count is repeated here on purpose: measured, the "hooked ... 0 slot(s)" line was written in
    // one process that then captured calls anyway, and a future session reading only that line would
    // conclude the mechanism cannot work. Whatever the reading at install time says, this line is the count
    // in force at the moment of the call.
    ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd %p uid %u msg 0x%04X version %u flags 0x%X "
               L"guid %ls tip \"%ls\" (slots %ld)",
               which, MessageName(message), GetCurrentProcessId(), exe, caller, window, uid, callback,
               version, flags, guidText[0] ? guidText : L"-", tip, g_hooked);
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

    // The application's file name, for every line written from here on.
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    const wchar_t* exe = leaf ? leaf + 1 : self;

    // ---- PIN THIS MODULE IN THIS PROCESS, BEFORE THE FIRST SLOT IS TOUCHED ---------------------------------
    //
    // This is the fix for the crash this module shipped on 2026-10-15, and the reason it is first rather
    // than best-effort.
    //
    // What happened: the bar unhooked and freed this module at the end of its learning window, the loader
    // then unloaded the copy inside each application, and the import slots this walk had redirected kept
    // pointing at the detour's address -- in a page that no longer belonged to anything. The next time the
    // application called Shell_NotifyIcon it jumped into freed memory. Measured in the Application log:
    // Explorer.EXE six times, wallpaper64.exe four times, module `island_tray_hook.dll_unloaded`, exception
    // 0xc0000005 / 0xc000041d, fault offset 0x10a0 -- the detour's own entry point.
    //
    // GET_MODULE_HANDLE_EX_FLAG_PIN makes the loader ignore every later FreeLibrary for this module, for the
    // life of this process. The slot therefore always points at code that is still mapped, whether the hook
    // has been removed or the bar has exited. The cost is honest and has to be stated: the file this module
    // was loaded from stays locked until the application restarts, which is why the bar stages it under a
    // content-stamped name (see TrayLearn.cpp) instead of loading `island_tray_hook.dll` directly.
    //
    // If the pin FAILS the walk does not run at all: a redirected slot that can be unloaded again is exactly
    // the state that crashed those applications, and "not watching this application" is the safe answer.
    HMODULE pinned = nullptr;
    const BOOL pinOk = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&IslandTrayHookProc), &pinned);
    if (!pinOk || !pinned) {
        const DWORD error = GetLastError();
        // 2 = decided, and the decision was "no": the hook procedure must not try this again on every
        // message, and the bar is told why in the report.
        InterlockedExchange(&g_state, 2);
        ReportLine(L"hooked pid %lu exe %ls: REFUSED -- this module could not be pinned in the process "
                   L"(error %lu), so not one import slot was touched",
                   GetCurrentProcessId(), exe, error);
        return;
    }

    // ... AND NOT IN THE BAR'S OWN PROCESS. The global hook (see TrayLearn.cpp) delivers this module to every
    // GUI process on the desktop, its installer included; the bar must not repoint its own imports or appear
    // in the report as one of the applications being watched.
    if (IsOurOwnProcess()) {
        InterlockedExchange(&g_state, 2);
        ReportLine(L"hooked pid %lu exe %ls: this is the bar itself -- not patching its own imports",
                   GetCurrentProcessId(), exe);
        return;
    }

    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    void* targetW = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconW")) : nullptr;
    void* targetA = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconA")) : nullptr;
    g_realW = targetW;
    g_realA = targetA;

    // The application's own look-up route as well (see DetourGetProcAddress). kernel32's GetProcAddress is
    // taken BEFORE any slot is touched, so the detour can forward through it without ever re-entering itself.
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    void* targetG = kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "GetProcAddress")) : nullptr;
    if (kernel && !targetG) {
        // Forwarded exports live in kernelbase since Windows 7, and a GetProcAddress that cannot be captured
        // would make the walk redirect slots at a null pointer: measured risk, not a theoretical one.
        HMODULE base = GetModuleHandleW(L"kernelbase.dll");
        if (base) targetG = reinterpret_cast<void*>(GetProcAddress(base, "GetProcAddress"));
    }
    g_realGetProcAddress = targetG;

    const PatchCount count =
        WalkProcessImports(targetW, reinterpret_cast<void*>(&DetourShellNotifyIconW), targetA,
                           reinterpret_cast<void*>(&DetourShellNotifyIconA), targetG,
                           targetG ? reinterpret_cast<void*>(&DetourGetProcAddress) : nullptr, true);
    InterlockedExchange(&g_hooked, count.patched + count.alreadyOurs);
    ReportLine(L"hooked pid %lu exe %ls: %d slot(s) redirected (%d of them repaired a pointer into a module "
               L"that is no longer loaded), %d already pointing here, %d GetProcAddress slot(s) redirected "
               L"(the route a framework that resolves Shell_NotifyIcon by name uses), this module is pinned; "
               L"watching=%ls",
               GetCurrentProcessId(), exe, count.patched, count.healed, count.alreadyOurs,
               count.patchedGetProcAddress,
               (count.patched + count.alreadyOurs) ? L"yes"
                                                   : L"not yet -- it does not import Shell_NotifyIcon; if it "
                                                     L"asks for the address at run time the GetProcAddress "
                                                     L"slots above are what will catch it");

    // TEMPORARY: two channels onto what the shell sends this application's tray window. Off unless the marker
    // file exists; see the note at the top of that block.
    if (MarkerPresent()) {
        g_watch = true;
        InstallSentWatch();
        ReportLine(L"observing pid %lu exe %ls: marker present, %d thread(s) hooked for SENT messages and "
                   L"the posted channel is on", GetCurrentProcessId(), exe, g_sentHookCount);
    }
}

} // namespace

// The hook procedure the bar installs. It is inert: it looks at no message and changes none. Its presence is
// what gets this module loaded into the application at all, and the first call is what does the hooking.
//
// IT MAY NOT THROW, AND THAT IS NOT DEFENSIVENESS. This runs inside somebody else's message pump: an
// exception that leaves it either kills the application (measured on this machine as exception 0xc000041d,
// "a fatal exception escaped a user callback", which is how wallpaper64.exe died twice on 2026-10-15) or is
// swallowed and leaves the hook in an unknown state. Every failure mode this module has is already reported
// through the report file, so the answer is to let nothing escape.
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam, LPARAM lparam) {
    __try {
        EnsureHooked();
        // TEMPORARY (see the observation block above): messages the application PULLS OUT OF ITS QUEUE that
        // are for its own tray window, or that carry its registered callback number. The filter is what keeps
        // this from logging the application's whole private message traffic.
        if (g_watch && code >= 0 && lparam) {
            const MSG* message = reinterpret_cast<const MSG*>(lparam);
            // ... and only the private message range for the tray window: measured, its queue carries a
            // WM_TIMER every few milliseconds, which is the application beating, not the shell pressing.
            if (message &&
                (message->message == (UINT)g_callbackMessage ||
                 (message->hwnd == g_trayWindow && message->message >= WM_USER))) {
                ReportObserved(L"posted", message->hwnd, message->message, message->wParam, message->lParam);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_state, 2);   // see EnsureHooked: no second attempt after a failure
        ReportLine(L"hook procedure pid %lu: the install raised 0x%08lX and the application was left "
                   L"untouched", GetCurrentProcessId(), static_cast<unsigned long>(GetExceptionCode()));
    }
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
