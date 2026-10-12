# =====================================================================
#  toast-card-test.ps1 -- does a real Windows toast become a card?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\toast-card-test.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  The whole loop for the SECOND notification channel:
#      start the bar  ->  raise a real toast through the notification platform
#      ->  wait for the reader's 2 s poll  ->  photograph the card  ->  read the log
#      ->  stop the bar  ->  start the user's own bar again
#
#  It is a script rather than a command line because the bar must be started and stopped
#  inside one invocation: the bar is single-instance, so every other island_bar is stopped
#  first and put back at the end.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$OutDir = '',
    [string]$Title = 'IslandBar toast test',
    [string]$Body = 'A real toast, raised through the notification platform.',
    [int]$SettleMs = 3000,
    [int]$PollWaitMs = 4000,
    [int]$X = 620, [int]$Y = 0, [int]$W = 1010, [int]$H = 340,
    [string]$UserShortcut = (Join-Path $env:USERPROFILE 'IslandBar\IslandBar (replace taskbar).lnk')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Exe = (Resolve-Path $Exe).Path
$log = Join-Path (Split-Path -Parent $Exe) 'island_bar.log'
$shot = Join-Path $OutDir 'toast-card.png'

Get-Process island_bar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700

$process = $null
try {
    $process = Start-Process -FilePath $Exe -ArgumentList @('--keep-taskbar') -PassThru
    Start-Sleep -Milliseconds $SettleMs

    # A REAL toast, via the notification platform: the bar's own pipe would be the other channel.
    & (Join-Path (Split-Path -Parent $PSCommandPath) 'toast-test.ps1') -Title $Title -Body $Body |
        ForEach-Object { Write-Output $_ }

    # Longer than the reader's two-second poll, so a failure here means the READER failed rather than that the
    # photograph was early.
    Start-Sleep -Milliseconds $PollWaitMs

    $bitmap = New-Object System.Drawing.Bitmap $W, $H
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
    $bitmap.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    Write-Output "captured $shot"

    if (Test-Path $log) {
        Get-Content $log | Select-String -Pattern 'toasts:|notification: ' |
            Select-Object -First 8 | ForEach-Object { Write-Output $_.Line.Trim() }
    }
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    if (Test-Path $UserShortcut) {
        Start-Process -FilePath 'C:\Windows\explorer.exe' -ArgumentList ('"' + $UserShortcut + '"')
        Start-Sleep -Seconds 4
        $running = (Get-Process island_bar -ErrorAction SilentlyContinue | Measure-Object).Count
        Write-Output "user bar restarted: $running instance(s)"
    }
}
