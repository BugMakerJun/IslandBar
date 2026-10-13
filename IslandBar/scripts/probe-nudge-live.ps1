# =====================================================================
#  probe-nudge-live.ps1 -- the re-assert and the nudge, with the bar running
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-nudge-live.ps1
#
#  WHY THIS EXISTS
#
#  The bar checks its taskbar every ~0.8 s (50 frames at ~15 ms, not the 5 s the comment claims), so
#  putting the strip back by hand is undone almost immediately -- which is the re-assert working, and
#  is also why the first version of this measurement saw nothing. The window has to be maximized
#  INSIDE that window, before the bar releases the strip again, and the point of the test is what
#  happens to it next:
#
#    * it was maximized while the strip was there, so its rectangle stops below the band (-8,24)
#    * Windows will NOT move it when the work area changes under it (measured)
#    * so the bar has to bring it forward itself, or the band the release removed stays on screen
#
#  Sequence: put the strip back, maximize fast, and read the rectangle before and after the bar's
#  next cycle. -8,24 -> -8,-8 is the nudge. The bar's own log lines are printed alongside.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace NudgeLive -Name Win -MemberDefinition @'
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
    wc.lpszClassName = "IslandBarNudgeLive";
    wc.hbrBackground = (IntPtr)6;
    RegisterClassEx(ref wc);
    return CreateWindowEx(0, "IslandBarNudgeLive", "IslandBar nudge probe -- safe to close",
        0x00CF0000 | 0x10000000, 240, 240, 520, 340, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
}
'@

function WorkArea {
    $rect = New-Object NudgeLive.Win+RECT
    [void][NudgeLive.Win]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)
    return ('{0},{1} - {2},{3}' -f $rect.L, $rect.T, $rect.R, $rect.B)
}

function RectOf([IntPtr]$h) {
    $r = New-Object NudgeLive.Win+RECT
    [void][NudgeLive.Win]::GetWindowRect($h, [ref]$r)
    return ('{0},{1} - {2},{3}  (zoomed {4})' -f $r.L, $r.T, $r.R, $r.B, [NudgeLive.Win]::IsZoomed($h))
}

if ([NudgeLive.Win]::FindWindow('Shell_TrayWnd', $null) -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. Run this on the interactive desktop.'
    exit 2
}

"1. work area        : " + (WorkArea)

$wnd = [NudgeLive.Win]::MakeWindow()
[void][NudgeLive.Win]::ShowWindow($wnd, 9)     # SW_RESTORE

# The strip goes back, and the window is maximized at once -- inside the ~0.8 s the bar needs to
# notice. No sleeps between the two calls: a sleep here is exactly how the first attempt missed it.
$rect = New-Object NudgeLive.Win+RECT
$rect.L = 0; $rect.T = 32; $rect.R = 2560; $rect.B = 1440
[void][NudgeLive.Win]::SystemParametersInfo(0x002F, 0, [ref]$rect, 0)
[void][NudgeLive.Win]::ShowWindow($wnd, 3)     # SW_MAXIMIZE
Start-Sleep -Milliseconds 120
"2. maximized while the strip was back:"
"   work area        : " + (WorkArea)
"   window rect      : " + (RectOf $wnd)

Start-Sleep -Seconds 4
"3. four seconds later (the bar's cycle has run twice):"
"   work area        : " + (WorkArea)
"   window rect      : " + (RectOf $wnd)

"4. the bar's log:"
$path = 'C:\Users\18340\IslandBar\island_bar.log'
Get-Content $path -Encoding UTF8 -Tail 40 | Select-String -Pattern 'work area|maximized window' |
    Select-Object -Last 6 | ForEach-Object { "   " + $_.Line }

[void][NudgeLive.Win]::DestroyWindow($wnd)
"5. window destroyed; work area: " + (WorkArea)
'done'
