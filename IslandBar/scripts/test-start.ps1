# =====================================================================
#  test-start.ps1 -- find out which way of opening the Start menu works
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-start.ps1
#
#  Windows 11 hosts the Start menu in a XAML process, so the Windows 7 route
#  (WM_SYSCOMMAND with SC_TASKLIST) does nothing at all, and the "Start" child
#  window of Shell_TrayWnd that older code looks for does not exist either.
#
#  What almost every utility does instead is synthesize the Windows key: the Start
#  menu IS the Windows key's action, so the most reliable way to open it is to send
#  the same keystroke the keyboard sends. That is a claim worth checking rather than
#  assuming, which is what this script is for.
#
#  It tries each route in turn, waits, and reports what appeared -- so the answer
#  comes from the machine rather than from memory.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [int]$WaitMs = 1500
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace StartProbe -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern IntPtr GetShellWindow();

public const byte VK_LWIN = 0x5B;
public const byte VK_CONTROL = 0x11;
public const uint KEYEVENTF_KEYUP = 0x0002;
'@

function Get-ShellSnapshot {
    # Everything that could appear when the Start menu opens. Windows 11 puts it in
    # a XAML host whose class name has changed between builds, so the NAMES are not
    # relied on: what is compared is which windows are visible before and after.
    $names = @()
    foreach ($cls in 'Windows.UI.Core.CoreWindow', 'XamlExplorerHostIslandWindow',
                     'Shell_StartMenu', 'Windows.UI.Composition.DesktopWindowContentBridge',
                     'Start', 'Shell_TrayWnd', 'Progman', 'WorkerW') {
        $h = [StartProbe.Native]::FindWindow($cls, $null)
        $names += ('{0}={1}' -f $cls, $h)
    }
    $fg = [StartProbe.Native]::GetForegroundWindow()
    $sb = New-Object System.Text.StringBuilder 128
    [StartProbe.Native]::GetClassName($fg, $sb, 128) | Out-Null
    $names += ('foreground={0}' -f $sb.ToString())
    return ($names -join '; ')
}

Write-Host '=== which route opens the Windows 11 Start menu? ===' -ForegroundColor Cyan
Write-Host ''
Write-Host 'before:'
Write-Host "  $(Get-ShellSnapshot)"
Write-Host ''

# --- route 1: the Windows key, synthesized --------------------------------
Write-Host '[1] sending the Windows key (with a Ctrl tap first, to clear any stuck modifier)...'
# A modifier already held turns Win into a shortcut instead of the Start action, so
# a plain Ctrl tap first is the defence: it costs nothing and it is what the
# utilities that get this wrong are missing.
[StartProbe.Native]::keybd_event([StartProbe.Native]::VK_CONTROL, 0, 0, [IntPtr]::Zero)
[StartProbe.Native]::keybd_event([StartProbe.Native]::VK_CONTROL, 0, [StartProbe.Native]::KEYEVENTF_KEYUP, [IntPtr]::Zero)
Start-Sleep -Milliseconds 60
[StartProbe.Native]::keybd_event([StartProbe.Native]::VK_LWIN, 0, 0, [IntPtr]::Zero)
[StartProbe.Native]::keybd_event([StartProbe.Native]::VK_LWIN, 0, [StartProbe.Native]::KEYEVENTF_KEYUP, [IntPtr]::Zero)

Start-Sleep -Milliseconds $WaitMs
Write-Host 'after:'
Write-Host "  $(Get-ShellSnapshot)"
Write-Host ''

# Close it again with the same key, so the probe leaves the machine as it found it.
Write-Host '[1b] sending the Windows key again to close whatever opened...'
[StartProbe.Native]::keybd_event([StartProbe.Native]::VK_LWIN, 0, 0, [IntPtr]::Zero)
[StartProbe.Native]::keybd_event([StartProbe.Native]::VK_LWIN, 0, [StartProbe.Native]::KEYEVENTF_KEYUP, [IntPtr]::Zero)
Start-Sleep -Milliseconds $WaitMs
Write-Host 'after:'
Write-Host "  $(Get-ShellSnapshot)"
Write-Host ''

Write-Host 'Read the three snapshots above.' -ForegroundColor Cyan
Write-Host 'If a window appeared and then disappeared, the Windows key route works and'
Write-Host 'the class name of the host tells us what to look for when deciding whether'
Write-Host 'the menu is already open.'
