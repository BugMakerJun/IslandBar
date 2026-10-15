// =====================================================================
//  probe-tray-in-explorer-hook.cpp -- CAN THIS PROJECT GET CODE INTO EXPLORER AT ALL?
//
//  Build (once, with a normal MSVC environment):
//      cl /nologo /EHsc /LD /Fe:probe-tray-in-explorer-hook.dll probe-tray-in-explorer-hook.cpp user32.lib
//
//  Used by scripts\probe-tray-in-explorer.ps1, which installs it as a WH_GETMESSAGE hook on explorer's UI
//  thread for about a second and then removes it.
//
//  WHY, AND WHY IT IS INERT. The tray callback message is learned per application today, which the user
//  rightly called out (HANDOFF 8.106). The way to make it one single mechanism -- the way MyDockFinder does
//  it -- is to have a little code INSIDE explorer, where the shell's own tray state lives. That is a
//  version-adapted route and it has to be proven step by step, so this first step proves only the smallest
//  fact of all: that this project can get a module of its own loaded into explorer.exe and can make it write
//  down what it sees from the inside. The hook procedure does nothing but pass the message on.
//
//  WHAT IT WRITES: %USERPROFILE%\IslandBar\tray_probe_in_explorer.txt -- the time, this module's own path,
//  the process it is running in (explorer.exe is the answer being looked for), and the classes of the tray
//  windows it can see from there.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include <windows.h>
#include <stdio.h>

namespace {

void WriteReport(const wchar_t* stage) {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\IslandBar\\tray_probe_in_explorer.txt", home);

    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path, L"a, ccs=UTF-8") != 0 || !stream) return;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    fwprintf(stream, L"[%02u:%02u:%02u.%03u] %s: module loaded in '%s' (pid %lu)\n",
             now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, stage, self,
             GetCurrentProcessId());

    // The windows this process has, asked from inside it: if the module really is in explorer, these are
    // the shell's own tray windows and they answer from the inside where they belong.
    const wchar_t* classes[] = {L"Shell_TrayWnd", L"TopLevelWindowForOverflowXamlIsland", L"TrayNotifyWnd",
                                L"SysPager", L"NotifyIconOverflowWindow"};
    for (const wchar_t* cls : classes) {
        const HWND window = FindWindowW(cls, nullptr);
        if (!window) continue;
        RECT rect = {};
        GetWindowRect(window, &rect);
        fwprintf(stream, L"    %s -> hwnd %p, %ld,%ld %ldx%ld, visible %d\n", cls, window, rect.left,
                 rect.top, rect.right - rect.left, rect.bottom - rect.top, IsWindowVisible(window) ? 1 : 0);
    }
    fclose(stream);
}

} // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK TrayProbeHookProc(int code, WPARAM wparam, LPARAM lparam) {
    // INERT ON PURPOSE. This is not the observation yet -- it is the proof that a module of ours can be in
    // this process at all. It does not look at the message, and it does not change it.
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        WriteReport(L"attached");
    } else if (reason == DLL_PROCESS_DETACH) {
        WriteReport(L"detached");
    }
    return TRUE;
}
