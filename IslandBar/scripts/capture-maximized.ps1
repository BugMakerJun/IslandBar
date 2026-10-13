# =====================================================================
#  capture-maximized.ps1 -- photograph a maximized window with the bar over it
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\capture-maximized.ps1
#
#  The claim this has to support is a picture: a maximized window reaches the top of the screen and
#  the island floats over it, instead of the window stopping below a band that holds nothing but the
#  island. Until now that band was there -- "when I maximize an application it should go full screen,
#  not leave this blank band" -- and the only way to see whether it is gone is to look.
#
#  Writes captures\maximized-fullscreen.png and captures\maximized-top-strip.png (the top 120 px,
#  where the band would be). ASCII-only on purpose.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName System.Drawing

Add-Type -Namespace CaptureProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct WNDCLASSEX {
    public uint cbSize; public uint style; public IntPtr lpfnWndProc; public int cbClsExtra;
    public int cbWndExtra; public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor;
    public IntPtr hbrBackground; public string lpszMenuName; public string lpszClassName;
    public IntPtr hIconSm;
}
public delegate IntPtr WndProcD(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string name);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern ushort RegisterClassEx(ref WNDCLASSEX wc);
[DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError = true)]
public static extern IntPtr CreateWindowEx(uint ex, string cls, string name, uint style,
    int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

public static WndProcD Proc = delegate(IntPtr h, uint m, IntPtr w, IntPtr l) { return DefWindowProc(h, m, w, l); };

public static IntPtr MakeWindow() {
    WNDCLASSEX wc = new WNDCLASSEX();
    wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
    wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(Proc);
    wc.hInstance = GetModuleHandle(null);
    wc.lpszClassName = "IslandBarCaptureProbe";
    wc.hbrBackground = (IntPtr)6;
    RegisterClassEx(ref wc);
    return CreateWindowEx(0, "IslandBarCaptureProbe",
        "MAXIMIZED WINDOW -- the island floats over it, and this text starts at y=0",
        0x00CF0000 | 0x10000000, 240, 240, 900, 600, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
}
'@

if ([CaptureProbe.Win]::FindWindow('Shell_TrayWnd', $null) -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. Run this on the interactive desktop.'
    exit 2
}

$captures = 'C:\PROJECT\Taskbar\IslandBar\captures'
New-Item -ItemType Directory -Force -Path $captures | Out-Null

$wnd = [CaptureProbe.Win]::MakeWindow()
[void][CaptureProbe.Win]::ShowWindow($wnd, 3)        # SW_MAXIMIZE
[void][CaptureProbe.Win]::SetForegroundWindow($wnd)
Start-Sleep -Milliseconds 1200

$rect = New-Object CaptureProbe.Win+RECT
[void][CaptureProbe.Win]::GetWindowRect($wnd, [ref]$rect)
"window rect: {0},{1} - {2},{3}  (a top of -8 means the client area starts at y=0)" -f `
    $rect.L, $rect.T, $rect.R, $rect.B

$full = New-Object System.Drawing.Bitmap 2560, 1440
$g = [System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 2560, 1440))
$full.Save((Join-Path $captures 'maximized-fullscreen.png'), [System.Drawing.Imaging.ImageFormat]::Png)

$strip = New-Object System.Drawing.Bitmap 2560, 120
$g2 = [System.Drawing.Graphics]::FromImage($strip)
$g2.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 2560, 120))
$strip.Save((Join-Path $captures 'maximized-top-strip.png'), [System.Drawing.Imaging.ImageFormat]::Png)

$g.Dispose(); $g2.Dispose(); $full.Dispose(); $strip.Dispose()

Start-Sleep -Milliseconds 400
[void][CaptureProbe.Win]::DestroyWindow($wnd)
"saved: $captures\maximized-fullscreen.png and maximized-top-strip.png"
'done'
