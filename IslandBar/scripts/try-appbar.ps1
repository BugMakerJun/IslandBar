# =====================================================================
#  try-appbar.ps1 -- claim the top strip as an AppBar, with a way back
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\try-appbar.ps1
#
#  WHAT THIS DOES
#
#  Runs island_bar with the native taskbar hidden and the top strip claimed as an
#  AppBar. That is what makes maximized windows stop covering the bar -- the
#  difference between a taskbar replacement and a bar floating over the desktop.
#
#  WHY IT NEEDS ITS OWN SCRIPT
#
#  Claiming the strip rewrites the desktop's WORK AREA, and every maximized window
#  follows it. An earlier attempt at this moved every window on the machine to the
#  bottom of the screen and reported no error anywhere. The code now verifies the
#  result and gives the space back when it does not land where it was asked to,
#  but the operation is not something to run casually -- so it runs under a
#  timeout, and the recovery command is printed before it starts rather than after
#  something goes wrong.
#
#  MUST BE RUN FROM YOUR OWN SHELL. From a sandbox, a service or an IDE the
#  request to hide the taskbar is silently dropped by Windows (UIPI), the bar ends
#  up floating over the real taskbar, and nothing says why.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [int]$Seconds = 45,
    [switch]$LeaveTaskbarHidden
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Exe = Join-Path $Root 'build\Debug\src\Debug\island_bar.exe'
$Restore = Join-Path $Root 'scripts\restore-desktop.ps1'

if (-not (Test-Path $Exe)) {
    Write-Host "[appbar] no executable at $Exe" -ForegroundColor Red
    Write-Host '[appbar] build it first: scripts\build.ps1'
    exit 1
}

# Refuse to be misleading about which build is running.
$exeTime = (Get-Item $Exe).LastWriteTime
$newest = Get-ChildItem (Join-Path $Root 'src') -Recurse -Include '*.cpp', '*.h' |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newest -and $newest.LastWriteTime -gt $exeTime) {
    Write-Host "[appbar] STALE: $($newest.Name) is newer than the executable." -ForegroundColor Red
    Write-Host '[appbar]        Build first, or this tests the old code.'
    exit 1
}

# Say how to get out BEFORE going in.
Write-Host '=== IslandBar: claiming the top strip ===' -ForegroundColor Cyan
Write-Host ''
Write-Host '  The desktop work area is about to change so maximized windows stop at the'
Write-Host '  bar. The bar releases it on exit, and this script forces a release when the'
Write-Host '  time runs out.'
Write-Host ''
Write-Host '  IF THE DESKTOP LOOKS WRONG, run:' -ForegroundColor Yellow
Write-Host "    powershell -NoProfile -ExecutionPolicy Bypass -File `"$Restore`"" -ForegroundColor Yellow
Write-Host ''
Write-Host ("  running for {0} seconds -- close the bar window when you are done" -f $Seconds)
Write-Host ''

$log = Join-Path $Root 'appbar-run.log'
Remove-Item $log -Force -ErrorAction SilentlyContinue

# --- the same measurement the watchdog uses ---------------------------
Add-Type -Namespace IslandBar -Name Work -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
[StructLayout(LayoutKind.Sequential)] public struct MONITORINFO {
  public int cbSize; public RECT monitor; public RECT work; public uint flags; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
[DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(POINT p, uint f);
[DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr h, ref MONITORINFO i);
'@

function Get-ReservedTop {
    $mi = New-Object IslandBar.Work+MONITORINFO
    $mi.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($mi)
    $pt = New-Object IslandBar.Work+POINT
    $pt.X = 0; $pt.Y = 0
    $mon = [IslandBar.Work]::MonitorFromPoint($pt, 0)
    if (-not [IslandBar.Work]::GetMonitorInfo($mon, [ref]$mi)) { return -1 }
    return $mi.work.T - $mi.monitor.T
}

$reservedAtStart = Get-ReservedTop

$p = Start-Process -FilePath $Exe -ArgumentList '--appbar' -PassThru -NoNewWindow `
                   -RedirectStandardOutput $log
if (-not $p) { throw 'the program could not be started' }

# --- watchdog: stop the moment the reservation drifts -----------------
#
# A reservation that keeps growing is the failure that pushed the whole desktop down: the bar
# re-claimed, computed its rectangle from the work area it had just changed, and took another strip.
# Every step was small and legitimate-looking, and it walked 1440 px of screen 42 px at a time.
#
# The first version of this script waited out its full timeout and only looked afterwards. Watching the
# top of the work area during the run catches the walk on its second or third step, while the desktop is
# still nearly right and before every maximized window has moved.
$deadline = (Get-Date).AddSeconds($Seconds)
$drifted = $false
$worstTop = $reservedAtStart
$settle = (Get-Date).AddSeconds(4)     # give the bar time to claim before judging

while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if ((Get-Date) -lt $settle) { continue }

    $now = Get-ReservedTop
    if ($now -gt $worstTop) { $worstTop = $now }

    # More than a bar's worth beyond the expected reservation means it is walking.
    if ($now -gt $reservedAtStart + 120) {
        $drifted = $true
        Write-Host ''
        Write-Host ("[appbar] the reservation is DRIFTING: {0} px now, {1} px before the run" -f `
                    $now, $reservedAtStart) -ForegroundColor Red
        Write-Host '[appbar] stopping the bar and giving the space back.' -ForegroundColor Red
        $p.Kill()
        Start-Sleep -Seconds 2
        break
    }
}

$forced = $false
if (-not $p.HasExited) {
    $forced = $true
    Write-Host ''
    Write-Host '[appbar] time is up; stopping the bar so it releases the strip' -ForegroundColor Yellow
    $p.Kill()
    Start-Sleep -Seconds 2
}

Write-Host ''
Write-Host ("[appbar] the bar exit{0}" -f $(if ($forced) { 'ed by timeout' } else { 'ed on its own' }))
Write-Host ("[appbar] reserved at the top: {0} px before the run, {1} px at the worst point" -f `
            $reservedAtStart, $worstTop)

# --- report what the run actually asked for and got -------------------
#
# Read from the log rather than summarised from memory: the numbers that decide
# whether the reservation is sane are written before the call that changes the
# work area, precisely because after it there is no way back.
if (Test-Path $log) {
    $lines = Get-Content $log -ErrorAction SilentlyContinue
    $interesting = $lines | Where-Object { $_ -match 'appbar:|native taskbar:|SETPOS|work area|ABM_' }

    Write-Host ''
    Write-Host '--- what the run reported ---' -ForegroundColor Cyan
    if ($interesting) {
        $interesting | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host '  (nothing about the AppBar in the output)'
        Write-Host '  the last lines were:'
        $lines | Select-Object -Last 15 | ForEach-Object { Write-Host "  $_" }
    }
}

# --- verify the desktop is not left short of a strip ------------------
#
# Uses Get-ReservedTop, defined above and the same measurement the watchdog watches: one definition, so
# the reading taken during the run and the reading taken after it cannot disagree.
$reserved = Get-ReservedTop

Write-Host ''
Write-Host ("[appbar] reserved at the top of the work area now: {0} px" -f $reserved)

# --- decide whether that reservation is OURS --------------------------
#
# A reserved strip is not by itself evidence of anything: the shell's own taskbar reserves its strip
# too, and on this machine that is 32 px at the top. The first version of this script treated any
# reservation as ours and restarted Explorer for it -- a disruptive recovery for a desktop that was
# never wrong.
#
# So the log decides. The bar states whether it registered; only when it says so is a remaining
# reservation treated as ours to give back.
$registeredByUs = $false
if (Test-Path $log) {
    $text = Get-Content $log -ErrorAction SilentlyContinue
    $registeredByUs = ($text | Where-Object { $_ -match 'appbar: registered' }).Count -gt 0
}

Write-Host ''
if ($registeredByUs) {
    Write-Host '[appbar] the bar DID register as an AppBar.' -ForegroundColor Cyan
    if ($reserved -le 0) {
        Write-Host '[appbar] and the work area is the whole screen: it released cleanly.' -ForegroundColor Green
    } elseif ($LeaveTaskbarHidden) {
        Write-Host ("[appbar] {0} px is reserved and -LeaveTaskbarHidden was passed: expected." -f $reserved)
    } else {
        Write-Host ("[appbar] {0} px is STILL reserved after exit -- giving it back." -f $reserved) -ForegroundColor Yellow
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Restore
    }
} else {
    Write-Host '[appbar] the bar did NOT register as an AppBar, so it reserved nothing.' -ForegroundColor Yellow
    if ($reserved -gt 0) {
        Write-Host ("[appbar] the {0} px reserved at the top belongs to the SHELL's taskbar, not to us." -f $reserved)
        Write-Host '[appbar] nothing to undo -- the recovery script was deliberately not run.'
    }
    Write-Host ''
    Write-Host '[appbar] why it did not register is in the lines above. The usual cause is that'
    Write-Host '[appbar] the native taskbar could not be hidden from this process, so it still'
    Write-Host '[appbar] owns the top edge. Run this from your own shell, not a sandbox or an IDE,'
    Write-Host '[appbar] and confirm with:  island_bar.exe --probe-taskbar'
}
