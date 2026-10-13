# =====================================================================
#  probe-glass-region.ps1 -- does a WINDOW REGION clip the acrylic backdrop?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-glass-region.ps1
#
#  Measured already (probe-glass-blur.ps1): ACCENT_ENABLE_ACRYLICBLURBEHIND DOES blur what is behind
#  this window -- across the WHOLE window rectangle. The window is a 360 DIP transparent strip with an
#  island drawn inside it, so switching it on paints a blurred band across the top of the screen, which
#  is why a real blurred backdrop has been parked.
#
#  The way out would be to SHAPE the window: SetWindowRgn to what is actually drawn. That works only if
#  DWM clips the accent blur by the region. This asks exactly that and then takes the machine back.
#
#  The reading is the edge energy of a strip INSIDE the window but OUTSIDE the region: still sharp means
#  the region clips, blurred means it does not.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace GlassRegion -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct ACCENTPOLICY { public int nAccentState; public int nFlags; public int nColor; public int nAnimationId; }
[StructLayout(LayoutKind.Sequential)]
public struct WINCOMPATTRDATA { public int nAttribute; public IntPtr pData; public int ulDataSize; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern int SetWindowCompositionAttribute(IntPtr h, ref WINCOMPATTRDATA data);
[DllImport("user32.dll")] public static extern int SetWindowRgn(IntPtr h, IntPtr region, bool redraw);
[DllImport("gdi32.dll")] public static extern IntPtr CreateRoundRectRgn(int l, int t, int r, int b, int w, int h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("dwmapi.dll")] public static extern int DwmEnableBlurBehindWindow(IntPtr h, ref BLURBEHIND data);
[StructLayout(LayoutKind.Sequential)]
public struct BLURBEHIND { public int dwFlags; public bool fEnable; public IntPtr hRgnBlur; public bool fTransitionOnMaximized; }
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public const int DWM_BB_ENABLE = 0x1;
public const int DWM_BB_BLURREGION = 0x2;
public const int WCA_ACCENT_POLICY = 19;
public const int ACCENT_DISABLED = 0;
public const int ACCENT_ENABLE_ACRYLICBLURBEHIND = 4;
'@

function FindBar {
    $script:bar = [IntPtr]::Zero
    $cb = [GlassRegion.Win+EnumProc]{
        param([IntPtr]$h, [IntPtr]$p)
        if (-not [GlassRegion.Win]::IsWindowVisible($h)) { return $true }
        $c = New-Object System.Text.StringBuilder 128
        [void][GlassRegion.Win]::GetClassName($h, $c, 128)
        if ($c.ToString() -eq 'IslandBar.IslandWindow') { $script:bar = $h }
        return $true
    }
    [void][GlassRegion.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:bar
}

function Accent([int]$state) {
    $policy = New-Object GlassRegion.Win+ACCENTPOLICY
    $policy.nAccentState = $state
    $policy.nFlags = 2
    $policy.nColor = 0x99000000
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type][GlassRegion.Win+ACCENTPOLICY])
    $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size)
    [System.Runtime.InteropServices.Marshal]::StructureToPtr($policy, $buffer, $false)
    $data = New-Object GlassRegion.Win+WINCOMPATTRDATA
    $data.nAttribute = [GlassRegion.Win]::WCA_ACCENT_POLICY
    $data.pData = $buffer
    $data.ulDataSize = $size
    $result = [GlassRegion.Win]::SetWindowCompositionAttribute($script:bar, [ref]$data)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    return $result
}

function EdgeEnergy([int]$x, [int]$y, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $sum = 0.0
    $count = 0
    for ($yy = 0; $yy -lt $h; $yy++) {
        for ($xx = 0; $xx -lt $w - 2; $xx++) {
            $a = $bmp.GetPixel($xx, $yy)
            $b = $bmp.GetPixel($xx + 2, $yy)
            $sum += [math]::Abs($a.R - $b.R) + [math]::Abs($a.G - $b.G) + [math]::Abs($a.B - $b.B)
            $count++
        }
    }
    $g.Dispose()
    $bmp.Dispose()
    if ($count -eq 0) { return 0.0 }
    return [math]::Round($sum / $count, 2)
}

function Shot([string]$name, [int]$x, [int]$y, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $bmp.Save("C:\PROJECT\Taskbar\IslandBar\captures\glass-region-$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
}

$bar = FindBar
if ($bar -eq [IntPtr]::Zero) { 'STOP: the bar window was not found'; exit 2 }
"bar window: $bar"
$rect = New-Object GlassRegion.Win+RECT
[void][GlassRegion.Win]::GetWindowRect($bar, [ref]$rect)
"window rect: $($rect.L),$($rect.T) - $($rect.R),$($rect.B)"

# WELL OUTSIDE the region (which ends at x=1428): this is desktop the window still covers but does not
# draw into. The first version of this probe put the strip at x=200, which is INSIDE the region, so it
# measured what was already known and answered nothing.
$probeX = 1600
$probeY = 4
$probeW = 600
$probeH = 26
"baseline, nothing applied : " + (EdgeEnergy $probeX $probeY $probeW $probeH)

"--- region = the pill only, then acrylic ---"
$region = [GlassRegion.Win]::CreateRoundRectRgn(48, 0, 1428, 44, 44, 44)
[void][GlassRegion.Win]::SetWindowRgn($bar, $region, $true)
Start-Sleep -Milliseconds 700
"region only               : " + (EdgeEnergy $probeX $probeY $probeW $probeH)
[void](Accent ([GlassRegion.Win]::ACCENT_ENABLE_ACRYLICBLURBEHIND))
Start-Sleep -Milliseconds 900
"region + acrylic          : " + (EdgeEnergy $probeX $probeY $probeW $probeH)
"inside the region (pill)  : " + (EdgeEnergy 1150 4 260 26)
Shot 'region-acrylic' 1140 0 300 60

# --- the older API, which is the one that TAKES a region -------------------------------
#
# DwmEnableBlurBehindWindow + DWM_BB_BLURREGION is documented to blur only inside the given region, which
# is precisely what the acrylic path refuses to do. On Windows 8 and later the classic Aero glass was
# removed and this is widely described as doing nothing -- which is exactly why it is measured rather
# than assumed: if it still blurs, a real glass pill is available after all.
"--- blur behind with a region (the API that takes one) ---"
[void](Accent ([GlassRegion.Win]::ACCENT_DISABLED))
$blurRegion = [GlassRegion.Win]::CreateRoundRectRgn(48, 0, 1428, 44, 44, 44)
$behind = New-Object GlassRegion.Win+BLURBEHIND
$behind.dwFlags = [GlassRegion.Win]::DWM_BB_ENABLE -bor [GlassRegion.Win]::DWM_BB_BLURREGION
$behind.fEnable = $true
$behind.hRgnBlur = $blurRegion
$behind.fTransitionOnMaximized = $false
$blurHr = [GlassRegion.Win]::DwmEnableBlurBehindWindow($bar, [ref]$behind)
Start-Sleep -Milliseconds 900
"DwmEnableBlurBehindWindow -> 0x{0:X8}" -f $blurHr
"outside its region        : " + (EdgeEnergy $probeX $probeY $probeW $probeH)
"inside its region (pill)  : " + (EdgeEnergy 1150 4 260 26)
Shot 'blurbehind' 1140 0 300 60
$behind.fEnable = $false
$behind.dwFlags = [GlassRegion.Win]::DWM_BB_ENABLE
[void][GlassRegion.Win]::DwmEnableBlurBehindWindow($bar, [ref]$behind)
Start-Sleep -Milliseconds 400

"--- taking it all back ---"
[void][GlassRegion.Win]::SetWindowRgn($bar, [IntPtr]::Zero, $true)
[void](Accent ([GlassRegion.Win]::ACCENT_DISABLED))
Start-Sleep -Milliseconds 900
"restored                  : " + (EdgeEnergy $probeX $probeY $probeW $probeH)
Shot 'restored' 1140 0 300 60
'done'
