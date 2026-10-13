# =====================================================================
#  probe-maximize-fullscreen.ps1 -- if the work area is released, does a maximized
#                                    window really fill the screen?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-maximize-fullscreen.ps1
#
#  WHY THIS EXISTS
#
#  probe-workarea-release.ps1 found that SPI_SETWORKAREA with NO SPIF_* flags moves the
#  work area and the value sticks -- while SPIF_SENDCHANGE|SPIF_UPDATEINIFILE reports success
#  and changes nothing. A work area is only worth anything if maximized windows obey it, so
#  this measures that directly, on a window of our own making, rather than on whatever the
#  user happens to have open.
#
#  It also asks the follow-up question the first probe raises: WHO puts the strip back when
#  the change is announced. If an explicit WM_SETTINGCHANGE broadcast undoes the release, then
#  the release has to be quiet -- and re-asserted after anything that would announce it.
#
#  MUST RUN ON THE INTERACTIVE DESKTOP (see the sandbox note in the other probe).
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace MaximizeProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
// CharSet matters on the struct as well as on the DllImport: with the default the class-name
// fields are written as ANSI bytes and RegisterClassExW reads them as UTF-16, so the class is
// registered under a name nobody can look up and CreateWindowEx returns 0.
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
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint ms, out IntPtr result);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern uint GetDpiForSystem();

// Kept alive for the lifetime of the window: a collected delegate is a crash.
public static WndProcD Proc = delegate(IntPtr h, uint m, IntPtr w, IntPtr l) { return DefWindowProc(h, m, w, l); };

public static IntPtr MakeWindow() {
    WNDCLASSEX wc = new WNDCLASSEX();
    wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
    wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(Proc);
    wc.hInstance = GetModuleHandle(null);
    wc.lpszClassName = "IslandBarMaximizeProbe";
    wc.hbrBackground = (IntPtr)6;   // COLOR_WINDOW
    RegisterClassEx(ref wc);
    // WS_OVERLAPPEDWINDOW | WS_VISIBLE
    return CreateWindowEx(0, "IslandBarMaximizeProbe", "IslandBar maximize probe",
        0x00CF0000 | 0x10000000, 200, 200, 480, 320, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
}
'@

function WorkArea {
    $rect = New-Object MaximizeProbe.Win+RECT
    [void][MaximizeProbe.Win]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)
    return ('{0},{1} - {2},{3}' -f $rect.L, $rect.T, $rect.R, $rect.B)
}

function SetWorkArea([int]$l, [int]$t, [int]$r, [int]$b) {
    $rect = New-Object MaximizeProbe.Win+RECT
    $rect.L = $l; $rect.T = $t; $rect.R = $r; $rect.B = $b
    # No SPIF_* flags: with SPIF_SENDCHANGE the call reports success and the value does not take.
    [void][MaximizeProbe.Win]::SystemParametersInfo(0x002F, 0, [ref]$rect, 0)
}

function RectOf([IntPtr]$h) {
    $r = New-Object MaximizeProbe.Win+RECT
    [void][MaximizeProbe.Win]::GetWindowRect($h, [ref]$r)
    return ('{0},{1} - {2},{3}  (zoomed {4})' -f $r.L, $r.T, $r.R, $r.B, [MaximizeProbe.Win]::IsZoomed($h))
}

$SW_RESTORE = 9
$SW_MAXIMIZE = 3

"dpi: " + [MaximizeProbe.Win]::GetDpiForSystem()
$tray = [MaximizeProbe.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. This probe must run on the interactive desktop (outside the sandbox).'
    exit 2
}

"0. work area          : " + (WorkArea)

$wnd = [MaximizeProbe.Win]::MakeWindow()
"   probe window       : $wnd"

"1. maximize as the shell left it (work area still insets the strip)"
[void][MaximizeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 600
"   window rect        : " + (RectOf $wnd)

"2. release the work area (SPI_SETWORKAREA, no flags)"
SetWorkArea 0 0 2560 1440
Start-Sleep -Milliseconds 400
"   work area          : " + (WorkArea)
"   window rect        : " + (RectOf $wnd) + "   <- an already-maximized window keeps its rect"

"3. re-maximize it (restore, then maximize)"
[void][MaximizeProbe.Win]::ShowWindow($wnd, $SW_RESTORE)
Start-Sleep -Milliseconds 300
[void][MaximizeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 600
"   window rect        : " + (RectOf $wnd)

"4. now ANNOUNCE the setting change (HWND_BROADCAST, WM_SETTINGCHANGE 0x1A, SPI_SETWORKAREA)"
$out = [IntPtr]::Zero
$sent = [MaximizeProbe.Win]::SendMessageTimeout([IntPtr]0xFFFF, 0x1A, [IntPtr]0x2F, [IntPtr]::Zero,
                                                0x0002, 3000, [ref]$out)   # SMTO_ABORTIFHUNG
Start-Sleep -Milliseconds 600
"   broadcast          : $sent"
"   work area          : " + (WorkArea)
"   window rect        : " + (RectOf $wnd)

"5. with the strip back at 32, re-maximize once more"
SetWorkArea 0 32 2560 1440
[void][MaximizeProbe.Win]::ShowWindow($wnd, $SW_RESTORE)
Start-Sleep -Milliseconds 300
[void][MaximizeProbe.Win]::ShowWindow($wnd, $SW_MAXIMIZE)
Start-Sleep -Milliseconds 500
"   work area          : " + (WorkArea)
"   window rect        : " + (RectOf $wnd)

[void][MaximizeProbe.Win]::DestroyWindow($wnd)
"6. window destroyed; work area: " + (WorkArea)
'done'
