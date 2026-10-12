$ErrorActionPreference = 'Continue'

# =====================================================================
#  Find the WeChat call window WITHOUT matching a Chinese literal.
#
#  The previous attempt searched for a window whose title contained the
#  Chinese phrase for "audio/video call", and found nothing -- even though a
#  window with that exact title had just been enumerated. The cause is the
#  reason this project's scripts are kept ASCII: Windows PowerShell 5.1 reads
#  a .ps1 with the system ANSI codepage unless it carries a UTF-8 BOM, so the
#  Chinese literal in the script was mangled before the comparison ran.
#
#  So this matches by NUMBER OF CHARACTERS and by process instead. Read-only.
# =====================================================================

Add-Type -Namespace W3 -Name N -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
public delegate bool EnumChildProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool QueryFullProcessImageName(IntPtr h, uint flags, System.Text.StringBuilder name, ref uint size);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
'@

function Get-PidName([uint32]$procId) {
    $h = [W3.N]::OpenProcess(0x1000, $false, $procId)
    if ($h -eq [IntPtr]::Zero) { return ("pid " + $procId + " (open denied)") }
    try {
        $sb = New-Object System.Text.StringBuilder 512
        $sz = [uint32]512
        if ([W3.N]::QueryFullProcessImageName($h, 0, $sb, [ref]$sz)) { return (Split-Path $sb.ToString() -Leaf) }
        return ("pid " + $procId + " (name unavailable)")
    } finally { [void][W3.N]::CloseHandle($h) }
}

# Collect every window whose owning process is WeChat, whatever its title.
$wx = New-Object System.Collections.ArrayList
$cb = [W3.N+EnumProc]{
    param($h, $p)
    $procId = 0
    [void][W3.N]::GetWindowThreadProcessId($h, [ref]$procId)
    $name = Get-PidName $procId
    if ($name -match 'WeChat|Weixin') {
        $cls = New-Object System.Text.StringBuilder 256
        [void][W3.N]::GetClassName($h, $cls, 256)
        $txt = New-Object System.Text.StringBuilder 512
        [void][W3.N]::GetWindowText($h, $txt, 512)
        $r = New-Object W3.N+RECT
        [void][W3.N]::GetWindowRect($h, [ref]$r)
        [void]$script:wx.Add([pscustomobject]@{
            H = $h; Name = $name; Pid = $procId; Class = $cls.ToString()
            Title = $txt.ToString(); Vis = [W3.N]::IsWindowVisible($h)
            L = $r.L; T = $r.T; R = $r.R; B = $r.B
        })
    }
    return $true
}
[void][W3.N]::EnumWindows($cb, [IntPtr]::Zero)

Write-Output ("WeChat-owned top-level windows: " + $wx.Count)
foreach ($w in $wx) {
    Write-Output ("  vis=" + $w.Vis.ToString().PadRight(5) + " " + $w.Name.PadRight(16) +
                  " class='" + $w.Class + "' len=" + $w.Title.Length +
                  " rect=" + $w.L + "," + $w.T + "," + $w.R + "," + $w.B)
    Write-Output ("        title: " + $w.Title)
}
Write-Output ""

# The call window is the VISIBLE one that is not the main panel; list children of each visible one.
Write-Output "=== children of each VISIBLE WeChat window (looking for a peer name) ==="
foreach ($w in ($wx | Where-Object { $_.Vis -and ($_.R - $_.L) -gt 50 })) {
    Write-Output ("--- " + $w.Name + " '" + $w.Title + "' (" + ($w.R - $w.L) + "x" + ($w.B - $w.T) + ") ---")
    $script:owner = $w.H
    $ccb = [W3.N+EnumChildProc]{
        param($h, $p)
        $c = New-Object System.Text.StringBuilder 256
        [void][W3.N]::GetClassName($h, $c, 256)
        $t = New-Object System.Text.StringBuilder 512
        [void][W3.N]::GetWindowText($h, $t, 512)
        $s = $t.ToString()
        if ($s.Length -gt 0) {
            $rv = New-Object W3.N+RECT
            [void][W3.N]::GetWindowRect($h, [ref]$rv)
            Write-Output ("    vis=" + ([W3.N]::IsWindowVisible($h)).ToString().PadRight(5) +
                          " class='" + $c.ToString() + "' text='" + $s + "'")
            Write-Output ("          rect=" + $rv.L + "," + $rv.T + "," + $rv.R + "," + $rv.B)
        }
        return $true
    }
    [void][W3.N]::EnumChildWindows($w.H, $ccb, [IntPtr]::Zero)
}
