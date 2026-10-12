# =====================================================================
#  probe-minimised-marker.ps1 -- does a minimised window's icon actually look
#  different from a normal one?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-minimised-marker.ps1
#
#  WHY THIS EXISTS
#
#  The frame has carried `minimised` from the window list since the row was built, and no drawing code
#  ever read it -- so with ten windows open a minimised one and a normal one were the same picture. The
#  marker added for that is an icon drawn at 55% opacity, and opacity is exactly the kind of thing a
#  screenshot can settle: this opens a window of its own (Notepad), photographs the bar with it normal,
#  minimises it, photographs again, and reports where the two pictures differ and by how much.
#
#  Its own window, so nothing of the user's is touched, and it is closed at the end.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace Mini -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
'@

$shots = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
New-Item -ItemType Directory -Force -Path $shots | Out-Null

function Save-Band([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap 2560, 60
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 2560, 60))
    $g.Dispose()
    $path = Join-Path $shots $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $path
}

# A WINDOW OF THIS SCRIPT'S OWN, not somebody else's application.
#
# Notepad was the first choice and does not work: the Notepad of Windows 11 is a packaged application, so
# the process Start-Process returns has no window at all (measured: its MainWindowHandle is 0 three seconds
# later). A form created here is a plain Win32 window in this process -- the bar lists it like any other
# window, and the handle is ours to minimise.
$form = New-Object System.Windows.Forms.Form
$form.Text = 'IslandBar marker test'
$form.Width = 420
$form.Height = 220
$form.StartPosition = 'CenterScreen'
$form.Show()
$form.Refresh()
"  test window hwnd $($form.Handle)"
Start-Sleep -Seconds 3      # let the bar's window list notice it and the icon appear
$normal = Save-Band 'minimise-normal.png'

[void][Mini.Win]::ShowWindow($form.Handle, 6)   # SW_MINIMIZE
Start-Sleep -Seconds 3      # the list refreshes twice a second; two seconds is a margin
$minimised = Save-Band 'minimise-minimised.png'

$a = [System.Drawing.Bitmap]::FromFile($normal)
$b = [System.Drawing.Bitmap]::FromFile($minimised)
# COMPARED BEAD BY BEAD, not picture against picture.
#
# The whole band cannot be diffed: the wallpaper behind the bar is a running game and it changes between
# the two photographs (measured on the first attempt: 54,887 pixels "changed" across the entire width,
# which is the game and not the bar). A bead's own background, though, is the island's colour at 98% --
# opaque -- so the pixels inside one bead ARE comparable.
#
# And the beads do not have to be found: the row is laid out from the log the bar prints (`draw icons:
# island x [1170, 1428]` -> the first bead starts at 1428 + 8), at a 30 DIP bead with a 4 DIP gap, so bead
# k spans 1436 + 34k .. 1466 + 34k. Measuring those ranges is arithmetic rather than detection.
$rowY = 24
$rowFirstBead = 1436
$pitch = 34
$beadWidth = 30
$worst = 0.0
$worstIndex = -1
for ($bead = 0; $bead -lt 12; $bead++) {
    $from = $rowFirstBead + $pitch * $bead
    $to = $from + $beadWidth - 1
    if ($to -ge 2560) { break }
    $sumA = 0.0; $sumB = 0.0; $n = 0
    for ($x = $from; $x -le $to; $x++) {
        $ca = $a.GetPixel($x, $rowY); $cb = $b.GetPixel($x, $rowY)
        $sumA += ($ca.R + $ca.G + $ca.B); $sumB += ($cb.R + $cb.G + $cb.B); $n++
    }
    $meanA = $sumA / 3.0 / $n; $meanB = $sumB / 3.0 / $n
    $ratio = $meanB / [math]::Max(0.001, $meanA)
    "  bead {0} at x {1}..{2}: {3:N1} -> {4:N1} (ratio {5:N2})" -f $bead, $from, $to, $meanA, $meanB, $ratio
    if ([math]::Abs(1.0 - $ratio) -gt $worst) { $worst = [math]::Abs(1.0 - $ratio); $worstIndex = $bead }
}
if ($worst -lt 0.05) {
    "the bead that changed most moved by only {0:P0}: the minimised marker is NOT visible" -f $worst
} else {
    "the bead that changed most is number {0}, by {1:P0} (a dimmed icon is expected to be about 45% of the icon's own brightness)" -f $worstIndex, $worst
}
$a.Dispose(); $b.Dispose()

$form.Close()
$form.Dispose()
'test window closed'
