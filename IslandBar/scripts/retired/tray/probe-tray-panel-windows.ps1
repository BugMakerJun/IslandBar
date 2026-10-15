# =====================================================================
#  probe-tray-panel-windows.ps1 -- WHICH window shows the overflow panel?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-panel-windows.ps1
#
#  WHY THIS EXISTS
#
#  Keeping the shell's overflow panel off the screen matters: the press happens in it, and the user asked
#  for an icon to be pressed, not to watch the shell's icon grid open on the way. Two things have been
#  measured so far, and they do not agree with each other:
#
#    * making it transparent does nothing -- `WS_EX_LAYERED` + alpha 0 works on the taskbar (its band
#      photographs as wallpaper) and leaves THIS window fully painted, because it is a XAML island drawn
#      through DirectComposition, which ignores the layered attributes;
#    * MOVING it works as far as the shell is concerned -- the icons' own rectangles move with it, which is
#      what the press matches on (-1632,89 for the WeChat icon, 4000 px to the left) -- and the panel is
#      STILL on the screen where it was.
#
#  Both of those can only be true if the pixels come from a window that is not the one moved. So this lists
#  every window that overlaps the panel's rectangle while it is open, with its class, owner process and
#  visibility: the painter is in that list.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace TrayPanel -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
[DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
'@

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002
$GW_OWNER      = 4

$tray = [TrayPanel.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }
$ex = [TrayPanel.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayPanel.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TrayPanel.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
[void][TrayPanel.Win]::ShowWindow($tray, 8)

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
if ($null -eq $chevron) { 'no chevron'; [void][TrayPanel.Win]::ShowWindow($tray, 0); exit 1 }

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
if ($null -eq $flyout) { 'the panel did not open'; [void][TrayPanel.Win]::ShowWindow($tray, 0); exit 1 }
Start-Sleep -Milliseconds 600

$fr = $flyout.Current.BoundingRectangle
"the panel's element says it is at {0},{1} {2}x{3}" -f [int]$fr.Left, [int]$fr.Top, [int]$fr.Width, [int]$fr.Height

"--- every window overlapping that rectangle ---"
$script:hits = @()
$cb = [TrayPanel.Win+EnumProc]{
    param([IntPtr]$h, [IntPtr]$p)
    $r = New-Object TrayPanel.Win+RECT
    if ([TrayPanel.Win]::GetWindowRect($h, [ref]$r)) {
        $overlaps = $r.L -lt ($fr.Left + $fr.Width) -and $r.R -gt $fr.Left -and
                    $r.T -lt ($fr.Top + $fr.Height) -and $r.B -gt $fr.Top
        if ($overlaps) {
            $cls = New-Object System.Text.StringBuilder 256
            [void][TrayPanel.Win]::GetClassName($h, $cls, 256)
            $procId = 0
            [void][TrayPanel.Win]::GetWindowThreadProcessId($h, [ref]$procId)
            $script:hits += [pscustomobject]@{
                hwnd = $h; cls = $cls.ToString(); vis = [TrayPanel.Win]::IsWindowVisible($h);
                rect = ('{0},{1} {2}x{3}' -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T));
                pid = $procId;
                owner = [TrayPanel.Win]::GetWindow($h, $GW_OWNER);
                ex = ([TrayPanel.Win]::GetWindowLongPtr($h, -20).ToInt64() -band 0x80000) -ne 0
            }
        }
    }
    return $true
}
[void][TrayPanel.Win]::EnumWindows($cb, [IntPtr]::Zero)
foreach ($w in $script:hits) {
    "  hwnd $($w.hwnd) pid $($w.pid) vis=$($w.vis) layered=$($w.ex) [$($w.cls)] owner=$($w.owner) $($w.rect)"
}

'--- putting it all back ---'
$pattern.Invoke()
Start-Sleep -Milliseconds 400
[void][TrayPanel.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
[void][TrayPanel.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]$ex)
[void][TrayPanel.Win]::ShowWindow($tray, 0)
'=== done ==='
