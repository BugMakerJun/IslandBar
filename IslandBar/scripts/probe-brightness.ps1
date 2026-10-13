# =====================================================================
#  probe-brightness.ps1 -- can this machine's brightness be READ and SET from a desktop process?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-brightness.ps1
#      ... -Set 40    (also try setting 40% and then put the old value back)
#
#  WHY THIS EXISTS
#
#  The quick-settings panel has a volume slider and no brightness one, and the reason given in the
#  notes was "WMI, cannot be verified here". Measured: `WmiMonitorBrightness` is EMPTY on this
#  machine -- a desktop with an external panel, which is exactly the case WMI's internal-panel
#  interface does not cover. The other documented route is DDC/CI (dxva2's physical-monitor API),
#  which talks to the MONITOR over the video cable and works on external displays that have DDC/CI
#  enabled in their own on-screen menu.
#
#  So this asks the monitor directly: how many physical monitors the display reports, whether each
#  one claims brightness support, and what its minimum/current/maximum are. Only with -Set does it
#  write anything, and then it restores the value it found.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [int]$Set = -1
)

$ErrorActionPreference = 'Continue'

Add-Type -Namespace Brightness -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
public struct PHYSICAL_MONITOR {
    public IntPtr hPhysicalMonitor;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szPhysicalMonitorDescription;
}
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

[DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(POINT p, uint flags);
[DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);

[DllImport("dxva2.dll", SetLastError = true)]
public static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, out uint count);
[DllImport("dxva2.dll", SetLastError = true)]
public static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, uint count, [Out] PHYSICAL_MONITOR[] monitors);
[DllImport("dxva2.dll", SetLastError = true)]
public static extern bool DestroyPhysicalMonitors(uint count, [In] PHYSICAL_MONITOR[] monitors);
[DllImport("dxva2.dll", SetLastError = true)]
public static extern bool GetMonitorCapabilities(IntPtr hMonitor, out uint caps, out uint colorTemperatures);
[DllImport("dxva2.dll", SetLastError = true)]
public static extern bool GetMonitorBrightness(IntPtr hMonitor, out uint minimum, out uint current, out uint maximum);
[DllImport("dxva2.dll", SetLastError = true)]
public static extern bool SetMonitorBrightness(IntPtr hMonitor, uint value);

public const uint MC_CAPS_BRIGHTNESS = 0x00000002;
public const uint MONITOR_DEFAULTTONEAREST = 2;
'@

$tray = [Brightness.Win]::FindWindow('Shell_TrayWnd', $null)
$monitor = if ($tray -ne [IntPtr]::Zero) {
    [Brightness.Win]::MonitorFromWindow($tray, [Brightness.Win]::MONITOR_DEFAULTTONEAREST)
} else {
    [Brightness.Win]::MonitorFromPoint((New-Object Brightness.Win+POINT), [Brightness.Win]::MONITOR_DEFAULTTONEAREST)
}
"monitor: $monitor"

$count = 0
if (-not [Brightness.Win]::GetNumberOfPhysicalMonitorsFromHMONITOR($monitor, [ref]$count)) {
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    "GetNumberOfPhysicalMonitorsFromHMONITOR FAILED (error $err)"
    exit 1
}
"physical monitors: $count"

if ($count -eq 0) { 'nothing to ask'; exit 1 }

$monitors = New-Object 'Brightness.Win+PHYSICAL_MONITOR[]' $count
if (-not [Brightness.Win]::GetPhysicalMonitorsFromHMONITOR($monitor, $count, $monitors)) {
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    "GetPhysicalMonitorsFromHMONITOR FAILED (error $err)"
    exit 1
}

foreach ($pm in $monitors) {
    "  handle $($pm.hPhysicalMonitor)  '$($pm.szPhysicalMonitorDescription)'"

    $caps = 0; $temps = 0
    $haveCaps = [Brightness.Win]::GetMonitorCapabilities($pm.hPhysicalMonitor, [ref]$caps, [ref]$temps)
    "    capabilities: ok=$haveCaps caps=0x$('{0:X}' -f $caps) colorTemps=0x$('{0:X}' -f $temps) brightness=$([bool]($caps -band [Brightness.Win]::MC_CAPS_BRIGHTNESS))"

    $min = 0; $cur = 0; $max = 0
    $haveBrightness = [Brightness.Win]::GetMonitorBrightness($pm.hPhysicalMonitor, [ref]$min, [ref]$cur, [ref]$max)
    $readError = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    "    brightness  : ok=$haveBrightness min=$min current=$cur max=$max (last error $readError)"

    if ($Set -ge 0 -and $haveBrightness -and $max -gt $min) {
        $wanted = [uint32]$min + [uint32](($max - $min) * $Set / 100)
        "    writing $wanted ($Set%) ..."
        $ok = [Brightness.Win]::SetMonitorBrightness($pm.hPhysicalMonitor, $wanted)
        Start-Sleep -Milliseconds 700
        $min2 = 0; $cur2 = 0; $max2 = 0
        [void][Brightness.Win]::GetMonitorBrightness($pm.hPhysicalMonitor, [ref]$min2, [ref]$cur2, [ref]$max2)
        "    write returned $ok; now reads back $cur2  <-- the number that matters"

        "    putting $cur back ..."
        [void][Brightness.Win]::SetMonitorBrightness($pm.hPhysicalMonitor, [uint32]$cur)
        Start-Sleep -Milliseconds 700
        $min3 = 0; $cur3 = 0; $max3 = 0
        [void][Brightness.Win]::GetMonitorBrightness($pm.hPhysicalMonitor, [ref]$min3, [ref]$cur3, [ref]$max3)
        "    restored to $cur3"
    }
}

[void][Brightness.Win]::DestroyPhysicalMonitors($count, $monitors)
'done'
