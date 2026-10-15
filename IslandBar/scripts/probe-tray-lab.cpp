// =====================================================================
//  probe-tray-lab.cpp -- the three experiments that decide how a tray of our own
//  can be built. One exe, four subcommands.
//
//      probe-tray-lab.exe --host <seconds>     become a fake tray host
//      probe-tray-lab.exe --client <phase>     an application that registers a tray icon
//      probe-tray-lab.exe --crop on|off|status clip the live taskbar down to its tray area
//      probe-tray-lab.exe --reparent           can the shell's tray window be re-parented?
//      probe-tray-lab.exe --move-tray <secs>   re-parent it into a visible window of OURS, hold, put back
//      probe-tray-lab.exe --park <x> <y> <s>   crop to the tray, then PARK the whole taskbar so the tray sits at x,y
//      probe-tray-lab.exe --uia [invoke <name>] what does UI Automation see of the tray right now?
//      probe-tray-lab.exe --traycom [watch <secs>]  does CLSID_TrayNotify exist, and what interfaces does it answer to?
//      probe-tray-lab.exe --traynotify <secs>  register a callback with the shell's own tray and watch it
//      probe-tray-lab.exe --traywindow <secs> [x y]  a tray of OUR OWN: our window, our drawing, the shell pressing
//      probe-tray-lab.exe --traysample <secs> [x y] a look at what a tray of our own would be: real icons, our style
//      probe-tray-lab.exe --rightclick <name> [post|send]  can an icon's OWN right-click menu be reached?
//      probe-tray-lab.exe --appbar <x> <y> [secs]  the user's idea: can the taskbar be MOVED (AppBar, not SetWindowPos)?
//      probe-tray-lab.exe --thumbdump <x> <y> <w> <h> [clip]  the REAL tray drawn inside our own window
//      probe-tray-lab.exe --observe <secs> [broadcast]  route A, step 1: watch the registrations in explorer
//      probe-tray-lab.exe --sendright <name> <x> <y> [v4|v3|all]  route A, step 2: its own right click
//      probe-tray-lab.exe --traylive [x y]  THE CASE TO LOOK AT: our cells, the applications' own behaviour
//      probe-tray-lab.exe --dump <name>   what does the SHELL really send that application? (real click)
//
//  WHY THESE THREE. The tray is not an API: it is a WINDOW inside the shell's process, and
//  Shell_NotifyIcon is shell32 handing a registration to it (measured 2026-10-15: a synchronous
//  WM_COPYDATA with dwData=1 and a 1,484 byte extended NOTIFYICONDATA payload). So the question
//  "how do we get a real tray" reduces to:
//
//    E1  can we BE that window without being the shell?   (--host + --client)
//    E2  if not, can we at least SHOW the real one?        (--crop)
//    E3  and can it be MOVED out of the taskbar?           (--reparent)
//
//  E1 needs two processes: a host that answers to the class name shell32 looks for, and a client
//  that calls the real Shell_NotifyIcon. The interesting readings are (a) whether the host's
//  RegisterClassExW even succeeds, (b) what FindWindow returns once both windows exist, and
//  (c) which window the registration actually reaches -- including whether a client that already
//  registered once keeps talking to the FIRST host it used (a per-process cache) or looks again.
//
//  THE PAYLOAD OFFSETS ARE THE RETIRED ONES, kept in docs/TRAY-RESEARCH-RETIRED.md: this probe
//  reuses a measurement that was calibrated against a control program (uid 7 / message 0xABCD /
//  tip "ISLANDBAR-PROBE-4711") and cross-checked against Steam and the GUID-registered icons.
//
//  NOTHING HERE CHANGES THE MACHINE BEYOND ITS OWN WINDOW AND -- DELIBERATELY -- ONE OF TWO THINGS
//  THE CALLER ASKS FOR: the taskbar region (restored with --crop off) or a re-parent (undone
//  immediately). It never writes the registry and never touches the shell's configuration.
//
//  ASCII-only on purpose, like every file in scripts/.
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <objbase.h>
#include <uiautomation.h>
#include <winstring.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

FILE* g_log = nullptr;

void Log(const wchar_t* fmt, ...) {
    wchar_t body[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(body, 1024, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    if (g_log) {
        std::fwprintf(g_log, L"[%02u:%02u:%02u.%03u] %ls\n", now.wHour, now.wMinute, now.wSecond,
                      now.wMilliseconds, body);
        std::fflush(g_log);
    }
    std::wprintf(L"%ls\n", body);
    std::fflush(stdout);
}

void OpenLog() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* dot = std::wcsrchr(exe, L'.');
    if (dot) *dot = L'\0';
    wcsncat_s(exe, L".log", _TRUNCATE);
    // _SH_DENYNO, because this probe is deliberately run as TWO processes at once (a host and a
    // client) and both must be able to append: with the default sharing the second open failed and
    // that process' whole report went to a console nobody can see -- measured, and it looked exactly
    // like "the host received nothing".
    g_log = _wfsopen(exe, L"a, ccs=UTF-8", _SH_DENYNO);
}

// --- the retired wire format (docs/TRAY-RESEARCH-RETIRED.md) ---------------
constexpr unsigned kTag = 0x34753423u;
constexpr size_t kMinimum = 1024;
constexpr size_t kOffsetCommand = 0x004;
constexpr size_t kOffsetUid = 0x010;
constexpr size_t kOffsetFlags = 0x014;
constexpr size_t kOffsetCallback = 0x018;
constexpr size_t kOffsetTip = 0x020;
constexpr size_t kOffsetVersion = 0x328;

bool Decode(const unsigned char* bytes, size_t size, unsigned* command, unsigned* uid,
            unsigned* callback, unsigned* flags, unsigned* version, wchar_t* tip, size_t tipCapacity) {
    if (!bytes || size < kMinimum) return false;
    unsigned tag = 0;
    std::memcpy(&tag, bytes, 4);
    if (tag != kTag) return false;
    std::memcpy(command, bytes + kOffsetCommand, 4);
    std::memcpy(uid, bytes + kOffsetUid, 4);
    std::memcpy(flags, bytes + kOffsetFlags, 4);
    std::memcpy(callback, bytes + kOffsetCallback, 4);
    std::memcpy(version, bytes + kOffsetVersion, 4);
    const wchar_t* text = reinterpret_cast<const wchar_t*>(bytes + kOffsetTip);
    size_t i = 0;
    while (i + 1 < tipCapacity && text[i] != L'\0') {
        tip[i] = text[i];
        ++i;
    }
    tip[i] = L'\0';
    return true;
}

// --- E1, the host side ----------------------------------------------------
HWND g_host = nullptr;
int g_received = 0;

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COPYDATA) {
        const COPYDATASTRUCT* copy = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        DWORD pid = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(wparam), &pid);
        ++g_received;
        Log(L"HOST: WM_COPYDATA #%d from hwnd %p (pid %lu)  dwData=%llu  cbData=%llu", g_received,
            reinterpret_cast<void*>(wparam), pid, static_cast<unsigned long long>(copy->dwData),
            static_cast<unsigned long long>(copy->cbData));
        if (copy->dwData == 1 && copy->lpData && copy->cbData >= kMinimum) {
            unsigned command = 0, uid = 0, callback = 0, flags = 0, version = 0;
            wchar_t tip[128] = {};
            if (Decode(static_cast<const unsigned char*>(copy->lpData),
                       static_cast<size_t>(copy->cbData), &command, &uid, &callback, &flags, &version,
                       tip, 128)) {
                Log(L"HOST:   decoded: command=%u uid=%u callback=0x%04X flags=0x%X version=%u tip='%ls'",
                    command, uid, callback, flags, version, tip);
            } else {
                Log(L"HOST:   the payload does not carry the measured tag");
            }
        }
        return TRUE;   // what the real shell answers
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int RunHost(int seconds) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Shell_TrayWnd";      // the name shell32 looks for

    const ATOM atom = RegisterClassExW(&wc);
    Log(L"HOST: RegisterClassExW(\"Shell_TrayWnd\") -> atom %u (last error %lu)", atom,
        atom ? 0UL : GetLastError());
    if (atom == 0) {
        Log(L"HOST: the class name is taken session-wide -- a window of that name cannot be made here");
        return 1;
    }
    g_host = CreateWindowExW(0, L"Shell_TrayWnd", L"traylab host", WS_OVERLAPPED, 0, 0, 16, 16,
                             nullptr, nullptr, wc.hInstance, nullptr);
    Log(L"HOST: our window is %p (never shown)", reinterpret_cast<void*>(g_host));
    if (!g_host) return 1;

    const HWND found = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD foundPid = 0;
    if (found) GetWindowThreadProcessId(found, &foundPid);
    Log(L"HOST: FindWindow(\"Shell_TrayWnd\") returns %p (pid %lu) -- ours is %p -> %ls",
        reinterpret_cast<void*>(found), foundPid, reinterpret_cast<void*>(g_host),
        found == g_host ? L"OURS" : L"not ours (the shell's came first)");

    Log(L"HOST: listening for %d second(s); a client can register while this runs", seconds);
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg = {};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    Log(L"HOST: done -- %d registration message(s) reached this window", g_received);
    DestroyWindow(g_host);
    return 0;
}

// --- E1, the client side --------------------------------------------------
LRESULT CALLBACK ClientProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    // A real application would act on its callback here. The probe only records that one arrived.
    if (message == 0xABCD) {
        Log(L"CLIENT: our own callback 0xABCD arrived (wParam=%llu lParam=%llu)",
            static_cast<unsigned long long>(wparam), static_cast<unsigned long long>(lparam));
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int RunClient(const char* phase) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ClientProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabClient";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, L"TrayLabClient", L"traylab client", WS_OVERLAPPED, 0, 0, 16, 16,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    Log(L"CLIENT: our window is %p", reinterpret_cast<void*>(window));

    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD trayPid = 0;
    if (tray) GetWindowThreadProcessId(tray, &trayPid);
    Log(L"CLIENT: FindWindow(\"Shell_TrayWnd\") -> %p (pid %lu)", reinterpret_cast<void*>(tray),
        trayPid);

    NOTIFYICONDATAW icon = {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = 7;
    icon.uFlags = NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = 0xABCD;
    wcscpy_s(icon.szTip, L"TRAYLAB-PROBE-4711");

    const BOOL added = Shell_NotifyIconW(NIM_ADD, &icon);
    Log(L"CLIENT: NIM_ADD -> %d", added ? 1 : 0);

    if (std::strcmp(phase, "hold") == 0) {
        Log(L"CLIENT: holding 15 s so a host can appear, then re-registering");
        Sleep(15000);
        const BOOL modified = Shell_NotifyIconW(NIM_MODIFY, &icon);
        Log(L"CLIENT: NIM_MODIFY -> %d   (does the NEW host see this one?)", modified ? 1 : 0);
    } else if (std::strcmp(phase, "modify") == 0) {
        const BOOL modified = Shell_NotifyIconW(NIM_MODIFY, &icon);
        Log(L"CLIENT: NIM_MODIFY -> %d", modified ? 1 : 0);
    }
    Sleep(500);
    const BOOL removed = Shell_NotifyIconW(NIM_DELETE, &icon);
    Log(L"CLIENT: NIM_DELETE -> %d", removed ? 1 : 0);
    DestroyWindow(window);
    return 0;
}

// --- E2, clip the live taskbar down to its tray area ----------------------
int RunCrop(const char* what) {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        Log(L"CROP: there is no Shell_TrayWnd on this desktop");
        return 1;
    }
    RECT whole = {};
    GetWindowRect(taskbar, &whole);
    const HWND tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT trayRect = {};
    if (tray) GetWindowRect(tray, &trayRect);
    const bool visible = IsWindowVisible(taskbar) != FALSE;
    Log(L"CROP: taskbar %ld,%ld %ldx%ld visible=%d | TrayNotifyWnd %p %ld,%ld %ldx%ld",
        whole.left, whole.top, whole.right - whole.left, whole.bottom - whole.top, visible ? 1 : 0,
        reinterpret_cast<void*>(tray), trayRect.left, trayRect.top, trayRect.right - trayRect.left,
        trayRect.bottom - trayRect.top);

    if (std::strcmp(what, "status") == 0) {
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        const int kind = GetWindowRgn(taskbar, region);
        RECT box = {};
        if (kind != ERROR) GetRgnBox(region, &box);
        Log(L"CROP: GetWindowRgn -> kind %d, box %ld,%ld %ldx%ld", kind, box.left, box.top,
            box.right - box.left, box.bottom - box.top);
        DeleteObject(region);
        return 0;
    }

    if (std::strcmp(what, "off") == 0) {
        SetWindowRgn(taskbar, nullptr, TRUE);
        Log(L"CROP: region cleared (the whole taskbar is back)");
        return 0;
    }

    // Show it first: the bar hides the taskbar, and a hidden window cannot be looked at.
    ShowWindow(taskbar, SW_SHOWNA);
    Sleep(200);

    // `sliver` keeps one DIP of the tray: the taskbar is still "visible" as far as Windows is concerned,
    // and nothing of it is on screen. That is the state a tray of our own would want to leave it in.
    const bool sliver = std::strcmp(what, "sliver") == 0;
    HRGN region = sliver
                      ? CreateRectRgn(trayRect.left - whole.left, trayRect.top - whole.top,
                                      trayRect.left - whole.left + 1, trayRect.top - whole.top + 1)
                      : CreateRectRgn(trayRect.left - whole.left, trayRect.top - whole.top,
                                      trayRect.right - whole.left, trayRect.bottom - whole.top);
    if (!region) {
        Log(L"CROP: CreateRectRgn failed (%lu)", GetLastError());
        return 1;
    }
    const int applied = SetWindowRgn(taskbar, region, TRUE);   // the region is owned from here
    Log(L"CROP: SetWindowRgn(tray only) -> %d", applied);
    Sleep(400);

    // Would a click at the middle of the tray land on the shell?
    const POINT middle = {(trayRect.left + trayRect.right) / 2, (trayRect.top + trayRect.bottom) / 2};
    const HWND hit = WindowFromPoint(middle);
    DWORD hitPid = 0;
    if (hit) GetWindowThreadProcessId(hit, &hitPid);
    wchar_t cls[128] = {};
    if (hit) GetClassNameW(hit, cls, 128);
    Log(L"CROP: WindowFromPoint(%ld,%ld) -> %p '%ls' (pid %lu)", middle.x, middle.y,
        reinterpret_cast<void*>(hit), cls, hitPid);

    // ... and what a click in the part that was clipped away would land on.
    const POINT elsewhere = {whole.left + 100, whole.top + 10};
    const HWND hit2 = WindowFromPoint(elsewhere);
    DWORD hit2Pid = 0;
    if (hit2) GetWindowThreadProcessId(hit2, &hit2Pid);
    wchar_t cls2[128] = {};
    if (hit2) GetClassNameW(hit2, cls2, 128);
    Log(L"CROP: WindowFromPoint(%ld,%ld) -> %p '%ls' (pid %lu)  [the clipped part]",
        elsewhere.x, elsewhere.y, reinterpret_cast<void*>(hit2), cls2, hit2Pid);
    return 0;
}

// --- E3, can the shell's tray window be re-parented ----------------------
int RunReparent() {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!tray) {
        Log(L"REPARENT: no TrayNotifyWnd to try");
        return 1;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabHost";
    RegisterClassExW(&wc);
    const HWND ours = CreateWindowExW(0, L"TrayLabHost", L"traylab parent", WS_OVERLAPPED, 0, 0, 200,
                                      200, nullptr, nullptr, wc.hInstance, nullptr);
    Log(L"REPARENT: TrayNotifyWnd %p -> our window %p", reinterpret_cast<void*>(tray),
        reinterpret_cast<void*>(ours));

    SetLastError(0);
    const HWND previous = SetParent(tray, ours);
    const DWORD error = GetLastError();
    // WIDE literals: the narrow overload of this project's own log line has bitten it twice, and a
    // narrow string handed to a wide %s is what printed "ACCEPTED" as four Chinese-looking glyphs the
    // first time this probe ran.
    Log(L"REPARENT: SetParent -> %p, GetLastError=%lu -> %ls", reinterpret_cast<void*>(previous), error,
        previous ? L"ACCEPTED" : L"REFUSED");

    RECT after = {};
    GetWindowRect(tray, &after);
    Log(L"REPARENT: tray rect afterwards %ld,%ld %ldx%ld", after.left, after.top,
        after.right - after.left, after.bottom - after.top);

    if (previous) {
        SetParent(tray, taskbar);
        Log(L"REPARENT: put back under the taskbar");
    }
    DestroyWindow(ours);
    return 0;
}

// --- E3b, and if it can be moved, do the PIXELS come with it? -------------
//
//  E3 proved the call is accepted (it returns the old parent with no error). That is not the same
//  question as "does the tray then live in my window": Windows 11 paints this taskbar from a
//  Windows.UI.Composition.DesktopWindowContentBridge spanning all 2,560 DIP, and TrayNotifyWnd is
//  only one part of that surface. This mode moves the window into a visible window of ours and holds
//  it there long enough to be photographed, then puts it back and destroys OUR window only after the
//  tray is safe again -- destroying the parent with the tray still inside would destroy the tray.
int RunMoveTray(int seconds) {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!tray) {
        Log(L"MOVE: no TrayNotifyWnd to move");
        return 1;
    }
    RECT before = {};
    GetWindowRect(tray, &before);
    Log(L"MOVE: tray starts at %ld,%ld %ldx%ld", before.left, before.top,
        before.right - before.left, before.bottom - before.top);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabMoveHost";
    wc.hbrBackground = CreateSolidBrush(RGB(200, 40, 120));   // magenta, so a missing tray is obvious
    RegisterClassExW(&wc);
    const HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"TrayLabMoveHost", L"tray move host",
                                      WS_POPUP | WS_VISIBLE, 700, 500, 400, 80, nullptr, nullptr,
                                      wc.hInstance, nullptr);
    Log(L"MOVE: our window is %p at 700,500 400x80", reinterpret_cast<void*>(host));
    if (!host) return 1;
    UpdateWindow(host);
    Sleep(500);

    SetLastError(0);
    const HWND previous = SetParent(tray, host);
    Log(L"MOVE: SetParent -> %p (last error %lu)", reinterpret_cast<void*>(previous), GetLastError());
    SetWindowPos(tray, nullptr, 20, 24, 144, 32, SWP_NOZORDER | SWP_SHOWWINDOW);
    RECT during = {};
    GetWindowRect(tray, &during);
    Log(L"MOVE: tray rect while moved %ld,%ld %ldx%ld -- hold for %d s (photograph now)",
        during.left, during.top, during.right - during.left, during.bottom - during.top, seconds);

    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg = {};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    // Where would a click land while it is moved?
    const POINT middle = {(during.left + during.right) / 2, (during.top + during.bottom) / 2};
    const HWND hit = WindowFromPoint(middle);
    DWORD hitPid = 0;
    if (hit) GetWindowThreadProcessId(hit, &hitPid);
    wchar_t cls[128] = {};
    if (hit) GetClassNameW(hit, cls, 128);
    Log(L"MOVE: WindowFromPoint(%ld,%ld) -> %p '%ls' (pid %lu)", middle.x, middle.y,
        reinterpret_cast<void*>(hit), cls, hitPid);

    SetParent(tray, taskbar);
    SetWindowPos(tray, nullptr, before.left, before.top, before.right - before.left,
                 before.bottom - before.top, SWP_NOZORDER | SWP_SHOWWINDOW);
    RECT after = {};
    GetWindowRect(tray, &after);
    Log(L"MOVE: put back -- tray rect now %ld,%ld %ldx%ld (was %ld,%ld)", after.left, after.top,
        after.right - after.left, after.bottom - after.top, before.left, before.top);
    DestroyWindow(host);
    Log(L"MOVE: done");
    return 0;
}

// --- E4, park the cropped taskbar so the real tray sits where we want -----
//
//  E3b answered the pixel question the hard way: the tray WINDOW moves (input follows it) but the
//  PIXELS do not, because Windows 11 paints the taskbar from a Windows.UI.Composition.
//  DesktopWindowContentBridge that spans the whole 2,560 DIP and is a CHILD OF THE TASKBAR. So move
//  the thing the bridge belongs to. Crop first (so nothing but the tray is painted), then move the
//  taskbar by the delta that lands TrayNotifyWnd on the wanted screen point.
int RunPark(int trayX, int trayY, int seconds) {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!tray) {
        Log(L"PARK: no TrayNotifyWnd");
        return 1;
    }
    RECT bar = {}, trayRect = {};
    GetWindowRect(taskbar, &bar);
    GetWindowRect(tray, &trayRect);
    Log(L"PARK: taskbar %ld,%ld | tray %ld,%ld %ldx%ld | wanted %d,%d", bar.left, bar.top,
        trayRect.left, trayRect.top, trayRect.right - trayRect.left, trayRect.bottom - trayRect.top,
        trayX, trayY);

    ShowWindow(taskbar, SW_SHOWNA);
    Sleep(200);
    HRGN region = CreateRectRgn(trayRect.left - bar.left, trayRect.top - bar.top,
                                trayRect.right - bar.left, trayRect.bottom - bar.top);
    Log(L"PARK: crop -> %d", SetWindowRgn(taskbar, region, TRUE));
    Sleep(300);

    const int dx = trayX - trayRect.left;
    const int dy = trayY - trayRect.top;
    const BOOL moved = SetWindowPos(taskbar, nullptr, bar.left + dx, bar.top + dy, 0, 0,
                                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Sleep(400);
    RECT afterBar = {}, afterTray = {};
    GetWindowRect(taskbar, &afterBar);
    GetWindowRect(tray, &afterTray);
    Log(L"PARK: SetWindowPos(dx=%d dy=%d) -> %d | taskbar now %ld,%ld | tray now %ld,%ld %ldx%ld",
        dx, dy, moved, afterBar.left, afterBar.top, afterTray.left, afterTray.top,
        afterTray.right - afterTray.left, afterTray.bottom - afterTray.top);

    const POINT middle = {(afterTray.left + afterTray.right) / 2, (afterTray.top + afterTray.bottom) / 2};
    const HWND hit = WindowFromPoint(middle);
    DWORD hitPid = 0;
    if (hit) GetWindowThreadProcessId(hit, &hitPid);
    wchar_t cls[128] = {};
    if (hit) GetClassNameW(hit, cls, 128);
    Log(L"PARK: WindowFromPoint(%ld,%ld) -> %p '%ls' (pid %lu)", middle.x, middle.y,
        reinterpret_cast<void*>(hit), cls, hitPid);
    Log(L"PARK: holding %d s -- photograph now", seconds);

    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg = {};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, QS_ALLINPUT)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    // And does it STAY there, or does the shell drag it back?
    RECT settled = {};
    GetWindowRect(tray, &settled);
    Log(L"PARK: %d s later the tray is at %ld,%ld (asked for %d,%d)", seconds, settled.left,
        settled.top, trayX, trayY);

    SetWindowRgn(taskbar, nullptr, TRUE);
    SetWindowPos(taskbar, nullptr, bar.left, bar.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Log(L"PARK: restored (region cleared, taskbar back at %ld,%ld)", bar.left, bar.top);
    return 0;
}

// --- E5, WHAT UIA STILL SEES WHEN THE NATIVE TRAY IS NOT ON SCREEN -------------
//
//  This is the question a tray of OUR OWN turns on. Drawing our own icons needs to know which icons the
//  shell is really showing (the registry lies -- it lists NVIDIA icons that are not in the tray at all),
//  and pressing one needs a route that works. UI Automation answers both, but only while the tray is
//  "alive": measured 2026-10-15, with the taskbar hidden Shell_TrayWnd is not in the tree at all.
//
//  So the states below are the experiment: hidden / clipped to a sliver / normal. If the tree and the
//  Invoke both survive CLIPPED, then a tray can be drawn by us with no native pixels anywhere on screen.
namespace {

template <typename T>
struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** Put() { if (value) { value->Release(); value = nullptr; } return &value; }
    T* Get() const { return value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};

std::wstring ElementText(IUIAutomationElement* element, PROPERTYID id) {
    if (!element) return {};
    VARIANT value = {};
    if (FAILED(element->GetCurrentPropertyValue(id, &value))) return {};
    std::wstring text;
    if (value.vt == VT_BSTR && value.bstrVal) {
        text.assign(value.bstrVal, SysStringLen(value.bstrVal));
    }
    VariantClear(&value);
    return text;
}

int RunUia(const wchar_t* invokeName) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        Log(L"UIA: CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(com));
        return 1;
    }
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.Put()))) ||
        !automation) {
        Log(L"UIA: could not create the automation client");
        return 1;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    Log(L"UIA: taskbar %p visible=%d", reinterpret_cast<void*>(taskbar),
        taskbar && IsWindowVisible(taskbar) ? 1 : 0);

    // The taskbar first, then the tray window: this is the order the retired shell/TrayUia.cpp used, and
    // it is kept because it was measured -- on this build the buttons hang off the taskbar.
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationElement> desktop;
    if (FAILED(automation->GetRootElement(desktop.Put())) || !desktop) {
        Log(L"UIA: no desktop element");
        return 1;
    }
    for (const wchar_t* className : {L"Shell_TrayWnd", L"SystemTray_Main"}) {
        ComPtr<IUIAutomationCondition> byClass;
        VARIANT value = {};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(className);
        if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value, byClass.Put()))) {
            continue;
        }
        for (TreeScope scope : {TreeScope_Children, TreeScope_Descendants}) {
            if (SUCCEEDED(desktop->FindFirst(scope, byClass.Get(), root.Put())) && root) {
                Log(L"UIA: root for '%ls' found (scope %d)", className, static_cast<int>(scope));
                break;
            }
        }
        if (root) break;
    }
    if (!root) {
        Log(L"UIA: THE TRAY IS NOT IN THE TREE AT ALL in this state");
        return 0;
    }

    // Every button under it, with the three things that decide whether a tray of ours could use them:
    // its name (identity), whether automation says it is OFF SCREEN, and whether it offers Invoke.
    VARIANT buttonType = {};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> buttons;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, buttons.Put()))) {
        Log(L"UIA: could not build the button condition");
        return 1;
    }
    ComPtr<IUIAutomationElementArray> all;
    if (FAILED(root->FindAll(TreeScope_Descendants, buttons.Get(), all.Put())) || !all) {
        Log(L"UIA: no buttons under the tray root");
        return 0;
    }
    int count = 0;
    all->get_Length(&count);
    Log(L"UIA: %d button(s) under the tray root", count);
    IUIAutomationElement* chosen = nullptr;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(all->GetElement(i, element.Put())) || !element) continue;
        const std::wstring name = ElementText(element.Get(), UIA_NamePropertyId);
        const std::wstring cls = ElementText(element.Get(), UIA_ClassNamePropertyId);
        BOOL offscreen = FALSE;
        element->get_CurrentIsOffscreen(&offscreen);
        RECT box = {};
        element->get_CurrentBoundingRectangle(&box);
        ComPtr<IUIAutomationInvokePattern> invoke;
        const bool hasInvoke = SUCCEEDED(element->GetCurrentPatternAs(
            UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
            reinterpret_cast<void**>(invoke.Put()))) && invoke;
        Log(L"UIA:   [%d] '%.60ls' class='%ls' offscreen=%d rect=%ld,%ld %ldx%ld invoke=%d", i,
            name.c_str(), cls.c_str(), offscreen ? 1 : 0, box.left, box.top, box.right - box.left,
            box.bottom - box.top, hasInvoke ? 1 : 0);
        if (invokeName && !chosen && !name.empty() && name.find(invokeName) != std::wstring::npos) {
            element.Get()->AddRef();
            chosen = element.Get();
        }
    }

    if (invokeName) {
        if (!chosen) {
            Log(L"UIA: nothing matched '%ls' -- not invoking", invokeName);
        } else {
            ComPtr<IUIAutomationInvokePattern> pattern;
            const bool got = SUCCEEDED(chosen->GetCurrentPatternAs(
                UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                reinterpret_cast<void**>(pattern.Put()))) && pattern;
            const HRESULT invoked = got ? pattern->Invoke() : E_FAIL;
            Log(L"UIA: Invoke('%ls') -> pattern=%d hr=0x%08lX", invokeName, got ? 1 : 0,
                static_cast<unsigned long>(invoked));
            chosen->Release();
        }
    }

    // ... AND THE OVERFLOW, which is where the APPLICATIONS' icons live: the strip that is always on
    // screen holds the shell's own buttons only (measured). The overflow is a separate top-level window,
    // so it is a separate walk -- and the question this answers is whether its icons are in the tree
    // BEFORE the panel is opened, because a tray of ours has to list them without showing the panel.
    HRESULT overflowHr = S_OK;
    ComPtr<IUIAutomationCondition> overflowClass;
    VARIANT overflowValue = {};
    overflowValue.vt = VT_BSTR;
    overflowValue.bstrVal = SysAllocString(L"TopLevelWindowForOverflowXamlIsland");
    overflowHr = automation->CreatePropertyCondition(UIA_ClassNamePropertyId, overflowValue,
                                                     overflowClass.Put());
    ComPtr<IUIAutomationElement> overflow;
    if (SUCCEEDED(overflowHr)) {
        const HRESULT found = desktop->FindFirst(TreeScope_Descendants, overflowClass.Get(),
                                                 overflow.Put());
        Log(L"UIA: overflow window (TopLevelWindowForOverflowXamlIsland) -> %ls (0x%08lX)",
            overflow ? L"found" : L"NOT found", static_cast<unsigned long>(found));
    }
    if (overflow) {
        ComPtr<IUIAutomationElementArray> inside;
        if (SUCCEEDED(overflow->FindAll(TreeScope_Descendants, buttons.Get(), inside.Put())) && inside) {
            int insideCount = 0;
            inside->get_Length(&insideCount);
            Log(L"UIA: %d button(s) inside the overflow window", insideCount);
            for (int i = 0; i < insideCount && i < 24; ++i) {
                ComPtr<IUIAutomationElement> element;
                if (FAILED(inside->GetElement(i, element.Put())) || !element) continue;
                const std::wstring name = ElementText(element.Get(), UIA_NamePropertyId);
                const std::wstring cls = ElementText(element.Get(), UIA_ClassNamePropertyId);
                BOOL offscreen = FALSE;
                element->get_CurrentIsOffscreen(&offscreen);
                RECT box = {};
                element->get_CurrentBoundingRectangle(&box);
                ComPtr<IUIAutomationInvokePattern> invoke;
                const bool hasInvoke = SUCCEEDED(element->GetCurrentPatternAs(
                    UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                    reinterpret_cast<void**>(invoke.Put()))) && invoke;
                Log(L"UIA:   overflow[%d] '%.60ls' class='%ls' offscreen=%d rect=%ld,%ld %ldx%ld invoke=%d",
                    i, name.c_str(), cls.c_str(), offscreen ? 1 : 0, box.left, box.top,
                    box.right - box.left, box.bottom - box.top, hasInvoke ? 1 : 0);
            }
        }
    }
    CoUninitialize();
    return 0;
}

} // namespace

// --- E6, CLSID_TrayNotify: is the shell's own tray interface reachable from here? ----------------
//
//  WHAT THIS IS FOR. A tray of our own can be drawn (the UIA tree gives the icons and the shell presses
//  them -- measured), but UIA publishes no context-menu pattern, so a RIGHT click has nowhere to go
//  unless we know each application's callback message. The shell's own notification-area interface
//  (ITrayNotify, undocumented) hands registrations to a callback and is the only route to those message
//  numbers that does not involve injecting anything. This mode asks whether it is reachable at all.
//
//  HOW THE IID IS FOUND WITHOUT KNOWING IT. The class is registered by name (measured: CLSID_TrayNotify
//  {25dead04-1eac-4911-9e3a-ad0a4ab560fd}, AppID "TrayNotify", RunAs Interactive User) but its interfaces
//  are not, and it has no type library. So: activate it, ask for the interfaces this project does know
//  (IDispatch, IUnknown), and if neither names the interface, QI every IID registered under
//  HKLM\SOFTWARE\Classes\Interface -- several thousand, each one a cheap lookup -- and report the ones the
//  object accepts. QI on an unknown IID is a query, not a call: it cannot run any of the object's code
//  beyond the lookup itself.
namespace {

int RunTrayCom(int watchSeconds) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        Log(L"TRAYCOM: CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(com));
        return 1;
    }
    const CLSID clsid = {0x25dead04, 0x1eac, 0x4911,
                         {0x9e, 0x3a, 0xad, 0x0a, 0x4a, 0xb5, 0x60, 0xfd}};

    ComPtr<IUnknown> object;
    const HRESULT created = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IUnknown,
                                             reinterpret_cast<void**>(object.Put()));
    Log(L"TRAYCOM: CoCreateInstance(CLSID_TrayNotify) -> 0x%08lX %ls", static_cast<unsigned long>(created),
        SUCCEEDED(created) ? L"(the object is reachable)" : L"(not reachable from here)");

    if (SUCCEEDED(created) && object) {
        // A type library would name the interface and give its IID, which is the whole question.
        ComPtr<IDispatch> dispatch;
        const HRESULT dispHr = object->QueryInterface(IID_IDispatch,
                                                      reinterpret_cast<void**>(dispatch.Put()));
        Log(L"TRAYCOM:   QueryInterface(IDispatch) -> 0x%08lX", static_cast<unsigned long>(dispHr));
        if (SUCCEEDED(dispHr) && dispatch) {
            ComPtr<ITypeInfo> info;
            const HRESULT infoHr = dispatch->GetTypeInfo(0, LOCALE_SYSTEM_DEFAULT, info.Put());
            Log(L"TRAYCOM:   GetTypeInfo(0) -> 0x%08lX", static_cast<unsigned long>(infoHr));
            if (SUCCEEDED(infoHr) && info) {
                TYPEATTR* attributes = nullptr;
                if (SUCCEEDED(info->GetTypeAttr(&attributes)) && attributes) {
                    wchar_t iid[64] = {};
                    StringFromGUID2(attributes->guid, iid, 64);
                    BSTR typeName = nullptr;
                    BSTR documentation = nullptr;
                    info->GetDocumentation(MEMBERID_NIL, &typeName, &documentation, nullptr, nullptr);
                    Log(L"TRAYCOM:   type info: name='%ls' iid=%ls kind=%d functions=%d",
                        typeName ? typeName : L"?", iid, static_cast<int>(attributes->typekind),
                        attributes->cFuncs);
                    if (typeName) SysFreeString(typeName);
                    if (documentation) SysFreeString(documentation);
                    info->ReleaseTypeAttr(attributes);
                }
            }
        }

        // IINSPECTABLE IS THE OBJECT TELLING US WHAT IT IS. It answers to it (measured), and that interface
        // is metadata: GetIids lists the interface IIDs from the object's own side -- which is the question
        // a "QI everything in the registry" sweep can only guess at -- and GetRuntimeClassName names the
        // class. Both are read-only queries; neither can change the shell's behaviour.
        const IID iidInspectable = {0xAF86E2E0, 0xB12D, 0x4c6a,
                                    {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
        struct IInspectableLike : public IUnknown {
            virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(LONG* level) = 0;
        };
        ComPtr<IInspectableLike> inspectable;
        const HRESULT inspectHr = object->QueryInterface(iidInspectable,
                                                        reinterpret_cast<void**>(inspectable.Put()));
        Log(L"TRAYCOM:   QueryInterface(IInspectable) -> 0x%08lX", static_cast<unsigned long>(inspectHr));
        if (SUCCEEDED(inspectHr) && inspectable) {
            LONG trust = 0;
            const HRESULT trustHr = inspectable->GetTrustLevel(&trust);
            Log(L"TRAYCOM:   GetTrustLevel -> 0x%08lX (%ld)", static_cast<unsigned long>(trustHr), trust);
            HSTRING className = nullptr;
            const HRESULT nameHr = inspectable->GetRuntimeClassName(&className);
            Log(L"TRAYCOM:   GetRuntimeClassName -> 0x%08lX", static_cast<unsigned long>(nameHr));
            if (SUCCEEDED(nameHr) && className) {
                // Resolved at run time: the HSTRING helpers live in combase.dll and this build has no
                // import library for them, and a probe has no business adding a link dependency for two
                // read-only calls it can do without.
                const HMODULE combase = LoadLibraryW(L"combase.dll");
                using RawFn = const wchar_t*(WINAPI*)(HSTRING, UINT32*);
                using FreeFn = HRESULT(WINAPI*)(HSTRING);
                const auto rawBuffer = combase ? reinterpret_cast<RawFn>(
                                                     GetProcAddress(combase, "WindowsGetStringRawBuffer"))
                                               : nullptr;
                const auto freeString = combase ? reinterpret_cast<FreeFn>(
                                                      GetProcAddress(combase, "WindowsDeleteString"))
                                                : nullptr;
                if (rawBuffer) {
                    UINT32 length = 0;
                    const wchar_t* raw = rawBuffer(className, &length);
                    Log(L"TRAYCOM:   runtime class name = '%ls'", raw ? raw : L"?");
                } else {
                    Log(L"TRAYCOM:   got a runtime class name but cannot read it (no combase helper)");
                }
                if (freeString) freeString(className);
            }
            ULONG iidCount = 0;
            IID* iids = nullptr;
            const HRESULT iidsHr = inspectable->GetIids(&iidCount, &iids);
            Log(L"TRAYCOM:   GetIids -> 0x%08lX (%lu interface(s))", static_cast<unsigned long>(iidsHr),
                iidCount);
            if (SUCCEEDED(iidsHr) && iids) {
                Log(L"TRAYCOM:   GetIids says the object implements %lu interface(s):", iidCount);
                for (ULONG i = 0; i < iidCount; ++i) {
                    wchar_t text[64] = {};
                    StringFromGUID2(iids[i], text, 64);
                    Log(L"TRAYCOM:     %ls", text);
                }
                CoTaskMemFree(iids);
            }
        }

        // ... AND IF NOTHING NAMED IT, ASK THE OBJECT ITSELF. Every IID Windows knows by name is under
        // HKLM\SOFTWARE\Classes\Interface; the ones this object accepts are the answer.
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Interface", 0, KEY_READ, &root) ==
            ERROR_SUCCESS) {
            DWORD index = 0;
            wchar_t name[64] = {};
            DWORD nameLength = 64;
            int accepted = 0;
            LARGE_INTEGER start = {};
            QueryPerformanceCounter(&start);
            while (RegEnumKeyExW(root, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr) ==
                   ERROR_SUCCESS) {
                nameLength = 64;
                CLSID candidate = {};
                if (FAILED(CLSIDFromString(name, &candidate))) continue;
                ComPtr<IUnknown> probe;
                if (SUCCEEDED(object->QueryInterface(candidate,
                                                     reinterpret_cast<void**>(probe.Put()))) &&
                    probe) {
                    ++accepted;
                    Log(L"TRAYCOM:   ACCEPTS %ls", name);
                }
            }
            LARGE_INTEGER end = {};
            QueryPerformanceCounter(&end);
            LARGE_INTEGER frequency = {};
            QueryPerformanceFrequency(&frequency);
            const double ms = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
            Log(L"TRAYCOM: %d IID(s) accepted out of the registry's list (%.0f ms)", accepted, ms);
            RegCloseKey(root);
        } else {
            Log(L"TRAYCOM: could not enumerate HKLM\\SOFTWARE\\Classes\\Interface");
        }
    }

    if (watchSeconds > 0) {
        Log(L"TRAYCOM: holding the apartment for %d s -- nothing is registered or changed", watchSeconds);
        Sleep(static_cast<DWORD>(watchSeconds) * 1000);
    }
    CoUninitialize();
    return 0;
}

} // namespace

} // namespace

// --- E7, the shell's own tray callback (ITrayNotifyWin8) -------------------------------------
//
//  WHAT IT IS FOR. A tray drawn by us can press an icon through UI Automation (measured: the shell
//  performs the icon's own left click), but UIA publishes no context-menu pattern, so the only honest
//  right-click is a menu of our own. This interface was the last hope of something better: it is the
//  channel the SHELL uses to tell listeners that an icon was added, changed or removed.
//
//  WHAT IT TURNS OUT TO BE. It is reachable (measured: CLSID_TrayNotify activates from this
//  medium-integrity process, and the only non-standard IID the object accepts is
//  {D133CE13-3537-48BA-93A7-AFCD5D2053B4}, which is ITrayNotifyWin8). Its NOTIFYITEM carries pszExeName,
//  pszTip, hIcon, hWnd, dwPreference, uID and guidItem -- and NO callback message number, so it can
//  never hand us an application's own context menu. What it CAN give is the truth about which icons are
//  registered, in real time, with a real icon handle rather than the registry's stale PNG snapshot.
//
//  The two interfaces are declared exactly as the Win8+ ABI has them: a vtable that is one parameter out
//  is a call into the wrong function, and the shell is the one being called.
namespace {

struct NotifyItem {
    PWSTR exeName;
    PWSTR tip;
    HICON icon;
    HWND window;
    DWORD preference;
    UINT uid;
    GUID guid;
};

struct INotificationCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Notify(ULONG event, NotifyItem* item) = 0;
};

struct ITrayNotifyWin8 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCallback* callback, ULONG* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(ULONG* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const NotifyItem* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoAction(BOOL enabled) = 0;
};

const IID kIidNotificationCallback = {
    0xD782CCBA, 0xAFB0, 0x43F1, {0x94, 0xDB, 0xFD, 0xA3, 0x77, 0x9E, 0xAC, 0xCB}};
const IID kIidTrayNotifyWin8 = {
    0xD133CE13, 0x3537, 0x48BA, {0x93, 0xA7, 0xAF, 0xCD, 0x5D, 0x20, 0x53, 0xB4}};

class TrayCallback : public INotificationCallback {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == kIidNotificationCallback) {
            *out = static_cast<INotificationCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Notify(ULONG event, NotifyItem* item) override {
        ++events;
        if (item && logged < 40) {
            ++logged;
            wchar_t guid[64] = {};
            StringFromGUID2(item->guid, guid, 64);
            Log(L"TRAYNOTIFY: event=%lu exe='%ls' tip='%ls' hIcon=%p hWnd=%p pref=%lu uid=%u guid=%ls",
                event, item->exeName ? item->exeName : L"", item->tip ? item->tip : L"",
                reinterpret_cast<void*>(item->icon), reinterpret_cast<void*>(item->window),
                item->preference, item->uid, guid);
        }
        return S_OK;
    }

    int events = 0;
    int logged = 0;

private:
    ULONG refs_ = 1;
};

int RunTrayNotify(int seconds) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        Log(L"TRAYNOTIFY: CoInitializeEx failed");
        return 1;
    }
    // The ABI, checked rather than assumed: one wrong offset and the shell writes into the wrong field.
    Log(L"TRAYNOTIFY: NOTIFYITEM size=%zu offsets exe=%zu tip=%zu icon=%zu hwnd=%zu pref=%zu uid=%zu guid=%zu",
        sizeof(NotifyItem), offsetof(NotifyItem, exeName), offsetof(NotifyItem, tip),
        offsetof(NotifyItem, icon), offsetof(NotifyItem, window), offsetof(NotifyItem, preference),
        offsetof(NotifyItem, uid), offsetof(NotifyItem, guid));

    const CLSID clsid = {0x25dead04, 0x1eac, 0x4911,
                         {0x9e, 0x3a, 0xad, 0x0a, 0x4a, 0xb5, 0x60, 0xfd}};
    ComPtr<IUnknown> object;
    const HRESULT created = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IUnknown,
                                             reinterpret_cast<void**>(object.Put()));
    if (FAILED(created) || !object) {
        Log(L"TRAYNOTIFY: cannot activate CLSID_TrayNotify (0x%08lX)",
            static_cast<unsigned long>(created));
        return 1;
    }
    ComPtr<ITrayNotifyWin8> trays;
    const HRESULT qi = object->QueryInterface(kIidTrayNotifyWin8,
                                              reinterpret_cast<void**>(trays.Put()));
    Log(L"TRAYNOTIFY: QueryInterface(ITrayNotifyWin8) -> 0x%08lX", static_cast<unsigned long>(qi));
    if (FAILED(qi) || !trays) return 1;

    TrayCallback callback;
    ULONG cookie = 0;
    const HRESULT registered = trays->RegisterCallback(&callback, &cookie);
    Log(L"TRAYNOTIFY: RegisterCallback -> 0x%08lX (cookie %lu)", static_cast<unsigned long>(registered),
        cookie);
    if (FAILED(registered)) {
        Log(L"TRAYNOTIFY: refused -- nothing was changed on the shell's side");
        return 1;
    }
    Log(L"TRAYNOTIFY: watching for %d s -- start or stop a tray application now", seconds);
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg = {};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    Log(L"TRAYNOTIFY: %d event(s) arrived", callback.events);
    const HRESULT unregistered = trays->UnregisterCallback(&cookie);
    Log(L"TRAYNOTIFY: UnregisterCallback -> 0x%08lX", static_cast<unsigned long>(unregistered));
    CoUninitialize();
    return 0;
}

} // namespace

// --- E8, A TRAY OF OUR OWN, END TO END --------------------------------------------------------
//
//  THE EXPERIMENT. Everything the design needs, in one process, before any of it goes into the product:
//
//    * the native taskbar is kept ALIVE but clipped to a one-pixel sliver, so the shell's tray is not on
//      screen anywhere (measured: UIA still enumerates it and still offers Invoke in that state);
//    * the icons come from the UIA tree -- which is the truth about what the shell is showing, the thing
//      the registry cannot tell us (it lists NVIDIA icons that are not in the tray at all);
//    * the pictures come from the registry's NotifyIconSettings (the executable path), asked of the shell
//      for its own icon -- the same source the retired card used for its grid;
//    * OUR window draws the cells, in our own place, in our own style;
//    * a LEFT click is handed to the shell: UIA Invoke on that cell's element, so the icon does exactly
//      what a real left click on it does, with the pointer untouched;
//    * a RIGHT click opens OUR menu -- the only honest answer, since no callback message number is
//      obtainable without injecting and ITrayNotify does not carry one.
//
//  WHAT IT IS NOT: it is not the product. It draws flat cells with no animation and no paging, it takes
//  the first registry record whose tooltip matches, and it closes after N seconds and puts everything
//  back. Its job is to answer "does this chain work at all" with pictures and a log.
namespace {

constexpr int kMaxCells = 20;
constexpr int kCellDip = 40;
constexpr int kCellGapDip = 4;
constexpr int kMenuOpen = 1;
constexpr int kMenuShowDesktop = 2;

struct TrayCell {
    std::wstring name;
    RECT screen = {};
    IUIAutomationElement* element = nullptr;
    HICON icon = nullptr;
    std::wstring matched;      // the registry record this cell's picture came from
};

TrayCell g_cells[kMaxCells];
int g_cellsUsed = 0;
HWND g_window = nullptr;
RECT g_windowScreen = {};
int g_invoked = 0;

std::wstring Flattened(const std::wstring& text) {
    std::wstring flat;
    flat.reserve(text.size());
    bool lastWasSpace = false;
    for (wchar_t c : text) {
        const bool space = (c == L'\n' || c == L'\r' || c == L'\t' || c == L' ');
        if (space) {
            if (!lastWasSpace && !flat.empty()) flat.push_back(L' ');
            lastWasSpace = true;
            continue;
        }
        flat.push_back(c);
        lastWasSpace = false;
    }
    while (!flat.empty() && flat.back() == L' ') flat.pop_back();
    return flat;
}

// The picture for one cell: the tooltip the shell is showing, looked up in the registry records the
// shell keeps for the tray, then the executable's own icon asked of the shell. Containment in both
// directions because neither string is a superset of the other in practice (the shell adds state to
// its tooltips: "NVIDIA 设置", "Steam - 正在同步").
bool RegistryPicture(const std::wstring& cellName, std::wstring* exeOut, HICON* iconOut) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return false;
    }
    const std::wstring wanted = Flattened(cellName);
    DWORD index = 0;
    wchar_t subkey[256] = {};
    DWORD subkeyLength = 256;
    bool found = false;
    while (!found &&
           RegEnumKeyExW(root, index++, subkey, &subkeyLength, nullptr, nullptr, nullptr, nullptr) ==
               ERROR_SUCCESS) {
        subkeyLength = 256;
        wchar_t tip[512] = {};
        DWORD tipBytes = sizeof(tip);
        if (RegGetValueW(root, subkey, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, tip, &tipBytes) !=
            ERROR_SUCCESS) {
            continue;
        }
        wchar_t exe[520] = {};
        DWORD exeBytes = sizeof(exe);
        if (RegGetValueW(root, subkey, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, exe, &exeBytes) !=
            ERROR_SUCCESS) {
            continue;
        }
        const std::wstring flatTip = Flattened(tip);
        if (flatTip.empty() || wanted.empty()) continue;
        if (flatTip.find(wanted) == std::wstring::npos && wanted.find(flatTip) == std::wstring::npos) {
            continue;
        }
        SHFILEINFOW info = {};
        // LARGEICON (32x32) rather than SMALLICON: the cell draws it at about 23 DIP, and scaling a 16
        // pixel icon up is the difference between an icon and a smudge.
        if (!SHGetFileInfoW(exe, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) {
            continue;
        }
        *exeOut = exe;
        *iconOut = info.hIcon;
        found = true;
    }
    RegCloseKey(root);
    return found;
}

// Where a cell is, in THIS window's coordinates.
RECT CellRect(int index) {
    RECT box = {};
    box.left = 8 + index * (kCellDip + kCellGapDip);
    box.top = 8;
    box.right = box.left + kCellDip;
    box.bottom = box.top + kCellDip;
    return box;
}

int CellAt(int x, int y) {
    for (int i = 0; i < g_cellsUsed; ++i) {
        const RECT box = CellRect(i);
        if (x >= box.left && x < box.right && y >= box.top && y < box.bottom) return i;
    }
    return -1;
}

void InvokeCell(int index) {
    if (index < 0 || index >= g_cellsUsed || !g_cells[index].element) return;
    ComPtr<IUIAutomationInvokePattern> pattern;
    const bool got = SUCCEEDED(g_cells[index].element->GetCurrentPatternAs(
        UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
        reinterpret_cast<void**>(pattern.Put()))) && pattern;
    const HRESULT invoked = got ? pattern->Invoke() : E_FAIL;
    ++g_invoked;
    Log(L"TRAYWINDOW: left click on cell %d '%ls' -> Invoke hr=0x%08lX", index,
        g_cells[index].name.c_str(), static_cast<unsigned long>(invoked));
}

LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;   // painted in WM_PAINT, and erasing first would flicker
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client = {};
            GetClientRect(hwnd, &client);
            HBRUSH background = CreateSolidBrush(RGB(28, 28, 34));
            FillRect(dc, &client, background);
            DeleteObject(background);
            for (int i = 0; i < g_cellsUsed; ++i) {
                const RECT box = CellRect(i);
                HBRUSH cell = CreateSolidBrush(RGB(48, 48, 58));
                FillRect(dc, &box, cell);
                DeleteObject(cell);
                if (g_cells[i].icon) {
                    DrawIconEx(dc, box.left + (kCellDip - 16) / 2, box.top + (kCellDip - 16) / 2,
                               g_cells[i].icon, 16, 16, 0, nullptr, DI_NORMAL);
                }
                wchar_t label[8] = {};
                wsprintfW(label, L"%d", i);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(200, 200, 210));
                TextOutW(dc, box.left + 3, box.bottom - 14, label, static_cast<int>(wcslen(label)));
            }
            SetTextColor(dc, RGB(150, 150, 160));
            TextOutW(dc, 8, kCellDip + 12, L"a tray of our own -- left click: the shell presses it",
                     static_cast<int>(wcslen(L"a tray of our own -- left click: the shell presses it")));
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int cell = CellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            InvokeCell(cell);
            return 0;
        }
        case WM_RBUTTONUP: {
            const int cell = CellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (cell < 0) return 0;
            Log(L"TRAYWINDOW: right click on cell %d '%ls' -- opening OUR menu", cell,
                g_cells[cell].name.c_str());
            POINT at = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(hwnd, &at);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kMenuOpen, L"open (the shell's own left click)");
            AppendMenuW(menu, MF_STRING, kMenuShowDesktop, L"show the desktop");
            SetForegroundWindow(hwnd);
            const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, at.x, at.y, 0, hwnd,
                                              nullptr);
            DestroyMenu(menu);
            Log(L"TRAYWINDOW: our menu returned %d", chosen);
            if (chosen == kMenuOpen) InvokeCell(cell);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int RunTrayWindow(int seconds, int x, int y) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        Log(L"TRAYWINDOW: CoInitializeEx failed");
        return 1;
    }
    // 1. The taskbar stays ALIVE and becomes invisible: one pixel of it is left on screen.
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT barRect = {}, trayRect = {};
    if (taskbar && tray) {
        GetWindowRect(taskbar, &barRect);
        GetWindowRect(tray, &trayRect);
        ShowWindow(taskbar, SW_SHOWNA);
        Sleep(150);
        HRGN sliver = CreateRectRgn(trayRect.left - barRect.left, trayRect.top - barRect.top,
                                    trayRect.left - barRect.left + 1, trayRect.top - barRect.top + 1);
        Log(L"TRAYWINDOW: taskbar clipped to a 1-pixel sliver -> %d",
            SetWindowRgn(taskbar, sliver, TRUE));
        Sleep(250);
    }

    // 2. The icons, from the shell's own automation tree.
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.Put()))) ||
        !automation) {
        Log(L"TRAYWINDOW: no automation client");
        return 1;
    }
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationElement> desktop;
    automation->GetRootElement(desktop.Put());
    for (const wchar_t* className : {L"Shell_TrayWnd", L"SystemTray_Main"}) {
        ComPtr<IUIAutomationCondition> byClass;
        VARIANT value = {};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(className);
        if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value, byClass.Put()))) {
            continue;
        }
        if (SUCCEEDED(desktop->FindFirst(TreeScope_Descendants, byClass.Get(), root.Put())) && root) break;
    }
    if (!root) {
        Log(L"TRAYWINDOW: the tray is not in the tree");
        return 1;
    }
    VARIANT buttonType = {};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> buttons;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, buttons.Put());
    ComPtr<IUIAutomationElementArray> list;
    if (FAILED(root->FindAll(TreeScope_Descendants, buttons.Get(), list.Put())) || !list) {
        Log(L"TRAYWINDOW: no buttons under the tray root");
        return 1;
    }
    int total = 0;
    list->get_Length(&total);
    for (int i = 0; i < total && g_cellsUsed < kMaxCells; ++i) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(list->GetElement(i, element.Put())) || !element) continue;
        const std::wstring cls = ElementText(element.Get(), UIA_ClassNamePropertyId);
        if (cls != L"SystemTray.AccentButton") continue;      // the tray's own items only
        RECT box = {};
        element->get_CurrentBoundingRectangle(&box);
        TrayCell& cell = g_cells[g_cellsUsed];
        cell.name = Flattened(ElementText(element.Get(), UIA_NamePropertyId));
        cell.screen = box;
        element.Get()->AddRef();
        cell.element = element.Get();
        std::wstring exe;
        HICON icon = nullptr;
        if (RegistryPicture(cell.name, &exe, &icon)) {
            cell.icon = icon;
            cell.matched = exe;
        }
        Log(L"TRAYWINDOW: cell %d '%ls' at %ld,%ld %ldx%ld -- picture from registry: %ls", g_cellsUsed,
            cell.name.c_str(), box.left, box.top, box.right - box.left, box.bottom - box.top,
            cell.matched.empty() ? L"NONE (no icon drawn)" : cell.matched.c_str());
        ++g_cellsUsed;
    }
    Log(L"TRAYWINDOW: %d cell(s) drawn by us", g_cellsUsed);
    if (g_cellsUsed == 0) return 0;

    // 3. Our window, in our place, with our own look.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabOwnTray";
    const ATOM ownClass = RegisterClassExW(&wc);
    Log(L"TRAYWINDOW: RegisterClassExW(TrayLabOwnTray) -> atom %u (error %lu)", ownClass,
        ownClass ? 0UL : GetLastError());
    const int width = 16 + g_cellsUsed * (kCellDip + kCellGapDip);
    SetLastError(0);
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TrayLabOwnTray",
                               L"tray of our own", WS_POPUP | WS_VISIBLE, x, y, width, kCellDip + 44,
                               nullptr, nullptr, wc.hInstance, nullptr);
    Log(L"TRAYWINDOW: CreateWindowExW -> %p (error %lu)", reinterpret_cast<void*>(g_window),
        g_window ? 0UL : GetLastError());
    if (g_window) GetWindowRect(g_window, &g_windowScreen);

    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg = {};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    Log(L"TRAYWINDOW: closing -- %d left click(s) pressed through the shell", g_invoked);
    DestroyWindow(g_window);
    for (int i = 0; i < g_cellsUsed; ++i) {
        if (g_cells[i].icon) DestroyIcon(g_cells[i].icon);
        if (g_cells[i].element) g_cells[i].element->Release();
    }
    // 4. Everything back: the whole taskbar, and hidden again by the bar if it is running.
    if (taskbar) SetWindowRgn(taskbar, nullptr, TRUE);
    Log(L"TRAYWINDOW: taskbar region cleared");
    CoUninitialize();
    return 0;
}

} // namespace

// --- E9, THE SAMPLE: what a tray of ours would look like, with the real icons in it -----------------
//
//  Everything measured so far, dressed as the thing itself. The taskbar is clipped away (no native pixel
//  on screen), the icons come from the two automation trees -- the strip's own buttons and, in the
//  overflow window, the APPLICATIONS' icons, which is where they actually live -- the pictures come from
//  the shell's registry records, the cells are drawn in the bar's own material, a left click is handed to
//  the shell through UIA, and a right click opens a menu of ours.
//
//  It is a SAMPLE, and says so: the five shell buttons are drawn as text glyphs rather than as art (they
//  have no image source and would be drawn by the bar's own code in the real thing), the menu offers only
//  what actually works here, and nothing is saved.
namespace {

constexpr int kSampleMax = 32;
constexpr int kSampleCell = 34;
constexpr int kSampleGap = 4;
constexpr int kSamplePad = 8;
constexpr int kSampleLabel = 26;   // a line under the strip, for the name under the pointer

struct SampleItem {
    std::wstring name;         // flattened, for the menu and the log
    RECT screen = {};          // where the shell has it, which is where a real click must land
    bool shellButton = false;  // one of the shell's own, drawn as a glyph
    wchar_t glyph[8] = {};     // ... that glyph
    HICON icon = nullptr;      // or the application's icon, from the registry match
    std::wstring exe;
    std::wstring registryTip;  // what the registry record was called, when one matched
    IUIAutomationElement* element = nullptr;
};

SampleItem g_sample[kSampleMax];
int g_sampleCount = 0;
int g_sampleHover = -1;
HWND g_sampleWindow = nullptr;
HWND g_samplePanel = nullptr;   // the shell's overflow panel, clipped while this sample reads it
bool g_sampleQuit = false;

RECT SampleCellRect(int index) {
    RECT box = {};
    const int pitch = kSampleCell + kSampleGap;
    box.left = kSamplePad + index * pitch;
    box.top = kSamplePad;
    box.right = box.left + kSampleCell;
    box.bottom = box.top + kSampleCell;
    return box;
}

int SampleCellAt(int x, int y) {
    for (int i = 0; i < g_sampleCount; ++i) {
        const RECT box = SampleCellRect(i);
        if (x >= box.left && x < box.right && y >= box.top && y < box.bottom) return i;
    }
    return -1;
}

void SampleInvoke(int index) {
    if (index < 0 || index >= g_sampleCount || !g_sample[index].element) return;
    ComPtr<IUIAutomationInvokePattern> pattern;
    const bool got = SUCCEEDED(g_sample[index].element->GetCurrentPatternAs(
        UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
        reinterpret_cast<void**>(pattern.Put()))) && pattern;
    const HRESULT invoked = got ? pattern->Invoke() : E_FAIL;
    Log(L"TRAYSAMPLE: pressed '%ls' -> Invoke hr=0x%08lX", g_sample[index].name.c_str(),
        static_cast<unsigned long>(invoked));
}

LRESULT CALLBACK SampleProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client = {};
            GetClientRect(hwnd, &client);
            // The bar's own material: one dark rounded surface, cells a shade lighter on it.
            HBRUSH surface = CreateSolidBrush(RGB(30, 30, 36));
            HPEN edge = CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
            HGDIOBJ oldBrush = SelectObject(dc, surface);
            HGDIOBJ oldPen = SelectObject(dc, edge);
            RoundRect(dc, client.left, client.top, client.right, client.bottom, 14, 14);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(surface);
            DeleteObject(edge);
            SetBkMode(dc, TRANSPARENT);
            for (int i = 0; i < g_sampleCount; ++i) {
                const RECT box = SampleCellRect(i);
                HBRUSH cell = CreateSolidBrush(i == g_sampleHover ? RGB(64, 64, 76) : RGB(44, 44, 52));
                HPEN noPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
                oldBrush = SelectObject(dc, cell);
                oldPen = SelectObject(dc, noPen);
                RoundRect(dc, box.left, box.top, box.right, box.bottom, 8, 8);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(cell);
                DeleteObject(noPen);
                const int inner = kSampleCell - 11;
                if (g_sample[i].icon) {
                    DrawIconEx(dc, box.left + (kSampleCell - inner) / 2,
                               box.top + (kSampleCell - inner) / 2, g_sample[i].icon, inner, inner, 0,
                               nullptr, DI_NORMAL);
                } else {
                    SetTextColor(dc, RGB(226, 226, 234));
                    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                    DrawTextW(dc, g_sample[i].glyph, -1, const_cast<RECT*>(&box),
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            // The name of whatever is under the pointer, on its own line: the sample has no tooltips, and
            // "which of these is WeChat" is exactly what somebody looking at it wants to know.
            if (g_sampleHover >= 0 && g_sampleHover < g_sampleCount) {
                RECT line = client;
                line.left += kSamplePad;
                line.right -= kSamplePad;
                line.top = kSamplePad + kSampleCell + 3;
                HBRUSH prompt = CreateSolidBrush(RGB(22, 22, 28));
                HPEN noPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
                HGDIOBJ oldBrush2 = SelectObject(dc, prompt);
                HGDIOBJ oldPen2 = SelectObject(dc, noPen);
                RoundRect(dc, line.left, line.top, line.right, line.bottom, 8, 8);
                SelectObject(dc, oldBrush2);
                SelectObject(dc, oldPen2);
                DeleteObject(prompt);
                DeleteObject(noPen);
                SetTextColor(dc, RGB(210, 210, 220));
                SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                RECT text = line;
                text.left += 8;
                text.right -= 8;
                DrawTextW(dc, g_sample[g_sampleHover].name.c_str(), -1, &text,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Log(L"TRAYSAMPLE: Esc -- closing");
                g_sampleQuit = true;
            }
            return 0;
        case WM_MOUSEMOVE: {
            const int cell = SampleCellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (cell != g_sampleHover) {
                g_sampleHover = cell;
                InvalidateRect(hwnd, nullptr, FALSE);
                const wchar_t* tip = cell >= 0 ? g_sample[cell].name.c_str() : L"";
                Log(L"TRAYSAMPLE: hovering '%ls'", tip);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            SampleInvoke(SampleCellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)));
            return 0;
        case WM_RBUTTONUP: {
            const int cell = SampleCellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (cell < 0) return 0;
            POINT at = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(hwnd, &at);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"打开（左键那一下）");
            AppendMenuW(menu, MF_STRING, 2, L"打开文件位置");
            if (!g_sample[cell].shellButton) {
                // Only for an application: the shell's own five have no menu of their own to ask for.
                AppendMenuW(menu, MF_STRING, 4, L"应用自己的菜单…（会闪一下）");
            }
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 3, L"关闭样例");
            SetForegroundWindow(hwnd);
            const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, at.x, at.y, 0, hwnd,
                                              nullptr);
            DestroyMenu(menu);
            Log(L"TRAYSAMPLE: menu for '%ls' returned %d", g_sample[cell].name.c_str(), chosen);
            if (chosen == 1) {
                SampleInvoke(cell);
            } else if (chosen == 2) {
                if (g_sample[cell].exe.empty()) {
                    Log(L"TRAYSAMPLE: no executable is known for that one");
                } else {
                    std::wstring argument = L"/select,\"" + g_sample[cell].exe + L"\"";
                    const HINSTANCE run = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                                        argument.c_str(), nullptr, SW_SHOWNORMAL);
                    Log(L"TRAYSAMPLE: opened the location of '%ls' -> %p", g_sample[cell].exe.c_str(),
                        reinterpret_cast<void*>(run));
                }
            } else if (chosen == 3) {
                Log(L"TRAYSAMPLE: closing from the menu");
                g_sampleQuit = true;
            } else if (chosen == 4) {
                // THE APPLICATION'S OWN MENU, AND WHAT IT COSTS (measured, HANDOFF 8.138): the shell has to
                // be on screen at that spot for the length of the click -- a real click can only land on
                // pixels that are really there, and a posted message is not honoured by the XAML tray. So:
                // put the shell's overflow panel back on screen, right-click the icon the way a user
                // would (pointer moved and restored), then clip the panel away again.
                const HWND panel = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
                if (panel) {
                    SetWindowRgn(panel, nullptr, TRUE);
                    Sleep(160);
                }
                const POINT middle = {(g_sample[cell].screen.left + g_sample[cell].screen.right) / 2,
                                      (g_sample[cell].screen.top + g_sample[cell].screen.bottom) / 2};
                POINT saved = {};
                GetCursorPos(&saved);
                SetCursorPos(middle.x, middle.y);
                Sleep(70);
                INPUT inputs[2] = {};
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                const UINT sent = SendInput(2, inputs, sizeof(INPUT));
                Sleep(250);
                SetCursorPos(saved.x, saved.y);
                if (panel) {
                    HRGN sliver = CreateRectRgn(0, 0, 1, 1);
                    SetWindowRgn(panel, sliver, TRUE);
                }
                Log(L"TRAYSAMPLE: asked the shell for '%ls' own menu at %ld,%ld (%u event(s), pointer back"
                    L" at %ld,%ld) -- the panel was shown for that click and is away again",
                    g_sample[cell].name.c_str(), middle.x, middle.y, sent, saved.x, saved.y);
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int RunTraySample(int seconds, int x, int y) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return 1;

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT barRect = {}, trayRect = {};
    if (taskbar && tray) {
        GetWindowRect(taskbar, &barRect);
        GetWindowRect(tray, &trayRect);
        ShowWindow(taskbar, SW_SHOWNA);
        Sleep(150);
        HRGN sliver = CreateRectRgn(trayRect.left - barRect.left, trayRect.top - barRect.top,
                                    trayRect.left - barRect.left + 1, trayRect.top - barRect.top + 1);
        SetWindowRgn(taskbar, sliver, TRUE);
        Sleep(250);
    }

    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.Put()))) ||
        !automation) {
        Log(L"TRAYSAMPLE: no automation client");
        return 1;
    }
    ComPtr<IUIAutomationElement> desktop;
    automation->GetRootElement(desktop.Put());
    VARIANT buttonType = {};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> buttons;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, buttons.Put());

    // The shell's own five, first, because a real tray puts them last and we want the applications
    // first where the eye lands -- and each is drawn as a glyph, not as an icon.
    auto collect = [&](const wchar_t* rootClass, bool shellButtons) {
        ComPtr<IUIAutomationCondition> byClass;
        VARIANT value = {};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(rootClass);
        if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value, byClass.Put()))) {
            return;
        }
        ComPtr<IUIAutomationElement> root;
        if (FAILED(desktop->FindFirst(TreeScope_Descendants, byClass.Get(), root.Put())) || !root) {
            Log(L"TRAYSAMPLE: no '%ls' in the tree", rootClass);
            return;
        }
        ComPtr<IUIAutomationElementArray> list;
        if (FAILED(root->FindAll(TreeScope_Descendants, buttons.Get(), list.Put())) || !list) return;
        int count = 0;
        list->get_Length(&count);
        for (int i = 0; i < count && g_sampleCount < kSampleMax; ++i) {
            ComPtr<IUIAutomationElement> element;
            if (FAILED(list->GetElement(i, element.Put())) || !element) continue;
            const std::wstring cls = ElementText(element.Get(), UIA_ClassNamePropertyId);
            const bool fromStrip = cls == L"SystemTray.AccentButton";
            const bool fromOverflow = cls == L"SystemTray.NormalButton";
            if (!fromStrip && !fromOverflow) continue;
            if (shellButtons != fromStrip) continue;      // one pass for each of the two groups
            SampleItem& item = g_sample[g_sampleCount];
            item.name = Flattened(ElementText(element.Get(), UIA_NamePropertyId));
            element->get_CurrentBoundingRectangle(&item.screen);
            item.shellButton = fromStrip;
            if (item.name.empty()) item.name = L"(no name)";
            element.Get()->AddRef();
            item.element = element.Get();
            if (fromStrip) {
                // Our own glyphs: these five have no image source, and in the real thing the bar draws
                // its own volume, network and clock already.
                if (item.name.find(L"显示隐藏") != std::wstring::npos) wcscpy_s(item.glyph, L"\u22EF");
                else if (item.name.find(L"输入") != std::wstring::npos) wcscpy_s(item.glyph, L"\u4E2D");
                else if (item.name.find(L"网络") != std::wstring::npos) wcscpy_s(item.glyph, L"\u7F51");
                else if (item.name.find(L"音量") != std::wstring::npos) wcscpy_s(item.glyph, L"\u97F3");
                else wcscpy_s(item.glyph, L"\u65F6");
            } else {
                std::wstring exe;
                HICON icon = nullptr;
                if (RegistryPicture(item.name, &exe, &icon)) {
                    item.icon = icon;
                    item.exe = exe;
                } else {
                    // No picture for this one. A cell that draws NOTHING reads as a bug rather than as
                    // "we have no image", so the first character of the shell's own name stands in --
                    // and the real thing would do the same until the icon arrives.
                    item.glyph[0] = item.name.empty() || item.name[0] == L'(' ? L'?' : item.name[0];
                    item.glyph[1] = L'\0';
                }
            }
            Log(L"TRAYSAMPLE: [%d] %ls '%ls'%ls", g_sampleCount, fromStrip ? L"shell" : L"app ",
                item.name.c_str(),
                item.icon ? L" (icon from the registry record)" : L" (drawn as a glyph)");
            ++g_sampleCount;
        }
    };

    // THE OVERFLOW PANEL IS LAZY, AND A TRAY OF OURS MUST NOT SHOW IT. Measured: the applications' icons
    // live in TopLevelWindowForOverflowXamlIsland, that window does not exist until the panel has been
    // opened once in the session, and the shell tears it down again afterwards. So the sample asks for it
    // with the same Invoke a click on the chevron performs, and then HIDES it again: the buttons stay in
    // the automation tree, and the shell's own panel is not left on screen while we read it.
    auto findOverflow = [&](IUIAutomationElement** out) {
        *out = nullptr;
        ComPtr<IUIAutomationCondition> byClass;
        VARIANT value = {};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(L"TopLevelWindowForOverflowXamlIsland");
        if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value, byClass.Put()))) {
            return;
        }
        desktop->FindFirst(TreeScope_Descendants, byClass.Get(), out);
    };
    ComPtr<IUIAutomationElement> overflow;
    findOverflow(overflow.Put());
    if (!overflow) {
        ComPtr<IUIAutomationCondition> trayClass;
        VARIANT value = {};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(L"Shell_TrayWnd");
        ComPtr<IUIAutomationElement> trayRoot;
        if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value,
                                                          trayClass.Put()))) {
            desktop->FindFirst(TreeScope_Descendants, trayClass.Get(), trayRoot.Put());
        }
        if (trayRoot) {
            ComPtr<IUIAutomationElementArray> strip;
            if (SUCCEEDED(trayRoot->FindAll(TreeScope_Descendants, buttons.Get(), strip.Put())) &&
                strip) {
                int count = 0;
                strip->get_Length(&count);
                for (int i = 0; i < count; ++i) {
                    ComPtr<IUIAutomationElement> element;
                    if (FAILED(strip->GetElement(i, element.Put())) || !element) continue;
                    if (Flattened(ElementText(element.Get(), UIA_NamePropertyId)).find(L"显示隐藏") ==
                        std::wstring::npos) {
                        continue;
                    }
                    ComPtr<IUIAutomationInvokePattern> invoke;
                    if (SUCCEEDED(element->GetCurrentPatternAs(
                            UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                            reinterpret_cast<void**>(invoke.Put()))) &&
                        invoke) {
                        Log(L"TRAYSAMPLE: asking the shell for its overflow panel -> Invoke 0x%08lX",
                            static_cast<unsigned long>(invoke->Invoke()));
                    }
                    break;
                }
            }
        }
        Sleep(900);
        findOverflow(overflow.Put());
        Log(L"TRAYSAMPLE: overflow window after asking -> %ls", overflow ? L"there" : L"still absent");
        if (overflow) {
            UIA_HWND native = nullptr;
            if (SUCCEEDED(overflow->get_CurrentNativeWindowHandle(&native)) && native) {
                const HWND panel = reinterpret_cast<HWND>(native);
                g_samplePanel = panel;
                // CLIPPED, NOT HIDDEN, and that is the whole lesson: measured twice now, a window hidden
                // with SW_HIDE leaves the automation tree (the taskbar's tray disappears from it, and so
                // does this panel's icon list), while a window clipped to one pixel stays in it and stays
                // invokable. So the shell's panel gets the same treatment the shell's taskbar gets.
                HRGN sliver = CreateRectRgn(0, 0, 1, 1);
                Log(L"TRAYSAMPLE: clipped the shell's panel to a sliver (not hidden) -> %d",
                    SetWindowRgn(panel, sliver, TRUE));
                Sleep(500);
            }
        }
    }

    collect(L"TopLevelWindowForOverflowXamlIsland", /*shellButtons=*/false);
    const int separatorAfter = g_sampleCount;
    collect(L"Shell_TrayWnd", /*shellButtons=*/true);
    Log(L"TRAYSAMPLE: %d cell(s) -- %d application(s) then %d of the shell's own", g_sampleCount,
        separatorAfter, g_sampleCount - separatorAfter);
    if (g_sampleCount == 0) return 0;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SampleProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabSample";
    RegisterClassExW(&wc);
    const int width = kSamplePad * 2 + g_sampleCount * kSampleCell + (g_sampleCount - 1) * kSampleGap;
    const int height = kSamplePad * 2 + kSampleCell + kSampleLabel;
    g_sampleWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TrayLabSample",
                                     L"tray sample", WS_POPUP | WS_VISIBLE, x, y, width, height,
                                     nullptr, nullptr, wc.hInstance, nullptr);
    Log(L"TRAYSAMPLE: window %p at %d,%d %dx%d -- click a cell, right-click for our menu, Esc to close",
        reinterpret_cast<void*>(g_sampleWindow), x, y, width, height);

    // seconds <= 0 means "stay until it is closed", which is what somebody looking at an example wants.
    const bool untilClosed = seconds <= 0;
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds > 0 ? seconds : 1) * 1000;
    MSG msg = {};
    while (!g_sampleQuit && (untilClosed || GetTickCount64() < until)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    DestroyWindow(g_sampleWindow);
    for (int i = 0; i < g_sampleCount; ++i) {
        if (g_sample[i].icon) DestroyIcon(g_sample[i].icon);
        if (g_sample[i].element) g_sample[i].element->Release();
    }
    if (taskbar) SetWindowRgn(taskbar, nullptr, TRUE);
    if (g_samplePanel && IsWindow(g_samplePanel)) {
        SetWindowRgn(g_samplePanel, nullptr, TRUE);   // the panel goes back to its own shape
        ShowWindow(g_samplePanel, SW_HIDE);           // ... and away, as the shell leaves it when closed
        g_samplePanel = nullptr;
    }
    Log(L"TRAYSAMPLE: closed and the taskbar's region cleared");
    CoUninitialize();
    return 0;
}

} // namespace

// --- E10, THE APPLICATION'S OWN RIGHT-CLICK MENU: two ways to ask the shell for it ----------------
//
//  WHY THIS IS NEEDED. UI Automation publishes no context-menu pattern -- an icon can be INVOKED (which is
//  its left click) and nothing else -- so a menu of ours is the only honest right click unless the SHELL
//  can be made to perform the right click itself. The shell is the one that knows the application's
//  callback message, so if anything can produce 退出微信, it is the shell. Two ways to ask:
//
//    post -- post WM_RBUTTONDOWN/UP to the window the icon belongs to, at the icon's own coordinates. No
//            pointer movement, no input injection; whether XAML accepts a POSTED mouse message is the
//            question (it reads its input from the input site, which normally means real input).
//    send -- a REAL right click: save the pointer, move it onto the icon, right press and release, put the
//            pointer back. It moves the pointer for a few milliseconds, which is exactly what the user
//            asked this project not to do -- so it is here as the fallback that decides whether the
//            capability exists at all, not as a design.
namespace {

int RunRightClick(const wchar_t* wanted, bool sendInput, bool keepPanelVisible, bool byKeyboard) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return 1;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.Put()))) ||
        !automation) {
        return 1;
    }
    ComPtr<IUIAutomationElement> desktop;
    automation->GetRootElement(desktop.Put());
    VARIANT buttonType = {};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> buttons;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, buttons.Put());

    // Both trees, and the overflow is asked for first if it is not there yet: measured, its window is
    // created lazily and only exists after the panel has been opened once in the session.
    ComPtr<IUIAutomationElement> match;
    RECT box = {};
    for (int attempt = 0; attempt < 2 && !match; ++attempt) {
        for (const wchar_t* rootClass : {L"TopLevelWindowForOverflowXamlIsland", L"Shell_TrayWnd"}) {
            ComPtr<IUIAutomationCondition> byClass;
            VARIANT value = {};
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(rootClass);
            ComPtr<IUIAutomationElement> root;
            if (FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value,
                                                           byClass.Put()))) {
                continue;
            }
            if (FAILED(desktop->FindFirst(TreeScope_Descendants, byClass.Get(), root.Put())) || !root) {
                continue;
            }
            ComPtr<IUIAutomationElementArray> list;
            if (FAILED(root->FindAll(TreeScope_Descendants, buttons.Get(), list.Put())) || !list) continue;
            int count = 0;
            list->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                ComPtr<IUIAutomationElement> element;
                if (FAILED(list->GetElement(i, element.Put())) || !element) continue;
                // THE TRAY'S OWN CLASSES ONLY. Without this the walk finds the TASKBAR's task buttons as
                // well -- "微信 - 1 个运行窗口" matches a search for 微信 -- and clicking one of those is a
                // different thing entirely (it activates a window; it does not open a tray menu).
                const std::wstring cls = ElementText(element.Get(), UIA_ClassNamePropertyId);
                if (cls != L"SystemTray.AccentButton" && cls != L"SystemTray.NormalButton") continue;
                const std::wstring name = Flattened(ElementText(element.Get(), UIA_NamePropertyId));
                if (name.find(wanted) == std::wstring::npos) continue;
                element->get_CurrentBoundingRectangle(&box);
                element.Get()->AddRef();
                match = element;
                Log(L"RIGHTCLICK: found '%ls' at %ld,%ld %ldx%ld", name.c_str(), box.left, box.top,
                    box.right - box.left, box.bottom - box.top);
                break;
            }
            if (match) break;
        }
        if (!match && attempt == 0) {
            // Ask the shell for the overflow panel, exactly as a click on the chevron does.
            ComPtr<IUIAutomationCondition> trayClass;
            VARIANT value = {};
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(L"Shell_TrayWnd");
            ComPtr<IUIAutomationElement> trayRoot;
            if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, value,
                                                              trayClass.Put()))) {
                desktop->FindFirst(TreeScope_Descendants, trayClass.Get(), trayRoot.Put());
            }
            if (trayRoot) {
                ComPtr<IUIAutomationElementArray> strip;
                if (SUCCEEDED(trayRoot->FindAll(TreeScope_Descendants, buttons.Get(), strip.Put())) &&
                    strip) {
                    int count = 0;
                    strip->get_Length(&count);
                    for (int i = 0; i < count; ++i) {
                        ComPtr<IUIAutomationElement> element;
                        if (FAILED(strip->GetElement(i, element.Put())) || !element) continue;
                        if (Flattened(ElementText(element.Get(), UIA_NamePropertyId)).find(L"显示隐藏") ==
                            std::wstring::npos) {
                            continue;
                        }
                        ComPtr<IUIAutomationInvokePattern> invoke;
                        if (SUCCEEDED(element->GetCurrentPatternAs(
                                UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                                reinterpret_cast<void**>(invoke.Put()))) &&
                            invoke) {
                            invoke->Invoke();
                            Log(L"RIGHTCLICK: asked the shell for its overflow panel to find that icon");
                        }
                        break;
                    }
                }
            }
            Sleep(900);
            // ... and clip it to a pixel, so the panel is not on screen while we work with it.
            ComPtr<IUIAutomationCondition> panelClass;
            VARIANT panelValue = {};
            panelValue.vt = VT_BSTR;
            panelValue.bstrVal = SysAllocString(L"TopLevelWindowForOverflowXamlIsland");
            ComPtr<IUIAutomationElement> panel;
            if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, panelValue,
                                                              panelClass.Put()))) {
                desktop->FindFirst(TreeScope_Descendants, panelClass.Get(), panel.Put());
            }
            if (panel) {
                UIA_HWND native = nullptr;
                if (SUCCEEDED(panel->get_CurrentNativeWindowHandle(&native)) && native) {
                    HRGN sliver = CreateRectRgn(0, 0, 1, 1);
                    if (keepPanelVisible) {
                        // LEFT ON SCREEN ON PURPOSE. A real click can only land on pixels that are
                        // really there -- measured twice: with the panel clipped away, a right click at
                        // an icon's coordinates opens the DESKTOP's menu, because the desktop is what
                        // is on screen there. So asking the shell for an application's own menu means
                        // showing the shell's tray for the length of the click.
                        //
                        // The region is CLEARED first, not just left alone: an earlier run of this probe
                        // clipped the panel and the region survives until something removes it, which is
                        // why the first attempt at this printed the desktop's menu as well.
                        SetWindowRgn(reinterpret_cast<HWND>(native), nullptr, TRUE);
                        Log(L"RIGHTCLICK: leaving the panel visible -- a real click needs real pixels");
                    } else {
                        SetWindowRgn(reinterpret_cast<HWND>(native), sliver, TRUE);
                        Log(L"RIGHTCLICK: clipped the panel to a sliver");
                    }
                }
            }
        }
    }
    if (!match) {
        Log(L"RIGHTCLICK: nothing named '%ls' in either tree", wanted);
        return 1;
    }

    const POINT middle = {(box.left + box.right) / 2, (box.top + box.bottom) / 2};

    // --- the keyboard route, which needs neither pixels nor the pointer ---------------------------------
    //
    //  A tray icon is a handled control: in the real tray, giving it focus and pressing the context-menu key
    //  opens its menu -- the shell does it, so an application's own entries appear. Neither half of that
    //  costs what a real click costs: the keyboard does not need the icon to be under the pointer, and it
    //  does not need the icon to be ON SCREEN at all as far as hit testing is concerned. Whether the shell's
    //  XAML tray honours it -- and whether SetFocus can reach another process's element at all -- is the
    //  question this answers.
    if (byKeyboard) {
        const HRESULT focus = match->SetFocus();
        Sleep(150);
        UIA_HWND native = nullptr;
        match->get_CurrentNativeWindowHandle(&native);
        HWND target = reinterpret_cast<HWND>(native);
        if (!target) target = FindWindowW(L"Shell_TrayWnd", nullptr);
        const HWND focused = GetFocus();
        const HWND foreground = GetForegroundWindow();
        Log(L"RIGHTCLICK: SetFocus -> 0x%08lX; our focus %p, foreground %p, target %p",
            static_cast<unsigned long>(focus), reinterpret_cast<void*>(focused),
            reinterpret_cast<void*>(foreground), reinterpret_cast<void*>(target));
        PostMessageW(target, WM_KEYDOWN, VK_APPS, 0);
        PostMessageW(target, WM_KEYUP, VK_APPS, 0);
        Sleep(200);
        PostMessageW(target, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(target),
                     (middle.y << 16) | (middle.x & 0xFFFF));
        Log(L"RIGHTCLICK: posted the context-menu key and WM_CONTEXTMENU to %p", target);
        Sleep(1200);
        match->Release();
        CoUninitialize();
        return 0;
    }

    // A REAL CLICK NEEDS REAL PIXELS, and the panel may still be clipped from an earlier run of this probe
    // (a window region outlives the process that set it). So the panel is put back on screen here, after
    // the search rather than during it -- the search can succeed while the panel is one invisible pixel.
    if (sendInput && keepPanelVisible) {
        ComPtr<IUIAutomationCondition> panelClass;
        VARIANT panelValue = {};
        panelValue.vt = VT_BSTR;
        panelValue.bstrVal = SysAllocString(L"TopLevelWindowForOverflowXamlIsland");
        ComPtr<IUIAutomationElement> panel;
        if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, panelValue,
                                                          panelClass.Put()))) {
            desktop->FindFirst(TreeScope_Descendants, panelClass.Get(), panel.Put());
        }
        if (panel) {
            UIA_HWND native = nullptr;
            if (SUCCEEDED(panel->get_CurrentNativeWindowHandle(&native)) && native) {
                SetWindowRgn(reinterpret_cast<HWND>(native), nullptr, TRUE);
                Log(L"RIGHTCLICK: cleared the panel's region -- it is on screen for this click");
                Sleep(250);
            }
        }
    }
    if (sendInput) {
        POINT saved = {};
        GetCursorPos(&saved);
        SetCursorPos(middle.x, middle.y);
        Sleep(80);
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        const UINT sent = SendInput(2, inputs, sizeof(INPUT));
        Sleep(300);
        SetCursorPos(saved.x, saved.y);
        Log(L"RIGHTCLICK: real right click at %ld,%ld -> %u event(s); pointer put back at %ld,%ld",
            middle.x, middle.y, sent, saved.x, saved.y);
    } else {
        // The window the element belongs to -- for a XAML island that is the island's own HWND -- and the
        // icon's coordinates inside it.
        UIA_HWND native = nullptr;
        match->get_CurrentNativeWindowHandle(&native);
        HWND target = reinterpret_cast<HWND>(native);
        if (!target) target = FindWindowW(L"Shell_TrayWnd", nullptr);
        POINT local = middle;
        ScreenToClient(target, &local);
        const LPARAM packed = static_cast<LPARAM>((local.y << 16) | (local.x & 0xFFFF));
        PostMessageW(target, WM_RBUTTONDOWN, MK_RBUTTON, packed);
        PostMessageW(target, WM_RBUTTONUP, 0, packed);
        Log(L"RIGHTCLICK: posted WM_RBUTTONDOWN/UP to %p at client %ld,%ld (screen %ld,%ld)", target,
            local.x, local.y, middle.x, middle.y);
    }
    Sleep(1200);
    match->Release();
    CoUninitialize();
    return 0;
}

} // namespace

// --- E11, CAN THE TASKBAR BE MOVED AT ALL? (the user's idea needs this) ---------------------------
//
//  THE IDEA: crop the shell's taskbar down to its tray and put THAT inside a card of ours. Then the tray
//  inside the card is the REAL tray -- real pixels, real clicks, real application menus -- and none of the
//  expensive routes (a module in Explorer, or being the shell) are needed.
//
//  WHAT IT NEEDS, AND WHAT HAS ALREADY BEEN MEASURED:
//    * the taskbar does get clipped to the tray (measured, E2), and clicks in the clipped-away part fall
//      through to the desktop;
//    * SetWindowPos on the taskbar REPORTS SUCCESS AND DOES NOTHING (measured, E4) -- the shell puts its
//      own rectangle straight back;
//    * re-parenting the tray window is accepted, but its PIXELS stay where the shell paints them, because
//      Windows 11 paints this taskbar from a composition surface belonging to the whole bar (measured,
//      E3b).
//
//  So the one lever left is the API the shell's own taskbar uses to place itself: the AppBar message
//  ABM_SETPOS. If the taskbar is positioned by that, a third party may be able to ask for a new rectangle
//  and have it stick. This mode asks, reports the rectangle the shell granted, watches whether the window
//  actually moves, and then asks for the original rectangle back.
//
//  MEASURED: IT DOES NOT MOVE. ABM_SETPOS returns 1 and answers with the requested rectangle, and 250 ms
//  later the taskbar is exactly where it was -- the shell puts its own position straight back, the same way
//  it does for SetWindowPos. So the real tray cannot be MOVED into a card.
//
//  WHICH LEAVES THE IMAGE. A DWM thumbnail can be told which part of a source window to draw, and where to
//  draw it, which is exactly what "the tray's pixels, inside my card" needs -- the bar already uses this for
//  its window previews. It is a picture: clicks do not follow, so what it can and cannot give is worth
//  knowing before anything is designed around it.
#include <shellapi.h>

#include <dwmapi.h>

// A CARD THAT LETS THE CLICK THROUGH. The one thing the whole idea turns on: if a topmost window of ours
// covers the shell's tray, does a click land on OUR window (in which case the real tray is unreachable), or
// does it reach the shell underneath (in which case the real tray keeps every one of its behaviours and we
// only have to dress it). WS_EX_TRANSPARENT + WS_EX_LAYERED is Windows' own way of saying "not here".
LRESULT CALLBACK CoverProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) {
        RECT client = {};
        GetClientRect(hwnd, &client);
        HBRUSH wash = CreateSolidBrush(RGB(40, 120, 200));
        FillRect(reinterpret_cast<HDC>(wparam), &client, wash);
        DeleteObject(wash);
        return 1;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int RunCover(int x, int y, int width, int height, int seconds) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CoverProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabCover";
    RegisterClassExW(&wc);
    const HWND cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                       L"TrayLabCover", L"cover", WS_POPUP | WS_VISIBLE, x, y, width,
                                       height, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(cover, 0, 150, LWA_ALPHA);
    Log(L"COVER: our click-through card %p over %d,%d %dx%d (alpha 150 so it is visible in a photo)",
        reinterpret_cast<void*>(cover), x, y, width, height);
    HWND under = WindowFromPoint(POINT{(x + width) / 2, (y + height) / 2});
    wchar_t cls[128] = {};
    if (under) GetClassNameW(under, cls, 128);
    DWORD pid = 0;
    if (under) GetWindowThreadProcessId(under, &pid);
    Log(L"COVER: the window under the middle of it is %p '%ls' (pid %lu) -- that is what a click would hit",
        reinterpret_cast<void*>(under), cls, pid);
    Sleep(static_cast<DWORD>(seconds) * 1000);
    DestroyWindow(cover);
    Log(L"COVER: gone");
    return 0;
}

int RunThumbDump(int x, int y, int width, int height, bool clipTaskbar) {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!taskbar || !tray) {
        Log(L"THUMB: no taskbar/tray");
        return 1;
    }
    RECT bar = {}, trayRect = {};
    GetWindowRect(taskbar, &bar);
    GetWindowRect(tray, &trayRect);
    if (clipTaskbar) {
        ShowWindow(taskbar, SW_SHOWNA);
        Sleep(200);
        HRGN sliver = CreateRectRgn(trayRect.left - bar.left, trayRect.top - bar.top,
                                    trayRect.left - bar.left + 1, trayRect.top - bar.top + 1);
        SetWindowRgn(taskbar, sliver, TRUE);
        Sleep(300);
        Log(L"THUMB: the taskbar is clipped to one pixel -- nothing of it is on screen");
    } else {
        Log(L"THUMB: the taskbar is as it is (visible=%d)", IsWindowVisible(taskbar) ? 1 : 0);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabThumb";
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 30));
    RegisterClassExW(&wc);
    const HWND card = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TrayLabThumb", L"thumb",
                                      WS_POPUP | WS_VISIBLE, x, y, width, height, nullptr, nullptr,
                                      wc.hInstance, nullptr);
    Log(L"THUMB: our card %p at %d,%d %dx%d", reinterpret_cast<void*>(card), x, y, width, height);

    // THE POINT: the source rectangle is the TRAY's own rectangle inside the taskbar, and the destination is
    // our whole card -- so the card shows exactly those pixels of the shell's bar and nothing else.
    HTHUMBNAIL thumbnail = nullptr;
    const HRESULT registered = DwmRegisterThumbnail(card, taskbar, &thumbnail);
    Log(L"THUMB: DwmRegisterThumbnail -> 0x%08lX, handle %p", static_cast<unsigned long>(registered),
        reinterpret_cast<void*>(thumbnail));
    if (!thumbnail) return 1;
    DWM_THUMBNAIL_PROPERTIES properties = {};
    properties.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    properties.rcSource.left = trayRect.left - bar.left;
    properties.rcSource.top = trayRect.top - bar.top;
    properties.rcSource.right = trayRect.right - bar.left;
    properties.rcSource.bottom = trayRect.bottom - bar.top;
    properties.rcDestination.left = 0;   // our card's client area
    properties.rcDestination.top = 0;
    properties.rcDestination.right = width;
    properties.rcDestination.bottom = height;
    properties.opacity = 255;
    properties.fVisible = TRUE;
    const HRESULT set = DwmUpdateThumbnailProperties(thumbnail, &properties);
    Log(L"THUMB: source %ld,%ld %ldx%ld -> destination %dx%d, DwmUpdateThumbnailProperties 0x%08lX",
        properties.rcSource.left, properties.rcSource.top,
        properties.rcSource.right - properties.rcSource.left,
        properties.rcSource.bottom - properties.rcSource.top, width, height,
        static_cast<unsigned long>(set));
    RedrawWindow(card, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    Log(L"THUMB: holding 12 s -- photograph the card now");
    Sleep(12000);
    DwmUnregisterThumbnail(thumbnail);
    DestroyWindow(card);
    if (clipTaskbar) SetWindowRgn(taskbar, nullptr, TRUE);
    Log(L"THUMB: unregistered and put back");
    return 0;
}

// --- E12, ROUTE A: watch the registrations, then send an application its own click -------------------
//
//  STEP ONE, --observe: load the observer module into explorer with a WH_CALLWNDPROC hook on the thread that
//  owns the shell's taskbar, optionally broadcast TaskbarCreated so applications re-register (that message
//  is how the shell tells them the taskbar was rebuilt, and the applications we need to hear from registered
//  before we were listening), then read the file the module writes.
//
//  STEP TWO, --sendright: find that application's registration in the file, and send it exactly the message
//  its own tray icon would receive -- WM_RBUTTONDOWN then WM_RBUTTONUP with the uid in wParam -- from a
//  process that is NOT the shell. Whether the application answers, and whether its menu appears where the
//  pointer is, is the whole question.
namespace {

std::wstring ObservedFilePath() {
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return {};
    std::wstring path = profile;
    path += L"\\IslandBar\\tray_observed.txt";
    return path;
}

int RunObserve(int seconds, bool broadcast) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring directory = exe;
    const size_t slash = directory.find_last_of(L'\\');
    directory = directory.substr(0, slash + 1);

    // The newest build of the observer, because the build script stamps every build with a new name (the copy
    // Explorer holds is pinned and locked, so a fixed name could never be rebuilt).
    std::wstring dllPath;
    WIN32_FIND_DATAW found = {};
    const std::wstring pattern = directory + L"probe-tray-observer*.dll";
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        FILETIME newest = {};
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (CompareFileTime(&found.ftLastWriteTime, &newest) >= 0) {
                newest = found.ftLastWriteTime;
                dllPath = directory + found.cFileName;
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (dllPath.empty()) {
        Log(L"OBSERVE: no observer module in '%ls' -- run scripts\\probe-tray-lab.ps1 first",
            directory.c_str());
        return 1;
    }

    const HMODULE module = LoadLibraryW(dllPath.c_str());
    Log(L"OBSERVE: LoadLibrary('%ls') -> %p (error %lu)", dllPath.c_str(),
        reinterpret_cast<void*>(module), module ? 0UL : GetLastError());
    if (!module) return 1;
    const auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "IslandTrayObserverProc"));
    if (!proc) {
        Log(L"OBSERVE: the module has no IslandTrayObserverProc");
        return 1;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        Log(L"OBSERVE: no Shell_TrayWnd to find the shell's thread from");
        return 1;
    }
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(taskbar, &pid);
    Log(L"OBSERVE: the shell's taskbar is pid %lu thread %lu", pid, thread);

    SetLastError(0);
    const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, proc, module, thread);
    Log(L"OBSERVE: SetWindowsHookEx(WH_CALLWNDPROC, thread %lu) -> %p (error %lu)", thread,
        reinterpret_cast<void*>(hook), hook ? 0UL : GetLastError());
    if (!hook) return 1;

    if (broadcast) {
        // What the shell sends when its taskbar is rebuilt. Applications answer it by registering their
        // icons again -- which is the only way to hear from one that registered before we were listening.
        const UINT created = RegisterWindowMessageW(L"TaskbarCreated");
        PostMessageW(HWND_BROADCAST, created, 0, 0);
        Log(L"OBSERVE: broadcast TaskbarCreated (0x%04X) and waiting for registrations", created);
    }

    Log(L"OBSERVE: watching for %d s -- start or restart a tray application now", seconds);
    Sleep(static_cast<DWORD>(seconds) * 1000);

    UnhookWindowsHookEx(hook);
    Log(L"OBSERVE: hook removed (the module stays pinned in the shell until it restarts)");
    return 0;
}

// One registration, as the observer file holds it.
struct ObservedRegistration {
    unsigned pid = 0;          // the receiver of the registration: the shell. Useless, and kept so the log
                               // shows that it is: the sender's identity lives in windowPid.
    unsigned uid = 0;
    unsigned callback = 0;
    unsigned window = 0;
    unsigned command = 0;
    unsigned windowPid = 0;    // the process that actually registered the icon
    unsigned version = 0;      // uVersion, non-zero only on the NIM_SETVERSION line
    std::wstring windowClass;
    std::wstring tip;
};

bool ParseObserved(const std::wstring& line, ObservedRegistration* out) {
    int pid = 0, command = 0, uid = 0, callback = 0, window = 0, windowPid = 0, version = 0;
    unsigned flags = 0;
    wchar_t windowClass[65] = {};
    wchar_t tip[129] = {};
    // The tail matches the observer's current format; lines written before it existed fail to parse and are
    // skipped rather than mis-read (a wrong field silently produces a wrong experiment).
    if (swscanf_s(line.c_str(),
                  L"[%*d:%*d:%*d.%*d] registration from pid %d: command=%d uid=%d flags=0x%X "
                  L"callback=0x%X window=0x%X windowPid=%d windowClass='%64[^']' version=%d tip='%128[^']'",
                  &pid, &command, &uid, &flags, &callback, &window, &windowPid, windowClass, 65, &version,
                  tip, 129) < 10) {
        return false;
    }
    out->pid = static_cast<unsigned>(pid);
    out->command = static_cast<unsigned>(command);
    out->uid = static_cast<unsigned>(uid);
    out->callback = static_cast<unsigned>(callback);
    out->window = static_cast<unsigned>(window);
    out->windowPid = static_cast<unsigned>(windowPid);
    out->version = static_cast<unsigned>(version);
    out->windowClass = windowClass;
    out->tip = tip;
    return true;
}

// --- screen evidence: a full-screen grab, a difference, and a file ----------
//
// The question "did the application show its menu" has to be answered by pixels, not by the return value of
// SendMessage (a window procedure returning 0 says nothing at all). So every attempt is bracketed by grabs:
// two before the first attempt establish the noise floor (this machine has an animated bar in the top strip),
// and one after each attempt shows whether anything appeared and where.
struct ScreenShot {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;   // 32 bpp BGRA, top-down
};

bool CaptureScreen(ScreenShot* shot) {
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;      // negative: rows come out top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap && bits) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, width, height, screen, originX, originY, SRCCOPY) != FALSE;
        SelectObject(memory, previous);
        if (ok) {
            shot->width = width;
            shot->height = height;
            shot->pixels.assign(static_cast<unsigned char*>(bits),
                                static_cast<unsigned char*>(bits) + (static_cast<size_t>(width) * height * 4));
        }
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

// How many pixels changed by more than a hair, and where the change is. A menu is thousands of pixels; the
// bar's own animation is the noise floor and is measured before anything is sent.
long long ShotDiff(const ScreenShot& a, const ScreenShot& b, RECT* box) {
    if (a.width != b.width || a.height != b.height || a.pixels.empty()) return -1;
    long long changed = 0;
    RECT bounds = {a.width, a.height, -1, -1};
    for (int y = 0; y < a.height; ++y) {
        const unsigned char* rowA = a.pixels.data() + static_cast<size_t>(y) * a.width * 4;
        const unsigned char* rowB = b.pixels.data() + static_cast<size_t>(y) * b.width * 4;
        for (int x = 0; x < a.width; ++x) {
            const unsigned char* pa = rowA + x * 4;
            const unsigned char* pb = rowB + x * 4;
            const int d = std::abs(pa[0] - pb[0]) + std::abs(pa[1] - pb[1]) + std::abs(pa[2] - pb[2]);
            if (d > 24) {
                ++changed;
                if (x < bounds.left) bounds.left = x;
                if (x > bounds.right) bounds.right = x;
                if (y < bounds.top) bounds.top = y;
                if (y > bounds.bottom) bounds.bottom = y;
            }
        }
    }
    if (box) *box = bounds;
    return changed;
}

bool SaveShotBmp(const ScreenShot& shot, const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const unsigned rowBytes = static_cast<unsigned>(shot.width) * 4;
    const unsigned imageBytes = rowBytes * static_cast<unsigned>(shot.height);
    BITMAPFILEHEADER header = {};
    BITMAPINFOHEADER info = {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + imageBytes;
    info.biSize = sizeof(info);
    info.biWidth = shot.width;
    info.biHeight = -shot.height;    // negative: the rows are stored top-down, and a file that says
                                     // otherwise opens upside down (measured, and it was confusing)
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = imageBytes;
    const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                    std::fwrite(&info, sizeof(info), 1, file) == 1 &&
                    std::fwrite(shot.pixels.data(), imageBytes, 1, file) == 1;
    std::fclose(file);
    return ok;
}

// The mandatory level of a process' token: the number that decides whether a message from us is delivered at
// all. An application running higher than us is silently deaf to every message we send (UIPI), which looks
// exactly like "the application ignored the message".
std::wstring IntegrityOf(unsigned pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"unreadable";
    HANDLE token = nullptr;
    std::wstring result = L"unreadable";
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        if (size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
            const unsigned char count = *GetSidSubAuthorityCount(label->Label.Sid);
            const DWORD rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
            result = (rid >= SECURITY_MANDATORY_SYSTEM_RID)     ? L"System"
                     : (rid >= SECURITY_MANDATORY_HIGH_RID)     ? L"High"
                     : (rid >= SECURITY_MANDATORY_MEDIUM_RID)   ? L"Medium"
                     : (rid >= SECURITY_MANDATORY_LOW_RID)      ? L"Low"
                                                                : L"Untrusted";
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return result;
}

// What an application puts on the screen is a WINDOW, and a window can be listed, named and measured. That
// is a far better answer to "did its menu appear, and where" than a pixel count: a pixel count cannot tell a
// menu apart from any other window that happened to open at the same moment (which is not hypothetical --
// it happened on the first run of this experiment, and the only reason it was visible was a screenshot).
struct WindowInfo {
    HWND handle = nullptr;
    std::wstring className;
    std::wstring title;
    RECT rect = {};
    bool visible = false;
    bool topLevel = true;
};

struct WindowEnumContext {
    DWORD pid = 0;
    std::vector<WindowInfo>* out = nullptr;
};

BOOL CALLBACK CollectWindow(HWND handle, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowEnumContext*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(handle, &pid);
    if (context->pid != 0 && pid != context->pid) return TRUE;   // pid 0 means "every process"
    {
        // Only windows somebody could see: a menu that never becomes visible is not a menu, and the
        // hidden helper windows of an Electron application would otherwise drown the report.
        if (!IsWindowVisible(handle)) return TRUE;
        RECT box = {};
        GetWindowRect(handle, &box);
        if (box.right - box.left <= 0 || box.bottom - box.top <= 0) return TRUE;
    }
    WindowInfo info;
    info.handle = handle;
    wchar_t text[256] = {};
    GetClassNameW(handle, text, 256);
    info.className = text;
    text[0] = L'\0';
    GetWindowTextW(handle, text, 256);
    info.title = text;
    GetWindowRect(handle, &info.rect);
    info.visible = IsWindowVisible(handle) != FALSE;
    context->out->push_back(info);
    return TRUE;
}

void ListWindows(DWORD pid, std::vector<WindowInfo>* out) {
    out->clear();
    WindowEnumContext context;
    context.pid = pid;
    context.out = out;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&context));
}

const WindowInfo* FindWindowInfo(const std::vector<WindowInfo>& list, HWND handle) {
    for (const WindowInfo& info : list) {
        if (info.handle == handle) return &info;
    }
    return nullptr;
}

// Compares two listings and writes what is new and what changed visibility. Silence is the interesting case:
// it means the application answered nothing at all.
int ReportWindowDelta(const std::vector<WindowInfo>& before, const std::vector<WindowInfo>& after,
                      const wchar_t* label) {
    int reported = 0;
    for (const WindowInfo& info : after) {
        const WindowInfo* was = FindWindowInfo(before, info.handle);
        const bool isNew = was == nullptr;
        const bool appeared = was && !was->visible && info.visible;
        const bool moved = was && (was->rect.left != info.rect.left || was->rect.top != info.rect.top ||
                                   was->rect.right != info.rect.right || was->rect.bottom != info.rect.bottom);
        if (!isNew && !appeared && !moved) continue;
        Log(L"SENDRIGHT: %ls window 0x%X %ls class '%ls' title '%ls' rect %ld,%ld..%ld,%ld (%lux%lu)%s",
            label, static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(info.handle)),
            isNew ? L"NEW" : (appeared ? L"APPEARED" : L"MOVED"), info.className.c_str(), info.title.c_str(),
            info.rect.left, info.rect.top, info.rect.right, info.rect.bottom,
            static_cast<unsigned long>(info.rect.right - info.rect.left),
            static_cast<unsigned long>(info.rect.bottom - info.rect.top), info.visible ? L"" : L" (hidden)");
        ++reported;
    }
    for (const WindowInfo& info : before) {
        const WindowInfo* now = FindWindowInfo(after, info.handle);
        if (now && info.visible == now->visible) continue;
        if (!now) {
            Log(L"SENDRIGHT: %ls window 0x%X GONE class '%ls' title '%ls'",
                label, static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(info.handle)), info.className.c_str(),
                info.title.c_str());
            ++reported;
        }
    }
    if (reported == 0) Log(L"SENDRIGHT: %ls no window of that process appeared, moved or went away", label);
    return reported;
}

int RunSendRight(const wchar_t* wanted, int x, int y, const std::wstring& mode) {
    const std::wstring path = ObservedFilePath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || !file) {
        Log(L"SENDRIGHT: cannot read '%ls' -- run --observe first", path.c_str());
        return 1;
    }
    ObservedRegistration last;
    bool found = false;
    // EVERY registration that matches, not only the last one: an application can leave more than one
    // window registered under the same name (Electron apps do), and sending to the wrong one of them looks
    // exactly like "the application ignored the click". Mode 'scan' walks all of them.
    std::vector<ObservedRegistration> matches;
    wchar_t line[1024] = {};
    while (std::fgetws(line, 1024, file)) {
        ObservedRegistration candidate;
        if (!ParseObserved(line, &candidate)) continue;
        if (wanted && *wanted && candidate.tip.find(wanted) == std::wstring::npos) continue;
        last = candidate;        // the LAST one wins: a later registration is the current one
        bool duplicate = false;
        for (const ObservedRegistration& seen : matches) {
            if (seen.window == candidate.window && seen.uid == candidate.uid) duplicate = true;
        }
        if (!duplicate) matches.push_back(candidate);
        found = true;
    }
    std::fclose(file);
    if (!found) {
        Log(L"SENDRIGHT: nothing in the file matches '%ls'", wanted);
        return 1;
    }
    if (mode != L"scan") {
        matches.clear();
        matches.push_back(last);
    }
    Log(L"SENDRIGHT: %zu registration(s) match, trying them all", matches.size());
    POINT saved = {};
    GetCursorPos(&saved);
    bool anySignal = false;
    for (const ObservedRegistration& chosen : matches) {
    Log(L"SENDRIGHT: registration uid %u callback 0x%04X window 0x%X windowPid %u class '%ls' version %u "
        L"tip '%ls' (recorded receiver pid %u -- the shell, that field is expected to be useless)",
        chosen.uid, chosen.callback, chosen.window, chosen.windowPid, chosen.windowClass.c_str(),
        chosen.version, chosen.tip.c_str(), chosen.pid);
    const HWND target = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(chosen.window));
    if (!IsWindow(target)) {
        // A dead handle means the record is stale: the application restarted and its window is a new one.
        // Sending to it would be sending nowhere, and the log would read "the application ignored us".
        Log(L"SENDRIGHT: window 0x%X is gone -- the record is stale, run --observe again", chosen.window);
        continue;
    }
    DWORD targetPid = 0;
    GetWindowThreadProcessId(target, &targetPid);
    wchar_t targetClass[64] = {};
    GetClassNameW(target, targetClass, 64);
    Log(L"SENDRIGHT: the window is alive: pid %u class '%ls' integrity %ls (this probe is %ls)",
        targetPid, targetClass, IntegrityOf(targetPid).c_str(), IntegrityOf(GetCurrentProcessId()).c_str());

    // WHICH ENCODING. The shell and the application agree on a callback *protocol* through uVersion, and the
    // two protocols put the same click in different places:
    //   classic (version 0/3):  wParam = uid,  lParam = the mouse message
    //   version 4:              wParam = MAKELONG(x, y) [the pointer],  lParam = MAKELONG(event, uid)
    //                           -- measured 2026-10-16 by listening to the REAL click; the reverse order
    //                           this file used before made WeChat index out of range and crash (8.142)
    // The observer cannot always see the version (it only appears on the application's own NIM_SETVERSION
    // line, and several applications never send one), so all of them are tried and the screen says which one
    // the application acts on. There is no other way to find out: a window procedure that ignores a message
    // returns exactly the same thing as one that acts on it.
    struct Variant {
        const wchar_t* name;
        UINT event;
        bool version4;
        bool withDown;
    };
    std::vector<Variant> variants;
    if (mode == L"all") {
        variants = {{L"v4-contextmenu", WM_CONTEXTMENU, true, false},
                    {L"v4-rbuttonup", WM_RBUTTONUP, true, false},
                    {L"v3-rbuttonup", WM_RBUTTONUP, false, true},
                    {L"v3-contextmenu", WM_CONTEXTMENU, false, true},
                    {L"v4-select", NIN_SELECT, true, false}};
    } else if (mode == L"v4") {
        variants = {{L"v4-contextmenu", WM_CONTEXTMENU, true, false}};
    } else if (mode == L"v3") {
        variants = {{L"v3-rbuttonup", WM_RBUTTONUP, false, true}};
    } else if (mode == L"scan") {
        // The two right-click candidates, for every window that answered to that name.
        variants = {{L"v4-contextmenu", WM_CONTEXTMENU, true, false},
                    {L"v3-rbuttonup", WM_RBUTTONUP, false, true}};
    } else {
        Log(L"SENDRIGHT: unknown mode '%ls' (v4 | v3 | all | scan)", mode.c_str());
        return 2;
    }

    // Evidence first: two grabs 400 ms apart give this machine's noise floor (the bar animates), so a change
    // afterwards can be read as ours and not as the bar's.
    ScreenShot baseline, before, after;
    CaptureScreen(&baseline);
    Sleep(400);
    CaptureScreen(&before);
    RECT noiseBox = {};
    const long long noise = ShotDiff(baseline, before, &noiseBox);
    Log(L"SENDRIGHT: noise floor over 400 ms: %lld pixel(s)", noise);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring prefix = exe;
    const size_t slash = prefix.find_last_of(L'\\');
    prefix = prefix.substr(0, slash + 1) + L"sendright-";
    SaveShotBmp(before, prefix + L"before.bmp");

    const HWND foregroundBefore = GetForegroundWindow();
    SetCursorPos(x, y);
    Sleep(150);
    Log(L"SENDRIGHT: the pointer is parked at %d,%d -- a cell of ours would be there -- and the application "
        L"is sent its own message. Foreground before: 0x%X", x, y,
        static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(foregroundBefore)));

    const UINT packed = MAKELONG(static_cast<WORD>(x), static_cast<WORD>(y));
    for (const Variant& variant : variants) {
        std::vector<WindowInfo> windowsBefore;
        ListWindows(targetPid, &windowsBefore);
        std::vector<WindowInfo> anyBefore;
        if (mode == L"scan") ListWindows(0, &anyBefore);   // the screen is what matters, not the pid

        LARGE_INTEGER frequency = {}, start = {}, stop = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        // A TIMEOUT, never a plain SendMessage: the application is allowed to run a modal menu loop inside
        // this call (measured: one application held it for 10.4 seconds), and a tray that freezes while an
        // application thinks is not a tray. The call is abandoned, not cancelled -- the application keeps
        // working, exactly as it does when the shell itself sends it this message.
        ULONG_PTR answer = 0;
        const auto deliver = [&](WPARAM wparam, LPARAM lparam) {
            return SendMessageTimeoutW(target, chosen.callback, wparam, lparam,
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &answer);
        };
        LRESULT delivered = 0;
        if (variant.version4) {
            if (variant.withDown) {
                delivered = deliver(packed, MAKELONG(static_cast<WORD>(WM_RBUTTONDOWN),
                                                     static_cast<WORD>(chosen.uid)));
            }
            delivered = deliver(packed, MAKELONG(static_cast<WORD>(variant.event),
                                                 static_cast<WORD>(chosen.uid)));
        } else {
            if (variant.withDown) {
                deliver(chosen.uid, WM_RBUTTONDOWN);
                Sleep(40);
            }
            delivered = deliver(chosen.uid, variant.event);
        }
        QueryPerformanceCounter(&stop);
        // A procedure that acts on the message takes measurable time (it builds a window); one that ignores it
        // returns in microseconds. This is the only clue the return value cannot give: both answer 0.
        Log(L"SENDRIGHT: '%ls' -> delivered=%ls, the window was busy for %.1f ms (answer %llu)",
            variant.name, delivered ? L"yes" : L"NO (timed out or blocked)",
            (stop.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart,
            static_cast<unsigned long long>(answer));

        Sleep(600);
        std::vector<WindowInfo> windowsAfter;
        ListWindows(targetPid, &windowsAfter);
        const int appeared = ReportWindowDelta(windowsBefore, windowsAfter, variant.name);
        if (mode == L"scan") {
            std::vector<WindowInfo> anyAfter;
            ListWindows(0, &anyAfter);
            ReportWindowDelta(anyBefore, anyAfter, L"ANY PROCESS:");
        }

        if (!CaptureScreen(&after)) {
            Log(L"SENDRIGHT: the screen grab failed");
            break;
        }
        RECT box = {};
        const long long changed = ShotDiff(before, after, &box);
        Log(L"SENDRIGHT: '%ls' -> %lld pixel(s) differ from the state before it, at %ld,%ld..%ld,%ld "
            L"(noise floor was %lld)", variant.name, changed, box.left, box.top, box.right, box.bottom, noise);
        std::wstring shot = prefix + variant.name + L".bmp";
        SaveShotBmp(after, shot);
        Log(L"SENDRIGHT: saved '%ls'; foreground window is 0x%X", shot.c_str(),
            static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(GetForegroundWindow())));
        if (appeared > 0 || changed > noise * 3 + 3000) anySignal = true;

        // ... and does it stay? A menu that closes by itself is not a menu.
        if (mode == L"scan") continue;      // the scan is about which encoding works, not about duration
        Sleep(2500);
        std::vector<WindowInfo> windowsLater;
        ListWindows(targetPid, &windowsLater);
        ReportWindowDelta(windowsAfter, windowsLater, L"three seconds later:");
    }
    }
    SetCursorPos(saved.x, saved.y);
    Log(L"SENDRIGHT: done -- %ls; the pointer is back at %ld,%ld", anySignal ? L"SOMETHING APPEARED" : L"nothing appeared",
        saved.x, saved.y);
    return 0;
}

// --- E13, THE LIVE TRAY: our cells and the applications' OWN behaviour -----------------------------
//
//  THIS IS THE THING TO LOOK AT. Everything this round measured, put on screen: a strip of cells drawn by
//  us, each carrying the registration the read-only module read out of the shell (window, uid,
//  uCallbackMessage, uVersion, tip). A click is delivered as the message that application's own tray icon
//  would receive, chosen by ONE rule -- uVersion -- and nothing else is learned per application:
//
//      version 4 : wParam = MAKELONG(x, y) [the pointer], lParam = MAKELONG(event, uid)
//      otherwise : wParam = uid,                  lParam = the mouse message
//
//  Left click  -> the application's own left click (NIN_SELECT / WM_LBUTTONDOWN+UP)
//  Right click -> the application's OWN menu (WM_CONTEXTMENU / WM_RBUTTONUP) at the pointer, which is on
//                 the cell, so it opens where a user right-clicked and the pointer never moves.
//  Escape      -> close.
//
//  The shell's own cells (network, volume, clock) are deliberately NOT here: they are the shell's own
//  icons with no application behind them, and this sample is about applications.

constexpr int kLiveCell = 40;
constexpr int kLiveGap = 4;
constexpr int kLivePad = 8;
constexpr int kLiveLabel = 24;
constexpr int kLiveMax = 24;
constexpr UINT kLiveTimeout = 250;      // milliseconds; see the modal-menu note below

struct LiveCell {
    std::wstring name;      // the application's own tooltip, flattened
    HICON icon = nullptr;
    wchar_t glyph[4] = {};
    HWND target = nullptr;
    UINT callback = 0;
    UINT uid = 0;
    UINT version = 0;
    DWORD pid = 0;
};

LiveCell g_live[kLiveMax];
int g_liveCount = 0;
int g_liveHover = -1;
HWND g_liveWindow = nullptr;
bool g_liveQuit = false;

RECT LiveCellRect(int index) {
    RECT box = {};
    const int pitch = kLiveCell + kLiveGap;
    box.left = kLivePad + index * pitch;
    box.top = kLivePad;
    box.right = box.left + kLiveCell;
    box.bottom = box.top + kLiveCell;
    return box;
}

int LiveCellAt(int x, int y) {
    for (int i = 0; i < g_liveCount; ++i) {
        const RECT box = LiveCellRect(i);
        if (x >= box.left && x < box.right && y >= box.top && y < box.bottom) return i;
    }
    return -1;
}

// The process behind a registration, and its executable: the picture for a cell comes from the executable
// (SHGetFileInfo asks the shell for the icon the file itself carries). Reading a path is a limited query and
// works across integrity levels -- measured, it answers for the elevated Task Manager too.
std::wstring ExeOfProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[1024] = {};
    DWORD size = 1024;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    return ok ? std::wstring(path) : std::wstring();
}

void LiveDeliver(int index, bool rightClick) {
    const LiveCell& cell = g_live[index];
    if (!cell.target || !IsWindow(cell.target)) {
        Log(L"TRAYLIVE: '%ls' has no window any more -- the application has restarted; press the cell "
            L"again after the strip is reopened", cell.name.c_str());
        return;
    }
    // WHERE THE POINTER IS, NOT WHERE WE THINK IT IS: the user has just clicked the cell, so the pointer is
    // on it, and that is the coordinate the application is told about -- which is how its menu ends up under
    // the cell instead of somewhere else. Nothing here moves the pointer.
    POINT at = {};
    GetCursorPos(&at);
    const LPARAM packed = MAKELONG(static_cast<WORD>(at.x), static_cast<WORD>(at.y));

    LARGE_INTEGER frequency = {}, started = {}, finished = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    ULONG_PTR answer = 0;
    LRESULT delivered = 0;
    wchar_t encoding[48] = {};
    if (cell.version >= 4) {
        const UINT event = rightClick ? WM_CONTEXTMENU : NIN_SELECT;
        // version 4 goes the other way round: the coordinates in wParam, the (event, uid) pair in lParam.
        // Measured against a real click -- HANDOFF 8.142. Getting it backwards crashes some applications.
        delivered = SendMessageTimeoutW(cell.target, cell.callback, static_cast<WPARAM>(packed),
                                        MAKELONG(static_cast<WORD>(event), static_cast<WORD>(cell.uid)),
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, kLiveTimeout, &answer);
        _snwprintf_s(encoding, 48, _TRUNCATE, L"version 4, event 0x%04X", event);
    } else {
        const UINT down = rightClick ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
        const UINT up = rightClick ? WM_RBUTTONUP : WM_LBUTTONUP;
        SendMessageTimeoutW(cell.target, cell.callback, static_cast<WPARAM>(cell.uid), down,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, kLiveTimeout, &answer);
        Sleep(30);
        delivered = SendMessageTimeoutW(cell.target, cell.callback, static_cast<WPARAM>(cell.uid), up,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, kLiveTimeout, &answer);
        _snwprintf_s(encoding, 48, _TRUNCATE, L"classic, message 0x%04X", up);
    }
    QueryPerformanceCounter(&finished);
    // A TIMEOUT, NOT A BLOCKING SEND. An application is allowed to run its menu loop inside this call
    // (measured: 10.4 seconds for one of them), and inside that loop our strip must still answer the mouse.
    Log(L"TRAYLIVE: %ls click -> '%ls' uid=%u callback=0x%04X window=0x%X pid=%u (%ls): delivered=%ls, "
        L"the application was busy %.1f ms",
        rightClick ? L"right" : L"left", cell.name.c_str(), cell.uid, cell.callback,
        static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(cell.target)), cell.pid, encoding,
        delivered ? L"yes" : L"NO", (finished.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart);
}

LRESULT CALLBACK LiveProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client = {};
            GetClientRect(hwnd, &client);
            HBRUSH surface = CreateSolidBrush(RGB(30, 30, 36));
            HPEN edge = CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
            HGDIOBJ oldBrush = SelectObject(dc, surface);
            HGDIOBJ oldPen = SelectObject(dc, edge);
            RoundRect(dc, client.left, client.top, client.right, client.bottom, 14, 14);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(surface);
            DeleteObject(edge);
            SetBkMode(dc, TRANSPARENT);
            for (int i = 0; i < g_liveCount; ++i) {
                const RECT box = LiveCellRect(i);
                HBRUSH cell = CreateSolidBrush(i == g_liveHover ? RGB(64, 64, 76) : RGB(44, 44, 52));
                HPEN noPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
                oldBrush = SelectObject(dc, cell);
                oldPen = SelectObject(dc, noPen);
                RoundRect(dc, box.left, box.top, box.right, box.bottom, 8, 8);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(cell);
                DeleteObject(noPen);
                const int inner = kLiveCell - 11;
                if (g_live[i].icon) {
                    DrawIconEx(dc, box.left + (kLiveCell - inner) / 2, box.top + (kLiveCell - inner) / 2,
                               g_live[i].icon, inner, inner, 0, nullptr, DI_NORMAL);
                } else {
                    SetTextColor(dc, RGB(226, 226, 234));
                    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                    DrawTextW(dc, g_live[i].glyph, -1, const_cast<RECT*>(&box),
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            if (g_liveHover >= 0 && g_liveHover < g_liveCount) {
                RECT line = client;
                line.left += kLivePad;
                line.right -= kLivePad;
                line.top = kLivePad + kLiveCell + 2;
                HBRUSH prompt = CreateSolidBrush(RGB(22, 22, 28));
                HPEN noPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
                HGDIOBJ oldBrush2 = SelectObject(dc, prompt);
                HGDIOBJ oldPen2 = SelectObject(dc, noPen);
                RoundRect(dc, line.left, line.top, line.right, line.bottom, 8, 8);
                SelectObject(dc, oldBrush2);
                SelectObject(dc, oldPen2);
                DeleteObject(prompt);
                DeleteObject(noPen);
                SetTextColor(dc, RGB(210, 210, 220));
                SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                wchar_t caption[256] = {};
                _snwprintf_s(caption, 256, _TRUNCATE, L"%ls   uid %u, callback 0x%04X, version %u",
                             g_live[g_liveHover].name.c_str(), g_live[g_liveHover].uid,
                             g_live[g_liveHover].callback, g_live[g_liveHover].version);
                RECT text = line;
                text.left += 8;
                text.right -= 8;
                DrawTextW(dc, caption, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Log(L"TRAYLIVE: Escape -- closing");
                g_liveQuit = true;
            }
            return 0;
        case WM_MOUSEMOVE: {
            const int cell = LiveCellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (cell != g_liveHover) {
                g_liveHover = cell;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (cell >= 0) {
                TRACKMOUSEEVENT leave = {};
                leave.cbSize = sizeof(leave);
                leave.dwFlags = TME_LEAVE;
                leave.hwndTrack = hwnd;
                TrackMouseEvent(&leave);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_liveHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const int cell = LiveCellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (cell >= 0) LiveDeliver(cell, /*rightClick=*/false);
            return 0;
        }
        case WM_RBUTTONUP: {
            const int cell = LiveCellAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (cell >= 0) LiveDeliver(cell, /*rightClick=*/true);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

long long FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// Reads what was appended to the observer's file since the offset -- the records from BEFORE this run are
// not evidence: an application that restarted has a different window, and its old record points at nothing.
std::wstring ReadAppended(const std::wstring& path, long long from) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return {};
    if (_fseeki64(file, from, SEEK_SET) != 0) {
        std::fclose(file);
        return {};
    }
    std::string bytes;
    char buffer[4096] = {};
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, got);
    std::fclose(file);
    if (bytes.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                          nullptr, 0);
    if (count <= 0) return {};
    std::wstring text(static_cast<size_t>(count), L'\0');
    // &text[0], not text.data(): this probe is built without /std:c++17, where a non-const std::wstring
    // still hands out a const pointer from data() and the call does not compile.
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], count);
    return text;
}

// --- E14, THE MESSAGE DUMP: what does the SHELL actually send that application? ---------------------
//
//  WHY THIS EXISTS. A registration tells us the callback number, not the wire format the application
//  expects back. Two applications on this machine answered the message we composed (QQ音乐's own menu, the
//  elevated Task Manager's real Win32 menu) and one did not: ChatGPT's host window takes the message, spends
//  30-50 ms, and shows nothing -- while a REAL right click on its real icon shows a WinUI menu raised by a
//  different process of the same application. So the shell must be sending it something else, and the only
//  honest way to learn what is to listen inside that application while a real click is performed on its
//  real tray icon.
//
//  It watches and nothing else: the module passes every message on with CallNextHookEx unchanged.
int RunTrayMessageDump(const wchar_t* wanted) {
    const std::wstring path = ObservedFilePath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || !file) {
        Log(L"DUMP: cannot read '%ls' -- run --observe first", path.c_str());
        return 1;
    }
    ObservedRegistration chosen;
    bool found = false;
    wchar_t line[1024] = {};
    while (std::fgetws(line, 1024, file)) {
        ObservedRegistration candidate;
        if (!ParseObserved(line, &candidate)) continue;
        if (wanted && *wanted && candidate.tip.find(wanted) == std::wstring::npos) continue;
        if (candidate.callback == 0) continue;
        chosen = candidate;
        found = true;
    }
    std::fclose(file);
    if (!found) {
        Log(L"DUMP: nothing in the file matches '%ls'", wanted);
        return 1;
    }
    const HWND target = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(chosen.window));
    if (!IsWindow(target)) {
        Log(L"DUMP: window 0x%X is gone -- the record is stale, run --observe again", chosen.window);
        return 1;
    }
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(target, &pid);
    Log(L"DUMP: '%ls' lives in pid %u thread %u (callback 0x%04X, version %u, class '%ls')",
        chosen.tip.c_str(), pid, thread, chosen.callback, chosen.version, chosen.windowClass.c_str());

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring directory = exe;
    const size_t slash = directory.find_last_of(L'\\');
    directory = directory.substr(0, slash + 1);
    std::wstring modulePath;
    WIN32_FIND_DATAW found2 = {};
    const std::wstring pattern = directory + L"probe-tray-observer*.dll";
    HANDLE search = FindFirstFileW(pattern.c_str(), &found2);
    if (search != INVALID_HANDLE_VALUE) {
        FILETIME newest = {};
        do {
            if (found2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (CompareFileTime(&found2.ftLastWriteTime, &newest) >= 0) {
                newest = found2.ftLastWriteTime;
                modulePath = directory + found2.cFileName;
            }
        } while (FindNextFileW(search, &found2));
        FindClose(search);
    }
    const HMODULE module = modulePath.empty() ? nullptr : LoadLibraryW(modulePath.c_str());
    const auto proc = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module,
                                                                        "IslandTrayMessageDumpProc"))
                             : nullptr;
    if (!proc) {
        Log(L"DUMP: the module beside this exe has no IslandTrayMessageDumpProc -- rebuild it");
        return 1;
    }
    SetLastError(0);
    const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, proc, module, thread);
    Log(L"DUMP: watching that process' messages -> hook %p (error %lu)", reinterpret_cast<void*>(hook),
        hook ? 0UL : GetLastError());
    if (!hook) return 1;
    Sleep(400);

    // Now the real thing, exactly as a user does it: the shell's overflow panel on screen, the pointer on
    // the icon, a genuine right press and release. Whatever arrives in the watched process is the answer.
    Log(L"DUMP: performing a real right click on the real tray icon of '%ls' (the pointer moves for it)", wanted);
    const int clicked = RunRightClick(wanted, /*sendInput=*/true, /*keepPanelVisible=*/true,
                                      /*byKeyboard=*/false);
    Sleep(2000);
    UnhookWindowsHookEx(hook);
    Log(L"DUMP: done (the click returned %d); the messages that arrived are the lines starting with "
        L"'message ' in the observer's file", clicked);
    return 0;
}

int RunTrayLive(int x, int y) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return 1;

    // --- the read-only module goes into the shell, and the applications are asked to register again ------
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring directory = exe;
    const size_t slash = directory.find_last_of(L'\\');
    directory = directory.substr(0, slash + 1);
    std::wstring modulePath;
    WIN32_FIND_DATAW found = {};
    const std::wstring pattern = directory + L"probe-tray-observer*.dll";
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        FILETIME newest = {};
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (CompareFileTime(&found.ftLastWriteTime, &newest) >= 0) {
                newest = found.ftLastWriteTime;
                modulePath = directory + found.cFileName;
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (modulePath.empty()) {
        Log(L"TRAYLIVE: no observer module beside this exe -- build it first");
        return 1;
    }
    const std::wstring observed = ObservedFilePath();
    const long long mark = FileSizeOf(observed);
    const HMODULE module = LoadLibraryW(modulePath.c_str());
    const auto proc = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module,
                                                                        "IslandTrayObserverProc"))
                             : nullptr;
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!module || !proc || !taskbar) {
        Log(L"TRAYLIVE: the module or the shell's taskbar is not reachable (module %ls, taskbar %p)",
            module ? L"loaded" : L"missing", reinterpret_cast<void*>(taskbar));
        if (module) FreeLibrary(module);
        return 1;
    }
    DWORD shellPid = 0;
    GetWindowThreadProcessId(taskbar, &shellPid);
    const DWORD thread = GetWindowThreadProcessId(taskbar, nullptr);
    const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, proc, module, thread);
    if (!hook) {
        Log(L"TRAYLIVE: could not watch the shell's registrations (error %lu)", GetLastError());
        FreeLibrary(module);
        return 1;
    }
    const UINT created = RegisterWindowMessageW(L"TaskbarCreated");
    PostMessageW(HWND_BROADCAST, created, 0, 0);
    Log(L"TRAYLIVE: asked every running application to register again (0x%04X); collecting for 4 s", created);
    Sleep(4000);

    // --- what registered ---------------------------------------------------------------------------------
    const std::wstring appended = ReadAppended(observed, mark);
    std::vector<ObservedRegistration> records;
    size_t start = 0;
    while (start < appended.size()) {
        size_t end = appended.find(L'\n', start);
        if (end == std::wstring::npos) end = appended.size();
        const std::wstring line = appended.substr(start, end - start);
        start = end + 1;
        if (line.find(L"registration from") == std::wstring::npos) continue;
        ObservedRegistration record;
        if (ParseObserved(line, &record)) records.push_back(record);
    }
    Log(L"TRAYLIVE: %zu registration line(s) arrived in that window", records.size());

    // The newest record per (window, uid) wins, except that a callback of 0 means "this message did not
    // carry one" (a modify or a delete usually does not) and must not erase a callback that did.
    struct Key {
        unsigned window = 0;
        unsigned uid = 0;
    };
    std::vector<Key> order;
    std::vector<ObservedRegistration> chosen;
    for (const ObservedRegistration& record : records) {
        if (record.window == 0) continue;
        Key key;
        key.window = record.window;
        key.uid = record.uid;
        size_t at = order.size();
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i].window == key.window && order[i].uid == key.uid) {
                at = i;
                break;
            }
        }
        if (record.command == 2) {                      // NIM_DELETE: the icon is gone
            if (at < order.size()) {
                order.erase(order.begin() + static_cast<long>(at));
                chosen.erase(chosen.begin() + static_cast<long>(at));
            }
            continue;
        }
        if (at < order.size()) {
            if (record.callback != 0) chosen[at] = record;
            if (!record.tip.empty()) chosen[at].tip = record.tip;
            continue;
        }
        if (record.callback == 0) continue;             // nothing to send a click to
        order.push_back(key);
        chosen.push_back(record);
    }

    for (const ObservedRegistration& record : chosen) {
        if (g_liveCount >= kLiveMax) break;
        if (record.windowPid == 0 || record.windowPid == shellPid) continue;   // the shell's own icons
        const HWND target = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(record.window));
        if (!IsWindow(target)) continue;
        std::wstring name = Flattened(record.tip);
        if (name.empty()) name = ExeOfProcess(record.windowPid);
        const size_t lastSlash = name.find_last_of(L'\\');
        if (lastSlash != std::wstring::npos) name = name.substr(lastSlash + 1);
        if (name.empty()) name = L"(no name)";
        LiveCell& cell = g_live[g_liveCount];
        cell.name = name;
        cell.target = target;
        cell.callback = record.callback;
        cell.uid = record.uid;
        cell.version = record.version;
        cell.pid = record.windowPid;
        const std::wstring executable = ExeOfProcess(record.windowPid);
        if (!executable.empty()) {
            SHFILEINFOW info = {};
            if (SHGetFileInfoW(executable.c_str(), 0, &info, sizeof(info),
                               SHGFI_ICON | SHGFI_LARGEICON) &&
                info.hIcon) {
                cell.icon = info.hIcon;
            }
        }
        if (!cell.icon) {
            cell.glyph[0] = name.empty() ? L'?' : name[0];
            cell.glyph[1] = L'\0';
        }
        Log(L"TRAYLIVE: cell %d '%ls' uid=%u callback=0x%04X version=%u pid=%u%s", g_liveCount,
            cell.name.c_str(), cell.uid, cell.callback, cell.version, cell.pid,
            cell.icon ? L"" : L" (no icon -- drawn as a glyph)");
        ++g_liveCount;
    }
    if (g_liveCount == 0) {
        Log(L"TRAYLIVE: nothing registered with a callback while we were listening");
        UnhookWindowsHookEx(hook);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LiveProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TrayLabLive";
    RegisterClassExW(&wc);
    const int width = kLivePad * 2 + g_liveCount * kLiveCell + (g_liveCount - 1) * kLiveGap;
    const int height = kLivePad * 2 + kLiveCell + kLiveLabel;
    g_liveWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TrayLabLive", L"tray live",
                                   WS_POPUP | WS_VISIBLE, x, y, width, height, nullptr, nullptr,
                                   wc.hInstance, nullptr);
    Log(L"TRAYLIVE: %d cell(s) at %d,%d %dx%d -- LEFT click = the application's own left click, RIGHT "
        L"click = the application's own menu, Escape closes. The pointer is never moved.",
        g_liveCount, x, y, width, height);

    MSG msg = {};
    while (!g_liveQuit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    DestroyWindow(g_liveWindow);
    for (int i = 0; i < g_liveCount; ++i) {
        if (g_live[i].icon) DestroyIcon(g_live[i].icon);
    }
    UnhookWindowsHookEx(hook);
    Log(L"TRAYLIVE: closed; the shell is left exactly as it was found (the module stays pinned in it until "
        L"the shell restarts)");
    CoUninitialize();
    return 0;
}

} // namespace

int RunAppBarMove(int x, int y, int seconds) {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        Log(L"APPBAR: no Shell_TrayWnd");
        return 1;
    }
    RECT before = {};
    GetWindowRect(taskbar, &before);
    Log(L"APPBAR: taskbar at %ld,%ld %ldx%ld before anything", before.left, before.top,
        before.right - before.left, before.bottom - before.top);

    APPBARDATA query = {};
    query.cbSize = sizeof(query);
    query.hWnd = taskbar;
    const UINT_PTR state = SHAppBarMessage(ABM_GETSTATE, &query);
    APPBARDATA position = {};
    position.cbSize = sizeof(position);
    position.hWnd = GetForegroundWindow();   // "our" appbar: this process owns no AppBar window
    const UINT_PTR got = SHAppBarMessage(ABM_GETTASKBARPOS, &position);
    Log(L"APPBAR: ABM_GETSTATE -> 0x%llX | ABM_GETTASKBARPOS -> %llu, edge %lu rect %ld,%ld %ldx%ld",
        static_cast<unsigned long long>(state), static_cast<unsigned long long>(got),
        static_cast<unsigned long>(position.uEdge), position.rc.left, position.rc.top,
        position.rc.right - position.rc.left, position.rc.bottom - position.rc.top);

    // The ask: move the SHELL's taskbar by x,y. hWnd has to name the appbar whose position is being set,
    // and for the shell's taskbar that is the taskbar's own window.
    APPBARDATA move = {};
    move.cbSize = sizeof(move);
    move.hWnd = taskbar;
    move.uEdge = position.uEdge ? position.uEdge : ABE_TOP;
    move.rc.left = before.left + x;
    move.rc.top = before.top + y;
    move.rc.right = before.right + x;
    move.rc.bottom = before.bottom + y;
    const UINT_PTR moved = SHAppBarMessage(ABM_SETPOS, &move);
    Log(L"APPBAR: ABM_SETPOS asked for %ld,%ld -- granted %llu, rect now %ld,%ld %ldx%ld",
        move.rc.left, move.rc.top, static_cast<unsigned long long>(moved), move.rc.left, move.rc.top,
        move.rc.right - move.rc.left, move.rc.bottom - move.rc.top);

    for (int i = 0; i < seconds * 4; ++i) {
        Sleep(250);
        RECT now = {};
        GetWindowRect(taskbar, &now);
        if (i == 0 || i == 3) {
            Log(L"APPBAR: after %d ms the taskbar is at %ld,%ld %ldx%ld", (i + 1) * 250, now.left,
                now.top, now.right - now.left, now.bottom - now.top);
        }
    }

    APPBARDATA restore = {};
    restore.cbSize = sizeof(restore);
    restore.hWnd = taskbar;
    restore.uEdge = move.uEdge;
    restore.rc = before;
    SHAppBarMessage(ABM_SETPOS, &restore);
    RECT after = {};
    GetWindowRect(taskbar, &after);
    Log(L"APPBAR: asked for the original rectangle back -- taskbar is at %ld,%ld %ldx%ld (was %ld,%ld)",
        after.left, after.top, after.right - after.left, after.bottom - after.top, before.left,
        before.top);
    return 0;
}

int main(int argc, char** argv) {
    OpenLog();
    if (argc < 2) {
        Log(L"usage: probe-tray-lab --host <seconds> | --client <add|modify|hold> | --crop on|off|status | --reparent");
        return 2;
    }
    if (std::strcmp(argv[1], "--host") == 0) {
        const int seconds = (argc > 2) ? std::atoi(argv[2]) : 20;
        return RunHost(seconds > 0 ? seconds : 20);
    }
    if (std::strcmp(argv[1], "--client") == 0) {
        return RunClient(argc > 2 ? argv[2] : "add");
    }
    if (std::strcmp(argv[1], "--crop") == 0) {
        return RunCrop(argc > 2 ? argv[2] : "status");
    }
    if (std::strcmp(argv[1], "--reparent") == 0) {
        return RunReparent();
    }
    if (std::strcmp(argv[1], "--move-tray") == 0) {
        const int seconds = (argc > 2) ? std::atoi(argv[2]) : 15;
        return RunMoveTray(seconds > 0 ? seconds : 15);
    }
    if (std::strcmp(argv[1], "--park") == 0 && argc > 3) {
        const int x = std::atoi(argv[2]);
        const int y = std::atoi(argv[3]);
        const int seconds = (argc > 4) ? std::atoi(argv[4]) : 12;
        return RunPark(x, y, seconds > 0 ? seconds : 12);
    }
    if (std::strcmp(argv[1], "--uia") == 0) {
        if (argc > 3 && std::strcmp(argv[2], "invoke") == 0) {
            wchar_t wanted[128] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, wanted, 128);
            return RunUia(wanted);
        }
        return RunUia(nullptr);
    }
    if (std::strcmp(argv[1], "--traycom") == 0) {
        const int seconds = (argc > 3 && std::strcmp(argv[2], "watch") == 0) ? std::atoi(argv[3]) : 0;
        return RunTrayCom(seconds);
    }
    if (std::strcmp(argv[1], "--traynotify") == 0) {
        const int seconds = (argc > 2) ? std::atoi(argv[2]) : 20;
        return RunTrayNotify(seconds > 0 ? seconds : 20);
    }
    if (std::strcmp(argv[1], "--traywindow") == 0) {
        const int seconds = (argc > 2) ? std::atoi(argv[2]) : 30;
        const int x = (argc > 3) ? std::atoi(argv[3]) : 1180;
        const int y = (argc > 4) ? std::atoi(argv[4]) : 60;
        return RunTrayWindow(seconds > 0 ? seconds : 30, x, y);
    }
    if (std::strcmp(argv[1], "--traysample") == 0) {
        const int seconds = (argc > 2) ? std::atoi(argv[2]) : 0;   // 0 = stay until it is closed
        const int x = (argc > 3) ? std::atoi(argv[3]) : 1700;
        const int y = (argc > 4) ? std::atoi(argv[4]) : 6;
        return RunTraySample(seconds, x, y);
    }
    if (std::strcmp(argv[1], "--rightclick") == 0 && argc > 2) {
        // THE WIDE COMMAND LINE, not argv: an application's name picked off the screen is almost always
        // non-ASCII, and on this machine argv arrives in the ANSI codepage (936) -- widening it as UTF-8 is
        // how `微信` became '΢��' and matched nothing. This project has paid for this exact trap twice.
        wchar_t wanted[128] = {};
        int wideCount = 0;
        LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        if (wideArgs && wideCount > 2) {
            wcsncpy_s(wanted, wideArgs[2], _TRUNCATE);
        }
        if (wideArgs) LocalFree(wideArgs);
        const bool sendInput = (argc > 3) && (std::strcmp(argv[3], "send") == 0 ||
                                              std::strcmp(argv[3], "sendvisible") == 0);
        const bool keepPanelVisible = (argc > 3) && std::strcmp(argv[3], "sendvisible") == 0;
        const bool byKeyboard = (argc > 3) && std::strcmp(argv[3], "keys") == 0;
        return RunRightClick(wanted, sendInput, keepPanelVisible, byKeyboard);
    }
    if (std::strcmp(argv[1], "--observe") == 0) {
        const int seconds = (argc > 2) ? std::atoi(argv[2]) : 20;
        const bool broadcast = (argc > 3) && std::strcmp(argv[3], "broadcast") == 0;
        return RunObserve(seconds > 0 ? seconds : 20, broadcast);
    }
    if (std::strcmp(argv[1], "--sendright") == 0 && argc > 4) {
        wchar_t wanted[128] = {};
        int wideCount = 0;
        LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        if (wideArgs && wideCount > 2) wcsncpy_s(wanted, wideArgs[2], _TRUNCATE);
        if (wideArgs) LocalFree(wideArgs);
        const int x = std::atoi(argv[3]);
        const int y = std::atoi(argv[4]);
        std::wstring mode = L"v4";
        if (argc > 5) {
            wchar_t wideMode[32] = {};
            MultiByteToWideChar(CP_ACP, 0, argv[5], -1, wideMode, 32);
            mode = wideMode;
        }
        return RunSendRight(wanted, x, y, mode);
    }
    if (std::strcmp(argv[1], "--traylive") == 0) {
        // The demo: our cells, the applications' own clicks and menus. Placed below the native taskbar by
        // default so that both are visible and can be compared side by side.
        const int x = (argc > 2) ? std::atoi(argv[2]) : 1080;
        const int y = (argc > 3) ? std::atoi(argv[3]) : 64;
        return RunTrayLive(x, y);
    }
    if (std::strcmp(argv[1], "--dump") == 0 && argc > 2) {
        wchar_t wanted[128] = {};
        int wideCount = 0;
        LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        if (wideArgs && wideCount > 2) wcsncpy_s(wanted, wideArgs[2], _TRUNCATE);
        if (wideArgs) LocalFree(wideArgs);
        return RunTrayMessageDump(wanted);
    }
    if (std::strcmp(argv[1], "--cover") == 0 && argc > 5) {
        const int x = std::atoi(argv[2]);
        const int y = std::atoi(argv[3]);
        const int w = std::atoi(argv[4]);
        const int h = std::atoi(argv[5]);
        const int seconds = (argc > 6) ? std::atoi(argv[6]) : 10;
        return RunCover(x, y, w, h, seconds > 0 ? seconds : 10);
    }
    if (std::strcmp(argv[1], "--thumbdump") == 0 && argc > 5) {
        const int x = std::atoi(argv[2]);
        const int y = std::atoi(argv[3]);
        const int w = std::atoi(argv[4]);
        const int h = std::atoi(argv[5]);
        const bool clip = (argc > 6) && std::strcmp(argv[6], "clip") == 0;
        return RunThumbDump(x, y, w, h, clip);
    }
    if (std::strcmp(argv[1], "--appbar") == 0 && argc > 3) {
        const int x = std::atoi(argv[2]);
        const int y = std::atoi(argv[3]);
        const int seconds = (argc > 4) ? std::atoi(argv[4]) : 3;
        return RunAppBarMove(x, y, seconds > 0 ? seconds : 3);
    }
    Log(L"unknown command");
    return 2;
}
