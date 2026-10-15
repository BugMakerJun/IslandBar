# =====================================================================
#  probe-tray-uia.ps1 -- what does the Windows 11 system tray expose?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-uia.ps1
#
#  WHY THIS EXISTS
#
#  The bar is to show the system tray at the right of its icon row, and the classic
#  way to read the tray -- walking Shell_TrayWnd to ToolbarWindow32 and reading the
#  buttons out of Explorer's memory -- is dead on this machine (build 26220): that
#  subtree does not exist any more, because the Windows 11 tray is not a toolbar.
#
#  So the question is what the tray IS, and the answer has to be measured rather than
#  guessed: UI Automation is the one interface a shell surface is obliged to publish,
#  and the bar's own WeChat probe already uses it successfully on this machine.
#
#  WHAT IT PRINTS, for every element it finds under the tray:
#
#    depth, control type, name, automation id, screen rectangle, and which UIA
#    patterns the element supports -- because "can I draw it" and "can I press it"
#    are different questions, and only the last one decides whether pressing an icon
#    in the bar can open the menu it belongs to.
#
#  Read-only: it enumerates and prints. It clicks nothing.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

# -ShowTaskbar: show the shell's taskbar for the duration of the probe, then put it back.
#
# It is normally HIDDEN -- the bar hides it, and that is the whole reason it has to be shown
# here: with the taskbar hidden its children and its UI Automation subtree are gone, so a probe
# run against the hidden window reads an empty shell and concludes the tray does not exist.
# (The hide is a plain ShowWindow(SW_HIDE) -- see NativeTaskbar::Hide -- so showing it again is
# a plain ShowWindow(SW_SHOW) and no Explorer restart is needed.)
param([switch]$ShowTaskbar)

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace TrayProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string win);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
'@

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
            [System.Windows.Automation.TogglePattern]::Pattern,
            [System.Windows.Automation.ExpandCollapsePattern]::Pattern,
            [System.Windows.Automation.SelectionItemPattern]::Pattern)) {
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

function Dump-Tree($element, [int]$depth, [int]$maxDepth) {
    if ($depth -gt $maxDepth) { return }
    $indent = '  ' * $depth
    $type = '?'
    $name = ''
    $id = ''
    try { $type = $element.Current.ControlType.ProgrammaticName -replace 'ControlType.', '' } catch { }
    try { $name = $element.Current.Name } catch { }
    try { $id = $element.Current.AutomationId } catch { }
    '{0}{1} "{2}" id="{3}" {4} [{5}]' -f $indent, $type, $name, $id, (Get-Rect $element), (Get-Patterns $element)

    $children = $null
    try { $children = $element.FindAll([System.Windows.Automation.TreeScope]::Children,
                                       [System.Windows.Automation.Condition]::TrueCondition) } catch { }
    if ($children) {
        foreach ($child in $children) { Dump-Tree $child ($depth + 1) $maxDepth }
    }
}

'=== the shell tray windows ==='
foreach ($cls in @('Shell_TrayWnd', 'Shell_SecondaryTrayWnd')) {
    $hwnd = [TrayProbe.Win]::FindWindow($cls, $null)
    if ($hwnd -eq [IntPtr]::Zero) { "  $cls : NOT FOUND"; continue }
    $wasVisible = [TrayProbe.Win]::IsWindowVisible($hwnd)
    if ($ShowTaskbar -and -not $wasVisible) {
        [void][TrayProbe.Win]::ShowWindow($hwnd, 5)   # SW_SHOW
        $script:taskbarWasHidden = $true
        Start-Sleep -Milliseconds 1200
        "  (shown for the probe; it was hidden, and will be hidden again at the end)"
    }
    $r = New-Object TrayProbe.Win+RECT
    [void][TrayProbe.Win]::GetWindowRect($hwnd, [ref]$r)
    "  $cls : hwnd $hwnd at $($r.L),$($r.T) $($r.R - $r.L)x$($r.B - $r.T) visible=$([TrayProbe.Win]::IsWindowVisible($hwnd))"

    # The children of the taskbar, by class: the notifications area used to be
    # TrayNotifyWnd, and knowing which child holds what is the first half of the answer.
    $child = [TrayProbe.Win]::FindWindowEx($hwnd, [IntPtr]::Zero, $null, $null)
    while ($child -ne [IntPtr]::Zero) {
        $sb = New-Object System.Text.StringBuilder 128
        [void][TrayProbe.Win]::GetClassName($child, $sb, 128)
        $cr = New-Object TrayProbe.Win+RECT
        [void][TrayProbe.Win]::GetWindowRect($child, [ref]$cr)
        "      child: $($sb.ToString()) hwnd $child at $($cr.L),$($cr.T) $($cr.R - $cr.L)x$($cr.B - $cr.T)"
        $child = [TrayProbe.Win]::FindWindowEx($hwnd, $child, $null, $null)
    }

    '=== UI Automation under it (depth <= 6) ==='
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        Dump-Tree $root 1 6
    } catch {
        "  UI Automation failed: $($_.Exception.Message)"
    }
}

'=== done ==='

# Put the taskbar back the way it was found, so a probe leaves no trace on the desktop.
if ($ShowTaskbar) {
    $hwnd = [TrayProbe.Win]::FindWindow('Shell_TrayWnd', $null)
    if ($hwnd -ne [IntPtr]::Zero -and $taskbarWasHidden) {
        [void][TrayProbe.Win]::ShowWindow($hwnd, 0)   # SW_HIDE
        '  (the taskbar is hidden again, as it was)'
    }
}
