$ErrorActionPreference = 'Continue'

# =====================================================================
#  Enumerate EVERY top-level window, visible or not, with its process.
#
#  Get-Process's MainWindowHandle only reports the window Windows considers
#  the process's "main" one, and it returns 0 for a great many real windows --
#  including, measured on this machine, every WeChatAppEx process while WeChat
#  plainly had windows on screen. Enumerating properly is the only way to see
#  a call window.
#
#  Read-only.
# =====================================================================

Add-Type -Namespace W -Name N -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr h);
'@

$rows = New-Object System.Collections.ArrayList
$cb = [W.N+EnumProc]{
    param($h, $p)
    $pid_ = 0
    [void][W.N]::GetWindowThreadProcessId($h, [ref]$pid_)
    $proc = '(gone)'
    try { $proc = (Get-Process -Id $pid_ -ErrorAction Stop).ProcessName } catch { }
    $cls = New-Object System.Text.StringBuilder 256
    [void][W.N]::GetClassName($h, $cls, 256)
    $txt = New-Object System.Text.StringBuilder 512
    [void][W.N]::GetWindowText($h, $txt, 512)
    $r = New-Object W.N+RECT
    [void][W.N]::GetWindowRect($h, [ref]$r)
    $vis = [W.N]::IsWindowVisible($h)
    [void]$rows.Add([pscustomobject]@{
        Proc = $proc; Pid = $pid_; Class = $cls.ToString(); Title = $txt.ToString()
        Vis = $vis; L = $r.L; T = $r.T; R = $r.R; B = $r.B
    })
    return $true
}
[void][W.N]::EnumWindows($cb, [IntPtr]::Zero)

Write-Output ("total top-level windows: " + $rows.Count)
Write-Output ""

Write-Output "=== windows owned by a WeChat process (any visibility) ==="
$wx = $rows | Where-Object { $_.Proc -match 'wechat|weixin' }
if ($wx) {
    foreach ($w in $wx) {
        Write-Output ("  vis=" + $w.Vis.ToString().PadRight(5) + " pid=" + $w.Pid.ToString().PadRight(6) +
                      " class='" + $w.Class + "'  rect=" + $w.L + "," + $w.T + "," + $w.R + "," + $w.B)
        if ($w.Title.Length -gt 0) { Write-Output ("        title: " + $w.Title) }
    }
} else { Write-Output "  (none)" }
Write-Output ""

Write-Output "=== EVERY window that has a title (visible or not) ==="
foreach ($w in ($rows | Where-Object { $_.Title.Length -gt 0 })) {
    Write-Output ("  " + $w.Proc.PadRight(22) + " vis=" + $w.Vis.ToString().PadRight(5) + " | " + $w.Title)
}
