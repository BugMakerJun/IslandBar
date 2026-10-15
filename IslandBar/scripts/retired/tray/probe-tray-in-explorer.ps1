# =====================================================================
#  probe-tray-in-explorer.ps1 -- can a module of OURS be loaded inside explorer?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-in-explorer.ps1
#
#  WHY THIS EXISTS
#
#  A press is a posted callback for one application (WeChat) and a real click -- which moves the pointer --
#  for every other one, because the callback message has to be LEARNED per application. The user is right
#  that this does not scale, and the review agrees that the only uniform answer is the one MyDockFinder
#  uses: a little code inside explorer, where the shell's own tray state lives (HANDOFF 8.106).
#
#  That is a version-adapted route, so it is proven in steps. THIS IS STEP ONE, and it asks the smallest
#  question of all: can this project get its own module into explorer.exe, and can that module write down
#  what it sees from the inside? The module is inert -- its hook procedure passes every message on and looks
#  at none of them (see probe-tray-in-explorer-hook.cpp).
#
#  HOW: a WH_GETMESSAGE hook installed on explorer's UI THREAD for about a second, then removed. A thread
#  hook loads the module into exactly that process, which is why this does not need a global hook, does not
#  need administrator, and does not touch anything for longer than the wait below.
#
#  WHAT THE ANSWER LOOKS LIKE: %USERPROFILE%\IslandBar\tray_probe_in_explorer.txt gains lines whose module
#  path is ...\explorer.exe. If it says island_bar.exe or powershell.exe instead, the hook did not take.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [string]$Dll = "$PSScriptRoot\probe-tray-in-explorer-hook.dll",
    [int]$Seconds = 2
)

$ErrorActionPreference = 'Continue'

Add-Type -Namespace InExplorer -Name Win -MemberDefinition @'
public delegate IntPtr HookProc(int code, IntPtr wparam, IntPtr lparam);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern IntPtr SetWindowsHookEx(int id, HookProc proc, IntPtr module, uint threadId);
[DllImport("user32.dll", SetLastError=true)] public static extern bool UnhookWindowsHookEx(IntPtr hook);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibrary(string path);
'@

if (-not (Test-Path $Dll)) {
    "no hook module at $Dll"
    "build it with:  cl /nologo /EHsc /LD /Fe:probe-tray-in-explorer-hook.dll probe-tray-in-explorer-hook.cpp user32.lib"
    exit 1
}

$tray = [InExplorer.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }
$pid2 = 0
$thread = [InExplorer.Win]::GetWindowThreadProcessId($tray, [ref]$pid2)
"the shell's taskbar is hwnd $tray, owned by pid $pid2, its UI thread is $thread"
"this shell's own thread is $([InExplorer.Win]::GetCurrentThreadId())"

$module = [InExplorer.Win]::LoadLibrary($Dll)
if ($module -eq [IntPtr]::Zero) { "LoadLibrary failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }

# WH_GETMESSAGE = 3.
$proc = [InExplorer.Win+HookProc]{ param($code, $wparam, $lparam) return [IntPtr]::Zero }
$hook = [InExplorer.Win]::SetWindowsHookEx(3, $proc, $module, $thread)
"SetWindowsHookEx -> $hook (error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
if ($hook -eq [IntPtr]::Zero) {
    'the hook was refused: a module of ours cannot be placed in that thread this way'
    exit 1
}

Start-Sleep -Seconds $Seconds
[void][InExplorer.Win]::UnhookWindowsHookEx($hook)
"hook removed after $Seconds s"

$report = "$env:USERPROFILE\IslandBar\tray_probe_in_explorer.txt"
if (Test-Path $report) {
    "--- $report ---"
    Get-Content $report | Select-Object -Last 20
} else {
    "no report file -- the module was loaded but wrote nothing (check the path it writes to)"
}
'=== done ==='
