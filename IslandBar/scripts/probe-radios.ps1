# =====================================================================
#  probe-radios.ps1 -- can a desktop process turn the Wi-Fi and Bluetooth radios on and off?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-radios.ps1
#      ... -Toggle Bluetooth      (off, then back to what it was)
#
#  WHY THIS EXISTS
#
#  The quick-settings tiles for Wi-Fi and Bluetooth open the Settings page instead of switching
#  anything, and the note in this project said the reason is that `Windows.Devices.Radios` needs
#  PACKAGE IDENTITY -- which an ordinary .exe does not have. That claim has never been measured
#  here, and it is the difference between a tile that toggles and a tile that opens a window, so it
#  is measured now: access status first, then the radios themselves.
#
#  -Toggle writes, and only to the radio named on the command line. It remembers the state and puts
#  it back. Wi-Fi is deliberately not toggled by default: this machine reaches the network over it.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Toggle = ''
)

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]

function Await($WinRtOperation, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtOperation))
    $netTask.Wait(-1) | Out-Null
    return $netTask.Result
}

[void][Windows.Devices.Radios.Radio, Windows.Devices.Radios, ContentType = WindowsRuntime]
[void][Windows.Devices.Radios.RadioState, Windows.Devices.Radios, ContentType = WindowsRuntime]
[void][Windows.Devices.Radios.RadioKind, Windows.Devices.Radios, ContentType = WindowsRuntime]
[void][Windows.Devices.Radios.RadioAccessStatus, Windows.Devices.Radios, ContentType = WindowsRuntime]

$access = Await ([Windows.Devices.Radios.Radio]::RequestAccessAsync()) ([Windows.Devices.Radios.RadioAccessStatus])
"access status: $access   (Allowed = the toggles can be written from this process)"

# RadioAccessStatus: 0 Unspecified, 1 Allowed, 2 DeniedByUser, 3 DeniedBySystem
if ($access -ne [Windows.Devices.Radios.RadioAccessStatus]::Allowed) {
    'NOT ALLOWED -- the tiles can only open a settings page from here.'
}

$listType = [System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]]
$radios = Await ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) $listType
"radios: $($radios.Count)"
foreach ($radio in $radios) {
    "  [$($radio.Kind)] '$($radio.Name)' state=$($radio.State)"
}

if ($Toggle) {
    $target = $radios | Where-Object { "$($_.Kind)" -eq $Toggle } | Select-Object -First 1
    if (-not $target) {
        "no radio whose Kind is '$Toggle' (allowed: WiFi, Bluetooth, MobileBroadband, FM)"
        exit 1
    }

    $before = $target.State
    "toggling '$($target.Name)' from $before"
    $off = if ($before -eq [Windows.Devices.Radios.RadioState]::On) {
        [Windows.Devices.Radios.RadioState]::Off
    } else {
        [Windows.Devices.Radios.RadioState]::On
    }

    $result = Await ($target.SetStateAsync($off)) ([Windows.Devices.Radios.RadioAccessStatus])
    Start-Sleep -Milliseconds 800
    "  SetStateAsync($off) returned $result; the radio now says $($target.State)"

    "  putting $before back ..."
    $back = Await ($target.SetStateAsync($before)) ([Windows.Devices.Radios.RadioAccessStatus])
    Start-Sleep -Milliseconds 800
    "  SetStateAsync($before) returned $back; the radio now says $($target.State)"
}

'done'
