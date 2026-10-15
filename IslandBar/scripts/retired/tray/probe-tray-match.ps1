# =====================================================================
#  probe-tray-match.ps1 -- can a cell of the bar's tray card be matched to
#  the icon the SHELL has, by its picture?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-match.ps1
#
#  WHY THIS EXISTS
#
#  Pressing an icon in the bar's card means pressing the same icon in the shell's tray, and the only
#  handle on one is what the shell calls it: the overflow flyout publishes buttons named
#  "QQ Yinyue <tagline> ..." (Chinese), " Weixin", "Clash Verge 2.5.8 ..." while the bar's own names come
#  from the registry and are often just the executable ("Weixin.exe"). A name match therefore covers half of
#  them and cannot ever cover WeChat.
#
#  Their PICTURES are the same picture, though: the registry holds the PNG the shell snapshots, and the
#  flyout draws the icon on screen. So this probe opens the flyout (with the taskbar made invisible --
#  see probe-tray-hidden-invoke.ps1 for why that works), photographs each icon cell, and prints the
#  distance from every cell to every registry icon: the mapping, and whether the right pair is clearly
#  nearer than the wrong ones.
#
#  Read-only: it presses nothing but the overflow chevron, and puts the taskbar back.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace TrayMatch -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
'@

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002

# The signature of one icon: a 4x4 grid of mean colours, composited over `bg` when the picture has
# transparency -- which most tray snapshots do. Coarse on purpose: the shell draws the same picture at
# 16x16 and the snapshots are 16x16 to 32x32, so the two only agree after averaging.
#
# FLAT arrays of doubles, not a grid of arrays: PowerShell's `+=` on a nested array inside a function
# produced "Object[] does not contain a method named op_Division" and cost a round of debugging. 48
# numbers, and the distance walks them.
function New-Signature($img, $bgR, $bgG, $bgB) {
    $sig = New-Object 'double[]' 48
    for ($cy = 0; $cy -lt 4; $cy++) {
        for ($cx = 0; $cx -lt 4; $cx++) {
            $x0 = [int]($cx * $img.Width / 4)
            $x1 = [int](($cx + 1) * $img.Width / 4)
            $y0 = [int]($cy * $img.Height / 4)
            $y1 = [int](($cy + 1) * $img.Height / 4)
            $sr = 0.0; $sg = 0.0; $sb = 0.0; $cnt = 0
            for ($y = $y0; $y -lt $y1; $y++) {
                for ($x = $x0; $x -lt $x1; $x++) {
                    $p = $img.GetPixel($x, $y)
                    $alpha = $p.A / 255.0
                    $sr += $p.R * $alpha + $bgR * (1.0 - $alpha)
                    $sg += $p.G * $alpha + $bgG * (1.0 - $alpha)
                    $sb += $p.B * $alpha + $bgB * (1.0 - $alpha)
                    $cnt++
                }
            }
            $cell = ($cy * 4 + $cx) * 3
            if ($cnt -gt 0) {
                $sig[$cell] = $sr / $cnt
                $sig[$cell + 1] = $sg / $cnt
                $sig[$cell + 2] = $sb / $cnt
            }
        }
    }
    return $sig
}

function Get-SignatureDistance($a, $b) {
    $sum = 0.0
    for ($i = 0; $i -lt $a.Length; $i++) { $sum += [math]::Abs($a[$i] - $b[$i]) }
    return $sum / $a.Length
}

'--- the registry icons (the list the bar draws) ---'
$ours = @()
foreach ($e in Get-ChildItem 'HKCU:\Control Panel\NotifyIconSettings') {
    $item = Get-Item $e.PSPath
    $png = $item.GetValue('IconSnapshot')
    if (-not $png) { continue }
    $tip = [string]$item.GetValue('InitialTooltip')
    $path = [string]$item.GetValue('ExecutablePath')
    $stream = New-Object System.IO.MemoryStream(, [byte[]]$png)
    $img = [System.Drawing.Image]::FromStream($stream)
    $bmp = New-Object System.Drawing.Bitmap $img
    $name = Split-Path $path -Leaf
    if ($tip -ne '') { $name = $tip }
    $ours += [pscustomobject]@{ name = $name; path = $path; w = $bmp.Width; h = $bmp.Height; bmp = $bmp }
    $img.Dispose(); $stream.Dispose()
}
"  {0} registry icon(s)" -f $ours.Count

$tray = [TrayMatch.Win]::FindWindow('Shell_TrayWnd', $null)
$hadLayered = (([TrayMatch.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64() -band $WS_EX_LAYERED) -ne 0)
$ex = [TrayMatch.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayMatch.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TrayMatch.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
[void][TrayMatch.Win]::ShowWindow($tray, 8)
Start-Sleep -Milliseconds 700

# The tray's own subtree takes a moment to appear once the taskbar is shown -- measured at ~300 ms, and
# the first run of this script simply waited 700 ms and then found nothing to press. Poll for it.
$root = [System.Windows.Automation.AutomationElement]::FromHandle($tray)
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'SystemTrayIcon')
$chevron = $null
for ($try = 0; $try -lt 20 -and $null -eq $chevron; $try++) {
    foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
        if ($null -eq $chevron -or $e.Current.BoundingRectangle.Left -lt $chevron.Current.BoundingRectangle.Left) {
            $chevron = $e
        }
    }
    if ($null -eq $chevron) { Start-Sleep -Milliseconds 100 }
}
if ($null -eq $chevron) { 'the tray never published its buttons'; [void][TrayMatch.Win]::ShowWindow($tray, 0); exit 1 }
"  the chevron is at {0}" -f [int]$chevron.Current.BoundingRectangle.Left
$chevronPattern = $null
[void]$chevron.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$chevronPattern)
$chevronPattern.Invoke()

# The flyout is a top-level window of the shell's, found through UI Automation rather than by class:
# FindWindow did not see it although the desktop's own UIA children list it (measured).
$flyoutCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'TopLevelWindowForOverflowXamlIsland')
$desktop = [System.Windows.Automation.AutomationElement]::RootElement
$flyout = $null
for ($try = 0; $try -lt 25 -and $null -eq $flyout; $try++) {
    $found = $desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $flyoutCond)
    if ($found.Count -gt 0) { $flyout = $found[0] }
    if ($null -eq $flyout) { Start-Sleep -Milliseconds 100 }
}
if ($null -eq $flyout) {
    'the flyout did not appear'
    $chevronPattern.Invoke()
    [void][TrayMatch.Win]::ShowWindow($tray, 0)
    exit 1
}

$froot = $flyout
$cond2 = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'NotifyItemIcon')
$buttons = $froot.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond2)
"--- the flyout: {0} icon(s) ---" -f $buttons.Count

# The panel's own colour, sampled between two icons: the composite background for transparency.
$frect = $flyout.Current.BoundingRectangle
$frL = [int]$frect.Left; $frT = [int]$frect.Top
$frW = [int]$frect.Width; $frH = [int]$frect.Height
$full = New-Object System.Drawing.Bitmap $frW, $frH
$gfx = [System.Drawing.Graphics]::FromImage($full)
$gfx.CopyFromScreen($frL, $frT, 0, 0, (New-Object System.Drawing.Size $frW, $frH))
$gfx.Dispose()
$bg = $full.GetPixel(4, 4)
# NOT the corner: the panel's corners are rounded, so (4,4) is outside it (measured: 1,1,1, which would
# have made every transparent snapshot composite against black). The panel's own colour is sampled in
# the band below the last row of icons, which is padding.
$bg = $full.GetPixel([int]($frW / 2), $frH - 10)
"  flyout background: {0},{1},{2}" -f $bg.R, $bg.G, $bg.B

$cells = @()
foreach ($b in $buttons) {
    $r = $b.Current.BoundingRectangle
    # The icon inside the 40x40 button: the picture is centred, so take the middle 16x16.
    $cx = [int]($r.Left + $r.Width / 2 - 8)
    $cy = [int]($r.Top + $r.Height / 2 - 8)
    $icon = New-Object System.Drawing.Bitmap 16, 16
    $g2 = [System.Drawing.Graphics]::FromImage($icon)
    $g2.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, 16, 16),
                  (New-Object System.Drawing.Rectangle ($cx - $frL), ($cy - $frT), 16, 16),
                  [System.Drawing.GraphicsUnit]::Pixel)
    $g2.Dispose()
    $cells += [pscustomobject]@{ name = $b.Current.Name; rect = ('{0},{1}' -f $cx, $cy);
                                 sig = (New-Signature $icon $bg.R $bg.G $bg.B) }
    $icon.Dispose()
}

"--- nearest registry icon for each flyout icon (mean channel distance, lower is nearer) ---"
foreach ($c in $cells) {
    $ranked = @()
    foreach ($o in $ours) {
        $d = Get-SignatureDistance (New-Signature $o.bmp $bg.R $bg.G $bg.B) $c.sig
        $ranked += [pscustomobject]@{ name = $o.name; d = $d }
    }
    $sorted = $ranked | Sort-Object d
    $best = $sorted[0]; $second = $sorted[1]
    "  flyout '{0}' ({1})" -f $c.name.Replace("`n", ' / '), $c.rect
    "      best   {0,7:N1}  '{1}'" -f $best.d, $best.name
    "      second {0,7:N1}  '{1}'" -f $second.d, $second.name
}

$chevronPattern.Invoke()
Start-Sleep -Milliseconds 300
foreach ($o in $ours) { $o.bmp.Dispose() }
$full.Dispose()
if (-not $hadLayered) {
    [void][TrayMatch.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
    [void][TrayMatch.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]$ex)
}
[void][TrayMatch.Win]::ShowWindow($tray, 0)
'=== done ==='
