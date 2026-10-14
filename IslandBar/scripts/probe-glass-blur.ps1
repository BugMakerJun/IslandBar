# =====================================================================
#  probe-glass-blur.ps1 -- can the bar's window have a REAL backdrop blur?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-glass-blur.ps1
#
#  WHY THIS EXISTS
#
#  The plan being reviewed asks for "Liquid Glass": a translucent dark background with the desktop
#  behind it BLURRED. Nothing in the bar does that today -- measured, there is no Mica, acrylic or
#  backdrop call anywhere in the source, and what "glass" means here now is a translucent dark fill
#  plus a soft edge (the album art is blurred, but that is inside the card, not behind it).
#
#  A per-pixel-alpha DirectComposition window (WS_EX_NOREDIRECTIONBITMAP) is NOT the case the Windows
#  backdrops were built for, so this is measured rather than assumed. Two candidates, each applied and
#  then taken back off:
#
#    * SetWindowCompositionAttribute + ACCENT_ENABLE_ACRYLICBLURBEHIND (undocumented, Win10+)
#    * DwmSetWindowAttribute + DWMWA_SYSTEMBACKDROP_TYPE = 3 (acrylic, Win11 22H2+)
#
#  What is checked, in order: does the window still draw at all (the island must survive), and do the
#  pixels BEHIND the island actually change -- the edge energy of the strip inside the pill, which falls
#  when a busy backdrop is blurred. Three crops are written to captures\ so the result can be looked at.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace GlassProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct ACCENTPOLICY {
    public int nAccentState;
    public int nFlags;
    public int nColor;
    public int nAnimationId;
}
[StructLayout(LayoutKind.Sequential)]
public struct WINCOMPATTRDATA {
    public int nAttribute;
    public IntPtr pData;
    public int ulDataSize;
}
public delegate bool EnumProc(IntPtr h, IntPtr p);

[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern int SetWindowCompositionAttribute(IntPtr h, ref WINCOMPATTRDATA data);
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int value, int size);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

public const int WCA_ACCENT_POLICY = 19;
public const int ACCENT_DISABLED = 0;
public const int ACCENT_ENABLE_ACRYLICBLURBEHIND = 4;
public const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
public const int DWMSBT_TRANSIENTWINDOW = 3;
'@

function FindBarWindow {
    $script:bar = [IntPtr]::Zero
    $cb = [GlassProbe.Win+EnumProc]{
        param([IntPtr]$h, [IntPtr]$p)
        if (-not [GlassProbe.Win]::IsWindowVisible($h)) { return $true }
        $c = New-Object System.Text.StringBuilder 128
        [void][GlassProbe.Win]::GetClassName($h, $c, 128)
        if ($c.ToString() -eq 'IslandBar.IslandWindow') { $script:bar = $h }
        return $true
    }
    [void][GlassProbe.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:bar
}

function Apply-Accent([int]$state, [int]$colour) {
    $policy = New-Object GlassProbe.Win+ACCENTPOLICY
    $policy.nAccentState = $state
    $policy.nFlags = 2
    $policy.nColor = $colour
    $policy.nAnimationId = 0
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type][GlassProbe.Win+ACCENTPOLICY])
    $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size)
    [System.Runtime.InteropServices.Marshal]::StructureToPtr($policy, $buffer, $false)
    $data = New-Object GlassProbe.Win+WINCOMPATTRDATA
    $data.nAttribute = [GlassProbe.Win]::WCA_ACCENT_POLICY
    $data.pData = $buffer
    $data.ulDataSize = $size
    $result = [GlassProbe.Win]::SetWindowCompositionAttribute($h, [ref]$data)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    return $result
}

# Edge energy of a strip: the mean absolute difference between neighbouring pixels. A BLURRED backdrop
# has less of it than a sharp one -- which is the only way to tell "it is blurred" from "it is dark".
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
    $g.Dispose(); $bmp.Dispose()
    if ($count -eq 0) { return 0.0 }
    return [math]::Round($sum / $count, 2)
}

function Snapshot([string]$name, [int]$x, [int]$y, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $dir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save((Join-Path $dir "glass-$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

$h = FindBarWindow
if ($h -eq [IntPtr]::Zero) { 'STOP: the bar window was not found (is island_bar running?)'; exit 2 }
"bar window: $h"

# The pill's own strip, and a strip of the same size just BELOW it, which is ordinary desktop content --
# the second is the control: whatever happens to it happens to any window on this machine.
$pillX = 1080; $pillY = 2; $pillW = 400; $pillH = 30
$belowX = 1080; $belowY = 60; $belowW = 400; $belowH = 30

"baseline   pill edge energy: " + (EdgeEnergy $pillX $pillY $pillW $pillH) + "   desktop control: " + (EdgeEnergy $belowX $belowY $belowW $belowH)
Snapshot 'before' $pillX $pillY $pillW $pillH

"--- ACCENT_ENABLE_ACRYLICBLURBEHIND ---"
$applied = Apply-Accent ([GlassProbe.Win]::ACCENT_ENABLE_ACRYLICBLURBEHIND) 0x99000000
Start-Sleep -Milliseconds 900
"SetWindowCompositionAttribute returned $applied"
"pill edge energy: " + (EdgeEnergy $pillX $pillY $pillW $pillH) + "   desktop control: " + (EdgeEnergy $belowX $belowY $belowW $belowH)
Snapshot 'acrylic' $pillX $pillY $pillW $pillH

"--- back off ---"
[void](Apply-Accent ([GlassProbe.Win]::ACCENT_DISABLED) 0)
Start-Sleep -Milliseconds 900
"pill edge energy: " + (EdgeEnergy $pillX $pillY $pillW $pillH)
Snapshot 'reverted' $pillX $pillY $pillW $pillH

"--- DWMWA_SYSTEMBACKDROP_TYPE = acrylic (Win11 22H2+) ---"
$backdrop = [GlassProbe.Win]::DWMSBT_TRANSIENTWINDOW
$hr = [GlassProbe.Win]::DwmSetWindowAttribute($h, [GlassProbe.Win]::DWMWA_SYSTEMBACKDROP_TYPE, [ref]$backdrop, 4)
Start-Sleep -Milliseconds 900
"DwmSetWindowAttribute returned 0x{0:X8}" -f $hr
"pill edge energy: " + (EdgeEnergy $pillX $pillY $pillW $pillH)
Snapshot 'backdrop' $pillX $pillY $pillW $pillH
$off = 0
[void][GlassProbe.Win]::DwmSetWindowAttribute($h, [GlassProbe.Win]::DWMWA_SYSTEMBACKDROP_TYPE, [ref]$off, 4)
Start-Sleep -Milliseconds 600
"--- left as found ---"
"pill edge energy: " + (EdgeEnergy $pillX $pillY $pillW $pillH)
Snapshot 'final' $pillX $pillY $pillW $pillH
'done'
