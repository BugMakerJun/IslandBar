$ErrorActionPreference = 'Continue'

# =====================================================================
#  Is the WeChat call peer's name reachable through UI Automation?
#
#  Measured, during a real WeChat call:
#
#    * WeChat publishes NO GSMTC session for the call -- the only session was
#      QQMusic.exe -- so the media path cannot see it;
#    * the call window is a Qt window (class Qt51514QWindowIcon) whose title is
#      the fixed string for "audio/video call" and carries no peer name;
#    * that window has NO child windows, because Qt draws its UI itself.
#
#  The reference design shows the peer's name and avatar, so the name has to
#  come from somewhere. Qt ships an accessibility backend, and if it is enabled
#  the name is in the UIA tree even though it is in no window title.
#
#  This walks the tree and prints every element's name. Read-only.
# =====================================================================

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace W4 -Name N -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool QueryFullProcessImageName(IntPtr h, uint flags, System.Text.StringBuilder name, ref uint size);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
'@

function Get-PidName([uint32]$procId) {
    $h = [W4.N]::OpenProcess(0x1000, $false, $procId)
    if ($h -eq [IntPtr]::Zero) { return "" }
    try {
        $sb = New-Object System.Text.StringBuilder 512
        $sz = [uint32]512
        if ([W4.N]::QueryFullProcessImageName($h, 0, $sb, [ref]$sz)) { return (Split-Path $sb.ToString() -Leaf) }
        return ""
    } finally { [void][W4.N]::CloseHandle($h) }
}

# Find a visible Weixin.exe window that is phone-call sized.
$target = [IntPtr]::Zero
$cb = [W4.N+EnumProc]{
    param($h, $p)
    if (-not [W4.N]::IsWindowVisible($h)) { return $true }
    $procId = 0
    [void][W4.N]::GetWindowThreadProcessId($h, [ref]$procId)
    if ((Get-PidName $procId) -notmatch 'Weixin') { return $true }
    $r = New-Object W4.N+RECT
    [void][W4.N]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L
    $ht = $r.B - $r.T
    # The call window measured 376 x 648 at 1079,365. The main panel is much larger.
    if ($w -gt 200 -and $w -lt 600 -and $ht -gt 300) {
        if ([W4.N]::GetWindowTextLength($h) -gt 0) { $script:target = $h }
    }
    return $true
}
[void][W4.N]::EnumWindows($cb, [IntPtr]::Zero)

if ($target -eq [IntPtr]::Zero) {
    Write-Output "no call-sized visible Weixin window found -- is a call running?"
    exit 0
}

$r = New-Object W4.N+RECT
[void][W4.N]::GetWindowRect($target, [ref]$r)
Write-Output ("target window: " + $target + "  rect=" + $r.L + "," + $r.T + "," + $r.R + "," + $r.B)
Write-Output ""

Write-Output "=== UI Automation tree ==="
try {
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($target)
    if (-not $root) { Write-Output "  FromHandle returned null"; exit 0 }

    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $script:count = 0

    function Walk($el, $depth) {
        if (-not $el) { return }
        if ($script:count -ge 120) { return }
        $script:count++
        $pad = "  " + (" " * ($depth * 2))
        $name = ""
        $cls = ""
        $ctype = ""
        try { $name = $el.Current.Name } catch { }
        try { $cls = $el.Current.ClassName } catch { }
        try { $ctype = $el.Current.ControlType.ProgrammaticName } catch { }
        if ($name.Length -gt 0 -or $depth -le 2) {
            Write-Output ($pad + $ctype + "  class='" + $cls + "'  name='" + $name + "'")
        }
        if ($depth -ge 6) { return }
        $child = $walker.GetFirstChild($el)
        while ($child -and $script:count -lt 120) {
            Walk $child ($depth + 1)
            $child = $walker.GetNextSibling($child)
        }
    }

    Walk $root 0
    Write-Output ("  (elements walked: " + $script:count + ")")
} catch {
    Write-Output ("  UIA failed: " + $_.Exception.Message)
}
