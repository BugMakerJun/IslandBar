# =====================================================================
#  shot-status-strip.ps1 -- photograph the status strip while the island is OCCUPIED
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\shot-status-strip.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  WHAT IT IS FOR. The strip -- the date, the weekday, the time and the weather in the band
#  left of the system button -- is shown ONLY while the island is occupied (music resting, or
#  a message up), because that is exactly when the pill's own line stops being the clock. So a
#  photograph of the bar at rest says nothing about it, and neither does a layout assertion.
#
#  How it makes the island occupied: it pushes one notification through the bar's OWN pipe
#  (--notify is dispatched before the single-instance guard, so a second process can send one).
#  The island widens into the notice strip, the pill shows the message instead of the clock,
#  and the strip appears beside the deck.
#
#  The bar must NOT already be running: it takes a single-instance mutex, so this run would
#  silently do nothing. Stop the user's bar first (scripts\verify-panel-anchor.ps1 -DeployOnly
#  starts it again afterwards).
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$OutDir = '',
    # The bar band to photograph, in screen pixels. The default covers the island, the deck and
    # the band the strip stands in on a 2560-wide monitor at 100% scaling.
    [int]$X = 620, [int]$Y = 0, [int]$W = 1010, [int]$H = 120,
    # The BAND the status strip occupies, in screen pixels, for the ink measurement. Measured
    # rather than eyeballed: at rest this band is desktop (bright wallpaper), and while the strip
    # is up it is the island's near-black capsule with light text in it.
    [int]$BandLeft = 820, [int]$BandRight = 1088, [int]$BandTop = 11, [int]$BandBottom = 38,
    [int]$SettleMs = 9000,
    [int]$HoldMs = 1200
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Exe = (Resolve-Path $Exe).Path

function Save-Band($path, $bitmap) {
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}

# Mean luminance of the strip's band, and how many pixels in it are dark. The two numbers
# separate the two things that can go wrong: the strip never appearing (the band stays as bright
# as the desktop) and the strip appearing empty (dark band, no text).
function Measure-Band($bitmap, $left, $top, $right, $bottom) {
    $sum = 0.0
    $count = 0
    $dark = 0
    $light = 0
    for ($y = $top; $y -le $bottom; $y += 2) {
        for ($x = $left; $x -le $right; $x += 2) {
            $c = $bitmap.GetPixel($x - $X, $y - $Y)
            $luma = ($c.R * 0.299) + ($c.G * 0.587) + ($c.B * 0.114)
            $sum += $luma
            $count++
            if ($luma -lt 60) { $dark++ }
            if ($luma -gt 150) { $light++ }
        }
    }
    if ($count -eq 0) { return @{ Mean = 0.0; Dark = 0; Light = 0; Count = 0 } }
    return @{ Mean = [math]::Round($sum / $count, 1); Dark = $dark; Light = $light; Count = $count }
}

$process = $null
try {
    $process = Start-Process -FilePath $Exe -ArgumentList @('--keep-taskbar') -PassThru
    Start-Sleep -Milliseconds $SettleMs

    # At rest: the island is the pill, the clock is in it, and the band beside the deck belongs
    # to the desktop.
    $before = New-Object System.Drawing.Bitmap $W, $H
    $graphics = [System.Drawing.Graphics]::FromImage($before)
    $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
    $graphics.Dispose()
    Save-Band (Join-Path $OutDir 'status-strip-rest.png') $before

    # One message makes the island occupied, which is what the strip is for.
    & $Exe --notify "status strip check" --body "the pill shows this instead of the clock" --app "DSH"
    Start-Sleep -Milliseconds $HoldMs

    $after = New-Object System.Drawing.Bitmap $W, $H
    $graphics = [System.Drawing.Graphics]::FromImage($after)
    $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
    $graphics.Dispose()
    Save-Band (Join-Path $OutDir 'status-strip-occupied.png') $after

    $restBand = Measure-Band $before $BandLeft $BandTop $BandRight $BandBottom
    $liveBand = Measure-Band $after $BandLeft $BandTop $BandRight $BandBottom
    $before.Dispose()
    $after.Dispose()

    Write-Output "captured $OutDir\status-strip-rest.png and status-strip-occupied.png"
    Write-Output ("band x {0}..{1} y {2}..{3}" -f $BandLeft, $BandRight, $BandTop, $BandBottom)
    Write-Output ("at rest   : mean luma {0}, dark {1}/{2}, light {3}" -f `
        $restBand.Mean, $restBand.Dark, $restBand.Count, $restBand.Light)
    Write-Output ("occupied  : mean luma {0}, dark {1}/{2}, light {3}" -f `
        $liveBand.Mean, $liveBand.Dark, $liveBand.Count, $liveBand.Light)
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
