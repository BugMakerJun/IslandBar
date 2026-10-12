# =====================================================================
#  dismiss-test.ps1 -- does a click outside the card put the panel away?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\dismiss-test.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  The click cannot be sent from THIS script: the bar runs at the shell's integrity
#  level and a script's synthetic input is delivered from lower down, so Windows drops
#  it before the bar's own GetAsyncKeyState can see it -- the feature looks broken when
#  it is only invisible. The bar therefore clicks FOR ITSELF (ISLANDBAR_CLICK_AT): a
#  click it sends is a click it can see.
#
#  ISLANDBAR_PANEL_LOCK is deliberately NOT set: the point is to watch the panel close.
#
#  The bar is single-instance, so every other island_bar is stopped first and the user's
#  own bar is started again at the end through its shortcut -- this script is the one
#  that borrowed the desktop.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    # Where to click, in screen pixels, and it must be OUTSIDE the bar: the card is x 853..1373 y 50..300 on
    # this monitor, so a point to its right is outside.
    [int]$ClickX = 1750,
    [int]$ClickY = 200,
    [string]$UserBarFolder = (Join-Path $env:USERPROFILE 'IslandBar'),
    [string]$UserShortcut = (Join-Path $env:USERPROFILE 'IslandBar\IslandBar (replace taskbar).lnk')
)

$ErrorActionPreference = 'Stop'
$log = Join-Path (Split-Path -Parent $Exe) 'island_bar.log'

# --- take the desktop ---
Get-Process island_bar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700

$env:ISLANDBAR_CLICK_AT = "$ClickX,$ClickY"
$process = $null
try {
    $process = Start-Process -FilePath $Exe `
        -ArgumentList @('--keep-taskbar', '--system-panel') -PassThru
    # Long enough for the card to settle and for the bar's own click at +3 s.
    Start-Sleep -Seconds 7

    if (Test-Path $log) {
        Get-Content $log | Select-String -Pattern 'will click|clicking at|press at|outside the bar|closed' |
            Select-Object -First 12 | ForEach-Object { Write-Output $_.Line.Trim() }
    } else {
        Write-Output "NO LOG at $log -- the bar did not start (another instance?)"
    }
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Remove-Item Env:\ISLANDBAR_CLICK_AT -ErrorAction SilentlyContinue

    # --- and give the desktop back ---
    if (Test-Path $UserShortcut) {
        Start-Process -FilePath 'C:\Windows\explorer.exe' -ArgumentList ('"' + $UserShortcut + '"')
        Start-Sleep -Seconds 4
        $running = (Get-Process island_bar -ErrorAction SilentlyContinue | Measure-Object).Count
        Write-Output "user bar restarted: $running instance(s)"
    }
}
