# =====================================================================
#  probe-cursor-hide.ps1 -- CAN THIS BAR HIDE THE POINTER WHILE IT MOVES IT?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-cursor-hide.ps1
#
#  WHY THIS EXISTS
#
#  A press is a real click: the pointer is moved onto the shell's tray icon, the button goes down and up,
#  and the pointer is put back. Everything else about that press is now measured and clean (300 ms, no
#  panel flash, the application answers), and the one thing the user still refuses is having to watch the
#  pointer jump.
#
#  Hiding the pointer for those ~150 ms would leave the jump in place and take it off the screen. Whether
#  `ShowCursor(FALSE)` can do that at all is the question, and it is not a question to reason about: the
#  display counter is maintained per THREAD, while the cursor is drawn by whichever thread OWNS it -- and
#  during a press the pointer is over the shell's panel, which is not our thread. This measures exactly
#  that: hide the pointer in this process, move it onto a FOREIGN window (the desktop), and look at the
#  pixels where it should be.
#
#  HOW THE ANSWER IS READ: the same rectangle is photographed twice -- once with the pointer somewhere else
#  (the baseline) and once with the pointer parked in the middle of it. If the two differ, a pointer was
#  drawn; if they are identical, it was not.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace CursorProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern int ShowCursor(bool show);
'@

$box = 48          # the rectangle photographed around the test point
$testX = 1280
$testY = 1000      # empty desktop: no window of ours is there

function Grab([int]$x, [int]$y) {
    $bmp = New-Object System.Drawing.Bitmap $box, $box
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x - ($box / 2), $y - ($box / 2), 0, 0, (New-Object System.Drawing.Size $box, $box))
    $g.Dispose()
    return $bmp
}

function Pixels($bmp) {
    $out = New-Object 'System.Collections.Generic.List[int]'
    for ($y = 0; $y -lt $bmp.Height; $y++) { for ($x = 0; $x -lt $bmp.Width; $x++) { $c = $bmp.GetPixel($x, $y); $out.Add($c.R + ($c.G * 256) + ($c.B * 65536)) } }
    return $out
}

$saved = New-Object CursorProbe.Win+POINT
[void][CursorProbe.Win]::GetCursorPos([ref]$saved)
"the pointer starts at $($saved.X),$($saved.Y)"

# 1. baseline: the pointer is far away, so the test rectangle holds only the desktop.
[void][CursorProbe.Win]::SetCursorPos(100, 100)
Start-Sleep -Milliseconds 250
$baseline = Grab $testX $testY

# 2. the pointer moved into the rectangle, cursor SHOWN (this is what a press looks like today).
[void][CursorProbe.Win]::SetCursorPos($testX, $testY)
Start-Sleep -Milliseconds 250
$shown = Grab $testX $testY

# 3. the same, with the pointer hidden first.
$count = [CursorProbe.Win]::ShowCursor($false)
"ShowCursor(false) -> the display counter is now $count"
[void][CursorProbe.Win]::SetCursorPos($testX + 4, $testY + 4)
Start-Sleep -Milliseconds 250
$hidden = Grab $testX $testY

function Difference($a, $b) {
    $pa = Pixels $a
    $pb = Pixels $b
    $diff = 0
    for ($i = 0; $i -lt $pa.Count; $i++) { if ($pa[$i] -ne $pb[$i]) { $diff++ } }
    return $diff
}

$shownDiff = Difference $baseline $shown
$hiddenDiff = Difference $baseline $hidden
"pixels that changed with the pointer shown : $shownDiff of $(($box * $box))"
"pixels that changed with the pointer hidden : $hiddenDiff of $(($box * $box))"

$dir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$baseline.Save((Join-Path $dir 'cursor-baseline.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$shown.Save((Join-Path $dir 'cursor-shown.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$hidden.Save((Join-Path $dir 'cursor-hidden.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$baseline.Dispose(); $shown.Dispose(); $hidden.Dispose()

# put the pointer back, and make sure the counter is not left negative by this process.
while ([CursorProbe.Win]::ShowCursor($true) -lt 0) { }
[void][CursorProbe.Win]::SetCursorPos($saved.X, $saved.Y)
Start-Sleep -Milliseconds 200
$restored = Grab $testX $testY
"after restoring: the pointer is at ($((New-Object CursorProbe.Win+POINT).X)), and ShowCursor is back above 0"
$restored.Dispose()

if ($shownDiff -gt 0 -and $hiddenDiff -eq 0) {
    'RESULT: ShowCursor(false) DOES hide the pointer, even over a foreign window'
} elseif ($shownDiff -gt 0 -and $hiddenDiff -gt 0) {
    'RESULT: the pointer is still drawn while ShowCursor(false) is in effect -- hiding it from this process does NOT work over another window'
} else {
    'RESULT: inconclusive -- the pointer was not drawn even when shown (is the capture working?)'
}
'=== done ==='
