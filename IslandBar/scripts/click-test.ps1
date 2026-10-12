# =====================================================================
#  click-test.ps1 -- press the panel's controls for real, and report
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\click-test.ps1 `
#          -Exe .\build\Debug\src\Debug\island_bar.exe
#
#  The bar's clicks are the one thing a screenshot cannot check: the geometry can be
#  asserted from the layout, but "the control that was drawn there is the one that acts"
#  needs an actual press at that point.
#
#  This script moves the pointer ONLY between the saved position and the controls on the
#  card, and puts it back afterwards. It presses, in order: the speaker (mute), the
#  speaker again (unmute), the slider at 60%, the output-device button (which opens the
#  sound page), the page's back arrow, and finally the island (which closes the panel).
#
#  Everything it does is reversible and every step is logged by the bar itself, so the
#  bar's own log is the evidence: each action prints what it was taken to mean.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [int]$SettleMs = 2600,
    [int]$BetweenMs = 700,
    # Where the CARD is, in screen pixels, at 100% scaling. Measured from a capture: the
    # card is 520 DIP wide and hangs from the bar, so these are its own edges.
    [int]$CardLeft = 853,
    [int]$CardRight = 1373,
    [int]$CardTop = 54,
    # The slider row's centre, from the card's bottom: 14 padding, 28 settings row, 8 gap,
    # then half of the 22 DIP slider row.
    [int]$SliderCentreY = 243
    ,
    # Put the volume back where it was before the test, in percent, and do nothing
    # else. The test changes the machine's own volume -- that is the point of it --
    # and this is how the old value is restored.
    [int]$RestorePercent = -1
    ,
    # Which set of presses to make. Every one of them is reversible and every one of them
    # is reported by the bar's own log.
    [ValidateSet('controls', 'drag', 'sweep', 'wheel', 'tile')]
    [string]$Scenario = 'controls'
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace IslandBar -Name NativeInput -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT point);
public struct POINT { public int X; public int Y; }
'@

function Get-CursorPoint {
    $point = New-Object IslandBar.NativeInput+POINT
    [void][IslandBar.NativeInput]::GetCursorPos([ref]$point)
    return $point
}

function Invoke-Click {
    param([int]$X, [int]$Y, [string]$What)
    [void][IslandBar.NativeInput]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 120
    # Left down, then up: the bar acts on the UP, and the window is click-through everywhere
    # except the card, so a press that misses the card reaches the desktop instead.
    [IslandBar.NativeInput]::mouse_event(0x0002, 0, 0, 0, [System.UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [IslandBar.NativeInput]::mouse_event(0x0004, 0, 0, 0, [System.UIntPtr]::Zero)
    Write-Output "clicked $What at ($X,$Y)"
    Start-Sleep -Milliseconds $BetweenMs
}

function Invoke-Press {
    param([int]$X, [int]$Y)
    [void][IslandBar.NativeInput]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 120
    [IslandBar.NativeInput]::mouse_event(0x0002, 0, 0, 0, [System.UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
}

function Invoke-Release {
    param([int]$X, [int]$Y)
    [void][IslandBar.NativeInput]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 260
    [IslandBar.NativeInput]::mouse_event(0x0004, 0, 0, 0, [System.UIntPtr]::Zero)
    Write-Output "released at ($X,$Y)"
    Start-Sleep -Milliseconds $BetweenMs
}

function Invoke-Wheel {
    param([int]$X, [int]$Y, [int]$Notches)
    [void][IslandBar.NativeInput]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 150
    # 0x0800 is MOUSEEVENTF_WHEEL; a notch is 120, positive is away from the user (up). The signed delta is
    # bit-cast to the unsigned parameter the API declares, or PowerShell refuses the negative value.
    $delta = [int32]($Notches * 120)
    [IslandBar.NativeInput]::mouse_event(0x0800, 0, 0, [uint32]([System.BitConverter]::ToUInt32([System.BitConverter]::GetBytes($delta), 0)), [System.UIntPtr]::Zero)
    Write-Output "wheel $Notches notch(es) at ($X,$Y)"
    Start-Sleep -Milliseconds $BetweenMs
}

$saved = Get-CursorPoint
Write-Output "cursor saved at ($($saved.X),$($saved.Y))"

$env:ISLANDBAR_PANEL_LOCK = '1'

try {
    $process = Start-Process -FilePath $Exe `
        -ArgumentList @('--keep-taskbar', '--state', 'expanded', '--system-panel') -PassThru
    Start-Sleep -Milliseconds $SettleMs

    $sliderLeft = $CardLeft + 14 + 26
    $sliderRight = $CardRight - 14 - 40 - 6

    if ($RestorePercent -ge 0) {
        Invoke-Click ([int]($sliderLeft + ($sliderRight - $sliderLeft) * ($RestorePercent / 100.0))) `
            $SliderCentreY "the slider at $RestorePercent% (restoring)"
        return
    }

    # The tiles: three columns of 152 DIP with 8 DIP gaps, two rows of 73 DIP, from the card's 14 DIP pad.
    $tileWidth = 152
    $tileGap = 8
    $tileCentreY = $CardTop + 14 + 25
    $tile2X = $CardLeft + 14 + 2 * ($tileWidth + $tileGap) + [int]($tileWidth / 2)

    if ($Scenario -eq 'tile') {
        # 飞行模式 is the third tile of the first row, and it is a TOGGLE: the press should switch the Wi-Fi
        # radio, and the panel should then put itself away.
        Invoke-Click $tile2X $tileCentreY 'the third tile of the first row (飞行模式)'
        return
    }

    if ($Scenario -eq 'drag') {
        # Press at ~49% of the track, DRAG to ~85%, release. A slider that only acted on the release would
        # show one jump at the end and nothing in between.
        $from = [int]($sliderLeft + ($sliderRight - $sliderLeft) * 0.49)
        $to = [int]($sliderLeft + ($sliderRight - $sliderLeft) * 0.85)
        Invoke-Press $from $SliderCentreY
        [void][IslandBar.NativeInput]::SetCursorPos($to, $SliderCentreY)
        Start-Sleep -Milliseconds 400
        Invoke-Release $to $SliderCentreY
        return
    }

    if ($Scenario -eq 'sweep') {
        # The same drag, but moved in small steps, which is what a hand does. A slider that only acts on the
        # press and the release shows two values in the log; one that follows shows the whole sweep.
        Invoke-Press ([int]($sliderLeft + ($sliderRight - $sliderLeft) * 0.15)) $SliderCentreY
        for ($step = 1; $step -le 9; $step++) {
            $x = [int]($sliderLeft + ($sliderRight - $sliderLeft) * (0.15 + 0.08 * $step))
            [void][IslandBar.NativeInput]::SetCursorPos($x, $SliderCentreY)
            Start-Sleep -Milliseconds 90
        }
        Invoke-Release ([int]($sliderLeft + ($sliderRight - $sliderLeft) * 0.87)) $SliderCentreY
        return
    }

    if ($Scenario -eq 'wheel') {
        # Open the page, then wheel over it. Without the wheel the page cannot be scrolled at all.
        Invoke-Click ($CardRight - 14 - 20) $SliderCentreY 'the output-device button'
        Invoke-Wheel ($CardLeft + 200) ($CardTop + 120) -2
        Invoke-Wheel ($CardLeft + 200) ($CardTop + 120) -1
        return
    }

    Invoke-Click ($CardLeft + 24) $SliderCentreY 'the speaker (mute)'
    Invoke-Click ($CardLeft + 24) $SliderCentreY 'the speaker again (unmute)'
    Invoke-Click ([int]($sliderLeft + ($sliderRight - $sliderLeft) * 0.6)) $SliderCentreY 'the slider at 60%'
    Invoke-Click ($CardRight - 14 - 20) $SliderCentreY 'the output-device button'
    Invoke-Click ($CardLeft + 14 + 13) ($CardTop + 20) 'the sound page''s back arrow'
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Remove-Item Env:\ISLANDBAR_PANEL_LOCK -ErrorAction SilentlyContinue
    [void][IslandBar.NativeInput]::SetCursorPos($saved.X, $saved.Y)
    Write-Output "cursor restored to ($($saved.X),$($saved.Y))"
}
