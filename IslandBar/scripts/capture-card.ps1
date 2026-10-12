# =====================================================================
#  capture-card.ps1 -- photograph the system panel while it opens
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\capture-card.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  Why this exists
#  ---------------
#  The panel's opening animation cannot be judged from a still: both ends of it
#  look right, and the complaint is about the movement between them. This machine
#  also cannot move the pointer, so a transition cannot be performed by hand.
#
#  The bar can do both things itself, through the diagnostic environment settings
#  in main.cpp:
#
#      ISLANDBAR_PANEL_MS    open the panel after N ms, close it at 2N
#      ISLANDBAR_SLOWMO      stretch every transition's duration by this factor
#      ISLANDBAR_TRACE_CARD  one log line per frame of a card transition
#
#  So this script launches the bar with those set, photographs a strip of the
#  screen at a fixed interval while the transition runs, and tiles the frames into
#  one contact sheet -- one picture that shows the whole movement.
#
#  The bar is started with --keep-taskbar: the shell's own taskbar is left alone,
#  which is what you want while developing. It is stopped at the end.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$OutDir       = '',
    [int]   $PanelMs      = 2500,
    # 0 means "twice the opening time". A stretched transition needs longer than that, or the
    # capture records a reversal instead of the movement being looked at.
    [int]   $CloseMs      = 0,
    # >0: after the first open and close, toggle the panel every CycleMs -- pressing the control
    # repeatedly is the only way to exercise the SECOND and later transitions, and that is where the
    # "the first press is smooth and the rest have no animation" report lived.
    [int]   $CycleMs      = 0,
    [double]$SlowMo       = 6,
    [int]   $FrameCount   = 96,
    [int]   $IntervalMs   = 30,
    [int]   $StartDelayMs = 2450,
    # The strip to photograph, in screen pixels. The default covers the island,
    # its two decks and the whole card underneath them at 100% scaling.
    [int]   $X = 620, [int]$Y = 0, [int]$W = 1010, [int]$H = 330,
    # The contact sheet.
    [int]   $Columns      = 8,
    [int]   $TileWidth    = 240,
    # Extra command-line arguments for the bar, e.g. '--state','expanded','--system-panel' to
    # photograph the panel standing still instead of opening.
    [string[]] $ExtraArgs = @(),
    [switch]$KeepFrames
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Exe = (Resolve-Path $Exe).Path

# The settings are inherited by the child process, so set them here and clear them
# again afterwards: leaving them set would change every later run in this session.
$env:ISLANDBAR_PANEL_MS   = "$PanelMs"
$env:ISLANDBAR_SLOWMO     = "$SlowMo"
$env:ISLANDBAR_TRACE_CARD = '1'
if ($CloseMs -gt 0) { $env:ISLANDBAR_PANEL_CLOSE_MS = "$CloseMs" }
if ($CycleMs -gt 0) { $env:ISLANDBAR_PANEL_CYCLE_MS = "$CycleMs" }

Write-Host "[capture] exe     : $Exe"
$closeText = if ($CloseMs -gt 0) { "$CloseMs" } else { "$($PanelMs * 2)" }
Write-Host "[capture] panel   : opens at $PanelMs ms, closes at $closeText ms"
Write-Host "[capture] slowmo  : ${SlowMo}x  (a 0.36 s transition becomes $([math]::Round(0.36 * $SlowMo, 2)) s)"
Write-Host "[capture] strip   : x $X y $Y ${W}x${H}"

# Launched through the .NET process API rather than Start-Process.
#
# Start-Process failed here with "cannot be run completely because the system cannot find all the
# information required", which names neither the file nor the reason. The .NET path takes the file
# name, the arguments and the working directory separately and says what is wrong if anything is,
# and UseShellExecute=$false passes the environment (and so the three settings above) straight
# through.
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName         = $Exe
$startInfo.Arguments        = (@('--keep-taskbar') + $ExtraArgs) -join ' '
$startInfo.WorkingDirectory = Split-Path -Parent $Exe
$startInfo.UseShellExecute  = $false
$proc = [System.Diagnostics.Process]::Start($startInfo)

try {
    Start-Sleep -Milliseconds $StartDelayMs

    # Captured into memory, not to a file per frame.
    #
    # Writing each frame cost more than the sampling interval did -- the first version of this script
    # asked for a frame every 25 ms and took ~100 ms per frame, so a transition stretched to 2.16 s
    # appeared in four frames out of a hundred and twenty. The work has to be smaller than the
    # interval, or the interval is a lie.
    $size = New-Object System.Drawing.Size $W, $H
    $frames = @()
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $FrameCount; $i++) {
        $strip = New-Object System.Drawing.Bitmap $W, $H
        $graphics = [System.Drawing.Graphics]::FromImage($strip)
        $graphics.CopyFromScreen($X, $Y, 0, 0, $size)
        $graphics.Dispose()
        $frames += $strip
        if ($IntervalMs -gt 0) {
            $due = $i * $IntervalMs
            $slack = $due - $watch.ElapsedMilliseconds
            if ($slack -gt 0) { Start-Sleep -Milliseconds $slack }
        }
    }
    $watch.Stop()
    $perFrame = $watch.ElapsedMilliseconds / [double]$frames.Count
    Write-Host ("[capture] cadence : {0:N1} ms per frame over {1} frames" -f $perFrame, $frames.Count)

    # The contact sheet: every frame scaled down and labelled with its number, in
    # reading order. One picture, so the movement can be judged in one look instead
    # of sixty.
    $scale       = $TileWidth / [double]$W
    $tileHeight  = [int][math]::Round($H * $scale)
    $rows        = [int][math]::Ceiling($frames.Count / [double]$Columns)
    $labelHeight = 14
    $sheetWidth  = $Columns * $TileWidth
    $sheetHeight = $rows * ($tileHeight + $labelHeight)
    $sheet = New-Object System.Drawing.Bitmap $sheetWidth, $sheetHeight
    $sheetGraphics = [System.Drawing.Graphics]::FromImage($sheet)
    $sheetGraphics.Clear([System.Drawing.Color]::FromArgb(20, 20, 22))
    $font = New-Object System.Drawing.Font 'Consolas', 8
    $ink  = [System.Drawing.Brushes]::White

    for ($i = 0; $i -lt $frames.Count; $i++) {
        $column = $i % $Columns
        $row    = [int][math]::Floor($i / $Columns)
        $left   = $column * $TileWidth
        $top    = $row * ($tileHeight + $labelHeight)
        $sheetGraphics.DrawImage($frames[$i], $left, $top, $TileWidth, $tileHeight)
        $sheetGraphics.DrawString("$i", $font, $ink, $left + 3, $top + $tileHeight)
    }
    $sheetGraphics.Dispose()

    if ($KeepFrames) {
        for ($i = 0; $i -lt $frames.Count; $i++) {
            $frames[$i].Save((Join-Path $OutDir ("frame-{0:D3}.png" -f $i)),
                             [System.Drawing.Imaging.ImageFormat]::Png)
        }
    }
    $sheetPath = Join-Path $OutDir 'sheet.png'
    $sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    foreach ($frame in $frames) { $frame.Dispose() }

    Write-Host "[capture] sheet   : $sheetPath" -ForegroundColor Green
}
finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Remove-Item Env:\ISLANDBAR_PANEL_MS   -ErrorAction SilentlyContinue
    Remove-Item Env:\ISLANDBAR_PANEL_CLOSE_MS -ErrorAction SilentlyContinue
    Remove-Item Env:\ISLANDBAR_PANEL_CYCLE_MS -ErrorAction SilentlyContinue
    Remove-Item Env:\ISLANDBAR_SLOWMO     -ErrorAction SilentlyContinue
    Remove-Item Env:\ISLANDBAR_TRACE_CARD -ErrorAction SilentlyContinue
}

$log = Join-Path (Split-Path -Parent $Exe) 'island_bar.log'
if (Test-Path $log) { Write-Host "[capture] log     : $log" }
