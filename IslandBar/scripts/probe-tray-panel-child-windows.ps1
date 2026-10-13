# =====================================================================
#  probe-tray-panel-child-windows.ps1 -- WHICH window paints the shell's overflow panel?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-panel-child-windows.ps1
#
#  WHY THIS EXISTS
#
#  The press of a tray icon happens inside the shell's overflow panel, and the panel is visible for the
#  length of that press. Two measurements say the panel cannot be hidden the way the taskbar is:
#
#    * `WS_EX_LAYERED` + alpha 0 on the FLYOUT WINDOW does nothing (it works on the taskbar);
#    * moving that window moves the ICONS' rectangles (UI Automation follows) and leaves the pixels on
#      screen where they were.
#
#  Those two are only consistent if the pixels are painted by a DIFFERENT window -- and the search for it
#  (probe-tray-panel-windows.ps1) enumerated TOP-LEVEL windows only. Windows 11 hosts the shell's XAML
#  islands in a CHILD window (the "Windows.UI.Composition.DesktopWindowContentBridge" family), and a
#  layered attribute on a parent does not touch a child that paints its own DirectComposition content --
#  which is exactly the shape of both measurements above.
#
#  So this walks the flyout's OWN window tree (children, grandchildren), prints it, and then TRIES the two
#  ways of making a window disappear, ONE WINDOW AT A TIME, measuring the panel's rectangle after each
#  attempt:
#
#    * `WS_EX_LAYERED` + alpha 0;
#    * `DwmSetWindowAttribute(DWMWA_CLOAK, TRUE)` -- the flag DWM itself uses to keep a window of another
#      virtual desktop off the screen, which works on composition content where the layered attribute does
#      not.
#
#  HOW THE RESULT IS READ, without asking anybody to look: a screen grab of the OPEN panel comes back
#  BLACK (measured when the matching idea was tried -- the island's pixels are not in the grab), while the
#  same rectangle with the panel away is the wallpaper. So the mean brightness of that rectangle is a
#  usable signal: baseline (wallpaper) -> panel up (black) -> and if a hiding attempt puts the mean back
#  to the baseline, that attempt WORKED.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace TrayChild -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
[DllImport("user32.dll")] public static extern bool InvalidateRect(IntPtr h, IntPtr r, bool erase);
[DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int value, int size);
'@

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002
$DWMWA_CLOAK   = 14

function Get-Mean($rect) {
    # Sample every 4th pixel of the rectangle: enough to tell wallpaper from the panel's flat dark fill.
    # The struct's fields are L/T/R/B (the same names the P/Invoke declares), not Left/Top/Right/Bottom.
    $w = [int]($rect.R - $rect.L)
    $h = [int]($rect.B - $rect.T)
    if ($w -lt 8 -or $h -lt 8) { return -1 }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.L, $rect.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $sum = 0.0
    $n = 0
    for ($y = 0; $y -lt $h; $y += 4) {
        for ($x = 0; $x -lt $w; $x += 4) {
            $c = $bmp.GetPixel($x, $y)
            $sum += ($c.R + $c.G + $c.B) / 3.0
            $n++
        }
    }
    $g.Dispose(); $bmp.Dispose()
    if ($n -eq 0) { return -1 }
    return [math]::Round($sum / $n, 2)
}

function Save-Shot($rect, $name) {
    $w = [int]($rect.R - $rect.L)
    $h = [int]($rect.B - $rect.T)
    if ($w -lt 8 -or $h -lt 8) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.L, $rect.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $dir = 'C:\PROJECT\Taskbar\IslandBar\captures'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $bmp.Save((Join-Path $dir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

# Is the panel still on screen? Asked of the shell, not of the pixels: the element's rectangle, which is
# what the press matches on. Without this, "the pixels changed" and "the panel closed" look the same.
function Panel-Alive($element) {
    try {
        $r = $element.Current.BoundingRectangle
        return ($r.Width -gt 20 -and $r.Height -gt 20)
    } catch { return $false }
}

function Get-Tree($parent, $depth) {
    $out = @()
    $kids = @()
    $cb = [TrayChild.Win+EnumProc]{
        param([IntPtr]$h, [IntPtr]$p)
        $script:kids += $h
        return $true
    }
    $script:kids = @()
    [void][TrayChild.Win]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    foreach ($h in $script:kids) {
        $cls = New-Object System.Text.StringBuilder 256
        [void][TrayChild.Win]::GetClassName($h, $cls, 256)
        $r = New-Object TrayChild.Win+RECT
        [void][TrayChild.Win]::GetWindowRect($h, [ref]$r)
        $procId = 0
        [void][TrayChild.Win]::GetWindowThreadProcessId($h, [ref]$procId)
        $out += [pscustomobject]@{
            hwnd = $h; depth = $depth; cls = $cls.ToString();
            vis = [TrayChild.Win]::IsWindowVisible($h);
            rect = ('{0},{1} {2}x{3}' -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T));
            pid = $procId
        }
        if ($depth -lt 3) { $out += Get-Tree $h ($depth + 1) }
    }
    return $out
}

$tray = [TrayChild.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }

# The panel only exists while the tray is laid out, so the taskbar is made present -- invisibly, the way
# the bar itself does it. It is put back at the end either way.
$ex = [TrayChild.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayChild.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TrayChild.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
[void][TrayChild.Win]::ShowWindow($tray, 8)   # SW_SHOWNA
Start-Sleep -Milliseconds 400

$root = [System.Windows.Automation.AutomationElement]::FromHandle($tray)
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'SystemTrayIcon')
$chevron = $null
for ($try = 0; $try -lt 60 -and $null -eq $chevron; $try++) {
    foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
        if ($null -eq $chevron -or
            $e.Current.BoundingRectangle.Left -lt $chevron.Current.BoundingRectangle.Left) {
            $chevron = $e
        }
    }
    if ($null -eq $chevron) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $chevron) {
    'no chevron'; [void][TrayChild.Win]::ShowWindow($tray, 0); exit 1
}

# The PANEL's own rectangle, taken from where the shell says the chevron is; the band above used to be
# measured whole, which includes the taskbar's own dark strip and made every reading ambiguous.
$panelRect = New-Object TrayChild.Win+RECT
$panelRect.L = [int]$chevron.Current.BoundingRectangle.Left
$panelRect.T = 32
$panelRect.R = 2544
$panelRect.B = 200
$baseline = Get-Mean $panelRect
"baseline (panel away): $baseline"
Save-Shot $panelRect 'probe-panel-away.png'

$pattern = $null
[void]$chevron.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)
$pattern.Invoke()
$flyoutCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty,
    'TopLevelWindowForOverflowXamlIsland')
$flyout = $null
for ($try = 0; $try -lt 60 -and $null -eq $flyout; $try++) {
    $found = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $flyoutCond)
    if ($found.Count -gt 0) { $flyout = $found[0] }
    if ($null -eq $flyout) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $flyout) {
    'the panel did not open'
    [void][TrayChild.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
    [void][TrayChild.Win]::ShowWindow($tray, 0)
    exit 1
}
Start-Sleep -Milliseconds 700
$open = Get-Mean $panelRect
"panel open: $open"
Save-Shot $panelRect 'probe-panel-open.png'

# A control with no trick applied: if the panel closes by itself while this runs, every later reading is
# meaningless -- and that is the one way the earlier run of this probe could have fooled itself.
Start-Sleep -Milliseconds 800
"control (nothing done, 800 ms later): " + (Get-Mean $panelRect) + "  panel alive: " + (Panel-Alive $flyout)

# NOT `$host`: that is a read-only automatic variable in PowerShell (the host object), and assigning to
# it throws rather than telling you your name is taken.
$flyoutHwnd = [IntPtr]$flyout.Current.NativeWindowHandle
"the panel's own window: $flyoutHwnd"

"--- the flyout's window tree ---"
$tree = Get-Tree $flyoutHwnd 1
foreach ($w in $tree) {
    "  depth $($w.depth) hwnd $($w.hwnd) pid $($w.pid) vis=$($w.vis) [$($w.cls)] $($w.rect)"
}

# The attempts, one window at a time. A control first: the top-level alone, which is what the bar already
# does and which is known not to work -- if it DOES work here the earlier measurement was wrong.
$candidates = @([pscustomobject]@{ hwnd = $flyoutHwnd; why = 'the top-level (the control)' })
foreach ($w in $tree) { if ($w.vis) { $candidates += [pscustomobject]@{ hwnd = $w.hwnd; why = "child $($w.cls)" } } }

foreach ($c in $candidates) {
    $h = [IntPtr]$c.hwnd
    $ex = [TrayChild.Win]::GetWindowLongPtr($h, $GWL_EXSTYLE).ToInt64()
    [void][TrayChild.Win]::SetWindowLongPtr($h, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
    [void][TrayChild.Win]::SetLayeredWindowAttributes($h, 0, 0, $LWA_ALPHA)
    [void][TrayChild.Win]::InvalidateRect($h, [IntPtr]::Zero, $true)
    [void][TrayChild.Win]::UpdateWindow($h)
    Start-Sleep -Milliseconds 250
    $mean = Get-Mean $panelRect
    $alive = Panel-Alive $flyout
    "alpha 0 on $($c.why) [$h]: mean $mean  panel alive: $alive"
    Save-Shot $panelRect ("probe-panel-alpha-{0}.png" -f $c.hwnd)
    if ($alive -and $mean -gt ($baseline - 10)) { "  ^^ THAT ONE HID THE PANEL"; break }

    $one = 1
    [void][TrayChild.Win]::DwmSetWindowAttribute($h, $DWMWA_CLOAK, [ref]$one, 4)
    Start-Sleep -Milliseconds 250
    $mean = Get-Mean $panelRect
    $alive = Panel-Alive $flyout
    "cloak on $($c.why) [$h]: mean $mean  panel alive: $alive"
    Save-Shot $panelRect ("probe-panel-cloak-{0}.png" -f $c.hwnd)
    if ($alive -and $mean -gt ($baseline - 10)) { "  ^^ THAT ONE HID THE PANEL (cloak)"; break }
}

# Put everything back: opacity, cloaking, the panel, the taskbar.
foreach ($c in $candidates) {
    $h = [IntPtr]$c.hwnd
    [void][TrayChild.Win]::SetLayeredWindowAttributes($h, 0, 255, $LWA_ALPHA)
    $zero = 0
    [void][TrayChild.Win]::DwmSetWindowAttribute($h, $DWMWA_CLOAK, [ref]$zero, 4)
}
try { $pattern.Invoke() } catch { }
Start-Sleep -Milliseconds 400
"restored: " + (Get-Mean $panelRect) + "  panel alive: " + (Panel-Alive $flyout)
[void][TrayChild.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
[void][TrayChild.Win]::ShowWindow($tray, 0)
'done'
