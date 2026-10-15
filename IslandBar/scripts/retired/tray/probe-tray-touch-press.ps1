# =====================================================================
#  probe-tray-touch-press.ps1 -- CAN A TRAY ICON BE PRESSED WITH INJECTED TOUCH?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-touch-press.ps1
#      ... -X 2394 -Y 109 -Seconds 3
#
#  WHY THIS EXISTS
#
#  The press of a tray icon is a real click, and a real click means the pointer is moved onto the icon and
#  put back -- the user watches the mouse jump, and that is the one part of this they have asked to be rid
#  of. `InitializeTouchInjection` + `InjectTouchInput` (Windows 8+, documented) deliver input AT A SCREEN
#  COORDINATE without touching the pointer, which would remove that jump by itself, with no injection into
#  anybody else's process.
#
#  Whether the shell's tray accepts it is not a question an opinion can settle: the overflow panel is a XAML
#  island hosted by the shell, and Windows promotes touch to mouse for windows that do not take touch. So
#  this injects one tap at the icon's own position and reports what appeared afterwards. It leaves the panel
#  VISIBLE on purpose -- the point here is the delivery, not the flash -- and it puts everything back.
#
#  WHAT THE ANSWER LOOKS LIKE: "the application's own windows: 0 -> 1" (it worked), or "no change" together
#  with whatever InjectTouchInput returned. Both are printed; the second is the interesting one -- and it is
#  the one this machine produced (measured 2026-10-15: pointer did not move, and the panel took nothing).
#
#  THE TAP COMES FROM A TINY C++ BINARY, not from PowerShell: the same call marshalled from PowerShell fails
#  with ERROR_INVALID_PARAMETER on this machine for every variant tried (with the struct sizes verified),
#  while the C++ version of it succeeds. Build it once with a normal MSVC environment:
#
#      cl /nologo /EHsc /Fe:touch_probe_inject.exe scripts\probe-tray-touch-inject.cpp user32.lib
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [int]$Col = 1,
    [int]$Row = 1,
    [string]$AppImage = 'wegame.exe',
    [string]$Injector = "$PSScriptRoot\touch_probe_inject.exe"
)

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace TouchProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] public struct POINTER_INFO {
    public uint pointerType; public uint pointerId; public uint frameId; public uint pointerFlags;
    public IntPtr sourceDevice; public IntPtr hwndTarget;
    public POINT ptPixelLocation; public POINT ptHimetricLocation;
    public POINT ptPixelLocationRaw; public POINT ptHimetricLocationRaw;
    public uint dwTime; public uint historyCount; public int InputData; public uint dwKeyStates;
    public ulong PerformanceCount; public int ButtonChangeType;
}
[StructLayout(LayoutKind.Sequential)] public struct POINTER_TOUCH_INFO {
    public POINTER_INFO pointerInfo;
    public uint touchFlags; public uint touchMask;
    public RECT rcContact; public RECT rcContactRaw;
    public uint orientation; public uint pressure;
}
[DllImport("user32.dll", SetLastError=true)] public static extern bool InitializeTouchInjection(uint count, uint feedback);
[DllImport("user32.dll", SetLastError=true)] public static extern bool InjectTouchInput(uint count, ref POINTER_TOUCH_INFO contacts);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string cls, string win);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int i, IntPtr v);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte a, uint flags);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
'@

$GW_OWNER           = 4
$WS_EX_LAYERED      = 0x00080000

function Get-AppWindows([string]$image) {
    $pids = @(Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($image)) -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $found = @()
    if ($pids.Count -eq 0) { return $found }
    $cb = [TouchProbe.Win+EnumProc]{
        param([IntPtr]$h, [IntPtr]$p)
        $procId = 0
        [void][TouchProbe.Win]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($script:pids -contains $procId) {
            $cls = New-Object System.Text.StringBuilder 256
            [void][TouchProbe.Win]::GetClassName($h, $cls, 256)
            $r = New-Object TouchProbe.Win+RECT
            [void][TouchProbe.Win]::GetWindowRect($h, [ref]$r)
            $script:found += ('{0} [{1}] {2},{3} {4}x{5} vis={6}' -f $h, $cls.ToString(), $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T), [TouchProbe.Win]::IsWindowVisible($h))
        }
        return $true
    }
    $script:pids = $pids
    $script:found = @()
    [void][TouchProbe.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$tray = [TouchProbe.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }

# The tray is laid out only while its taskbar is shown; here it is shown INVISIBLY, the way the bar does it.
$ex = ([TouchProbe.Win]::GetWindowLongPtr($tray, -20)).ToInt64()
[void][TouchProbe.Win]::SetWindowLongPtr($tray, -20, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TouchProbe.Win]::SetLayeredWindowAttributes($tray, 0, 0, 2)
[void][TouchProbe.Win]::ShowWindow($tray, 8)     # SW_SHOWNA

$root = [System.Windows.Automation.AutomationElement]::FromHandle($tray)
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'SystemTrayIcon')
$chevron = $null
for ($try = 0; $try -lt 60 -and $null -eq $chevron; $try++) {
    foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
        if ($null -eq $chevron -or
            $e.Current.BoundingRectangle.Left -lt $chevron.Current.BoundingRectangle.Left) {
            $chevron = $e
        }
    }
    if ($null -eq $chevron) { Start-Sleep -Milliseconds 50 }
}

function Put-Back {
    if ($null -ne $script:pattern) { try { $script:pattern.Invoke() } catch { } }
    Start-Sleep -Milliseconds 400
    [void][TouchProbe.Win]::SetLayeredWindowAttributes($tray, 0, 255, 2)
    [void][TouchProbe.Win]::SetWindowLongPtr($tray, -20, [IntPtr]$ex)
    [void][TouchProbe.Win]::ShowWindow($tray, 0)
}

if ($null -eq $chevron) { 'no chevron'; Put-Back; exit 1 }
$chevronRect = $chevron.Current.BoundingRectangle
"the chevron is at {0},{1} {2}x{3}" -f [int]$chevronRect.Left, [int]$chevronRect.Top, [int]$chevronRect.Width, [int]$chevronRect.Height
$script:pattern = $null
[void]$chevron.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$script:pattern)
$script:pattern.Invoke()

# The panel is found the way the older probe finds it -- the shell's element for it, then its window handle.
# A class-name lookup (`FindWindowEx`) did NOT see it from this script even while the bar's own press was
# opening it, and the reason is not worth chasing: this route is measured to work (probe-tray-panel-child-windows.ps1).
$flyoutCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty,
    'TopLevelWindowForOverflowXamlIsland')
$flyoutElement = $null
for ($try = 0; $try -lt 60 -and $null -eq $flyoutElement; $try++) {
    $tops = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $flyoutCond)
    if ($tops.Count -gt 0) { $flyoutElement = $tops[0] }
    if ($null -eq $flyoutElement) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $flyoutElement) { 'the panel did not open'; Put-Back; exit 1 }
$panel = [IntPtr]$flyoutElement.Current.NativeWindowHandle
if ($panel -eq [IntPtr]::Zero) { 'the panel has no window handle'; Put-Back; exit 1 }
Start-Sleep -Milliseconds 500
$panelRect = New-Object TouchProbe.Win+RECT
[void][TouchProbe.Win]::GetWindowRect($panel, [ref]$panelRect)
$script:panelLeft = $panelRect.L
$script:panelTop = $panelRect.T
"the panel is at $($panelRect.L),$($panelRect.T) $($panelRect.R - $panelRect.L)x$($panelRect.B - $panelRect.T)"

$before = Get-AppWindows $AppImage
"the application's own windows before: $($before.Count)"
foreach ($w in $before) { "  $w" }

# What is under the injected point, so "nothing happened" can be told from "the tap went elsewhere".
$pt = New-Object TouchProbe.Win+POINT
$pt.X = $script:panelLeft + 17 + ($Col * 40) + 20
$pt.Y = $script:panelTop + 17 + ($Row * 40) + 20
$under = [TouchProbe.Win]::WindowFromPoint($pt)
$underClass = New-Object System.Text.StringBuilder 256
[void][TouchProbe.Win]::GetClassName($under, $underClass, 256)
"the point $($pt.X),$($pt.Y) answers hwnd $under [$($underClass.ToString())] (the panel is $panel)"

# THE TAP ITSELF COMES FROM A TINY C++ BINARY, and that is a measurement rather than a preference: the same
# call marshalled from PowerShell failed with ERROR_INVALID_PARAMETER on this machine for every variant
# tried (struct sizes verified), while the C++ version of it returns success for down, update and up. The
# binary is built from captures/touchprobe.cpp and is not part of the bar.
if (-not (Test-Path $Injector)) {
    "no injector at $Injector"
    "build it with:  cl /nologo /EHsc /Fe:$Injector scripts\probe-tray-touch-inject.cpp user32.lib"
    Put-Back; exit 1
}

$iconX = $script:panelLeft + 17 + ($Col * 40) + 20
$iconY = $script:panelTop + 17 + ($Row * 40) + 20

$cursorBefore = New-Object TouchProbe.Win+POINT
[void][TouchProbe.Win]::GetCursorPos([ref]$cursorBefore)
"the pointer is at $($cursorBefore.X),$($cursorBefore.Y) before the tap"
"tapping the icon's own cell at $iconX,$iconY"
& $Injector $iconX $iconY | ForEach-Object { "  injector: $_" }

$frames = @()
for ($i = 0; $i -lt 12; $i++) {
    $bmp = New-Object System.Drawing.Bitmap 640, 420
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(1860, 0, 0, 0, (New-Object System.Drawing.Size 640, 420))
    $g.Dispose()
    $frames += $bmp
    Start-Sleep -Milliseconds 60
}
$dir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
for ($i = 0; $i -lt $frames.Count; $i++) {
    $frames[$i].Save((Join-Path $dir ("probe-touch-{0:D2}.png" -f $i)), [System.Drawing.Imaging.ImageFormat]::Png)
    $frames[$i].Dispose()
}

$cursorAfter = New-Object TouchProbe.Win+POINT
[void][TouchProbe.Win]::GetCursorPos([ref]$cursorAfter)
"the pointer is at $($cursorAfter.X),$($cursorAfter.Y) after the tap" +
    $(if ($cursorAfter.X -eq $cursorBefore.X -and $cursorAfter.Y -eq $cursorBefore.Y) { ' (it did NOT move)' } else { ' -- IT MOVED' })

Start-Sleep -Milliseconds 1500
$after = Get-AppWindows $AppImage
"the application's own windows after:  $($after.Count)"
foreach ($w in $after) { "  $w" }
if ($after.Count -gt $before.Count) {
    'RESULT: the injected touch made the application open something -- compare the lists above'
} else {
    'RESULT: nothing of the application appeared; the injected touch was not taken as a press'
}

Put-Back
'=== done ==='
