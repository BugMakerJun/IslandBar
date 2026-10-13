# =====================================================================
#  probe-tray-menu-surface.ps1 -- is the tray icon's context menu INSIDE the overflow panel?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-menu-surface.ps1
#
#  WHY THIS EXISTS
#
#  The press opens the shell's overflow panel and then either invokes the button (a left click, semantic)
#  or synthesises a REAL right click at the icon (there is no right-click pattern in UI Automation). The
#  panel is now made invisible -- layered alpha 0, applied while it slides and again once it has settled --
#  and with that in place a LEFT press still works ("-> pressed (shell calls it ' 微信')") while a RIGHT
#  press produces NO menu at all: a poll for `#32768` (the class of a Win32 popup menu) over the whole
#  press found nothing.
#
#  Two possible reasons, and they need different fixes:
#
#    A. the menu lives INSIDE the panel's own XAML -- then hiding the panel hides the menu by
#       construction, and "right click without the shell's panel" is impossible on this shell;
#    B. the panel, once layered, no longer takes the synthesised click -- then the menu is fine and the
#       ordering is what needs work.
#
#  So this probe does the opposite order from the bar: it makes the menu appear FIRST (panel visible
#  throughout), then hides the panel, then asks whether the menu is still there. It also prints every
#  top-level window that appears during the sequence, so the menu can be named even if its class is not
#  `#32768` -- Win11 draws some of the shell's menus through XAML.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace TrayMenu -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out System.Drawing.Point p);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, System.UIntPtr extra);
'@

$GWL_EXSTYLE = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA = 0x00000002

function Snapshot {
    # hwnd -> "class 'title' pid visible rect"
    $map = @{}
    $cb = [TrayMenu.Win+EnumProc]{
        param([IntPtr]$h, [IntPtr]$p)
        $cls = New-Object System.Text.StringBuilder 256
        [void][TrayMenu.Win]::GetClassName($h, $cls, 256)
        $txt = New-Object System.Text.StringBuilder 256
        [void][TrayMenu.Win]::GetWindowText($h, $txt, 256)
        $r = New-Object TrayMenu.Win+RECT
        [void][TrayMenu.Win]::GetWindowRect($h, [ref]$r)
        $procId = 0
        [void][TrayMenu.Win]::GetWindowThreadProcessId($h, [ref]$procId)
        $script:map[$h] = ('{0} ''{1}'' pid {2} vis={3} {4},{5}-{6},{7}' -f $cls.ToString(),
                           $txt.ToString(), $procId, [TrayMenu.Win]::IsWindowVisible($h),
                           $r.L, $r.T, $r.R, $r.B)
        return $true
    }
    $script:map = @{}
    [void][TrayMenu.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:map
}

$tray = [TrayMenu.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }
$before = Snapshot

$ex = [TrayMenu.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayMenu.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TrayMenu.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
[void][TrayMenu.Win]::ShowWindow($tray, 8)
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
if ($null -eq $chevron) { 'no chevron'; [void][TrayMenu.Win]::ShowWindow($tray, 0); exit 1 }

$invoke = $null
[void]$chevron.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invoke)
$invoke.Invoke()
Start-Sleep -Milliseconds 900   # the panel slides in and settles, and is LEFT VISIBLE on purpose

$flyoutCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty,
    'TopLevelWindowForOverflowXamlIsland')
$flyout = $null
for ($try = 0; $try -lt 40 -and $null -eq $flyout; $try++) {
    $found = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $flyoutCond)
    if ($found.Count -gt 0) { $flyout = $found[0] }
    if ($null -eq $flyout) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $flyout) { 'the panel did not open'; [void][TrayMenu.Win]::ShowWindow($tray, 0); exit 1 }

# The icon to right click: the first application icon in the panel, found by its automation id.
$iconCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'NotifyItemIcon')
$icon = $null
for ($try = 0; $try -lt 40 -and $null -eq $icon; $try++) {
    $found = $flyout.FindAll([System.Windows.Automation.TreeScope]::Descendants, $iconCond)
    if ($found.Count -gt 0) { $icon = $found[0] }
    if ($null -eq $icon) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $icon) { 'no icon in the panel'; [void][TrayMenu.Win]::ShowWindow($tray, 0); exit 1 }
$ir = $icon.Current.BoundingRectangle
$cx = [int]($ir.Left + $ir.Width / 2)
$cy = [int]($ir.Top + $ir.Height / 2)
"right clicking the icon at $cx,$cy (panel VISIBLE, nothing hidden yet)"

$saved = New-Object System.Drawing.Point
[void][TrayMenu.Win]::GetCursorPos([ref]$saved)
[void][TrayMenu.Win]::SetCursorPos($cx, $cy)
Start-Sleep -Milliseconds 60
[TrayMenu.Win]::mouse_event(0x0008, 0, 0, 0, [System.UIntPtr]::Zero)   # RIGHTDOWN
Start-Sleep -Milliseconds 60
[TrayMenu.Win]::mouse_event(0x0010, 0, 0, 0, [System.UIntPtr]::Zero)   # RIGHTUP
Start-Sleep -Milliseconds 700

$withMenu = Snapshot
"--- windows that appeared while the menu should be up ---"
$new = @()
foreach ($k in $withMenu.Keys) { if (-not $before.ContainsKey($k)) { $new += "  hwnd $k $($withMenu[$k])" } }
if ($new.Count -eq 0) { '  (none)' } else { $new }

# NOW hide the panel, exactly the way the bar does, and ask the same question again.
$panel = [IntPtr]$flyout.Current.NativeWindowHandle
$pex = [TrayMenu.Win]::GetWindowLongPtr($panel, $GWL_EXSTYLE).ToInt64()
[void][TrayMenu.Win]::SetWindowLongPtr($panel, $GWL_EXSTYLE, [IntPtr]($pex -bor $WS_EX_LAYERED))
[void][TrayMenu.Win]::SetLayeredWindowAttributes($panel, 0, 0, $LWA_ALPHA)
Start-Sleep -Milliseconds 600
$afterHide = Snapshot
"--- the same windows, after the panel was made invisible ---"
foreach ($line in $new) {
    $hwndText = ($line -split ' ')[2]
    if ($afterHide.ContainsKey([IntPtr]$hwndText)) { "  STILL THERE: $($afterHide[[IntPtr]$hwndText])" }
    else { "  GONE:        $line" }
}

# And the pixels: the menu's own rectangle, if one was found.
if ($new.Count -gt 0) {
    Add-Type -AssemblyName System.Drawing
    $menuLine = $new[0]
    $parts = $menuLine -split ' '
    $rectText = $parts[-1]
    $nums = $rectText -split '[,]'
    if ($nums.Count -ge 4) {
        $mx = [int]$nums[0]; $my = [int]$nums[1]
        $mw = [int]($nums[2] - $mx); $mh = [int]($nums[3] - $my)
        if ($mw -gt 8 -and $mh -gt 8) {
            $bmp = New-Object System.Drawing.Bitmap $mw, $mh
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($mx, $my, 0, 0, (New-Object System.Drawing.Size $mw, $mh))
            $dir = 'C:\PROJECT\Taskbar\IslandBar\captures'
            if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
            $bmp.Save((Join-Path $dir 'probe-menu-after-hide.png'), [System.Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
            "saved: $dir\probe-menu-after-hide.png"
        }
    }
}

"esc sent to dismiss whatever is up"
[void][TrayMenu.Win]::SetCursorPos($saved.X, $saved.Y)
