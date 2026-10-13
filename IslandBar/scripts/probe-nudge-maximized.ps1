# =====================================================================
#  probe-nudge-maximized.ps1 -- can a window that is ALREADY maximized be brought to the
#                                full screen after the strip is released?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-nudge-maximized.ps1
#
#  WHY THIS EXISTS
#
#  Measured: releasing the work area (SPI_SETWORKAREA, no flags) makes a NEWLY maximized window
#  cover the whole screen (-8,-8 - 2568,1448 at 100%), but a window that was already maximized
#  KEEPS its old rectangle (-8,24 - 2568,1448) -- neither Windows nor the application re-lays it
#  out. The user's own browser is in exactly that state, so releasing the strip at startup would
#  leave the very band they photographed until they maximize again.
#
#  Two candidate nudges, measured side by side:
#    A. SetWindowPos to the rectangle Windows would use (monitor inflated by the frame border),
#       with SWP_NOACTIVATE -- no focus stolen.
#    B. ShowWindow(SW_MAXIMIZE) again, letting Windows compute it.
#
#  MUST RUN ON THE INTERACTIVE DESKTOP (see the sandbox note in probe-workarea-release.ps1).
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace NudgeProbe -Name Win -MemberDefinition @'
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
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll", SetLastError = true)] public static extern int GetWindowLong(IntPtr h, int index);
[DllImport("user32.dll", SetLastError = true)] public static extern int SetWindowLong(IntPtr h, int index, int value);

public static WndProcD Proc = delegate(IntPtr h, uint m, IntPtr w, IntPtr l) { return DefWindowProc(h, m, w, l); };

public static IntPtr MakeWindow() {
    WNDCLASSEX wc = new WNDCLASSEX();
    wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
    wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(Proc);
    wc.hInstance = GetModuleHandle(null);
    wc.lpszClassName = "IslandBarNudgeProbe";
    wc.hbrBackground = (IntPtr)6;
    RegisterClassEx(ref wc);
    return CreateWindowEx(0, "IslandBarNudgeProbe", "IslandBar nudge probe",
        0x00CF0000 | 0x10000000, 200, 200, 480, 320, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
}
'@

function WorkArea {
    $rect = New-Object NudgeProbe.Win+RECT
    [void][NudgeProbe.Win]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)
    return ('{0},{1} - {2},{3}' -f $rect.L, $rect.T, $rect.R, $rect.B)
}

function RectOf([IntPtr]$h) {
    $r = New-Object NudgeProbe.Win+RECT
    [void][NudgeProbe.Win]::GetWindowRect($h, [ref]$r)
    return ('{0},{1} - {2},{3}  (zoomed {4})' -f $r.L, $r.T, $r.R, $r.B, [NudgeProbe.Win]::IsZoomed($h))
}

function SetWorkArea([int]$l, [int]$t, [int]$r, [int]$b) {
    $rect = New-Object NudgeProbe.Win+RECT
    $rect.L = $l; $rect.T = $t; $rect.R = $r; $rect.B = $b
    [void][NudgeProbe.Win]::SystemParametersInfo(0x002F, 0, [ref]$rect, 0)
}

$SW_RESTORE = 9
$SW_MAXIMIZE = 3
$SWP_NOACTIVATE = 0x0010
$SWP_NOZORDER = 0x0004
$SWP_NOSENDCHANGING = 0x0400

if ([NudgeProbe.Win]::FindWindow('Shell_TrayWnd', $null) -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. This probe must run on the interactive desktop (outside the sandbox).'
    exit 2
}

$borderX = [NudgeProbe.Win]::GetSystemMetrics(32) + [NudgeProbe.Win]::GetSystemMetrics(92)   # SM_CXSIZEFRAME + SM_CXPADDEDBORDER
$borderY = [NudgeProbe.Win]::GetSystemMetrics(33) + [NudgeProbe.Win]::GetSystemMetrics(92)   # SM_CYSIZEFRAME
"frame border: x $borderX, y $borderY"
"0. work area: " + (WorkArea)

$wnd = [NudgeProbe.Win]::MakeWindow()
"   probe window: $wnd"

# --- nudge A: SetWindowPos to the rectangle Windows uses for a maximized window, no activate ---
"1. nudge A -- SetWindowPos, SWP_NOACTIVATE"
[void][NudgeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 500
"   maximized behind the strip : " + (RectOf $wnd)
$fgBefore = [NudgeProbe.Win]::GetForegroundWindow()
SetWorkArea 0 0 2560 1440
Start-Sleep -Milliseconds 300
"   after the release         : " + (RectOf $wnd)
[void][NudgeProbe.Win]::SetWindowPos($wnd, [IntPtr]::Zero, (0 - $borderX), (0 - $borderY),
                                     (2560 + 2 * $borderX), (1440 + 2 * $borderY),
                                     ($SWP_NOACTIVATE -bor $SWP_NOZORDER -bor $SWP_NOSENDCHANGING))
Start-Sleep -Milliseconds 400
"   after the nudge           : " + (RectOf $wnd)
"   foreground held           : " + ($fgBefore -eq [NudgeProbe.Win]::GetForegroundWindow())

# --- nudge B: ShowWindow(SW_MAXIMIZE) again -----------------------------------------------
#
# The window goes back to the state the user's browser is in: maximized while the strip is
# reserved, then the strip released under it.
"2. nudge B -- ShowWindow(SW_MAXIMIZE)"
[void][NudgeProbe.Win]::ShowWindow($wnd, $SW_RESTORE)
SetWorkArea 0 32 2560 1440
Start-Sleep -Milliseconds 300
[void][NudgeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 500
"   maximized behind the strip : " + (RectOf $wnd)
SetWorkArea 0 0 2560 1440
Start-Sleep -Milliseconds 300
"   after the release         : " + (RectOf $wnd)
[void][NudgeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 500
"   after the nudge           : " + (RectOf $wnd)

[void][NudgeProbe.Win]::DestroyWindow($wnd)
SetWorkArea 0 32 2560 1440
"3. cleaned up; work area: " + (WorkArea)

# --- what an application that goes borderless from a MAXIMIZED state looks like -----------
#
# The release makes a maximized window and a full-screen window geometrically identical, so the
# bar has to tell them apart by something that is not geometry. IsZoomed is the candidate. The
# case that decides whether it is enough is this one: an application that is maximized and then
# drops its frame to cover the screen (what a player does for full-screen video). If Windows
# clears the maximized state with the frame, "zoomed" alone separates the two cases everywhere.
"4. maximized, then borderless over the whole screen"
$wnd = [NudgeProbe.Win]::MakeWindow()
[void][NudgeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 400
"   maximized           : " + (RectOf $wnd)
$style = [NudgeProbe.Win]::GetWindowLong($wnd, -16)   # GWL_STYLE
[void][NudgeProbe.Win]::SetWindowLong($wnd, -16, ($style -band (-bnot (0x00C00000 -bor 0x00040000))))  # ~WS_CAPTION, ~WS_THICKFRAME
[void][NudgeProbe.Win]::SetWindowPos($wnd, [IntPtr]::Zero, 0, 0, 2560, 1440, 0x0014)   # NOACTIVATE|NOZORDER
Start-Sleep -Milliseconds 400
"   borderless over all : " + (RectOf $wnd) + "   <- (zoomed False means IsZoomed separates them)"
[void][NudgeProbe.Win]::DestroyWindow($wnd)
"5. cleaned up; work area: " + (WorkArea)
'done'
