# =====================================================================
#  probe-tray-rect-after-flyout.ps1 -- WHEN does Shell_NotifyIconGetRect
#  start telling the truth?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-rect-after-flyout.ps1
#
#  WHY THIS EXISTS
#
#  A press in the bar's tray card asks the shell where the icon is and then presses the button on that
#  rectangle. It worked from the command line and did not work when the bar did it, and the bar's own log
#  says why: the shell answered 2416,0 24x32 -- the overflow chevron's slot -- for EVERY icon. That is
#  what it answers when the icons have never been laid out: an icon in the overflow flyout only gets a
#  rectangle once that flyout has been opened, and until then every query comes back as the first slot.
#
#  (The earlier command-line runs looked correct because a probe had just opened the flyout to look at it,
#  so the rectangles were current. A measurement taken after somebody else had already done the thing being
#  measured is not a measurement.)
#
#  This runs the sequence in the order the fix needs: show the taskbar invisibly, open the flyout, and ask
#  only then -- with the command-line tool as a separate process, so the answer is the shell's and not this
#  script's.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace TrayRect -Name Win -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
[DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
'@

$GWL_EXSTYLE   = -20
$WS_EX_LAYERED = 0x00080000
$LWA_ALPHA     = 0x00000002

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exe = Join-Path $env:TEMP 'island_tray_probe\island_bar.exe'
New-Item -ItemType Directory -Force -Path (Split-Path $exe) | Out-Null
Copy-Item (Join-Path $root 'build\Debug\src\Debug\island_bar.exe') $exe -Force

$tray = [TrayRect.Win]::FindWindow('Shell_TrayWnd', $null)
if ($tray -eq [IntPtr]::Zero) { 'Shell_TrayWnd not found'; exit 1 }
$ex = [TrayRect.Win]::GetWindowLongPtr($tray, $GWL_EXSTYLE).ToInt64()
[void][TrayRect.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]($ex -bor $WS_EX_LAYERED))
[void][TrayRect.Win]::SetLayeredWindowAttributes($tray, 0, 0, $LWA_ALPHA)
[void][TrayRect.Win]::ShowWindow($tray, 8)

# The tray's UI Automation subtree takes a moment to appear; poll for the chevron rather than sleeping
# and hoping (the first version of this file waited 600 ms, found nothing, and pressed nothing).
$root0 = [System.Windows.Automation.AutomationElement]::FromHandle($tray)
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'SystemTrayIcon')
$chevron = $null
for ($try = 0; $try -lt 60 -and $null -eq $chevron; $try++) {
    foreach ($e in $root0.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
        if ($null -eq $chevron -or
            $e.Current.BoundingRectangle.Left -lt $chevron.Current.BoundingRectangle.Left) {
            $chevron = $e
        }
    }
    if ($null -eq $chevron) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $chevron) { 'the tray never published its buttons'; [void][TrayRect.Win]::ShowWindow($tray, 0); exit 1 }

$pattern = $null
[void]$chevron.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)
$pattern.Invoke()

$flyoutCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty,
    'TopLevelWindowForOverflowXamlIsland')
$flyout = $null
for ($try = 0; $try -lt 60 -and $null -eq $flyout; $try++) {
    $found = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $flyoutCond)
    if ($found.Count -gt 0) { $flyout = $found[0] }
    if ($null -eq $flyout) { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $flyout) {
    'the flyout did not open'
} else {
    "the flyout is up at $($flyout.Current.BoundingRectangle); asking the shell for the rects now"
}

$log = Join-Path $env:TEMP 'island_tray_probe\island_bar.log'
& $exe --tray-surface 2>&1 | Out-Null
Get-Content $log -Encoding UTF8 | Select-String -Pattern "tray surface|^\S+ +\[\d+\]" |
    ForEach-Object { $_.Line }

'--- putting it all back ---'
$pattern.Invoke()                       # the chevron is a toggle
Start-Sleep -Milliseconds 400
[void][TrayRect.Win]::SetLayeredWindowAttributes($tray, 0, 255, $LWA_ALPHA)
[void][TrayRect.Win]::SetWindowLongPtr($tray, $GWL_EXSTYLE, [IntPtr]$ex)
[void][TrayRect.Win]::ShowWindow($tray, 0)
'=== done ==='
