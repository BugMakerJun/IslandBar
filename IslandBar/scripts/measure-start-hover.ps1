# =====================================================================
#  measure-start-hover.ps1 -- does the Start card ask for frames while the pointer moves?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\captures\measure-start-hover.ps1
#          -Left 1090 -Top 30 -Right 1520 -Bottom 300
#
#  This is a MEASUREMENT, not a fix. It starts the bar with the picker open, walks the
#  pointer over the card, then turns the wheel with the pointer standing still, and
#  prints the bar's own redraw report for each phase. The report names which term forced
#  each frame -- that is what says whether "the card is choppy under the pointer" is the
#  drawing, the pacing, or the condition that decides whether to draw at all.
#
#  The card's screen rectangle is not written anywhere the log can be read for, so it is
#  FOUND rather than assumed: the walk covers a rectangle wide enough to contain it, and
#  the bar's own `start hover:` trace says which points landed on a row. The wheel is then
#  turned at a point that is known to be on the card, because the wheel is only accepted
#  over the card.
#
#  Needs a real cursor, so it must run at the shell's integrity level: SetCursorPos from
#  inside the sandbox fails with error 203.
#
#  The bar is started through the .NET process API rather than Start-Process, which hangs
#  on this machine when it launches this executable (see capture-card.ps1).
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [int]$Left   = 1090,
    [int]$Top    = 30,
    [int]$Right  = 1520,
    [int]$Bottom = 300,
    [int]$IdleMs       = 9000,
    [int]$WalkMs       = 9000,
    [int]$WheelMs      = 9000,
    [int]$SettleMs     = 3500,
    [int]$WheelNotches = 8,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Exe  = Join-Path $Root 'build\Debug\src\Debug\island_bar.exe'
$Log  = Join-Path $Root 'build\Debug\src\Debug\island_bar.log'

Add-Type -Namespace IslandProbe -Name Cursor -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, IntPtr extra);
public const uint WHEEL = 0x0800;
'@
Add-Type -AssemblyName System.Windows.Forms

function Get-Pointer { $p = New-Object IslandProbe.Cursor+POINT; [void][IslandProbe.Cursor]::GetCursorPos([ref]$p); $p }
function Move-Pointer([int]$x, [int]$y) { [void][IslandProbe.Cursor]::SetCursorPos($x, $y) }
function Spin-Wheel([int]$notches) {
    $step = if ($notches -lt 0) { -120 } else { 120 }
    for ($i = 0; $i -lt [Math]::Abs($notches); $i++) {
        [IslandProbe.Cursor]::mouse_event([IslandProbe.Cursor]::WHEEL, 0, 0, $step, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 120
    }
}

# The bar truncates its log at startup, so a reader that holds the file open sees the
# previous run. Read a fresh copy each time and never keep it.
function Read-Log { if (Test-Path $Log) { Get-Content $Log -Encoding UTF8 } else { @() } }
function Show-Report([string]$phase) {
    $tail = Read-Log | Select-Object -Last 60
    $interval = $tail | Where-Object { $_ -match 'interval \[' } | Select-Object -Last 1
    $redraws  = $tail | Where-Object { $_ -match 'redraws:' }    | Select-Object -Last 1
    Write-Host ''
    Write-Host "[$phase] $interval"
    Write-Host "[$phase] $redraws" -ForegroundColor Cyan
}

# --- the bar, alone on the screen, with the picker open -----------------
$stale = Get-Process island_bar -ErrorAction SilentlyContinue
if ($stale) { $stale | ForEach-Object { $_.Kill() }; Start-Sleep -Seconds 2 }

$saved = Get-Pointer
Remove-Item $Log -Force -ErrorAction SilentlyContinue

$env:ISLANDBAR_START_PAGE    = '1'   # the card is behind a click this tooling cannot press
$env:ISLANDBAR_NO_FULLSCREEN = '1'   # this machine always has a full-screen game in front

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName         = $Exe
$startInfo.Arguments        = '--keep-taskbar'
$startInfo.WorkingDirectory = Split-Path -Parent $Exe
$startInfo.UseShellExecute  = $false
$proc = [System.Diagnostics.Process]::Start($startInfo)

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$awayX = [int]($bounds.Width / 2)
$awayY = $bounds.Height - 40
Write-Host "[measure] screen $($bounds.Width)x$($bounds.Height)"

try {
    Start-Sleep -Milliseconds $SettleMs

    # --- phase 1: the pointer parked away from the bar, nothing to draw ---------------
    Move-Pointer $awayX $awayY
    Write-Host "[measure] parked the pointer at ($awayX,$awayY), away from the bar"
    Start-Sleep -Milliseconds $IdleMs
    Show-Report 'idle'

    # --- phase 2: the walk over the card ---------------------------------------------
    # Recorded rather than assumed: the points that land on a row are the ones the bar
    # itself reported, and one of them is where the wheel is turned.
    $hitPoints = New-Object System.Collections.ArrayList
    $seen = @{}
    foreach ($line in (Read-Log | Where-Object { $_ -match 'start hover:' })) { $seen[$line] = $true }

    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $steps = 0
    $x = $Left
    while ($watch.ElapsedMilliseconds -lt $WalkMs) {
        for ($y = $Top; $y -le $Bottom; $y += 12) {
            Move-Pointer $x $y
            $steps++
            Start-Sleep -Milliseconds 12
            if (($steps % 8) -eq 0) {
                foreach ($line in (Read-Log | Where-Object { $_ -match 'start hover: picker row' })) {
                    if (-not $seen.ContainsKey($line)) {
                        $seen[$line] = $true
                        [void]$hitPoints.Add([pscustomobject]@{ X = $x; Y = $y })
                    }
                }
            }
        }
        $x += 20
        if ($x -gt $Right) { $x = $Left }
    }
    $watch.Stop()
    Write-Host "[measure] walked $steps points over x[$Left,$Right] y[$Top,$Bottom]; $($hitPoints.Count) of them landed on a row"
    Start-Sleep -Milliseconds 1500
    Show-Report 'walk'

    # --- phase 3: the wheel, with the pointer standing still --------------------------
    if ($hitPoints.Count -gt 0) {
        $row = $hitPoints[[int]($hitPoints.Count / 2)]
        Move-Pointer $row.X $row.Y
        Start-Sleep -Milliseconds 400
        Write-Host "[measure] parked on a row at ($($row.X),$($row.Y)) and turning the wheel"
        Spin-Wheel -$WheelNotches
    } else {
        Write-Host '[measure] no row was ever under the pointer: the wheel phase is skipped' -ForegroundColor Yellow
    }
    Start-Sleep -Milliseconds $WheelMs
    Show-Report 'wheel'

    # --- what the card thought the pointer was doing ----------------------------------
    $lines = Read-Log
    Write-Host ''
    Write-Host '[measure] start hover trace (last 20):'
    ($lines | Where-Object { $_ -match 'start hover:' } | Select-Object -Last 20) |
        ForEach-Object { Write-Host "  $_" }
    Write-Host '[measure] picker wheel:'
    ($lines | Where-Object { $_ -match 'picker wheel' } | Select-Object -Last 10) |
        ForEach-Object { Write-Host "  $_" }
}
finally {
    Move-Pointer $saved.X $saved.Y
    if (-not $KeepRunning) {
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
        Remove-Item Env:\ISLANDBAR_START_PAGE    -ErrorAction SilentlyContinue
        Remove-Item Env:\ISLANDBAR_NO_FULLSCREEN -ErrorAction SilentlyContinue
        Write-Host ''
        Write-Host '[measure] the bar was stopped; the deployed copy was never touched'
    }
}
