# =====================================================================
#  shot-notice.ps1 -- photograph the island with a notification in it
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\shot-notice.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  Starts the bar, pushes a notification through the bar's OWN pipe (a second process:
#  --notify is dispatched before the single-instance guard), waits for the card to open,
#  photographs the bar strip and stops the bar again.
#
#  The point of photographing it rather than reasoning about it: "the island widens and
#  shows the message inside" is a claim about pixels, and the previous behaviour -- a card
#  hanging under the bar -- also satisfies every geometry assertion.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$OutDir = '',
    [string]$Title = 'Build finished',
    [string]$Body = '0 errors, 3 warnings',
    [string]$App = 'DSH',
    [int]$SettleMs = 3000,
    [int]$HoldMs = 1200,
    [int]$X = 620, [int]$Y = 0, [int]$W = 1010, [int]$H = 120
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Exe = (Resolve-Path $Exe).Path

$process = $null
try {
    $process = Start-Process -FilePath $Exe -ArgumentList @('--keep-taskbar') -PassThru
    Start-Sleep -Milliseconds $SettleMs

    & $Exe --notify $Title --body $Body --app $App | ForEach-Object { Write-Output $_ }
    Start-Sleep -Milliseconds $HoldMs

    $bitmap = New-Object System.Drawing.Bitmap $W, $H
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
    $out = Join-Path $OutDir 'notice-strip.png'
    $bitmap.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    Write-Output "captured $out"
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
