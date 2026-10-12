# verify-panel-anchor.ps1 -- run ONE panel-open transition with the diagnostics on, photograph it at
# a fine cadence, and keep the numbers.
#
# Why the user's bar has to be stopped first: the bar takes a single-instance mutex
# (IslandBar.SingleInstance), so the diagnostic copy cannot run beside it.
#
# Why the transition is driven by ISLANDBAR_PANEL_MS: it opens the panel on a timer and closes it at
# twice that, with no pointer and no click involved. Input synthesised by a script is dropped by UIPI
# before it reaches the bar (see ISLANDBAR_CLICK_AT), so a script cannot press the button itself.
#
# Why the LOG is the real measurement: the bar rewrites its log on every run ("w" -- the log describes
# the current run), so the copy is made straight after the run and before the user's bar is started
# again. The line to watch is "start button: island x [a, b]", which is printed from the same
# RowAnchorBox that the island's fill and the icon row are laid out from.
#
# The captures are taken at 150 ms intervals across the opening, because the defect only showed while
# the card's box was under HALF its final height: the first ~40% of the growth, which at 6x slow motion
# is the first 0.9 s of a 2.2 s animation. A coarser cadence photographs the ends of it and misses the
# middle.
param(
    [string]$Label = 'new',
    # -Deploy copies the built binary over the one the user's shortcut runs, which has to happen while
    # nothing is running it (the image is locked). Done here rather than in a second stop.
    [switch]$Deploy,
    # -DeployOnly: stop the bar, copy the build over the one the shortcut runs, start it again. No
    # diagnostic run in between, so the bar does not have to flash a card to receive a new build.
    [switch]$DeployOnly,
    # -Shot <file>: photograph the bar's strip on the running desktop. Nothing is stopped and nothing is
    # started -- for showing what a change looks like on the user's own bar, which is the only place the
    # real fonts, wallpaper and music are.
    [string]$Shot = '',
    # -FlickerMs N: instead of opening the panel on a timer, flick the pointer on and off the island
    # every N ms. That is the gesture a fast sideways mouse move makes, and it is what INTERRUPTS a
    # card's exit -- the one frame range where the shape is card-sized with no card up.
    [int]$FlickerMs = 0,
    # -SelfTest runs the binary's own --self-test and prints the verdict out of its log. It needs no
    # stop -- and no approval dance for a command shape that is not already allowed -- because the
    # self-test path in main() runs before the single-instance mutex is taken.
    [switch]$SelfTest,
    # -LyricsProbe -Title T [-Artist A]: asks the lyric services for one track and prints what they
    # answered. The development sandbox has no outbound TLS, so this is the only way to find out whether
    # a fetch failed because the service refused, because nothing matched, or because there are no lyrics.
    [switch]$LyricsProbe,
    [string]$Title = '',
    [string]$Artist = '',
    # -Strip checks the OTHER half of the same rule, and needs no stop: the notice strip is what
    # RowAnchorBox exists for, so widening the strip is the one thing this rule's change could break.
    # It talks to the RUNNING bar (the notification goes over a named pipe), photographs the island
    # while the message is up, and reads the running bar's own log.
    [switch]$Strip
)
$ErrorActionPreference = 'Stop'

$repo     = 'C:\PROJECT\Taskbar\IslandBar'
$buildDir = Join-Path $repo 'build\Debug\src\Debug'
# `-Binary old` used to mean "a copy of the previous build, kept under another name". DELETED: a
# differently-named copy of this executable HANGS `Start-Process` when it is launched through this
# tooling (tried twice, with the bar stopped and nothing else running; the process never appears and the
# log is never created). The before/after comparison is made with the pixel assertions instead, where
# the old rule is restored in the source, run, and reverted.
$exe      = Join-Path $buildDir 'island_bar.exe'
$buildLog = Join-Path $buildDir 'island_bar.log'
# The folder the bar RUNS from, which must be outside the project tree (an executable in the project
# directory is demoted to LOW integrity by this environment, and synthetic input aimed at Explorer is then
# discarded -- see HANDOFF 7.84). Derived from the current user rather than written out, so this file carries
# nobody's account name.
$barDir   = Join-Path $env:USERPROFILE 'IslandBar'
$shots    = Join-Path $repo 'captures'
$lnk      = Join-Path $barDir 'IslandBar (replace taskbar).lnk'

if (-not (Test-Path $exe)) { throw "no such binary: $exe" }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

if ($SelfTest) {
    if (Test-Path $buildLog) { Remove-Item -LiteralPath $buildLog -Force }
    # stdout is REDIRECTED to a file, and that is not tidiness: the self-test prints its whole report
    # (hundreds of lines) and a GUI-subsystem process inherits the caller's console. When the caller is a
    # tool that stops draining the pipe, the write blocks and this -Wait never returns -- which is exactly
    # how this script's first version hung, twice, with the exe sitting in a pipe write.
    $outFile = Join-Path $shots 'self-test.out.txt'
    $errFile = Join-Path $shots 'self-test.err.txt'
    $proc = Start-Process -FilePath $exe -ArgumentList '--self-test' -Wait -PassThru -WindowStyle Hidden `
                          -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    "self-test on $exe"
    "exit code (the failure count): $($proc.ExitCode)"
    $lines = Get-Content $buildLog -Encoding UTF8
    "checks passed: $(($lines | Select-String -Pattern '\[ OK \]').Count)"
    $lines | Select-String -Pattern '\[FAIL\]|self-test finished|WARNING' |
        ForEach-Object { $_.Line }
    return
}

if ($LyricsProbe) {
    # The BUILD binary, not the deployed one: this writes a log beside the executable, and the deployed
    # log belongs to the bar that is running -- two processes writing one log truncates it mid-run.
    $outFile = Join-Path $shots 'lyrics-probe.out.txt'
    $errFile = Join-Path $shots 'lyrics-probe.err.txt'
    # Every argument is QUOTED, because Start-Process joins the list with spaces and a title with a
    # space in it arrives split in two -- which is how the first probe of "Maple Road（你的名字）"
    # fetched the right song with the wrong artist.
    $probeArgs = @('--lyrics-test', ('"' + $Title + '"'), '--lyrics-verbose')
    if ($Artist -ne '') {
        $probeArgs = @('--lyrics-test', ('"' + $Title + '"'), ('"' + $Artist + '"'), '--lyrics-verbose')
    }
    Start-Process -FilePath $exe -ArgumentList $probeArgs -Wait -WindowStyle Hidden `
                  -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    "--- lyric probe: title='$Title' artist='$Artist' ---"
    Get-Content $outFile -Encoding UTF8 |
        Select-String -Pattern 'lyrics test|netease |qq :|HTTP|at .* ms the strip' |
        ForEach-Object { $_.Line }
    return
}

if ($Shot -ne '') {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 1010, 130
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(620, 0, 0, 0, (New-Object System.Drawing.Size 1010, 130))
    $g.Dispose()
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "captured the bar's strip to $Shot"
    return
}

if ($DeployOnly) {
    # No diagnostic run: the bar does not have to flash a card to receive a new build. Stopping it
    # first is not optional -- the running image is locked, so the copy needs it gone.
    Get-Process island_bar -ErrorAction SilentlyContinue | ForEach-Object {
        "stopping island_bar pid $($_.Id)"
        Stop-Process -Id $_.Id -Force
    }
    Start-Sleep -Milliseconds 800
    Copy-Item -LiteralPath $exe -Destination (Join-Path $barDir 'island_bar.exe') -Force
    "deployed $exe"
    Start-Process explorer.exe -ArgumentList ('"' + $lnk + '"')
    Start-Sleep -Seconds 2
    Get-Process island_bar -ErrorAction SilentlyContinue | Select-Object Id, StartTime | Format-Table -AutoSize
    return
}

if ($Strip) {
    $running = Join-Path $barDir 'island_bar.exe'
    $runningLog = Join-Path $barDir 'island_bar.log'
    Add-Type -AssemblyName System.Drawing
    "--- sending one notification to the running bar ---"
    # Built from code points rather than written literally: this file has to stay pure ASCII (a .ps1
    # with non-ASCII bytes in it is decoded against the console's code page, and the title arrives
    # mangled). U+7EB8 U+6761 U+9A8C U+8BC1 is the Chinese for "the notice strip check".
    $title = [string][char]0x7EB8 + [char]0x6761 + [char]0x9A8C + [char]0x8BC1
    & $running --notify $title --body "the island must widen for this"
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    foreach ($t in @(0.20, 0.35, 0.55, 0.90, 1.60)) {
        while ($watch.Elapsed.TotalSeconds -lt $t) { Start-Sleep -Milliseconds 10 }
        $bmp = New-Object System.Drawing.Bitmap 1010, 330
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen(620, 0, 0, 0, (New-Object System.Drawing.Size 1010, 330))
        $g.Dispose()
        $bmp.Save((Join-Path $shots ('strip-' + [int]($t * 1000) + 'ms.png')),
                  [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
    }
    "captured 5 frames into $shots\strip-*.png"
    # The measurement that separates the two shapes: the strip's fill reaches 200 DIP further left
    # than the pill's (1070 against 1170), so counting near-black pixels in the band BETWEEN those two
    # left edges says whether the island actually widened. The deck buttons live in that band, so they
    # are counted and reported as the floor rather than as a false positive -- the pill's own number is
    # whatever they contribute, and the strip's is that plus a filled block.
    "--- near-black pixels in the band left of the pill (screen x 1071..1165, y 8..40) ---"
    foreach ($f in (Get-ChildItem (Join-Path $shots 'strip-*.png') | Sort-Object Name)) {
        $bmp = [System.Drawing.Bitmap]::FromFile($f.FullName)
        $hits = 0; $minx = 0; $maxx = 0
        for ($y = 8; $y -le 40; $y += 2) {
            for ($x = 900; $x -lt 1600; $x++) {
                $c = $bmp.GetPixel($x - 620, $y)
                if ($c.R -lt 45 -and $c.G -lt 45 -and $c.B -lt 45) {
                    if ($minx -eq 0 -or $x -lt $minx) { $minx = $x }
                    if ($x -gt $maxx) { $maxx = $x }
                    if ($x -ge 1071 -and $x -le 1165) { $hits = $hits + 1 }
                }
            }
        }
        "{0,-22} dark span {1}..{2} | pixels left of the pill: {3}" -f $f.Name, $minx, $maxx, $hits
        $bmp.Dispose()
    }
    "--- the running bar's own log, around the notification ---"
    Get-Content $runningLog | Select-String -Pattern 'state: |notice|notification:' |
        Select-Object -Last 12 | ForEach-Object { $_.Line }
    return
}

"--- 1. the user's bar down ---"
Get-Process island_bar -ErrorAction SilentlyContinue | ForEach-Object {
    "stopping island_bar pid $($_.Id)"
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Milliseconds 800

if ($Deploy) {
    Copy-Item -LiteralPath $exe -Destination (Join-Path $barDir 'island_bar.exe') -Force
    "deployed to $(Join-Path $barDir 'island_bar.exe')"
}

"--- 2. $Label : $exe ($((Get-Item $exe).LastWriteTime)) ---"
if (Test-Path $buildLog) { Remove-Item -LiteralPath $buildLog -Force }
$env:ISLANDBAR_PANEL_MS   = '1500'
$env:ISLANDBAR_SLOWMO     = '6'
$env:ISLANDBAR_TRACE_CARD = '1'
if ($FlickerMs -gt 0) {
    # The panel's timer stays ON: a card has to be up for its EXIT to be interrupted, and the panel is
    # the one card that can be opened without a media session (the first run of this reproduced nothing
    # because the music had stopped, so there was no card at all -- only `hover`/`collapsed`).
    $env:ISLANDBAR_PANEL_MS = '1500'
    $env:ISLANDBAR_FORCE_INSIDE = '1'
    $env:ISLANDBAR_INSIDE_FLICKER_MS = "$FlickerMs"
}
Add-Type -AssemblyName System.Drawing
$p = Start-Process -FilePath $exe -ArgumentList '--keep-taskbar' -PassThru -WindowStyle Hidden
$watch = [System.Diagnostics.Stopwatch]::StartNew()
# The last two frames are AFTER the panel has gone, so the pill can be compared with the card up and
# with it away. That comparison is the whole point when music is playing: the pill's own line must not
# change when a card opens.
foreach ($t in @(1.50, 1.80, 2.10, 2.40, 2.70, 3.00, 3.30, 3.90, 4.60, 5.00)) {
    while ($watch.Elapsed.TotalSeconds -lt $t) { Start-Sleep -Milliseconds 12 }
    $bmp = New-Object System.Drawing.Bitmap 1010, 330
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(620, 0, 0, 0, (New-Object System.Drawing.Size 1010, 330))
    $g.Dispose()
    $bmp.Save((Join-Path $shots ('anchor-' + $Label + '-' + [int]($t * 1000) + 'ms.png')),
              [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
"captured 10 frames into $shots\anchor-$Label-*.png"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 600

"--- 3. keep the numbers ---"
$kept = Join-Path $shots ('anchor-' + $Label + '.log')
Copy-Item -LiteralPath $buildLog -Destination $kept -Force
"log kept at $kept"

"--- 4. the user's bar back (through Explorer, so it keeps the shell's integrity) ---"
Remove-Item Env:ISLANDBAR_PANEL_MS -ErrorAction SilentlyContinue
Remove-Item Env:ISLANDBAR_SLOWMO -ErrorAction SilentlyContinue
Remove-Item Env:ISLANDBAR_TRACE_CARD -ErrorAction SilentlyContinue
Remove-Item Env:ISLANDBAR_FORCE_INSIDE -ErrorAction SilentlyContinue
Remove-Item Env:ISLANDBAR_INSIDE_FLICKER_MS -ErrorAction SilentlyContinue
Start-Process explorer.exe -ArgumentList ('"' + $lnk + '"')
Start-Sleep -Seconds 2
Get-Process island_bar -ErrorAction SilentlyContinue | Select-Object Id, StartTime | Format-Table -AutoSize

"--- 5. the anchor across the opening, frame by frame ---"
$lines = Get-Content $kept
$start = 0
for ($i = 0; $i -lt $lines.Count; $i++) {
    # The timer path says "diagnostic: panel opened at N ms"; a real press says "system panel: open".
    # Either is the moment the card was asked for, which is where the measurement starts.
    if ($lines[$i] -match 'system panel: open|panel opened at') { $start = $i }
}
$seen = @{}
for ($i = $start; $i -lt [Math]::Min($lines.Count, $start + 500); $i++) {
    if ($lines[$i] -match 'island x \[([-0-9]+), ([-0-9]+)\]') {
        $key = $Matches[1] + '..' + $Matches[2]
        if (-not $seen.ContainsKey($key)) { $seen[$key] = 0 }
        $seen[$key] = $seen[$key] + 1
    }
}
# A COUNT IS PRINTED EVEN WHEN IT IS ZERO, and the zero is the good answer: these lines are printed
# only when their value changes, so "0 lines after the marker" means nothing on the bar moved or faded
# for the whole of the transition. A silent section reads as "no evidence" instead (cost this project a
# round: the marker's wording had changed and the section was empty for that reason).
"anchor boxes reported after the panel opened: $($seen.Count) distinct value(s)"
$seen.GetEnumerator() | Sort-Object { $_.Name } | ForEach-Object { "   $($_.Name)  x$($_.Value)" }

# The deck's own opacity, which is the number the "the bar blinks" report was about: it must be 1.00
# for every frame of the transition. One value in the list is the pass; a ramp from 0.4 to 1.00 is what
# the bar used to do (and what the user saw as a flash).
$ops = @{}
for ($i = $start; $i -lt [Math]::Min($lines.Count, $start + 500); $i++) {
    if ($lines[$i] -match 'start button:.*opacity ([0-9.]+)') {
        $key = $Matches[1]
        if (-not $ops.ContainsKey($key)) { $ops[$key] = 0 }
        $ops[$key] = $ops[$key] + 1
    }
}
"deck opacities reported after the panel opened: $($ops.Count) distinct value(s) (1 is the good answer)"
$ops.GetEnumerator() | Sort-Object { [double]$_.Name } | ForEach-Object { "   $($_.Name)  x$($_.Value)" }

# The canary: the bar's own line must never be drawn into a box taller than the bar. Zero is the good
# answer, and it is the one piece of evidence that does not depend on catching a one-frame defect in a
# photograph.
"line-drawn-out-of-place warnings: " + (($lines | Select-String -Pattern 'WARNING: the bar').Count)
"pointer flicks: " + (($lines | Select-String -Pattern 'flicks ').Count)

# KEPT SHORT ON PURPOSE. This script is run through a tool that stops draining stdout once it has
# yielded, and a process that writes more than the pipe holds BLOCKS in the write -- which hung this
# script twice, once here and once in -SelfTest. The full log is kept in $kept; only the summary is
# printed.
"state lines (last 3):"
$lines | Select-String -Pattern 'state: ' | Select-Object -Last 3 | ForEach-Object { $_.Line }
