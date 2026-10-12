# =====================================================================
#  probe-tray-uid.ps1 -- a UID-registered tray icon whose owning window the
#  first search did not find (WeChat, QQ Music on this machine).
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-uid.ps1 -Image Weixin.exe
#
#  Shell_NotifyIconGetRect needs the pair the application registered with: a window of its own, and the
#  UID that goes with it. The registry carries the UID; the window is found by trying every window the
#  application's processes own (see TrayPress.cpp). For most icons one of them answers -- measured, 9 of
#  the bar's 16 -- and for the ones that do not, this prints the windows that were tried, and asks the
#  shell about each window with a RANGE of UIDs: if a pair answers at a UID other than the registry's,
#  then the registry's UID is not the application's and the search needs to change.
#
#  Read-only: it queries and prints. Nothing is clicked.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================
param([string]$Image = 'Weixin.exe', [int]$MaxUid = 24)

$ErrorActionPreference = 'Continue'

Add-Type -Namespace TrayUid -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct NOTIFYICONIDENTIFIER { public int cbSize; public IntPtr hWnd; public uint uID; public Guid guidItem; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref NOTIFYICONIDENTIFIER id, out RECT rect);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string win);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
'@

$want = $Image.ToLower()
$pids = @(Get-Process | Where-Object { $_.ProcessName.ToLower() + '.exe' -eq $want -or $_.ProcessName.ToLower() -eq $want.TrimEnd('.exe') } |
          Select-Object -ExpandProperty Id)
"processes named like '{0}': {1}" -f $Image, (($pids | ForEach-Object { $_ }) -join ',')
if ($pids.Count -eq 0) { '  none running'; exit 1 }

$windows = @()
$cb = [TrayUid.Win+EnumProc]{
    param([IntPtr]$h, [IntPtr]$p)
    $procId = 0
    [void][TrayUid.Win]::GetWindowThreadProcessId($h, [ref]$procId)
    if ($pids -contains [int]$procId) { $script:windows += $h }
    return $true
}
[void][TrayUid.Win]::EnumWindows($cb, [IntPtr]::Zero)
$child = [IntPtr]::Zero
while (($child = [TrayUid.Win]::FindWindowEx([IntPtr]::new(0xFFFF), $child, $null, $null)) -ne [IntPtr]::Zero) {
    $procId = 0
    [void][TrayUid.Win]::GetWindowThreadProcessId($child, [ref]$procId)
    if ($pids -contains [int]$procId) { $windows += $child }
}
$windows = $windows | Select-Object -Unique
"  {0} window(s)" -f $windows.Count

foreach ($h in $windows) {
    $cls = New-Object System.Text.StringBuilder 256
    [void][TrayUid.Win]::GetClassName($h, $cls, 256)
    $txt = New-Object System.Text.StringBuilder 256
    [void][TrayUid.Win]::GetWindowTextW($h, $txt, 256)
    "  hwnd $h vis=$([TrayUid.Win]::IsWindowVisible($h)) [$($cls.ToString())] '$($txt.ToString())'"
    for ($uid = 0; $uid -le $MaxUid; $uid++) {
        $id = New-Object TrayUid.Win+NOTIFYICONIDENTIFIER
        $id.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type]('TrayUid.Win+NOTIFYICONIDENTIFIER'))
        $id.hWnd = $h
        $id.uID = $uid
        $r = New-Object TrayUid.Win+RECT
        $hr = [TrayUid.Win]::Shell_NotifyIconGetRect([ref]$id, [ref]$r)
        if ($hr -eq 0) { "      uid $uid -> {0},{1} {2}x{3}" -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T) }
    }
}
'=== done ==='
