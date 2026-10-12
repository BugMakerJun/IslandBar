# =====================================================================
#  anim-check.ps1 -- does the island WIDEN over frames, or jump?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\anim-check.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  Runs the bar with ISLANDBAR_SLOWMO so a 0.22 s widening takes about two seconds,
#  pushes a notification, photographs the bar strip several times, and measures the
#  island's WIDTH in each frame by scanning a row through its middle.
#
#  A number per frame rather than a picture: "there is no animation" and "the animation
#  is too quick to see" are different problems, and only the numbers tell them apart.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$OutDir = '',
    [double]$SlowMo = 8,
    [int]$SettleMs = 3000,
    [int]$Frames = 6,
    [int]$IntervalMs = 120,
    [int]$X = 620, [int]$Y = 0, [int]$W = 1010, [int]$H = 60,
    [string]$UserShortcut = (Join-Path $env:USERPROFILE 'IslandBar\IslandBar (replace taskbar).lnk')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Exe = (Resolve-Path $Exe).Path

Get-Process island_bar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700

$env:ISLANDBAR_SLOWMO = "$SlowMo"
$process = $null
try {
    $process = Start-Process -FilePath $Exe -ArgumentList @('--keep-taskbar') -PassThru
    Start-Sleep -Milliseconds $SettleMs

    # Straight into the pipe: the sender is a second process and never touches the mutex.
    & $Exe --notify 'Build finished' --body 'Widening test' --app 'Anim' | Out-Null

    for ($i = 1; $i -le $Frames; $i++) {
        $bitmap = New-Object System.Drawing.Bitmap $W, $H
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
        $file = Join-Path $OutDir ("anim-{0}.png" -f $i)
        $bitmap.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose()
        $bitmap.Dispose()

        # The island's width: the longest run of dark pixels through the row that contains the centre.
        $row = [int]($H / 2 - 6)
        $centre = [int]($W / 2)
        $left = $centre
        while ($left -gt 0) {
            $c = $bitmap.GetPixel($left, $row)
            if ($c.R -gt 70 -or $c.G -gt 70 -or $c.B -gt 80) { break }
            $left--
        }
        $right = $centre
        while ($right -lt $W - 1) {
            $c = $bitmap.GetPixel($right, $row)
            if ($c.R -gt 70 -or $c.G -gt 70 -or $c.B -gt 80) { break }
            $right++
        }
        Write-Output ("frame {0}: island x {1}..{2}  width {3}" -f $i, ($X + $left), ($X + $right), ($right - $left))
        Start-Sleep -Milliseconds $IntervalMs
    }
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Remove-Item Env:\ISLANDBAR_SLOWMO -ErrorAction SilentlyContinue
    if (Test-Path $UserShortcut) {
        Start-Process -FilePath 'C:\Windows\explorer.exe' -ArgumentList ('"' + $UserShortcut + '"')
        Start-Sleep -Seconds 4
        $running = (Get-Process island_bar -ErrorAction SilentlyContinue | Measure-Object).Count
        Write-Output "user bar restarted: $running instance(s)"
    }
}
