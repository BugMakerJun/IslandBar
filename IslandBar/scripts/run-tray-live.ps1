# =====================================================================
#  run-tray-live.ps1 -- THE CASE: a tray of our own, with the applications' own behaviour
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-tray-live.ps1
#
#  WHAT YOU SEE: a strip drawn by this project, one cell per application that has a tray icon, with the
#  applications' real icons. A LEFT click on a cell is delivered as that application's own left click (the
#  pointer does not move); a RIGHT click is delivered as that application's own right click, so the
#  application's OWN menu opens under the cell -- including the entries only the application has, such as
#  "exit" in a chat client. Escape closes the strip.
#
#  WHY THIS IS NOT THE OLD SAMPLE. The old one read icons from the automation tree and could only fake a
#  right click by flashing the shell's tray on screen. This one reads each application's registration
#  (window, uid, callback message, protocol version) out of the shell, so a right click is the real thing:
#  no pointer movement, no flicker, no per-application learning. Measured on 2026-10-15/16; the numbers are
#  in HANDOFF.md 8.140.
#
#  THE STRIP SITS BELOW THE NATIVE TASKBAR ON PURPOSE: both are visible, so the two can be compared.
#  The bar (island_bar.exe) hides the taskbar, so it is closed for the length of the demo and started again
#  afterwards. The native taskbar is left exactly as the demo found it -- this one does not touch it.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [int]$X = 1080,
    [int]$Y = 64
)

$ErrorActionPreference = 'Stop'

$barDir = Join-Path $env:USERPROFILE 'IslandBar'
$barExe = Join-Path $barDir 'island_bar.exe'
$probeExe = Join-Path $barDir 'traylab\probe-tray-lab.exe'

if (-not (Test-Path $probeExe)) {
    throw "the probe is not built yet. Build it first: powershell -File .\scripts\probe-tray-lab.ps1"
}

# Another copy cannot run at the same time: the new one would overwrite nothing but the file is locked
# while it runs, and two strips on screen would be confusing.
Get-Process probe-tray-lab -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "[live] closing an already running probe"
    $null = $_.CloseMainWindow()
    Start-Sleep -Milliseconds 600
    if (-not $_.HasExited) { Stop-Process -Id $_.Id -Force }
}

$bar = Get-Process island_bar -ErrorAction SilentlyContinue
if ($bar) {
    Write-Host "[live] closing the bar (it hides the taskbar; the demo is easier to read without it)"
    $null = $bar.CloseMainWindow()
    Start-Sleep -Seconds 4
    if (Get-Process island_bar -ErrorAction SilentlyContinue) {
        Write-Host "[live] the bar did not close by itself -- stopping it"
        Stop-Process -Name island_bar -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
}

try {
    Write-Host "[live] showing the strip. LEFT click = that application's own left click,"
    Write-Host "[live] RIGHT click = that application's own menu (try the chat client's 'exit'),"
    Write-Host "[live] Escape closes it."
    & $probeExe --traylive $X $Y
    Write-Host "[live] the strip closed on its own"
} finally {
    $still = Get-Process island_bar -ErrorAction SilentlyContinue
    if (-not $still -and (Test-Path $barExe)) {
        Write-Host "[live] starting the bar again"
        Start-Process -FilePath $barExe -WorkingDirectory $barDir
    }
}
