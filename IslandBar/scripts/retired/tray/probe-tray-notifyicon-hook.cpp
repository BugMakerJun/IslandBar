// =====================================================================
//  probe-tray-notifyicon-hook.cpp -- WHAT DOES AN APPLICATION ASK THE SHELL FOR?
//
//  Build (once, with a normal MSVC environment):
//      cl /nologo /EHsc /LD /Fe:probe-tray-notifyicon-hook.dll probe-tray-notifyicon-hook.cpp user32.lib shell32.lib
//
//  Used by scripts\probe-tray-notifyicon.ps1, which loads it into ONE target process -- by default the
//  controlled registrar (probe-tray-registrar.exe), then a real application.
//
//  WHY THIS EXISTS. A press has to be a POSTED CALLBACK to be invisible, and that needs three things about
//  the icon: the owning window, the callback message number, and the icon's uid. Today the message number is
//  learned by hand, one application at a time, which the user rightly called out (HANDOFF 8.106), and the
//  route that reads them out of Explorer's memory turned out to be a heap lottery rather than a table: the
//  same scan gave 12 entries, then 8, then no WeChat entry at all, with eight near-identical copies for one
//  Task Manager icon.
//
//  The alternative -- the one MyDockFinder itself uses (its dockmod64.dll is loaded inside explorer AND
//  inside every application) -- is to be present in the application when it tells the shell about its icon.
//  That is a DOCUMENTED API call, Shell_NotifyIcon, and every field needed is in the structure the
//  application passes. So this module hooks that call in-process and writes down what it sees.
//
//  HOW IT HOOKS, AND WHY NOT CODE PATCHING. The import tables of the process are rewritten: every slot that
//  imports Shell_NotifyIconW/A from shell32 is pointed at the detour below. That is done by name for
//  delay-loaded imports (whose slots may still be unresolved) and by address for ordinary ones. No
//  instruction of shell32 is modified, so nothing has to be un-patched and a wrong guess cannot corrupt a
//  function body. What it does NOT catch: an application that resolves the address itself with
//  LoadLibrary+GetProcAddress and calls through its own pointer. That is exactly what the probe is for -- if
//  the report stays empty for a real application, say so rather than pretending.
//
//  WHAT IT WRITES: %USERPROFILE%\IslandBar\tray_notify_calls.txt, one line per call, with the caller module,
//  the message (NIM_ADD and friends), the window, the uid, the callback message, the version and the tooltip.
//  A line starting with "attached" means the module is inside that process; "hooked" says how many import
//  slots were redirected.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include <windows.h>
#include <delayimp.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <cstring>

namespace {

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
volatile LONG g_state = 0;   // 0 = not tried, 1 = done (see g_hooked)
volatile LONG g_hooked = 0;  // how many import slots were redirected
void* g_realW = nullptr;
void* g_realA = nullptr;

void ReportPath(wchar_t* out, size_t count) {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) {
        wcscpy_s(out, count, L"C:\\probe-tray-notify-calls.txt");
        return;
    }
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\IslandBar\\tray_notify_calls.txt", home);
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

// The module a return address belongs to, as a bare file name: it says whether the application itself made
// the call or some framework of its own (Qt, Electron, .NET) did.
void CallerModuleName(void* address, wchar_t* out, size_t count) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t full[MAX_PATH] = {};
        GetModuleFileNameW(module, full, MAX_PATH);
        const wchar_t* leaf = wcsrchr(full, L'\\');
        wcsncpy_s(out, count, leaf ? leaf + 1 : full, _TRUNCATE);
        return;
    }
    wcscpy_s(out, count, L"?");
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

// One import slot, by address: used for ordinary imports, where the slot already holds the address of the
// function in shell32 we are replacing.
int PatchThunkArray(IMAGE_THUNK_DATA* thunks, void* target, void* detour) {
    int patched = 0;
    if (!thunks || !target || !detour) return 0;
    for (; thunks->u1.Function; ++thunks) {
        if (reinterpret_cast<void*>(thunks->u1.Function) != target) continue;
        DWORD old = 0;
        if (!VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
        thunks->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
        DWORD ignored = 0;
        VirtualProtect(&thunks->u1.Function, sizeof(void*), old, &ignored);
        ++patched;
    }
    return patched;
}

// One import slot, by NAME: used for delay-loaded imports, whose slot may still point at the linker's
// delay-load stub rather than at the function, so an address comparison would miss it.
int PatchDelaySlot(IMAGE_THUNK_DATA* slot, IMAGE_THUNK_DATA* byName, const char* wanted, void* detour) {
    if (!slot || !byName || !wanted || !detour) return 0;
    if (byName->u1.Ordinal & IMAGE_ORDINAL_FLAG) return 0;   // no name to compare: leave it alone
    const IMAGE_IMPORT_BY_NAME* hint =
        reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(byName->u1.AddressOfData);
    if (!hint || std::strcmp(reinterpret_cast<const char*>(hint->Name), wanted) != 0) return 0;
    DWORD old = 0;
    if (!VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return 0;
    slot->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
    DWORD ignored = 0;
    VirtualProtect(&slot->u1.Function, sizeof(void*), old, &ignored);
    return 1;
}

// Every offset is checked against the module's own SizeOfImage before it is followed, and the whole walk is
// guarded: measured, an unchecked walk died on the third module of a target process and took it down.
int PatchModuleUnchecked(HMODULE module, size_t imageSize, void* targetW, void* detourW, void* targetA,
                         void* detourA) {
    if (!module) return 0;
    BYTE* base = reinterpret_cast<BYTE*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > imageSize)
        return 0;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    int patched = 0;
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress && imports.Size && imports.VirtualAddress < imageSize) {
        const IMAGE_IMPORT_DESCRIPTOR* descriptor =
            reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (; descriptor->Name && descriptor->Name < imageSize; ++descriptor) {
            if (descriptor->FirstThunk == 0 || descriptor->FirstThunk >= imageSize) continue;
            IMAGE_THUNK_DATA* first =
                reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            patched += PatchThunkArray(first, targetW, detourW);
            patched += PatchThunkArray(first, targetA, detourA);
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
            if (!iat || !byName) continue;
            int slots = 0;
            for (; iat->u1.Function && slots < 4096; ++iat, ++byName, ++slots) {
                patched += PatchDelaySlot(iat, byName, "Shell_NotifyIconW", detourW);
                patched += PatchDelaySlot(iat, byName, "Shell_NotifyIconA", detourA);
            }
        }
    }
    return patched;
}

int PatchModule(HMODULE module, size_t imageSize, void* targetW, void* detourW, void* targetA, void* detourA) {
    __try {
        return PatchModuleUnchecked(module, imageSize, targetW, detourW, targetA, detourA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReportLine(L"hooking: a module faulted while its imports were walked (skipped)");
        return 0;
    }
}

void* ResolveReal(void* volatile* slot, const char* name) {
    if (*slot) return *slot;
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    if (!shell) return nullptr;
    PVOID address = reinterpret_cast<PVOID>(GetProcAddress(shell, name));
    InterlockedCompareExchangePointer(slot, address, nullptr);
    return *slot;
}

void LogCall(const char* which, DWORD message, const void* data, void* returnAddress) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    wchar_t caller[64] = {};
    CallerModuleName(returnAddress, caller, 64);
    if (!data) {
        ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd NULL", which, MessageName(message),
                   GetCurrentProcessId(), leaf ? leaf + 1 : self, caller);
        return;
    }
    HWND window = nullptr;
    UINT uid = 0;
    UINT callback = 0;
    UINT version = 0;
    UINT flags = 0;
    const wchar_t* tip = L"";
    wchar_t guid[64] = {};
    if (strcmp(which, "W") == 0) {
        const NOTIFYICONDATAW* nid = reinterpret_cast<const NOTIFYICONDATAW*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        tip = nid->szTip;
        if (nid->uFlags & NIF_GUID) {
            _snwprintf_s(guid, 64, _TRUNCATE, L"%08lX-%04X-%04X", nid->guidItem.Data1, nid->guidItem.Data2,
                         nid->guidItem.Data3);
        } else {
            wcscpy_s(guid, 64, L"-");
        }
    } else {
        const NOTIFYICONDATAA* nid = reinterpret_cast<const NOTIFYICONDATAA*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        wchar_t wide[128] = {};
        MultiByteToWideChar(CP_ACP, 0, nid->szTip, -1, wide, 128);
        tip = wide;
    }
    ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd %p uid %u msg 0x%04X version %u flags 0x%X "
               L"tip \"%ls\" guid %ls",
               which, MessageName(message), GetCurrentProcessId(), leaf ? leaf + 1 : self, caller, window,
               uid, callback, version, flags, tip, guid);
}

typedef BOOL(WINAPI* ShellNotifyIconW_t)(DWORD, PNOTIFYICONDATAW);
typedef BOOL(WINAPI* ShellNotifyIconA_t)(DWORD, PNOTIFYICONDATAA);

BOOL WINAPI DetourShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data) {
    LogCall("W", message, data, _ReturnAddress());
    ShellNotifyIconW_t real = reinterpret_cast<ShellNotifyIconW_t>(ResolveReal(&g_realW, "Shell_NotifyIconW"));
    if (!real) return FALSE;
    return real(message, data);
}

BOOL WINAPI DetourShellNotifyIconA(DWORD message, PNOTIFYICONDATAA data) {
    LogCall("A", message, data, _ReturnAddress());
    ShellNotifyIconA_t real = reinterpret_cast<ShellNotifyIconA_t>(ResolveReal(&g_realA, "Shell_NotifyIconA"));
    if (!real) return FALSE;
    return real(message, data);
}

void EnsureHooked() {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;
    ReportLine(L"hooking starts in pid %lu", GetCurrentProcessId());
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    void* targetW = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconW")) : nullptr;
    void* targetA = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconA")) : nullptr;
    g_realW = targetW;
    g_realA = targetA;
    ReportLine(L"hooking: shell32 %ls, Shell_NotifyIconW %p, Shell_NotifyIconA %p",
               shell ? L"loaded" : L"MISSING", targetW, targetA);

    int patched = 0;
    int modules = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    ReportLine(L"hooking: module snapshot %ls", snapshot == INVALID_HANDLE_VALUE ? L"failed" : L"ok");
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                ++modules;
                patched += PatchModule(entry.hModule, entry.modBaseSize, targetW,
                                       reinterpret_cast<void*>(&DetourShellNotifyIconW), targetA,
                                       reinterpret_cast<void*>(&DetourShellNotifyIconA));
                if (patched) ReportLine(L"hooking: after module %d '%ls' patched=%d", modules, entry.szModule,
                                        patched);
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    InterlockedExchange(&g_hooked, patched);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    ReportLine(L"hooked pid %lu exe %ls: %d import slot(s) in %d module(s) point at the detour; shell32 %ls",
               GetCurrentProcessId(), leaf ? leaf + 1 : self, patched, modules,
               (targetW && targetA) ? L"answered" : L"DID NOT ANSWER -- nothing can be hooked");
}

} // namespace

// The hook procedure exists so that SetWindowsHookEx can deliver this module into a process at all (see the
// driver script). It is inert: it looks at no message and changes none.
extern "C" __declspec(dllexport) LRESULT CALLBACK TrayNotifyIconHookProc(int code, WPARAM wparam, LPARAM lparam) {
    static volatile LONG firstCall = 0;
    if (InterlockedCompareExchange(&firstCall, 1, 0) == 0) {
        ReportLine(L"hook proc called in pid %lu (code %d) -- the thread is pumping", GetCurrentProcessId(),
                   code);
    }
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
    } else if (reason == DLL_PROCESS_DETACH) {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const wchar_t* leaf = wcsrchr(self, L'\\');
        ReportLine(L"detached pid %lu exe %ls (hooked %ld slot(s))", GetCurrentProcessId(),
                   leaf ? leaf + 1 : self, g_hooked);
    }
    return TRUE;
}
