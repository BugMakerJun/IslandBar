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
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

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

    HRGN region = CreateRectRgn(trayRect.left - whole.left, trayRect.top - whole.top,
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

} // namespace

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
    Log(L"unknown command");
    return 2;
}
