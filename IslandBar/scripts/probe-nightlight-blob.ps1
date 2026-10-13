# =====================================================================
#  probe-nightlight-blob.ps1 -- learn night light's state, and whether writing it works
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-nightlight-blob.ps1
#      ... -SkipLearn    (only test the write with what is already known)
#
#  WHY THIS EXISTS
#
#  Night light has no public API, and the quick-settings tile therefore opens a Settings page. Two
#  candidates were left, and this decides between them by measurement:
#
#   * UI Automation on the Settings page. Measured here: the switch is a BUTTON
#     (`SystemSettings_Display_BlueLight_ManualToggleOn_Button`), so UIA can drive it -- but it needs
#     the Settings window on screen for seconds.
#   * The state blob the OS itself keeps in the registry (`...bluelightreductionstate`). Writing it
#     would be instant and invisible -- but only if the written value is the one the OS expects.
#
#  So: ask the OS to switch it on through UIA, write down the blob it produces, then write that blob
#  by hand and check two things -- the registry keeps it, and the SCREEN actually changes (the mean
#  red/blue of a screenshot shifts warm). The feature is put back to OFF at the end, and the blob is
#  restored to the exact bytes that were found.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [switch]$SkipLearn,
    [switch]$KeepOn
)

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

$statePath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default$windows.data.bluelightreduction.bluelightreductionstate\windows.data.bluelightreduction.bluelightreductionstate'
$settingsPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default$windows.data.bluelightreduction.settings\windows.data.bluelightreduction.settings'

function Blob([string]$path) {
    return (Get-ItemProperty -Path $path -ErrorAction SilentlyContinue).Data
}

function ShowBlob([string]$label, [string]$path) {
    $data = Blob $path
    if (-not $data) { "$label : <none>"; return }
    "$label : $($data.Length) bytes : " + (($data | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

# Mean colour of the screen. Night light removes blue, so the mean blue of a fixed scene drops -- and
# a screenshot is the only witness that does not take the API's word for it.
function MeanColour([string]$label) {
    $bmp = New-Object System.Drawing.Bitmap 480, 270
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 480, 270))
    $r = 0; $gr = 0; $b = 0
    for ($y = 0; $y -lt 270; $y += 3) {
        for ($x = 0; $x -lt 480; $x += 3) {
            $p = $bmp.GetPixel($x, $y)
            $r += $p.R; $gr += $p.G; $b += $p.B
        }
    }
    $n = 90 * 160
    $g.Dispose(); $bmp.Dispose()
    "$label : mean R $([int]($r / $n)) G $([int]($gr / $n)) B $([int]($b / $n))  (blue/red ratio $([math]::Round($b / [math]::Max(1, $r), 3)))"
}

function FindNightLightButton {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $isFrame = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'ApplicationFrameWindow')
    $windows = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $isFrame)
    foreach ($window in $windows) {
        foreach ($id in @('SystemSettings_Display_BlueLight_ManualToggleOn_Button',
                          'SystemSettings_Display_BlueLight_ManualToggleOff_Button')) {
            $byId = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
            $found = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $byId)
            if ($found) { return $found }
        }
    }
    return $null
}

function InvokeElement($element) {
    $pattern = $null
    if (-not $element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
        return $false
    }
    $pattern.Invoke()
    return $true
}

$originalState = Blob $statePath

"=== what it looks like now ==="
ShowBlob 'state   ' $statePath
ShowBlob 'settings' $settingsPath
MeanColour 'screen  '

if ($SkipLearn) {
    'skipping the UI Automation part (-SkipLearn)'
    exit 0
}

Start-Process 'ms-settings:nightlight'
$button = $null
for ($attempt = 1; $attempt -le 12 -and -not $button; $attempt++) {
    Start-Sleep -Seconds 1
    $button = FindNightLightButton
}
if (-not $button) {
    'the night-light switch never appeared in the Settings window -- cannot learn the ON shape'
    exit 1
}

"button found: '$($button.Current.Name)' autoId='$($button.Current.AutomationId)'"

"=== switching it ON through the Settings page ==="
if (-not (InvokeElement $button)) { 'the button has no InvokePattern'; exit 1 }
Start-Sleep -Seconds 3
$onState = Blob $statePath
ShowBlob 'state ON' $statePath
ShowBlob 'sett. ON' $settingsPath
MeanColour 'screen ON'

"=== switching it back OFF through the Settings page ==="
$button2 = FindNightLightButton
if ($button2) {
    [void](InvokeElement $button2)
    Start-Sleep -Seconds 3
}
ShowBlob 'stateOFF' $statePath
MeanColour 'screenOFF'

"=== now the same switch, by writing the blob the OS just produced ==="
if (-not $onState) {
    'no ON blob was captured -- the write test cannot run'
    exit 1
}
# The ON blob is the OS's own bytes, captured a moment ago: nothing here is invented.
Set-ItemProperty -Path $statePath -Name Data -Value $onState -Type Binary
Start-Sleep -Seconds 2
ShowBlob 'written ' $statePath
MeanColour 'screen  '
"  (compare with 'screen ON' above: the same shift means the write took)"

if ($KeepOn) {
    'leaving it ON (-KeepOn)'
} else {
    "putting the bytes that were found back ..."
    Set-ItemProperty -Path $statePath -Name Data -Value $originalState -Type Binary
    Start-Sleep -Seconds 2
    ShowBlob 'restored' $statePath
    MeanColour 'screen  '
}
'done'
