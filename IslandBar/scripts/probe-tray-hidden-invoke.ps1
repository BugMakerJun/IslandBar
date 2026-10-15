# =====================================================================
#  probe-tray-hidden-invoke.ps1 -- can a tray icon be PRESSED without the
#  taskbar being seen?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-hidden-invoke.ps1
#      ... -Invoke      also press the leftmost tray button (the overflow chevron)
#
#  WHY THIS EXISTS
#
#  Pressing an icon in the bar's tray card is the one thing the tray feature does not do yet, and the
#  only route measured so far is: show the shell's taskbar, press the icon through UI Automation, hide
#  it again. That works and costs a visible flash of the real taskbar for about a third of a second.
#
#  So the question this script answers is whether the taskbar can be made INVISIBLE TO THE EYE while
#  still being there for UI Automation:
#
#    A. SW_SHOWNA            -- shown but not activated: the baseline, and what the flash looks like.
#    B. + WS_EX_LAYERED, alpha 0 -- present and hit-testable for UIA, but painted with nothing.
#    C. + Invoke             -- is the press still accepted in that state?
#
#  Each state is photographed, so "invisible" is a picture rather than a claim.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file. The bar must be STOPPED while this
#  runs: the bar re-hides the taskbar every few seconds and would fight it.
# =====================================================================
# -Overflow: open the tray's overflow flyout and dump ITS tree -- because the application icons are
# not in the taskbar's own tree at all (probe-tray-band-windows.ps1: nothing between the last pinned
# app button at x 1547 and the chevron at x 2416, while the photograph of the shown taskbar has icons
# there). Where they live decides whether pressing one is possible at all.
param([switch]$Invoke, [switch]$Overflow)

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace TrayHit -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
[DllImport("user32.dll")] public static extern int GetLastError();
'@

$shots = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002

function Save-Band([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap 2560, 40
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 2560, 40))
    $g.Dispose()
    $path = Join-Path $shots $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    # How much of the band is NOT the wallpaper on the left: a taskbar is a solid band across the whole
    # width, so sum the pixels that differ from the pixel 4 px in from the left edge.
    $solid = 0
    $sample = $bmp.GetPixel(4, 20)
    for ($x = 200; $x -lt 2400; $x += 10) {
        $c = $bmp.GetPixel($x, 20)
        if ([math]::Abs($c.R - $sample.R) + [math]::Abs($c.G - $sample.G) +
            [math]::Abs($c.B - $sample.B) -gt 24) { $solid++ }
    }
    $bmp.Dispose()
    "  [{0}] written {1}; band samples differing from the left edge: {2} of 220" -f $name, 'ok', $solid
}

function Get-TrayButtons([IntPtr]$tray) {
    $found = @()
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($tray)
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'SystemTrayIcon')
        $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
        foreach ($e in $all) { $found += $e }
    } catch {
        "  UI Automation failed: $($_.Exception.Message)"
    }
    return $found
}

$tray = [TrayHit.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }
$r = New-Object TrayHit.Win+RECT
[void][TrayHit.Win]::GetWindowRect($tray, [ref]$r)
"Shell_TrayWnd $tray at $($r.L),$($r.T) $($r.R - $r.L)x$($r.B - $r.T) visible=$([TrayHit.Win]::IsWindowVisible($tray))"

$wasVisible = [TrayHit.Win]::IsWindowVisible($tray)
$hadLayered = (([TrayHit.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64() -band $WS_EX_LAYERED) -ne 0)

'--- hidden, as the bar leaves it ---'
[void][TrayHit.Win]::ShowWindow($tray, 0)
Start-Sleep -Milliseconds 400
"  UIA buttons: $((Get-TrayButtons $tray).Count)"
Save-Band 'tray-state-hidden.png'

'--- A. shown without activating (this is the flash) ---'
[void][TrayHit.Win]::ShowWindow($tray, 8)   # SW_SHOWNA
Start-Sleep -Milliseconds 700
$shown = Get-TrayButtons $tray
"  UIA buttons: $($shown.Count)"
foreach ($e in $shown) {
    $rect = $e.Current.BoundingRectangle
    "    '{0}' at {1},{2}" -f $e.Current.Name, [int]$rect.Left, [int]$rect.Top
}
Save-Band 'tray-state-shown.png'

'--- B. the same window, WS_EX_LAYERED with alpha 0 ---'
$ex = [TrayHit.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayHit.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
$alphaOk = [TrayHit.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
"  SetLayeredWindowAttributes -> $alphaOk (err $([TrayHit.Win]::GetLastError()))"
Start-Sleep -Milliseconds 700
$invisible = Get-TrayButtons $tray
"  UIA buttons: $($invisible.Count)"
Save-Band 'tray-state-alpha0.png'

if ($Invoke) {
    '--- C. pressing the leftmost tray button in that state ---'
    $leftmost = $null
    foreach ($e in $invisible) {
        if ($null -eq $leftmost) { $leftmost = $e; continue }
        if ($e.Current.BoundingRectangle.Left -lt $leftmost.Current.BoundingRectangle.Left) {
            $leftmost = $e
        }
    }
    if ($null -eq $leftmost) {
        '  nothing to press'
    } else {
        "  pressing '$($leftmost.Current.Name)'"
        $pattern = $null
        if ($leftmost.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,
                                           [ref]$pattern)) {
            $before = Get-Date
            try {
                $pattern.Invoke()
                "  Invoke returned after $([int]((Get-Date) - $before).TotalMilliseconds) ms"
            } catch {
                "  Invoke threw: $($_.Exception.Message)"
            }
        } else {
            '  no InvokePattern on it'
        }
        Start-Sleep -Milliseconds 900
        $bmp = New-Object System.Drawing.Bitmap 2560, 700
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 2560, 700))
        $g.Dispose()
        $bmp.Save((Join-Path $shots 'tray-invoked.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        '  written tray-invoked.png (does the flyout appear where the shell would have put it?)'
        # Put the shell's mouth away again: Escape closes the overflow flyout.
        [void][System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    }
}

if ($Overflow) {
    '--- C. the overflow flyout: open it, then read what is inside ---'
    # NOT copied from probe-tray-uia.ps1: this file never needed a rectangle formatter before, and
    # calling one that was not here is how the first run of this block produced nothing but errors.
    function Format-Rect($element) {
        try {
            $r = $element.Current.BoundingRectangle
            if ($r.IsEmpty) { return '(empty)' }
            return ('{0},{1} {2}x{3}' -f [int]$r.Left, [int]$r.Top, [int]$r.Width, [int]$r.Height)
        } catch { return '(none)' }
    }
    $leftmost = $null
    foreach ($e in $invisible) {
        if ($null -eq $leftmost) { $leftmost = $e; continue }
        if ($e.Current.BoundingRectangle.Left -lt $leftmost.Current.BoundingRectangle.Left) {
            $leftmost = $e
        }
    }
    if ($null -eq $leftmost) {
        '  nothing to press'
    } else {
        $chevron = $leftmost
        $pattern = $null
        if ($chevron.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
            $pattern.Invoke()
            Start-Sleep -Milliseconds 900
            "  the flyout should be up now; scanning every window below the band"
            # Every window whose rectangle sits just under the band: the flyout is one of them.
            Add-Type -Namespace TrayHit2 -Name Win2 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(IntPtr cb, IntPtr p);
'@
            $candidates = @()
            $windows = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 } |
                Select-Object -ExpandProperty MainWindowHandle
            # Cheaper and complete: walk the desktop's children through UIA itself.
            $desktop = [System.Windows.Automation.AutomationElement]::RootElement
            $kids = $desktop.FindAll([System.Windows.Automation.TreeScope]::Children,
                                     [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($k in $kids) {
                $r = $k.Current.BoundingRectangle
                if ([int]$r.Top -ge 28 -and [int]$r.Top -le 40 -and [int]$r.Width -gt 40) {
                    $candidates += $k
                }
            }
            "  $($candidates.Count) top-level window(s) just under the band"
            foreach ($c in $candidates) {
                "    '$($c.Current.Name)' class='$($c.Current.ClassName)' {0}" -f (Format-Rect $c)
                $inner = $c.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                                    [System.Windows.Automation.Condition]::TrueCondition)
                foreach ($e in $inner) {
                    $type = '?'; $name = ''; $id = ''
                    try { $type = $e.Current.ControlType.ProgrammaticName -replace 'ControlType.', '' } catch { }
                    try { $name = $e.Current.Name } catch { }
                    try { $id = $e.Current.AutomationId } catch { }
                    $hasInvoke = '-'
                    try {
                        $o = $null
                        if ($e.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$o)) {
                            $hasInvoke = 'Invoke'
                        }
                    } catch { }
                    "        $type '$name' id='$id' {0} [{1}]" -f (Format-Rect $e), $hasInvoke
                }
            }
            # The flyout by name as well, because the desktop root does not always list shell islands.
            $flyout = [TrayHit.Win]::FindWindow('TopLevelWindowForOverflowXamlIsland', $null)
            if ($flyout -eq [IntPtr]::Zero) {
                '  TopLevelWindowForOverflowXamlIsland: NOT FOUND'
            } else {
                $fr = New-Object TrayHit.Win+RECT
                [void][TrayHit.Win]::GetWindowRect($flyout, [ref]$fr)
                "  TopLevelWindowForOverflowXamlIsland $flyout at $($fr.L),$($fr.T) $($fr.R - $fr.L)x$($fr.B - $fr.T) visible=$([TrayHit.Win]::IsWindowVisible($flyout))"
                try {
                    $froot = [System.Windows.Automation.AutomationElement]::FromHandle($flyout)
                    $fk = $froot.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                                         [System.Windows.Automation.Condition]::TrueCondition)
                    "  its UIA descendants: $($fk.Count)"
                    foreach ($e in $fk) {
                        $type = '?'; $name = ''; $id = ''
                        try { $type = $e.Current.ControlType.ProgrammaticName -replace 'ControlType.', '' } catch { }
                        try { $name = $e.Current.Name } catch { }
                        try { $id = $e.Current.AutomationId } catch { }
                        "      $type '$name' id='$id' {0}" -f (Format-Rect $e)
                    }
                } catch {
                    "  UIA from that handle failed: $($_.Exception.Message)"
                }
            }
            # Put it away again: the same chevron is a toggle.
            $pattern.Invoke()
            Start-Sleep -Milliseconds 300
            '  (chevron pressed again: the flyout is a toggle)'
        } else {
            '  the chevron has no InvokePattern'
        }
    }
}

'--- restoring: no layered style, taskbar hidden again ---'
if (-not $hadLayered) {
    [void][TrayHit.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
    [void][TrayHit.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]$ex)
}
[void][TrayHit.Win]::ShowWindow($tray, 0)
Start-Sleep -Milliseconds 400
Save-Band 'tray-state-restored.png'
'=== done ==='
