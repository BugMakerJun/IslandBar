# =====================================================================
#  restore-desktop.ps1 -- put the work area back if the bar left it wrong
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File C:\PROJECT\Taskbar\IslandBar\scripts\restore-desktop.ps1
#
#  Why this exists
#  ---------------
#  Registering as an AppBar rewrites the desktop's WORK AREA, and every maximized
#  window follows it. When that goes wrong the symptom is not an error message: it
#  is every window on the machine piled at the bottom of the screen, which is
#  alarming and gives no clue about what caused it. Recovery has to be possible
#  without understanding any of that.
#
#  Run this if the desktop looks wrong after running island_bar WITHOUT
#  --keep-taskbar. It is safe to run at any time; it only ever restores the work
#  area to the full screen and restarts the shell's taskbar.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [switch]$NoExplorerRestart
)

$ErrorActionPreference = 'Continue'

Write-Host '=== restoring the desktop work area ===' -ForegroundColor Cyan
Write-Host ''

# --- 1. kill any island_bar that is still holding a reservation ------
$running = Get-Process island_bar -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[1/3] stopping $($running.Count) island_bar process(es)"
    $running | ForEach-Object { $_.Kill() }
    Start-Sleep -Seconds 2
} else {
    Write-Host '[1/3] no island_bar is running'
}

# --- 2. force the work area back to the full screen ------------------
#
# SPI_SETWORKAREA is the documented way to say "the work area is this". It is set
# per session, so it applies to everything on screen. The full screen rectangle is
# the right answer here: if a taskbar belongs on screen, Explorer re-derives the
# work area from it after the restart in step 3.
Add-Type -Namespace IslandBarRecovery -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }

[DllImport("user32.dll", SetLastError = true)]
public static extern bool SystemParametersInfo(uint uiAction, uint uiParam, ref RECT pvParam, uint fWinIni);

[DllImport("user32.dll")]
public static extern int GetSystemMetrics(int nIndex);

public const uint SPI_SETWORKAREA = 0x002F;
public const uint SPIF_UPDATEINIFILE = 0x01;
public const uint SPIF_SENDCHANGE = 0x02;
public const int SM_CXSCREEN = 0;
public const int SM_CYSCREEN = 1;
'@

$screenW = [IslandBarRecovery.Native]::GetSystemMetrics([IslandBarRecovery.Native]::SM_CXSCREEN)
$screenH = [IslandBarRecovery.Native]::GetSystemMetrics([IslandBarRecovery.Native]::SM_CYSCREEN)
Write-Host "[2/3] screen is ${screenW}x${screenH}"

$rect = New-Object IslandBarRecovery.Native+RECT
$rect.Left = 0
$rect.Top = 0
$rect.Right = $screenW
$rect.Bottom = $screenH

$ok = [IslandBarRecovery.Native]::SystemParametersInfo(
    [IslandBarRecovery.Native]::SPI_SETWORKAREA, 0, [ref]$rect,
    ([IslandBarRecovery.Native]::SPIF_UPDATEINIFILE -bor [IslandBarRecovery.Native]::SPIF_SENDCHANGE))

if ($ok) {
    Write-Host "[2/3] work area set to 0,0 $screenW x $screenH"
} else {
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host "[2/3] SPI_SETWORKAREA failed (error $err)" -ForegroundColor Yellow
    Write-Host '      the Explorer restart in step 3 is then the thing that fixes it' -ForegroundColor Yellow
}

# --- 3. restart Explorer ---------------------------------------------
#
# Explorer owns the AppBar registry. Restarting it drops every registration and
# rebuilds the taskbar, which re-derives the work area from scratch -- so this is
# the step that clears a reservation whose owner is long gone.
#
# IT IS ONLY DONE WHEN IT IS NEEDED, AND IT SAYS SO FIRST.
#
# The earlier version ran it unconditionally, so a caller that had already fixed the
# work area with SPI_SETWORKAREA still had its taskbar torn down and rebuilt. Worse, it
# was reached from the trial script for a desktop that had never been wrong at all --
# a destructive recovery applied to a healthy machine, because "a strip is reserved"
# (the shell's own 32 px) was mistaken for "our strip is reserved".
#
# So: if the work area is already what it should be, there is nothing to restart for.
if ($ok) {
    Write-Host '[3/3] not needed: the work area was set directly'
} elseif ($NoExplorerRestart) {
    Write-Host '[3/3] skipped the Explorer restart (-NoExplorerRestart)'
    Write-Host '      the work area could NOT be set directly, so it may still be wrong'
} else {
    Write-Host '[3/3] the work area could not be set directly, so Explorer must be restarted'
    Write-Host '      THE TASKBAR WILL DISAPPEAR FOR A MOMENT and any open File Explorer'
    Write-Host '      windows will close.'
    taskkill /f /im explorer.exe 2>&1 | Out-Null
    Start-Sleep -Seconds 3
    Start-Process explorer.exe
    Start-Sleep -Seconds 3
    Write-Host '[3/3] Explorer restarted'
}

Write-Host ''
Write-Host 'Done. The work area is the full screen again.' -ForegroundColor Green
Write-Host ''
Write-Host 'If windows are still not where they should be, log out and back in:'
Write-Host 'the work area is per-session and a fresh session always starts correct.'
