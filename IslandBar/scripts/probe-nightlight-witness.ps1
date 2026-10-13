# =====================================================================
#  probe-nightlight-witness.ps1 -- does writing night light's state value actually switch it?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-nightlight-witness.ps1
#
#  WHY THIS EXISTS
#
#  Two candidate implementations, and the cheap one needs one fact first.
#
#   * the Settings page's own switch, driven through UI Automation -- verified to work, but it puts a
#     Settings window on screen for a few seconds.
#   * writing the state blob the OS keeps in the registry, which would be instant and invisible.
#
#  The first witness tried for the second one was a screenshot, and it was the wrong instrument:
#  screenshots read the framebuffer BEFORE the output gamma ramp is applied (measured: mean colour
#  identical with the setting on and off). The gamma ramp itself read back flat here, so it says
#  nothing either.
#
#  What is left is the OS's OWN answer: the Settings page's switch is labelled "turn it on now" while the
#  feature is off and "turn it off now" while it is on. So this writes the value and then asks that page --
#  which also happens to be the same page the other candidate would drive.
#
#  Nothing is invented: the "on" value is the one captured from the OS in probe-nightlight-blob.ps1.
#  The original bytes are written back at the end.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$statePath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default$windows.data.bluelightreduction.bluelightreductionstate\windows.data.bluelightreduction.bluelightreductionstate'

$offBytes = [byte[]](0x43,0x42,0x01,0x00,0x0A,0x02,0x01,0x00,0x2A,0x2A,0x00,0x00,0x00)
$onBytes  = [byte[]](0x43,0x42,0x01,0x00,0x0A,0x02,0x01,0x00,0x2A,0x06,0xBA,0xE3,0xB9,0xD6,0x06,0x2A,0x2B,0x0E,0x15,0x43,0x42,0x01,0x00,0x10,0x00,0xD0,0x0A,0x02,0xC6,0x14,0xB3,0xD4,0xFA,0xCD,0xCB,0xE7,0xD6,0xEE,0x01,0x00,0x00,0x00,0x00)

$original = (Get-ItemProperty -Path $statePath).Data

function SwitchLabel {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $isFrame = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'ApplicationFrameWindow')
    foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $isFrame)) {
        $byPrefix = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            'SystemSettings_Display_BlueLight_ManualToggleOn_Button')
        $found = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $byPrefix)
        if ($found) { return "$($found.Current.Name) [$($found.Current.AutomationId)]" }
        $byPrefix2 = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            'SystemSettings_Display_BlueLight_ManualToggleOff_Button')
        $found2 = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $byPrefix2)
        if ($found2) { return "$($found2.Current.Name) [$($found2.Current.AutomationId)]" }
    }
    return '<no night-light switch on screen>'
}

Start-Process 'ms-settings:nightlight'
Start-Sleep -Seconds 4
"switch with the OFF value written : " + (SwitchLabel)

"writing the ON value ..."
Set-ItemProperty -Path $statePath -Name Data -Value $onBytes -Type Binary
Start-Sleep -Seconds 2
"switch with the ON value written  : " + (SwitchLabel)

# The page caches what it read when it opened, so asking it again after a fresh open is the honest
# reading: close it and let ms-settings start a new one.
Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
Start-Process 'ms-settings:nightlight'
Start-Sleep -Seconds 4
"after reopening the page          : " + (SwitchLabel)

"putting the bytes that were found back ..."
Set-ItemProperty -Path $statePath -Name Data -Value $original -Type Binary
Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
Start-Process 'ms-settings:nightlight'
Start-Sleep -Seconds 4
"switch after restoring the OFF value: " + (SwitchLabel)
Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
'done'
