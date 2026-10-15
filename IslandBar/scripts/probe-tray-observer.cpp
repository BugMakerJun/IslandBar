// =====================================================================
//  probe-tray-observer.cpp -- THE READ-ONLY OBSERVER, built as a DLL that the probe loads
//  into explorer with a WH_CALLWNDPROC hook on the shell's UI thread.
//
//  WHAT IT IS FOR (HANDOFF 8.139, route A). An application registering a tray icon sends the shell a
//  synchronous WM_COPYDATA carrying the extended NOTIFYICONDATA, and the one field that cannot be found
//  anywhere else is uCallbackMessage (the registry records have ExecutablePath, IconSnapshot, UID,
//  IconGuid, InitialTooltip, IsPromoted, Publisher -- and no callback). With that number our own tray can
//  send the application its own left and right clicks, which is what makes an application's own menu
//  appear, instantly, under the cell the user clicked -- no pointer movement, no flicker, and nothing
//  sent to the application except the messages it already expects.
//
//  IT OBSERVES AND NOTHING ELSE. It does not change the message, does not call anything on the sending
//  application, does not touch the shell's own tray state, and holds no UI. Every message is passed on
//  with CallNextHookEx unchanged; the only thing it does is read the payload and append one line to a file.
//
//  IT PINS ITSELF, and that is not optional: a module that has been loaded into another process and can
//  then be unloaded leaves a dangling import while that process lives, which has taken Explorer down six
//  times in this project's history (ARCHITECTURE.md). GET_MODULE_HANDLE_EX_FLAG_PIN is the fix the project
//  settled on, and it is the first thing this module does.
//
//  THE OFFSETS ARE THE MEASURED ONES, kept in docs/TRAY-RESEARCH-RETIRED.md: tag 0x34753423, command at
//  +0x004, tray window at +0x00C, uid at +0x010, flags at +0x014, uCallbackMessage at +0x018, the tooltip
//  at +0x020, uVersion at +0x328. A payload that does not carry the tag is ignored rather than guessed at.
//
//  WHAT A LINE CARRIES NOW (and why each field is there). "registration from pid" is the RECEIVER, which is
//  always the shell -- Explorer forwards the message on the applications' behalf, so that number is
//  useless; "windowPid" and "windowClass" come from the payload's own tray window handle and are the only
//  honest answer to "which process registered this". "version" is the field that decides HOW the
//  application expects to be called back (classic: lParam is the mouse message, NOTIFYICON_VERSION_4:
//  HIWORD(wParam) is the event and lParam carries packed coordinates) -- sending the wrong one is silent,
//  which is exactly the failure this record has to be able to explain.
//
//  ASCII-only on purpose, like everything in scripts/.
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

constexpr unsigned kTag = 0x34753423u;
constexpr size_t kMinimum = 1024;
constexpr size_t kOffsetCommand = 0x004;
constexpr size_t kOffsetWindow = 0x00C;
constexpr size_t kOffsetUid = 0x010;
constexpr size_t kOffsetFlags = 0x014;
constexpr size_t kOffsetCallback = 0x018;
constexpr size_t kOffsetTip = 0x020;
constexpr size_t kOffsetVersion = 0x328;

// One file for the whole experiment, opened for every line: the module lives inside explorer for a short
// while and there is nothing to gain from holding a handle open in another process' address space.
void Append(const wchar_t* text) {
    wchar_t path[MAX_PATH] = {};
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\IslandBar\\tray_observed.txt", profile);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a, ccs=UTF-8") != 0 || !file) return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    std::fwprintf(file, L"[%02u:%02u:%02u.%03u] %ls\n", now.wHour, now.wMinute, now.wSecond,
                  now.wMilliseconds, text);
    std::fclose(file);
}

} // namespace

// The hook procedure, exported because SetWindowsHookEx has to find it by name inside this module.
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayObserverProc(int code, WPARAM wparam,
                                                                       LPARAM lparam) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* call = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (call && call->message == WM_COPYDATA) {
            const COPYDATASTRUCT* copy = reinterpret_cast<const COPYDATASTRUCT*>(call->lParam);
            if (copy && copy->dwData == 1 && copy->lpData &&
                copy->cbData >= static_cast<DWORD>(kMinimum)) {
                const unsigned char* bytes = static_cast<const unsigned char*>(copy->lpData);
                unsigned tag = 0;
                std::memcpy(&tag, bytes, 4);
                if (tag == kTag) {
                    unsigned command = 0, window = 0, uid = 0, flags = 0, callback = 0, version = 0;
                    std::memcpy(&command, bytes + kOffsetCommand, 4);
                    std::memcpy(&window, bytes + kOffsetWindow, 4);
                    std::memcpy(&uid, bytes + kOffsetUid, 4);
                    std::memcpy(&flags, bytes + kOffsetFlags, 4);
                    std::memcpy(&callback, bytes + kOffsetCallback, 4);
                    std::memcpy(&version, bytes + kOffsetVersion, 4);

                    wchar_t tip[129] = {};
                    const wchar_t* raw = reinterpret_cast<const wchar_t*>(bytes + kOffsetTip);
                    for (int i = 0; i < 128 && raw[i] != L'\0'; ++i) tip[i] = raw[i];

                    // The payload's tray window says who really registered: the header's pid is the shell.
                    wchar_t windowClass[64] = L"(gone)";
                    DWORD windowPid = 0;
                    const HWND owner = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(window));
                    if (IsWindow(owner)) {
                        // Reading a class name and a process id of a window in this session is safe from
                        // here: no message is sent to it, and nothing is changed.
                        GetClassNameW(owner, windowClass, 64);
                        GetWindowThreadProcessId(owner, &windowPid);
                    }

                    wchar_t line[640] = {};
                    _snwprintf_s(line, 640, _TRUNCATE,
                                 L"registration from pid %lu: command=%u uid=%u flags=0x%X "
                                 L"callback=0x%04X window=0x%X windowPid=%u windowClass='%ls' "
                                 L"version=%u tip='%ls'",
                                 GetCurrentProcessId(), command, uid, flags, callback, window, windowPid,
                                 windowClass, version, tip);
                    Append(line);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        // See the header: an unloadable module in another process is how Explorer was crashed six times.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(instance), &pinned);
        Append(L"the observer is loaded and pinned in this process");
    }
    return TRUE;
}

// =======================================================================================
//  THE SECOND EXPORT: a message dump, for the one question the first one cannot answer.
//
//  WHAT IT IS FOR. A registration gives us the callback NUMBER but not the wire format the application
//  actually expects on the way back. Two applications on this machine answered a message we sent
//  (QQ音乐 with its menu, the elevated Task Manager with a real Win32 menu) and one did not: ChatGPT's host
//  window takes the message, spends 30-50 ms on it, and shows nothing -- while a REAL right click on its
//  real icon does show a menu (WinUI, raised by a different process of the same application). So the shell
//  must be sending it something else, and the only honest way to find out is to listen to that application
//  while a real click is performed on its icon.
//
//  WHAT IT LOGS. Every message that could be a tray event, with the window it landed on, the window's class
//  and both parameters -- so the exact encoding can be read off and replayed. Nothing is changed, nothing is
//  swallowed: the message is passed on with CallNextHookEx exactly as received.
// =======================================================================================
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayMessageDumpProc(int code, WPARAM wparam,
                                                                           LPARAM lparam) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* call = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (call) {
            const UINT message = call->message;
            const bool interesting =
                message >= WM_USER || message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN ||
                message == WM_RBUTTONUP || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
                message == WM_LBUTTONDBLCLK || message == WM_ACTIVATE || message == WM_NCACTIVATE ||
                message == WM_SETFOCUS || message == WM_KILLFOCUS;
            if (interesting) {
                wchar_t className[64] = {};
                GetClassNameW(call->hwnd, className, 64);
                wchar_t line[400] = {};
                _snwprintf_s(line, 400, _TRUNCATE,
                             L"message 0x%04X hwnd=0x%X class='%ls' wParam=0x%llX lParam=0x%llX",
                             message, static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(call->hwnd)),
                             className,
                             static_cast<unsigned long long>(static_cast<ULONG_PTR>(call->wParam)),
                             static_cast<unsigned long long>(static_cast<ULONG_PTR>(call->lParam)));
                Append(line);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
