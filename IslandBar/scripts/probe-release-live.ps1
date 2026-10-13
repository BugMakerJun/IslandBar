# =====================================================================
#  probe-release-live.ps1 -- the running bar's release, end to end
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-release-live.ps1
#
#  WHAT IT CHECKS, IN ORDER
#
#    1. the work area the running bar left: the whole monitor, or still inset?
#    2. a window maximized NOW: does its client area reach the top of the screen?
#    3. the bar's own reaction: a maximized window must NOT be taken for a full-screen application
#       (the bar stands down on those, island only -- see FullScreenAppInFront). Read from the log.
#    4. the RE-ASSERT and the nudge: the work area is put back to the inset the shell would derive,
#       a window is maximized while it is like that, and the bar's own 5 s cycle then has to both
#       release the strip again and bring that window forward. Both are read from the log, and the
#       window's rectangle is measured afterwards.
#
#  The window is this script's own: nothing of the user's is touched. Run it with the bar running.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace LiveProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct WNDCLASSEX {
    public uint cbSize; public uint style; public IntPtr lpfnWndProc; public int cbClsExtra;
    public int cbWndExtra; public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor;
    public IntPtr hbrBackground; public string lpszMenuName; public string lpszClassName;
    public IntPtr hIconSm;
}
public delegate IntPtr WndProcD(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", SetLastError = true)] public static extern bool SystemParametersInfo(uint action, uint param, ref RECT rect, uint flags);
[DllImport("user32.dll", SetLastError = true)] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string name);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern ushort RegisterClassEx(ref WNDCLASSEX wc);
[DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError = true)]
public static extern IntPtr CreateWindowEx(uint ex, string cls, string name, uint style,
    int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);

public static WndProcD Proc = delegate(IntPtr h, uint m, IntPtr w, IntPtr l) { return DefWindowProc(h, m, w, l); };

public static IntPtr MakeWindow() {
    WNDCLASSEX wc = new WNDCLASSEX();
    wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
    wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(Proc);
    wc.hInstance = GetModuleHandle(null);
    wc.lpszClassName = "IslandBarLiveProbe";
    wc.hbrBackground = (IntPtr)6;
    RegisterClassEx(ref wc);
    return CreateWindowEx(0, "IslandBarLiveProbe", "IslandBar live probe -- safe to close",
        0x00CF0000 | 0x10000000, 240, 240, 520, 340, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
}
'@

function WorkArea {
    $rect = New-Object LiveProbe.Win+RECT
    [void][LiveProbe.Win]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)
    return ('{0},{1} - {2},{3}' -f $rect.L, $rect.T, $rect.R, $rect.B)
}

function RectOf([IntPtr]$h) {
    $r = New-Object LiveProbe.Win+RECT
    [void][LiveProbe.Win]::GetWindowRect($h, [ref]$r)
    return ('{0},{1} - {2},{3}  (zoomed {4})' -f $r.L, $r.T, $r.R, $r.B, [LiveProbe.Win]::IsZoomed($h))
}

function SetWorkArea([int]$l, [int]$t, [int]$r, [int]$b) {
    $rect = New-Object LiveProbe.Win+RECT
    $rect.L = $l; $rect.T = $t; $rect.R = $r; $rect.B = $b
    [void][LiveProbe.Win]::SystemParametersInfo(0x002F, 0, [ref]$rect, 0)
}

function LogTail([int]$lines) {
    # The bar's log lives beside the deployed executable (Log.cpp). A missing log is reported rather
    # than ignored: the whole point of several checks here is what the bar wrote down.
    $path = 'C:\Users\18340\IslandBar\island_bar.log'
    if (-not (Test-Path $path)) { return @('<no log file at ' + $path + '>') }
    return (Get-Content $path -Encoding UTF8 -Tail $lines)
}

$SW_RESTORE = 9
$SW_MAXIMIZE = 3

if ([LiveProbe.Win]::FindWindow('Shell_TrayWnd', $null) -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. Run this on the interactive desktop.'
    exit 2
}

"1. work area the running bar left   : " + (WorkArea)

$wnd = [LiveProbe.Win]::MakeWindow()
"   probe window                     : $wnd"

# --- a window maximized now -----------------------------------------------------------------
"2. maximize it now"
[void][LiveProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 800
"   window rect                     : " + (RectOf $wnd)
"   (a top of -border, i.e. -8, means the client area reaches y=0: the screen's top)"

# --- did the bar mistake it for a full-screen application? ----------------------------------
Start-Sleep -Milliseconds 1500
"3. the bar's log for the last 2 s -- 'stands down' must NOT be there:"
$tail = LogTail 8
$standDown = $tail | Select-String -Pattern 'stands down|fullscreen:'
if ($standDown) {
    foreach ($line in $standDown) { "   " + $line.Line }
} else {
    "   (nothing about full-screen -- correct: a maximized window is not one)"
}

# --- the re-assert and the nudge -------------------------------------------------------------
"4. put the work area back to the inset the shell derives, then maximize while it is like that"
[void][LiveProbe.Win]::ShowWindow($wnd, $SW_RESTORE)
Start-Sleep -Milliseconds 300
SetWorkArea 0 32 2560 1440
Start-Sleep -Milliseconds 400
[void][LiveProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 600
"   work area (put back)             : " + (WorkArea)
"   window rect (behind the strip)   : " + (RectOf $wnd)
"   waiting 8 s for the bar's own cycle ..."
Start-Sleep -Seconds 8
"   work area (after the cycle)      : " + (WorkArea)
"   window rect (after the cycle)    : " + (RectOf $wnd)
"   the bar's log for those 8 s:"
foreach ($line in (LogTail 14)) { "   " + $line }

[void][LiveProbe.Win]::DestroyWindow($wnd)
"5. window destroyed; work area       : " + (WorkArea)
# Leave the strip where the bar wants it, whether or not it has caught up yet.
SetWorkArea 0 0 2560 1440
'done'
