# =====================================================================
#  probe-tray-click-route.ps1 -- can a tray icon be pressed BY POSITION, without
#  moving the user's pointer and without the taskbar being seen?
#
#  WHY THIS EXISTS
#
#  Pressing an icon in the bar's tray card needs the shell to do it, and the route through UI
#  Automation has a hole in the middle: the shell's buttons are named after the APPLICATION
#  (" Weixin", "QQ Yinyue <tagline> ...") while the bar's names come from the registry and are often just the
#  executable ("Weixin.exe"). Matching the two by picture was tried and does not work either -- a screen
#  grab of the overflow flyout comes back black, because that panel is drawn by the compositor (the
#  taskbar itself photographs fine, which is what made the picture idea look plausible).
#
#  What the shell CAN answer, per icon, is where the icon is:
#
#      Shell_NotifyIconGetRect(NOTIFYICONIDENTIFIER)
#
#  and its identifier is the same identity the registry entry carries (IconGuid, or hWnd + UID). That
#  would make the mapping unnecessary. The two questions this script asks:
#
#    A. does Shell_NotifyIconGetRect work for the entries that carry an IconGuid -- and does it still
#       work with the taskbar made invisible (WS_EX_LAYERED + alpha 0), which is how we keep the flash
#       away? Does it work for an icon that is in the OVERFLOW (not displayed) once the flyout is open?
#    B. does a POSTED click (WM_LBUTTONDOWN/UP to the taskbar's own window, no cursor movement) reach a
#       tray button? Tested on the overflow chevron, which is harmless: it opens the flyout, and the
#       same click closes it.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file. Run with the bar stopped: the bar
#  re-hides the taskbar every few seconds.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace TrayClick -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
[StructLayout(LayoutKind.Sequential)] public struct NOTIFYICONIDENTIFIER { public int cbSize; public IntPtr hWnd; public uint uID; public Guid guidItem; }
[DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref NOTIFYICONIDENTIFIER id, out RECT rect);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
'@

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP   = 0x0202
$MK_LBUTTON     = 0x0001

function New-Identifier($guid) {
    $id = New-Object TrayClick.Win+NOTIFYICONIDENTIFIER
    $id.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type]('TrayClick.Win+NOTIFYICONIDENTIFIER'))
    $id.hWnd = [IntPtr]::Zero
    $id.uID = 0
    $id.guidItem = $guid
    return $id
}

$tray = [TrayClick.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }
$hadLayered = (([TrayClick.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64() -band $WS_EX_LAYERED) -ne 0)
$ex = [TrayClick.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()

function Show-Invisible {
    [void][TrayClick.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
    [void][TrayClick.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
    [void][TrayClick.Win]::ShowWindow($tray, 8)
    Start-Sleep -Milliseconds 800
}

'--- the registry entries, with the identity each one carries ---'
$withGuid = @(); $withUid = @()
foreach ($e in Get-ChildItem 'HKCU:\Control Panel\NotifyIconSettings') {
    $item = Get-Item $e.PSPath
    $tip = [string]$item.GetValue('InitialTooltip')
    $path = [string]$item.GetValue('ExecutablePath')
    $name = Split-Path $path -Leaf
    if ($tip -ne '') { $name = $tip }
    $g = $item.GetValue('IconGuid')
    $u = $item.GetValue('UID')
    if ($g) {
        # The registry stores it as the TEXT "{BA82E2DC-F405-...}" (measured) -- not as 16 bytes, which is
        # what the first version of this script assumed and then failed to convert.
        $guid = if ($g -is [byte[]]) { New-Object Guid (,$g) } else { [Guid]::Parse(([string]$g).Trim('{','}')) }
        $withGuid += [pscustomobject]@{ name = $name; guid = $guid; key = $e.PSChildName }
    } elseif ($null -ne $u) {
        $withUid += [pscustomobject]@{ name = $name; uid = $u; key = $e.PSChildName }
    }
}
"  {0} entr(ies) with an IconGuid, {1} with a UID (a UID alone cannot be queried: no hWnd)" -f $withGuid.Count, $withUid.Count

'--- A. Shell_NotifyIconGetRect, taskbar hidden and then shown-invisible ---'
$hidden = @{}
foreach ($w in $withGuid) {
    $r = New-Object TrayClick.Win+RECT
    $hr = [TrayClick.Win]::Shell_NotifyIconGetRect([ref](New-Identifier $w.guid), [ref]$r)
    $hidden[$w.key] = "0x{0:X8}" -f $hr
}
$shown = @{}
Show-Invisible
foreach ($w in $withGuid) {
    $r = New-Object TrayClick.Win+RECT
    $hr = [TrayClick.Win]::Shell_NotifyIconGetRect([ref](New-Identifier $w.guid), [ref]$r)
    if ($hr -eq 0) {
        $shown[$w.key] = ('{0},{1} {2}x{3}' -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T))
    } else {
        $shown[$w.key] = ('0x{0:X8}' -f $hr)
    }
}
foreach ($w in $withGuid) {
    "  '{0}'" -f $w.name.Replace("`n", ' / ')
    "      hidden: {0}" -f $hidden[$w.key]
    "      shown-invisible: {0}" -f $shown[$w.key]
}

'--- B. a POSTED click on the overflow chevron (no cursor movement) ---'
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
if ($null -eq $chevron) {
    '  the tray never published its buttons'
} else {
    $cr = $chevron.Current.BoundingRectangle
    $cx = [int]($cr.Left + $cr.Width / 2); $cy = [int]($cr.Top + $cr.Height / 2)
    "  chevron centre $cx,$cy (screen) -> posting a click"
    $pt = New-Object TrayClick.Pt+POINT
    $pt.X = $cx; $pt.Y = $cy
    [void][TrayClick.Win]::ScreenToClient($tray, [ref]$pt)
    $lp = [IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))
    [void][TrayClick.Win]::PostMessageW($tray, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
    Start-Sleep -Milliseconds 60
    [void][TrayClick.Win]::PostMessageW($tray, $WM_LBUTTONUP, [IntPtr]::Zero, $lp)
    Start-Sleep -Milliseconds 900
    $flyoutCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'TopLevelWindowForOverflowXamlIsland')
    $found = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $flyoutCond)
    if ($found.Count -gt 0) {
        "  the flyout OPENED from a posted click: {0}" -f $found[0].Current.BoundingRectangle
        "  (posting the same click again to close it)"
        [void][TrayClick.Win]::PostMessageW($tray, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
        Start-Sleep -Milliseconds 60
        [void][TrayClick.Win]::PostMessageW($tray, $WM_LBUTTONUP, [IntPtr]::Zero, $lp)
        Start-Sleep -Milliseconds 400
    } else {
        '  the flyout did NOT open: a posted click is not accepted there'
    }
}

'--- restoring ---'
if (-not $hadLayered) {
    [void][TrayClick.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
    [void][TrayClick.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]$ex)
}
[void][TrayClick.Win]::ShowWindow($tray, 0)
'=== done ==='
