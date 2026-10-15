# =====================================================================
#  normalize-log.ps1 -- turn one run's log into something that can be diffed
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\regression\normalize-log.ps1
#      ... -Name startup                 # writes out\startup.txt and compares it with baseline\startup.txt
#      ... -Name startup -UpdateBaseline # writes the baseline instead of comparing
#
#  THE POINT. The bar's log is a stream of measurements: timestamps, frame intervals, memory figures, the
#  song that happens to be playing, which windows happen to be open. Diffing that verbatim is a noise
#  machine (reviewed in round 46: "do not make it a full-log verbatim diff"). So every line is put in one
#  of three classes:
#
#    STRICT   the line is kept exactly as written. Only the lifecycle lines go here: the ones that say what
#             was created and in which order, plus the tray's own count. A reordered phase, a duplicated
#             initialisation or a lost step changes this text.
#    SHAPE    the line is kept with every number replaced by <N>, hex by <H> and paths by <PATH>. That
#             keeps the order and the wording of everything that happens without keeping the measurement.
#    DROPPED  pure noise: per-frame draw lines, interval summaries, the ring's own chatter, the hook's.
#
#  ... AND A FOURTH BUCKET, "unordered": lines written by a worker thread (the learning pass, the weather,
#  the sound-page reader) land between frame-loop lines at whatever moment they finish, so their ORDER is a
#  race and not a behaviour. They are kept -- a line that vanishes or changes is still a difference -- but
#  sorted, below the ordered part. MEASURED: two identical runs put the weather line before the tray-learn
#  line once and after it the other time, which is what this bucket is for.
#
#  Exit code: 0 when the run matches the baseline (or a baseline was just written), 1 when it differs.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Name = 'startup',
    [string]$In = '',
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Stop'

$Here = Split-Path -Parent $PSCommandPath
$Out = Join-Path $Here 'out'
$Baseline = Join-Path $Here 'baseline'
if (-not $In) { $In = Join-Path $Out "${Name}-raw.txt" }
if (-not (Test-Path $In)) { throw "no raw log at $In -- run run-demo.ps1 first" }

New-Item -ItemType Directory -Force -Path $Baseline | Out-Null

# Lines whose NUMBERS are part of what is being checked: what exists, and in what order.
$Strict = @(
    'theme:', 'monitor ', 'native taskbar:', 'work area:', 'notifications: ', 'music: reading',
    'music: player window titles', 'volume: reading', 'sound: audio sessions', 'lyrics: on demand',
    'weather: position', 'preview: ', 'start: account', 'tray: \d+ icon'
)

# Lines that are only measurements of the machinery, never of what the bar did.
$Dropped = @(
    'interval \[', 'throttle:', 'redraws:', 'window region:', 'messages/s:', 'slow frame:',
    'tray ring:', 'tray hook:', 'draw ', 'anim:', 'loop:', 'mem\[', 'icon decode'
)

# Lines written by a worker thread: content matters, order does not (see the note above).
$Unordered = @(
    'tray learn:', 'weather:', 'sound page:', 'system status:', 'toasts:', 'volume:'
)

function Convert-Line([string]$line) {
    $trimmed = $line.Trim()
    if (-not $trimmed) { return '' }
    # The leading "[hh:mm:ss.mmm] " stamp is always noise.
    $text = $trimmed -replace '^\[\d{2}:\d{2}:\d{2}\.\d{3}\]\s*', ''
    foreach ($pattern in $Dropped) { if ($text -match $pattern) { return '' } }
    foreach ($pattern in $Strict) {
        if ($text -match $pattern) { return 'STRICT ' + ($text -replace '\s+', ' ') }
    }
    $shape = $text
    $shape = $shape -replace '[A-Za-z]:\\[^\s"]*', '<PATH>'
    $shape = $shape -replace '0x[0-9A-Fa-f]+', '<H>'
    $shape = $shape -replace '\d+', '<N>'
    $shape = $shape -replace '\s+', ' '
    return 'SHAPE  ' + $shape
}

function Test-Unordered([string]$text) {
    foreach ($pattern in $Unordered) { if ($text -match $pattern) { return $true } }
    return $false
}

$ordered = New-Object System.Collections.Generic.List[string]
$unordered = New-Object System.Collections.Generic.List[string]
foreach ($line in [System.IO.File]::ReadAllLines($In)) {
    $converted = Convert-Line $line
    if (-not $converted) { continue }
    if (Test-Unordered $line) { $unordered.Add($converted) } else { $ordered.Add($converted) }
}

# The file is written with the ordered part first and a marker, so a reader can see which half is a timeline
# and which half is a set. Both halves are compared.
$normalized = New-Object System.Collections.Generic.List[string]
$normalized.Add('=== ordered (the timeline) ===')
foreach ($line in $ordered) { $normalized.Add($line) }
$normalized.Add('=== unordered (worker threads; content only) ===')
foreach ($line in ($unordered | Sort-Object)) { $normalized.Add($line) }

$target = Join-Path $Baseline "${Name}.txt"
$written = Join-Path $Out "${Name}.txt"
[System.IO.File]::WriteAllLines($written, $normalized)

if ($UpdateBaseline -or -not (Test-Path $target)) {
    [System.IO.File]::WriteAllLines($target, $normalized)
    Write-Host ("normalize: {0} line(s) -> baseline {1} (written)" -f $normalized.Count, $target)
    exit 0
}

$want = [System.IO.File]::ReadAllLines($target)
$difference = Compare-Object -ReferenceObject $want -DifferenceObject $normalized.ToArray() -SyncWindow 0
if (-not $difference) {
    Write-Host ("log-regression: 0 line(s) differ ({0} line(s) compared, baseline {1})" -f $normalized.Count, "${Name}.txt")
    exit 0
}

$added = @($difference | Where-Object { $_.SideIndicator -eq '=>' })
$removed = @($difference | Where-Object { $_.SideIndicator -eq '<=' })
Write-Host ("log-regression: {0} line(s) differ -- {1} new, {2} gone (baseline {3})" -f $difference.Count, $added.Count, $removed.Count, "${Name}.txt")
foreach ($entry in $removed | Select-Object -First 12) { Write-Host ('  - ' + $entry.InputObject) }
foreach ($entry in $added | Select-Object -First 12) { Write-Host ('  + ' + $entry.InputObject) }
exit 1
