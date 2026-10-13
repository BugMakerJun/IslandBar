# =====================================================================
#  probe-toggle-extras.ps1 -- the three tiles that are not radios
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-toggle-extras.ps1
#      ... -SkipNightLight   (leave the user's screen colour alone)
#
#  Three separate questions, one script because they are all cheap:
#
#   1. LIVE CAPTIONS. Win+Ctrl+L is the documented shortcut, so a synthetic one is a candidate --
#      measured by whether LiveCaptions.exe appears and disappears again.
#   2. NIGHT LIGHT. There is no public API. The candidate is the Settings page's own toggle driven
#      through UI Automation's TOGGLE pattern (a supported pattern, unlike the tray's right-click,
#      which does not exist). Measured by the value the OS writes to its own state blob in the
#      registry -- which is also how the result is checked afterwards.
#   3. ENERGY SAVER. Reported as "no interface" too; this only asks powercfg what exists.
#
#  Everything it changes is changed back. ASCII-only on purpose.
# =====================================================================

[CmdletBinding()]
param(
    [switch]$SkipNightLight,
    [switch]$SkipCaptions
)

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -Namespace ToggleProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion u; }
[StructLayout(LayoutKind.Explicit)] public struct InputUnion {
    [FieldOffset(0)] public MOUSEINPUT mi;
    [FieldOffset(0)] public KEYBDINPUT ki;
}
[StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
[DllImport("user32.dll")] public static extern uint SendInput(uint count, INPUT[] inputs, int size);
public const uint INPUT_KEYBOARD = 1;
public const uint KEYEVENTF_KEYUP = 2;
public const ushort VK_LWIN = 0x5B, VK_CONTROL = 0x11, VK_L = 0x4C;
'@

function Send-Keys([uint16[]]$keys) {
    $inputs = @()
    foreach ($k in $keys) { $inputs += New-Object ToggleProbe.Win+INPUT -Property @{ type = 1; u = (New-Object ToggleProbe.Win+InputUnion -Property @{ ki = (New-Object ToggleProbe.Win+KEYBDINPUT -Property @{ wVk = $k }) }) } }
    for ($i = $keys.Count - 1; $i -ge 0; $i--) { $inputs += New-Object ToggleProbe.Win+INPUT -Property @{ type = 1; u = (New-Object ToggleProbe.Win+InputUnion -Property @{ ki = (New-Object ToggleProbe.Win+KEYBDINPUT -Property @{ wVk = $keys[$i]; dwFlags = 2 }) }) } }
    [void][ToggleProbe.Win]::SendInput($inputs.Count, $inputs, [System.Runtime.InteropServices.Marshal]::SizeOf([type][ToggleProbe.Win+INPUT]))
}

function NightLightState {
    $path = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default$windows.data.bluelightreduction.bluelightreductionstate\windows.data.bluelightreduction.bluelightreductionstate'
    $data = (Get-ItemProperty -Path $path -ErrorAction SilentlyContinue).Data
    if (-not $data) { return 'no blob' }
    # The blob is 13 bytes when the feature is OFF and longer once it is on, because switching it on
    # writes a timestamp into it. Length is a coarse but sufficient reading here; the bytes are printed
    # as well so the real shape is on the record.
    return "$($data.Length) bytes: " + (($data | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

if ($SkipCaptions) {
    "=== 1. live captions -- skipped (-SkipCaptions) ==="
} else {
"=== 1. live captions (Win+Ctrl+L) ==="
"before: LiveCaptions.exe running = " + [bool](Get-Process LiveCaptions -ErrorAction SilentlyContinue)
Send-Keys @([ToggleProbe.Win]::VK_LWIN, [ToggleProbe.Win]::VK_CONTROL, [ToggleProbe.Win]::VK_L)
Start-Sleep -Seconds 3
$afterOn = [bool](Get-Process LiveCaptions -ErrorAction SilentlyContinue)
"after Win+Ctrl+L: LiveCaptions.exe running = $afterOn"
if ($afterOn) {
    Send-Keys @([ToggleProbe.Win]::VK_LWIN, [ToggleProbe.Win]::VK_CONTROL, [ToggleProbe.Win]::VK_L)
    Start-Sleep -Seconds 4
    "after pressing it again: LiveCaptions.exe running = " + [bool](Get-Process LiveCaptions -ErrorAction SilentlyContinue)
    "  (the process can take a moment to go away; measured: it does)"
}
}

""
"=== 2. night light ==="
"before: " + (NightLightState)

if ($SkipNightLight) {
    'skipped (-SkipNightLight)'
} else {
    Start-Process 'ms-settings:nightlight'

    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $isToggle = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::ToggleSwitch)

    # The Settings app is a WinUI application in its own frame and takes a few seconds to publish its
    # tree; the first version of this waited a flat 3 s and searched only the DIRECT children of the
    # desktop, which found nothing. So: retry for up to 15 s, and search every top-level window's
    # descendants, printing the windows that do have toggles.
    $found = $null
    $windowsWithToggles = @()
    for ($attempt = 1; $attempt -le 15 -and -not $found; $attempt++) {
        Start-Sleep -Seconds 1
        $windowsWithToggles = @()
        foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
            $name = ''
            try { $name = $window.Current.Name } catch { }
            if (-not $name) { continue }
            $toggles = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $isToggle)
            if ($toggles.Count -gt 0) {
                $windowsWithToggles += ("    window '$name' has $($toggles.Count) toggle(s): " +
                    (($toggles | ForEach-Object { "'$($_.Current.Name)'" }) -join ', '))
                if (-not $found) { $found = $toggles[0] }
            }
        }
    }

    if ($windowsWithToggles.Count -gt 0) {
        $windowsWithToggles | Select-Object -Unique | ForEach-Object { $_ }
    }

    if ($found) {
        $pattern = $null
        if ($found.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) {
            "  toggling '$($found.Current.Name)' (was $($pattern.Current.ToggleState)) ..."
            $pattern.Toggle()
            Start-Sleep -Seconds 3
            "  after   : " + (NightLightState)
            "  toggling back ..."
            $pattern.Toggle()
            Start-Sleep -Seconds 3
            "  restored: " + (NightLightState)
        } else {
            '  the toggle has no TogglePattern -- UIA cannot drive it'
        }
    } else {
        '  no ToggleSwitch found in any open window; the visible top-level windows were:'
        foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
            $name = ''
            $cls = ''
            try { $name = $window.Current.Name } catch { }
            try { $cls = $window.Current.ClassName } catch { }
            if ($name -or $cls) { "    '$name' [$cls]" }
        }
    }
}

""
"=== 3. energy saver ==="
powercfg /getactivescheme
"energy-saver subgroup in the active scheme:"
powercfg /q SCHEME_CURRENT SUB_ENERGYSAVER 2>&1 | Select-Object -First 12

'done'
