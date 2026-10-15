# =====================================================================
#  measure-tray-panel.ps1 -- HOW LONG is the shell's overflow panel visible during a tray press?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\measure-tray-panel.ps1
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\measure-tray-panel.ps1 -Cell 5 -Right -Seconds 2
#
#  WHY THIS EXISTS
#
#  The acceptance criterion for the press is "the native tray must not visibly flash", and that is a question
#  about PIXELS, so this reads them: it runs the press and samples the panel's own area off the screen for
#  the whole of it, reporting every sample that is not the wallpaper. Two measured facts about that area:
#
#    * the wallpaper reads about 42 (of 255) on this machine's current desktop, and the panel painted reads
#      about 109 -- the panel is BRIGHTER than what is behind it here, which is the opposite of the note in
#      HANDOFF 8.79 (where the panel read 70 and the wallpaper 190), so a threshold taken from that note
#      would silently measure nothing;
#    * the strip BELOW the panel's icons (y 171..184) is not the panel's painted area: the panel's pixels
#      only reach y 172, and a strip just outside it reads wallpaper through the whole press even while the
#      panel's upper half is painted. That is how the painted episode was told apart from the icon tooltip
#      the shell pops up in the same neighbourhood.
#
#  IT MUST RUN OUTSIDE THE REPOSITORY. Anything started from under C:\PROJECT\Taskbar is lowered to LOW
#  integrity, UIPI then discards every synthetic input aimed at Explorer, and the press does nothing at all
#  -- see AGENTS.md. The default -Exe below is the deployed copy for that reason.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [int]$Cell = 5,
    [switch]$Right,
    [int]$Seconds = 2,
    [string]$Exe = "$env:USERPROFILE\IslandBar\island_bar.exe",
    # The panel's own rectangle as the shell reports it on this machine: 2317,32 234x154.
    [int]$PanelX = 2317,
    [int]$PanelY = 32,
    [int]$PanelW = 234,
    [int]$PanelH = 154
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path -LiteralPath $Exe)) {
    "no exe at $Exe -- deploy the bar there first (see AGENTS.md)"; exit 1
}

# The sample grid: every 4th pixel of the panel's own rectangle.
$step = 4

function Measure-Mean([System.Drawing.Bitmap]$bmp) {
    $sum = 0.0
    $n = 0
    for ($y = 0; $y -lt $bmp.Height; $y += $step) {
        for ($x = 0; $x -lt $bmp.Width; $x += $step) {
            $c = $bmp.GetPixel($x, $y)
            $sum += ($c.R * 0.299 + $c.G * 0.587 + $c.B * 0.114)
            $n++
        }
    }
    if ($n -eq 0) { return 0.0 }
    return $sum / $n
}

$bmp = New-Object System.Drawing.Bitmap $PanelW, $PanelH
$g = [System.Drawing.Graphics]::FromImage($bmp)

# The wallpaper, measured before anything is started: three reads, averaged.
$before = @()
for ($i = 0; $i -lt 3; $i++) {
    $g.CopyFromScreen($PanelX, $PanelY, 0, 0, (New-Object System.Drawing.Size $PanelW, $PanelH))
    $before += (Measure-Mean $bmp)
    Start-Sleep -Milliseconds 40
}
$baseline = ($before | Measure-Object -Average).Average
"the panel's own area reads $([math]::Round($baseline, 1)) before the press"

$arguments = @('--tray-press', "$Cell", '--fire')
if ($Right) { $arguments += '--right' }
$press = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru

$watch = [System.Diagnostics.Stopwatch]::StartNew()
$painted = @()
$samples = 0
while ($watch.ElapsedMilliseconds -lt ($Seconds * 1000)) {
    $g.CopyFromScreen($PanelX, $PanelY, 0, 0, (New-Object System.Drawing.Size $PanelW, $PanelH))
    $mean = Measure-Mean $bmp
    $samples++
    # 20 above the wallpaper: a painted panel is about 65 above it on this machine, and the shell's tooltip
    # moves it by less than that over this area.
    if ($mean -gt ($baseline + 20)) {
        $painted += [pscustomobject]@{ ms = $watch.ElapsedMilliseconds; mean = [math]::Round($mean, 1) }
    }
}
$g.Dispose()
$bmp.Dispose()

"$samples samples in $Seconds s; painted samples: $($painted.Count)"
foreach ($p in $painted) { "  painted at $($p.ms) ms, mean $($p.mean)" }
if ($painted.Count -eq 0) {
    "RESULT: the panel was never on screen during the press"
} else {
    $first = ($painted | Select-Object -First 1).ms
    $last = ($painted | Select-Object -Last 1).ms
    "RESULT: the panel was painted between about $first and $last ms -- roughly $($last - $first + 15) ms of a whole press"
}
"=== done ==="
