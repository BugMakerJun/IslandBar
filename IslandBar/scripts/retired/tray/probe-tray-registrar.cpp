// =====================================================================
//  probe-tray-registrar.cpp -- A TRAY ICON WHOSE EVERY FIELD IS KNOWN
//
//  Build (once, with a normal MSVC environment):
//      cl /nologo /EHsc /Fe:probe-tray-registrar.exe probe-tray-registrar.cpp user32.lib shell32.lib
//
//  WHY. probe-tray-notifyicon-hook.cpp claims it can see an application register a tray icon. A claim like
//  that is only worth something if the registration it captures can be checked against values that are
//  already known, so this program registers an icon whose window, uid, callback message and tooltip are all
//  constants, waits, and removes it again.
//
//  WHAT IT REGISTERS:  window class  IslandBarProbeRegistrarWnd
//                      uid           7
//                      callback      0xABCD
//                      tooltip       ISLANDBAR-PROBE-4711
//  and it registers AFTER a few seconds of running, so that the hook can be inside the process before the
//  call happens (the driver hooks the process first, then waits).
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include <windows.h>
#include <stdio.h>

namespace {

const wchar_t kClassName[] = L"IslandBarProbeRegistrarWnd";
const UINT kUid = 7;
const UINT kCallback = 0xABCD;
const wchar_t kTooltip[] = L"ISLANDBAR-PROBE-4711";

void Say(const wchar_t* format, ...) {
    wchar_t text[512] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, 512, _TRUNCATE, format, args);
    va_end(args);
    wprintf(L"%ls\n", text);
    fflush(stdout);
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\IslandBar\\tray_notify_calls.txt", home);
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path, L"a, ccs=UTF-8") == 0 && stream) {
        fwprintf(stream, L"[registrar] %ls\n", text);
        fclose(stream);
    }
}

LRESULT CALLBACK RegistrarProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kCallback || message == WM_CONTEXTMENU || message == WM_USER) {
        Say(L"the probe's own callback arrived: message 0x%04X wparam 0x%llX lparam 0x%llX -- a posted "
            L"callback reaches this window", message, (unsigned long long)wparam, (unsigned long long)lparam);
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int delaySeconds = 5;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--delay") == 0 && i + 1 < argc) delaySeconds = _wtoi(argv[++i]);
    }

    WNDCLASSW cls = {};
    cls.lpfnWndProc = RegistrarProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kClassName;
    if (!RegisterClassW(&cls)) {
        Say(L"RegisterClass failed (%lu)", GetLastError());
        return 1;
    }
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"island_bar tray probe",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 200, 120, nullptr, nullptr, cls.hInstance,
                                  nullptr);
    if (!window) {
        Say(L"CreateWindowEx failed (%lu)", GetLastError());
        return 1;
    }
    Say(L"window %p ready, pid %lu -- registering in %d s", window, GetCurrentProcessId(), delaySeconds);

    // A TIMER, so that this thread actually retrieves messages. A module injected with SetWindowsHookEx is
    // loaded when the thread takes a message out of its queue, and a thread with an EMPTY queue never does --
    // the second run of this probe attached nothing at all for exactly that reason.
    SetTimer(window, 1, 50, nullptr);

    // PUMP WHILE WAITING, and that is not politeness. A module injected with SetWindowsHookEx is loaded into
    // the target when that thread next checks its message queue, so a target that sleeps first would load the
    // hook AFTER the registration this probe is trying to see. Measured the hard way: the first run attached
    // 5 s too late and captured nothing.
    {
        MSG pending = {};
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(delaySeconds) * 1000;
        while (GetTickCount64() < until) {
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            Sleep(15);
        }
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window;
    nid.uID = kUid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kCallback;
    nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wcscpy_s(nid.szTip, kTooltip);
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
    Say(L"Shell_NotifyIconW(NIM_ADD) -> %ls (hwnd %p uid %u msg 0x%04X tip \"%ls\")",
        added ? L"TRUE" : L"FALSE", window, kUid, kCallback, kTooltip);

    NOTIFYICONDATAW version = {};
    version.cbSize = sizeof(version);
    version.hWnd = window;
    version.uID = kUid;
    version.uVersion = NOTIFYICON_VERSION_4;   // so the capture can be checked against a known version too
    const BOOL setVersion = Shell_NotifyIconW(NIM_SETVERSION, &version);
    Say(L"Shell_NotifyIconW(NIM_SETVERSION, %d) -> %ls", NOTIFYICON_VERSION_4,
        setVersion ? L"TRUE" : L"FALSE");

    MSG message = {};
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 20000) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(20);
    }

    NOTIFYICONDATAW remove = {};
    remove.cbSize = sizeof(remove);
    remove.hWnd = window;
    remove.uID = kUid;
    Shell_NotifyIconW(NIM_DELETE, &remove);
    Say(L"icon removed, done");
    DestroyWindow(window);
    return 0;
}
