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
# NOT in here: the weather position, which is the Windows location service's answer and jitters in its fifth
# decimal between two runs (MEASURED: 41.775621,123.441200 then 41.775624,123.441217).
$Strict = @(
    'theme:', 'native taskbar:', 'work area:', 'notifications: ', 'music: reading',
    'music: player window titles', 'volume: reading', 'sound: audio sessions', 'lyrics: on demand',
    'preview: ', 'start: account'
)
# NOT STRICT, and it was: the monitor line ends with the taskbar's own inset -- 32 px on this machine, 0 px
# after anything has touched the shell taskbar -- which is the machine's state and not the bar's. It is
# compared as SHAPE instead.

# Lines that are only measurements of the machinery, never of what the bar did.
$Dropped = @(
    'redraws:', 'window region:', 'messages/s:', 'slow frame:',
    'tray ring:', 'tray hook:', 'draw ', 'anim:', 'loop:', 'icon decode',
    # NOT dropped, like the mem[...] line and for the same reason: the interval and throttle lines are the
    # only evidence that the two periodic reports still run and still carry their fields. Their numbers are
    # measurements, so they are compared as SHAPE.
    # NOT dropped, and this is deliberate: the mem[...] line is the only evidence that the periodic memory
    # report still runs and still carries the same labels. Its numbers are noise, so it is compared as
    # SHAPE -- the whole line with every digit replaced -- rather than thrown away.
    # Machine-dependent events: which of the user's applications opened a window, and which taskbar
    # buttons exist right now. They change when the user does anything at all.
    'wechat: window appeared', 'taskbar icon: hwnd',
    # Live data, not behaviour: the weather reading changes between two runs a minute apart.
    '^weather: [\d\.-]+ C, code',
    # The row's own contents: how many application icons it has is the user's window count, and the layout
    # itself is asserted by --self-test, which is where geometry belongs.
    '^row: '
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
    # THREE FRAGMENTS THAT DESCRIBE THE MACHINE AND NOT THE BAR, dropped before the line is classified:
    # how many processes are running (it moves when anything at all starts, including this script), how many
    # entries the tray's registry key holds (installing an app changes it), and the "not shown" list (which
    # is the machine's application set). MEASURED: the first baseline went stale between two runs because
    # the process count read 305 and then 310.
    $text = $text -replace '\s*\(not shown:[^)]*\)', ''
    $text = $text -replace '\d+\s+entr\(ies\)', 'entr(ies)'
    $text = $text -replace '\d+\s+process\(es\) seen', 'process(es) seen'
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

# THE NAMES ARE NOT FREE: PowerShell variable names are case-insensitive, so a bucket called `$unordered`
# would overwrite the pattern list `$Unordered` -- which is exactly what happened, and it made the unordered
# bucket permanently empty while the weather line (a worker thread's) sat in the timeline and moved between
# runs. MEASURED 2026-10-16, the day the baseline was first written.
$orderedLines = New-Object System.Collections.Generic.List[string]
$unorderedLines = New-Object System.Collections.Generic.List[string]
foreach ($line in [System.IO.File]::ReadAllLines($In)) {
    $converted = Convert-Line $line
    if (-not $converted) { continue }
    if (Test-Unordered $line) { $unorderedLines.Add($converted) } else { $orderedLines.Add($converted) }
}

# The file is written with the ordered part first and a marker, so a reader can see which half is a timeline
# and which half is a set. Both halves are compared.
$normalized = New-Object System.Collections.Generic.List[string]
$normalized.Add('=== ordered (the timeline) ===')
foreach ($line in $orderedLines) { $normalized.Add($line) }
$normalized.Add('=== unordered (worker threads; content only) ===')
foreach ($line in ($unorderedLines | Sort-Object)) { $normalized.Add($line) }

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
