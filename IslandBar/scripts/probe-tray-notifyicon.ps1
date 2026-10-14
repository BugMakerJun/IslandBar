# =====================================================================
#  probe-tray-notifyicon.ps1 -- can we learn an application's tray callback from INSIDE it?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-notifyicon.ps1
#      ... -Target QQMusic -Seconds 40          (a real application instead of the registrar)
#      ... -Target explorer -NoBroadcast        (leave it running, wait for something to happen)
#
#  WHY THIS EXISTS
#
#  A press is invisible only when it is a POSTED CALLBACK, and that needs the icon's owning window, its
#  callback message number and its uid. Learning the message number by hand, one application at a time, is
#  what the user called out (HANDOFF 8.106); reading them out of Explorer's memory turned out to be a heap
#  lottery (12 entries, then 8, then no WeChat entry at all, and eight copies for one Task Manager icon).
#
#  So this asks the question the other way round: be inside the application when it CALLS the documented API
#  that registers the icon, and read the structure it passes. That is MyDockFinder's own mechanism -- its
#  dockmod64.dll is loaded into explorer and into the applications.
#
#  HOW: SetWindowsHookEx(WH_GETMESSAGE) with a THREAD id loads probe-tray-notifyicon-hook.dll into exactly
#  that process (no global hook, no administrator). The module then redirects the process's own import slots
#  for Shell_NotifyIconW/A and writes every call to
#
#      %USERPROFILE%\IslandBar\tray_notify_calls.txt
#
#  WHAT EACH ANSWER MEANS
#
#      "attached pid N exe X"     the module is inside that process -- injection worked
#      "hooked ... N slot(s)"     N import slots were redirected; 0 means the process does NOT import the
#                                 function statically (it may resolve it by hand, which this cannot see)
#      "call W NIM_ADD ... msg"   the prize: window, uid, callback message, version, tooltip
#
#  SIDE EFFECT TO KNOW ABOUT: the module stays in the target process until that process exits, so the target
#  keeps logging its tray calls. Restart the application to be rid of it.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [string]$Target = 'registrar',
    [int]$ProcessId = 0,
    [int]$Seconds = 25,
    [int]$Delay = 8,
    [switch]$NoBroadcast
)

$ErrorActionPreference = 'Continue'

$hookDll = Join-Path $PSScriptRoot 'probe-tray-notifyicon-hook.dll'
$registrar = Join-Path $PSScriptRoot 'probe-tray-registrar.exe'
$report = Join-Path $env:USERPROFILE 'IslandBar\tray_notify_calls.txt'

if (-not (Test-Path $hookDll)) {
    "no hook module at $hookDll"
    "build it with:  cl /nologo /EHsc /LD /Fe:probe-tray-notifyicon-hook.dll probe-tray-notifyicon-hook.cpp user32.lib shell32.lib"
    exit 1
}

Add-Type -Namespace NotifyIconProbe -Name Win -MemberDefinition @'
[DllImport("user32.dll", SetLastError=true)]
public static extern IntPtr SetWindowsHookEx(int id, IntPtr proc, IntPtr module, uint threadId);
[DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)]
public static extern IntPtr GetProcAddress(IntPtr module, string name);
[DllImport("user32.dll", SetLastError=true)] public static extern bool UnhookWindowsHookEx(IntPtr hook);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibrary(string path);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int RegisterWindowMessage(string message);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
'@

$linesBefore = 0
if (Test-Path $report) { $linesBefore = (Get-Content $report | Measure-Object -Line).Lines }

# --- the target ------------------------------------------------------
$child = $null
if ($Target -eq 'registrar') {
    if (-not (Test-Path $registrar)) {
        "no registrar at $registrar (build probe-tray-registrar.cpp)"
        exit 1
    }
    $child = Start-Process -FilePath $registrar -ArgumentList '--delay', $Delay -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $process = Get-Process -Id $child.Id
} else {
    # AN APPLICATION CAN BE SEVERAL PROCESSES, and the tray icon usually belongs to ONE of them. WeChat runs
    # five here and the icon's window lives in the heaviest one; hooking the first by name hooked an idle
    # helper and saw nothing at all. -ProcessId says which one to take (not -Pid: that name is taken).
    if ($ProcessId -ne 0) {
        $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    } else {
        $candidates = Get-Process -Name $Target -ErrorAction SilentlyContinue
        $process = $candidates | Sort-Object -Property WorkingSet64 -Descending | Select-Object -First 1
        if ($candidates.Count -gt 1) {
            "note: $($candidates.Count) processes named '$Target' (pids $(($candidates.Id) -join ', ')); taking " +
            "the largest ($($process.Id)). Use -Pid to choose."
        }
    }
}
if (-not $process) {
    "no process named '$Target'"
    exit 1
}
"target: $($process.ProcessName) pid $($process.Id), $($process.Threads.Count) thread(s)"

$module = [NotifyIconProbe.Win]::LoadLibrary($hookDll)
if ($module -eq [IntPtr]::Zero) {
    "LoadLibrary failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    exit 1
}
# THE HOOK PROCEDURE MUST LIVE IN THE DLL. A delegate declared here would sit in powershell.exe, and for a
# thread in ANOTHER process the system insists the procedure address be inside the module it was given -- with
# a foreign address the hook is simply never called (measured: the module attached and then nothing happened).
$proc = [NotifyIconProbe.Win]::GetProcAddress($module, 'TrayNotifyIconHookProc')
if ($proc -eq [IntPtr]::Zero) {
    "the DLL does not export TrayNotifyIconHookProc"
    exit 1
}

# WH_GETMESSAGE = 3, one hook per thread: a thread hook loads the module into that process only.
$hooks = @()
$n = 0
foreach ($thread in $process.Threads) {
    $hook = [NotifyIconProbe.Win]::SetWindowsHookEx(3, $proc, $module, [uint32]$thread.Id)
    if ($hook -ne [IntPtr]::Zero) { $hooks += $hook; $n++ }
}
"hooks installed on $n thread(s)"
if ($n -eq 0) {
    "SetWindowsHookEx was refused (error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    "a higher integrity level than this shell cannot be reached this way"
    exit 1
}

Start-Sleep -Seconds 2

if (-not $NoBroadcast -and $Target -ne 'registrar') {
    # The standard signal every tray application listens for: it re-adds its icon, which is the call this
    # probe is waiting to see. Without it, an application that registered its icon hours ago has no reason to
    # call Shell_NotifyIcon again while we watch.
    $taskbarCreated = [NotifyIconProbe.Win]::RegisterWindowMessage('TaskbarCreated')
    $result = [IntPtr]::Zero
    [void][NotifyIconProbe.Win]::SendMessageTimeout([IntPtr]0xFFFF, [uint32]$taskbarCreated, [IntPtr]::Zero,
                                                    [IntPtr]::Zero, 0x0002, 2000, [ref]$result)
    "broadcast TaskbarCreated (0x{0:X4})" -f $taskbarCreated
}

"watching for $Seconds s ..."
Start-Sleep -Seconds $Seconds

foreach ($hook in $hooks) { [void][NotifyIconProbe.Win]::UnhookWindowsHookEx($hook) }
"hooks removed"

if (Test-Path $report) {
    $all = Get-Content $report
    $new = if ($all.Count -gt $linesBefore) { $all[$linesBefore..($all.Count - 1)] } else { @() }
    "--- $report : $($new.Count) new line(s) ---"
    $new | ForEach-Object { $_ }
    $calls = $new | Where-Object { $_ -match 'call [WA] NIM_' }
    "verdict: $($calls.Count) Shell_NotifyIcon call(s) captured in $($process.ProcessName)"
} else {
    "no report file at $report -- the module never ran in the target"
}

if ($child -and -not $child.HasExited) { Stop-Process -Id $child.Id -Force }
'=== done ==='
