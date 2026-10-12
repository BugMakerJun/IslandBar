# =====================================================================
#  shot-panel.ps1 -- one still photograph of the system panel
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\shot-panel.ps1 `
#          -Out .\captures\panel.png -Page 0
#
#  The panel is opened by the bar itself (--state expanded --system-panel) so no
#  pointer is needed, the screen is photographed once, and the bar is stopped
#  again. --keep-taskbar throughout: this is a development look, and the shell's
#  own taskbar is left alone.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    # Which page of tiles to show, by the diagnostic env setting main.cpp reads.
    [int]$Page = 0,
    [int]$X = 620, [int]$Y = 0, [int]$W = 1010, [int]$H = 340,
    [int]$DelayMs = 1600,
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$env:ISLANDBAR_PANEL_PAGE = "$Page"

$barArgs = @('--keep-taskbar', '--state', 'expanded', '--system-panel') + $ExtraArgs
$process = Start-Process -FilePath $Exe -ArgumentList $barArgs -PassThru
Start-Sleep -Milliseconds $DelayMs

try {
    $bitmap = New-Object System.Drawing.Bitmap $W, $H
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    Write-Output "captured $Out"
}
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Remove-Item Env:\ISLANDBAR_PANEL_PAGE -ErrorAction SilentlyContinue
}
