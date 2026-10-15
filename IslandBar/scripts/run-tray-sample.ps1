# =====================================================================
#  run-tray-sample.ps1 -- show the tray sample
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-tray-sample.ps1
#
#  WHAT YOU SEE: a strip drawn by this project, one cell per icon, with the REAL icons of the
#  applications the shell is showing in its overflow area. A left click on a cell is handed to the
#  shell (it performs that icon's own left click -- the pointer does not move); a right click opens a
#  menu of ours; Esc, or the menu's last entry, closes it.
#
#  WHY THE BAR HAS TO GO FIRST. The bar hides the shell's taskbar, and a taskbar hidden with SW_HIDE
#  leaves the automation tree -- which is where this sample reads the icons from. So the bar is closed
#  for the length of the sample and started again afterwards. (Measured: clipping the taskbar to one
#  pixel keeps the tree alive; hiding it does not. See HANDOFF 8.135.)
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Stop'

$barDir = Join-Path $env:USERPROFILE 'IslandBar'
$barExe = Join-Path $barDir 'island_bar.exe'
$sampleExe = Join-Path $barDir 'traylab\probe-tray-lab.exe'

if (-not (Test-Path $sampleExe)) {
    throw "the sample is not built yet. Build it first: powershell -File .\scripts\probe-tray-lab.ps1"
}

$bar = Get-Process island_bar -ErrorAction SilentlyContinue
if ($bar) {
    Write-Host "[sample] closing the bar (it hides the taskbar, and the sample needs the shell's tray alive)"
    $null = $bar.CloseMainWindow()
    Start-Sleep -Seconds 4
    if (Get-Process island_bar -ErrorAction SilentlyContinue) {
        Write-Host "[sample] the bar did not close by itself -- stopping it"
        Stop-Process -Name island_bar -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
}

try {
    Write-Host "[sample] showing it. Left click a cell (the shell presses that icon), right click for the"
    Write-Host "[sample] menu, Esc or the menu's 'close' entry to finish."
    & $sampleExe --traysample
    Write-Host "[sample] the sample closed on its own"
} finally {
    $still = Get-Process island_bar -ErrorAction SilentlyContinue
    if (-not $still -and (Test-Path $barExe)) {
        Write-Host "[sample] starting the bar again"
        Start-Process -FilePath $barExe -WorkingDirectory $barDir
    }
}
