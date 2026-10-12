# =====================================================================
#  probe-tray-band-windows.ps1 -- WHERE are the promoted tray icons?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-band-windows.ps1
#
#  WHY THIS EXISTS
#
#  The dump of Shell_TrayWnd's UI Automation tree (probe-tray-uia.ps1) shows the taskbar's own
#  buttons and exactly six "SystemTrayIcon" buttons -- the chevron, the input indicator, the network,
#  the volume, the clock and show-desktop. The APPLICATION icons that are drawn in the tray proper
#  (WeChat, QQ Music, Clash ... ) are NOT in that tree, although they are on screen inside the same
#  band: a photograph of the shown taskbar has them, at x 2280-2410, and the tree has nothing between
#  the last pinned-app button (x 1547) and the chevron (x 2416).
#
#  So they are another window's business, and pressing one from the bar means knowing which. This
#  script enumerates the windows that share the band, and dumps the UI Automation children of each,
#  with the patterns each child supports -- because the only useful one here is Invoke.
#
#  The taskbar is shown with WS_EX_LAYERED alpha 0, so the probe leaves nothing on the screen.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace TrayBand -Name Win -MemberDefinition @'
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
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
'@

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002

function Get-Rect($element) {
    try {
        $r = $element.Current.BoundingRectangle
        if ($r.IsEmpty) { return '(empty)' }
        return ('{0},{1} {2}x{3}' -f [int]$r.Left, [int]$r.Top, [int]$r.Width, [int]$r.Height)
    } catch { return '(none)' }
}

function Get-Patterns($element) {
    $names = @()
    foreach ($p in @(
            [System.Windows.Automation.InvokePattern]::Pattern,
            [System.Windows.Automation.LegacyIAccessiblePattern]::Pattern,
            [System.Windows.Automation.ExpandCollapsePattern]::Pattern)) {
        try {
            $o = $null
            if ($element.TryGetCurrentPattern($p, [ref]$o)) {
                $names += ($p.ProgrammaticName -replace 'PatternIdentifiers.Pattern', '')
            }
        } catch { }
    }
    if ($names.Count -eq 0) { return '-' }
    return ($names -join ',')
}

$tray = [TrayBand.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }

$hadLayered = (([TrayBand.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64() -band $WS_EX_LAYERED) -ne 0)
$ex = [TrayBand.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayBand.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TrayBand.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
[void][TrayBand.Win]::ShowWindow($tray, 8)   # SW_SHOWNA
Start-Sleep -Milliseconds 900
"Shell_TrayWnd $tray shown invisible; looking for every window in the top band"

# Windows whose rectangle overlaps the taskbar's band, whether visible to the eye or not.
$script:bandHits = @()
$cb = [TrayBand.Win+EnumProc]{
    param([IntPtr]$h, [IntPtr]$p)
    $r = New-Object TrayBand.Win+RECT
    if ([TrayBand.Win]::GetWindowRect($h, [ref]$r)) {
        if ($r.T -lt 40 -and $r.B -gt 0 -and ($r.R - $r.L) -gt 0) {
            $cls = New-Object System.Text.StringBuilder 256
            [void][TrayBand.Win]::GetClassName($h, $cls, 256)
            $txt = New-Object System.Text.StringBuilder 256
            [void][TrayBand.Win]::GetWindowTextW($h, $txt, 256)
            # NOT $pid: PowerShell's own $PID is read-only, and assigning it throws (found the hard way).
            $procId = 0
            [void][TrayBand.Win]::GetWindowThreadProcessId($h, [ref]$procId)
            $script:bandHits += [pscustomobject]@{
                hwnd = $h; cls = $cls.ToString(); title = $txt.ToString(); pid = $procId;
                rect = ('{0},{1} {2}x{3}' -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T));
                vis = [TrayBand.Win]::IsWindowVisible($h)
            }
        }
    }
    return $true
}
[void][TrayBand.Win]::EnumWindows($cb, [IntPtr]::Zero)

"--- $($script:bandHits.Count) window(s) in the band ---"
foreach ($w in $script:bandHits | Sort-Object { $_.hwnd.ToInt64() }) {
    "  hwnd $($w.hwnd) pid $($w.pid) vis=$($w.vis) [$($w.cls)] '$($w.title)' $($w.rect)"
}

'--- UI Automation children of each (the app icons have to be in one of them) ---'
foreach ($w in $script:bandHits | Sort-Object { $_.hwnd.ToInt64() }) {
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($w.hwnd)
        $kids = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                              [System.Windows.Automation.Condition]::TrueCondition)
        if ($kids.Count -eq 0) { continue }
        "  [$($w.cls)] hwnd $($w.hwnd): $($kids.Count) element(s)"
        foreach ($k in $kids) {
            $type = '?'; $name = ''; $id = ''
            try { $type = $k.Current.ControlType.ProgrammaticName -replace 'ControlType.', '' } catch { }
            try { $name = $k.Current.Name } catch { }
            try { $id = $k.Current.AutomationId } catch { }
            if ($name -eq '' -and $id -eq '') { continue }
            "      $type '$name' id='$id' $(Get-Rect $k) [$(Get-Patterns $k)]"
        }
    } catch { }
}

'--- restoring ---'
if (-not $hadLayered) {
    [void][TrayBand.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
    [void][TrayBand.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]$ex)
}
[void][TrayBand.Win]::ShowWindow($tray, 0)
'=== done ==='
