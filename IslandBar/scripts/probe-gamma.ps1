# =====================================================================
#  probe-gamma.ps1 -- did the display's gamma ramp change?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-gamma.ps1
#
#  WHY THIS EXISTS
#
#  Night light removes blue by changing the OUTPUT gamma ramp, and a screen capture through BitBlt
#  reads the framebuffer BEFORE that ramp is applied -- measured: the mean colour of a screenshot is
#  identical with night light on and off (R 146 G 136 B 129 both times), so a screenshot cannot be
#  the witness for this feature. GetDeviceGammaRamp can: it asks the display driver for the ramp
#  that is actually in effect.
#
#  Prints the sum of each channel's ramp. A ramp that tilts warm shows a RED sum above the blue one.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace Gamma -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hwnd);
[DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
[DllImport("gdi32.dll")] public static extern bool GetDeviceGammaRamp(IntPtr dc, IntPtr ramp);
[DllImport("gdi32.dll")] public static extern bool SetDeviceGammaRamp(IntPtr dc, IntPtr ramp);
'@

function RampSums {
    $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(3 * 256 * 2)
    $dc = [Gamma.Win]::GetDC([IntPtr]::Zero)
    $ok = [Gamma.Win]::GetDeviceGammaRamp($dc, $buffer)
    [void][Gamma.Win]::ReleaseDC([IntPtr]::Zero, $dc)
    if (-not $ok) {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
        return 'GetDeviceGammaRamp FAILED'
    }

    $r = 0; $g = 0; $b = 0
    for ($i = 0; $i -lt 256; $i++) {
        $r += [System.Runtime.InteropServices.Marshal]::ReadInt16($buffer, $i * 2)
        $g += [System.Runtime.InteropServices.Marshal]::ReadInt16($buffer, (256 + $i) * 2)
        $b += [System.Runtime.InteropServices.Marshal]::ReadInt16($buffer, (512 + $i) * 2)
    }
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    return "ramp sums: R $r G $g B $b   (B/R ratio $([math]::Round($b / [math]::Max(1, $r), 4)))"
}

"now: " + (RampSums)
'done'
