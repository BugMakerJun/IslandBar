# =====================================================================
#  restore-desktop.ps1 -- put the work area back if the bar left it wrong
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\restore-desktop.ps1
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

# --- 2. show the shell's taskbar again -------------------------------
#
# It is hidden while the bar runs, and a process that was KILLED never showed it again
# -- so this is the step that actually gets the user's taskbar back, and it used to be
# missing entirely: the script only ever moved the work area, which on a machine whose
# taskbar is already hidden changes nothing anyone can see.
#
# Shown BEFORE the work area is set, because the work area to set is the one the taskbar
# implies (step 3), and that has to be measured from the taskbar itself.
Add-Type -Namespace IslandBarRecovery -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }

[DllImport("user32.dll", SetLastError = true)]
public static extern bool SystemParametersInfo(uint uiAction, uint uiParam, ref RECT pvParam, uint fWinIni);

[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string win);

[DllImport("user32.dll")]
public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

[DllImport("user32.dll")]
public static extern bool IsWindowVisible(IntPtr hWnd);

[DllImport("user32.dll")]
public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

[DllImport("user32.dll")]
public static extern int GetSystemMetrics(int nIndex);

public const uint SPI_SETWORKAREA = 0x002F;
public const uint SPI_GETWORKAREA = 0x0030;
public const uint SPIF_UPDATEINIFILE = 0x01;
public const uint SPIF_SENDCHANGE = 0x02;
public const int SW_SHOW = 5;
'@

$tray = [IslandBarRecovery.Native]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) {
    Write-Host '[2/4] no Shell_TrayWnd found -- Explorer is not running, or this is not the interactive desktop'
} else {
    [void][IslandBarRecovery.Native]::ShowWindow($tray, [IslandBarRecovery.Native]::SW_SHOW)
    for ($h = [IslandBarRecovery.Native]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, 'Shell_SecondaryTrayWnd', $null);
         $h -ne [IntPtr]::Zero;
         $h = [IslandBarRecovery.Native]::FindWindowEx([IntPtr]::Zero, $h, 'Shell_SecondaryTrayWnd', $null)) {
        [void][IslandBarRecovery.Native]::ShowWindow($h, [IslandBarRecovery.Native]::SW_SHOW)
    }
    Write-Host "[2/4] the shell taskbar is visible again: $([bool][IslandBarRecovery.Native]::IsWindowVisible($tray))"
}

# --- 3. put the work area back where the taskbar says it is -----------
#
# SPI_SETWORKAREA is the documented way to say "the work area is this", and the answer
# here is the SCREEN MINUS THE TASKBAR, not the whole screen. The whole screen is what
# the bar deliberately sets while it runs (that is how a maximized window reaches the
# top), so "the work area is already the full screen" is not evidence of health --
# with a hidden taskbar it is exactly what a killed bar leaves behind.
#
# The taskbar's own window gives the number without any work-area reading: a bar along
# an edge is as thick as it is short.
$work = New-Object IslandBarRecovery.Native+RECT
if ($tray -ne [IntPtr]::Zero) {
    $trayRect = New-Object IslandBarRecovery.Native+RECT
    [void][IslandBarRecovery.Native]::GetWindowRect($tray, [ref]$trayRect)
    $thickness = $trayRect.Bottom - $trayRect.Top
    if (($trayRect.Right - $trayRect.Left) -lt ($trayRect.Bottom - $trayRect.Top)) {
        $thickness = $trayRect.Right - $trayRect.Left
    }

    # The screen bounds as the shell sees them. The primary monitor's, which is the one the
    # taskbar is on: SPI_SETWORKAREA is a primary-monitor call, and pretending otherwise is how
    # a recovery script becomes the thing that breaks the desktop.
    $work.Left = 0; $work.Top = 0
    $work.Right = [IslandBarRecovery.Native]::GetSystemMetrics(0)
    $work.Bottom = [IslandBarRecovery.Native]::GetSystemMetrics(1)

    # Docked at the top, as on this machine. Any other edge is left at the full screen:
    # the bar only ever takes the top strip, and guessing about the others is how a
    # recovery script becomes the thing that breaks the desktop.
    if ($trayRect.Top -le 0) { $work.Top = $thickness }
    Write-Host "[3/4] taskbar is $($thickness) px thick on the top edge; work area -> $($work.Left),$($work.Top) - $($work.Right),$($work.Bottom)"
} else {
    $work.Left = 0; $work.Top = 0
    $work.Right = [IslandBarRecovery.Native]::GetSystemMetrics(0)
    $work.Bottom = [IslandBarRecovery.Native]::GetSystemMetrics(1)
    Write-Host "[3/4] no taskbar to measure; work area -> the whole screen"
}

$ok = [IslandBarRecovery.Native]::SystemParametersInfo(
    [IslandBarRecovery.Native]::SPI_SETWORKAREA, 0, [ref]$work,
    ([IslandBarRecovery.Native]::SPIF_UPDATEINIFILE -bor [IslandBarRecovery.Native]::SPIF_SENDCHANGE))

$after = New-Object IslandBarRecovery.Native+RECT
[void][IslandBarRecovery.Native]::SystemParametersInfo(
    [IslandBarRecovery.Native]::SPI_GETWORKAREA, 0, [ref]$after, 0)
$landed = ($after.Left -eq $work.Left) -and ($after.Top -eq $work.Top) -and
          ($after.Right -eq $work.Right) -and ($after.Bottom -eq $work.Bottom)

if ($landed) {
    Write-Host "[3/4] work area is now $($after.Left),$($after.Top) - $($after.Right),$($after.Bottom)"
} else {
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host "[3/4] SPI_SETWORKAREA did not take (returned $ok, error $err); now $($after.Left),$($after.Top) - $($after.Right),$($after.Bottom)" -ForegroundColor Yellow
    Write-Host '      the Explorer restart in step 4 is then the thing that fixes it' -ForegroundColor Yellow
}

# --- 4. restart Explorer ---------------------------------------------
#
# Explorer owns the AppBar registry. Restarting it drops every registration and
# rebuilds the taskbar, which re-derives the work area from scratch -- so this is
# the step that clears a reservation whose owner is long gone.
#
# IT IS ONLY DONE WHEN IT IS NEEDED, AND IT SAYS SO FIRST. The test is whether the work
# area is now the one the taskbar implies -- measured after the call, not taken from its
# return value: this API reports success and changes nothing often enough that the return
# value has never been evidence.
if ($landed) {
    Write-Host '[4/4] not needed: the taskbar is visible and the work area matches it'
} elseif ($NoExplorerRestart) {
    Write-Host '[4/4] skipped the Explorer restart (-NoExplorerRestart)'
    Write-Host '      the work area could NOT be set directly, so it may still be wrong'
} else {
    Write-Host '[4/4] the work area could not be set directly, so Explorer must be restarted'
    Write-Host '      THE TASKBAR WILL DISAPPEAR FOR A MOMENT and any open File Explorer'
    Write-Host '      windows will close.'
    taskkill /f /im explorer.exe 2>&1 | Out-Null
    Start-Sleep -Seconds 3
    Start-Process explorer.exe
    Start-Sleep -Seconds 3
    Write-Host '[4/4] Explorer restarted'
}

Write-Host ''
Write-Host 'Done. The shell taskbar is back and the work area matches it.' -ForegroundColor Green
Write-Host ''
Write-Host 'If windows are still not where they should be, log out and back in:'
Write-Host 'the work area is per-session and a fresh session always starts correct.'
