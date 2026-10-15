# =====================================================================
#  run-demo.ps1 -- run the bar through a fixed scenario and keep its log
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\regression\run-demo.ps1
#      ... -Name startup -Seconds 15 -Config Release [-Deploy] [-StopRunning] [-Exe <path>]
#
#  WHY THIS EXISTS. --self-test asserts pure logic; nothing asserts what the bar DOES. Every structural
#  change to the frame loop (splitting a 5,200-line function into phases, for instance) needs a way to say
#  "the program still does the same things, in the same order", and that evidence has to be mechanical.
#  This script produces one run of it; normalize-log.ps1 turns it into something that can be diffed against
#  a baseline kept in the repository.
#
#  FIXED BY CONSTRUCTION, so the run is reproducible:
#    * --keep-taskbar, so a diagnostic run never touches the user's own taskbar;
#    * ISLANDBAR_NO_MEDIA=1, because whatever is playing changes every few minutes;
#    * ISLANDBAR_NO_FULLSCREEN=1, because a game in front makes the bar stand down by design;
#    * Started through the .NET Process API, NOT Start-Process: measured 2026-10-16, Start-Process (which
#      goes through the shell) blocked for minutes on this machine while the same binary started in under a
#      second this way. See HANDOFF 8.152 for the same lesson from the other direction.
#
#  WHAT IT REFUSES TO DO: run while another island_bar is up. The program holds a single-instance mutex, so
#  a second copy exits immediately and the run would quietly capture nothing. -StopRunning closes the one
#  that is there (it asks the window to close, which is the path that puts the taskbar back).
#
#  IT RUNS THE DEPLOYED COPY, and that is not a preference: MEASURED 2026-10-16, an executable whose image
#  lives under the project tree starts at LOW integrity, and the run then says so in its own log
#  ("integrity: LOWER than the shell's") while seeing 28 processes, two tray entries and no location -- a
#  different machine from the one the bar actually runs on. -Deploy copies the freshly built pair over
#  first, which is what a slice's acceptance wants anyway.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Name = 'startup',
    [int]$Seconds = 15,
    [ValidateSet('Debug', 'Release')] [string]$Config = 'Release',
    [string]$Exe = '',
    [switch]$Deploy,
    [switch]$StopRunning
)

$ErrorActionPreference = 'Stop'

# ...\IslandBar\scripts\regression\run-demo.ps1 -> ...\IslandBar
$Here = Split-Path -Parent $PSCommandPath
$Root = Split-Path -Parent (Split-Path -Parent $Here)
$Out = Join-Path $Here 'out'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# The deployed copy by default (see the note above about integrity).
$Built = Join-Path $Root "build\$Config\src\$Config"
$Deployed = Join-Path $env:USERPROFILE 'IslandBar'
if (-not $Exe) { $Exe = Join-Path $Deployed 'island_bar.exe' }

if ($Deploy) {
    if (-not (Test-Path "$Built\island_bar.exe")) { throw "nothing built at $Built" }
    New-Item -ItemType Directory -Force -Path $Deployed | Out-Null
    Copy-Item "$Built\island_bar.exe" $Deployed -Force
    Copy-Item "$Built\island_tray_hook.dll" $Deployed -Force
    Write-Host "[demo] deployed the $Config build to $Deployed"
}

if (-not (Test-Path $Exe)) { throw "no executable at $Exe -- build it first (scripts\build.ps1 -Config $Config)" }
if ($Exe -like "$Root*") {
    Write-Host "[demo] WARNING: $Exe is inside the project tree -- it will start at LOW integrity and the"
    Write-Host "[demo]          run will not describe this machine (see the note at the top of this script)"
}

$running = Get-Process island_bar -ErrorAction SilentlyContinue
if ($running) {
    if (-not $StopRunning) {
        throw "island_bar is already running (pid $($running[0].Id)); close it first, or pass -StopRunning"
    }
    Write-Host "[demo] asking the running bar (pid $($running[0].Id)) to close"
    foreach ($process in $running) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(8000)) { $process.Kill() }
    }
    Start-Sleep -Seconds 2
}

$log = Join-Path (Split-Path -Parent $Exe) 'island_bar.log'
Remove-Item $log -Force -ErrorAction SilentlyContinue

$env:ISLANDBAR_NO_MEDIA = '1'
$env:ISLANDBAR_NO_FULLSCREEN = '1'

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = '--keep-taskbar'
$psi.WorkingDirectory = Split-Path -Parent $Exe
$psi.UseShellExecute = $false

Write-Host "[demo] ${Name}: running $Exe for $Seconds s"
$started = Get-Date
$process = [System.Diagnostics.Process]::Start($psi)

Start-Sleep -Seconds $Seconds

if (-not $process.HasExited) {
    [void]$process.CloseMainWindow()
    if (-not $process.WaitForExit(8000)) { $process.Kill(); $process.WaitForExit(3000) }
}

$elapsed = [int]((Get-Date) - $started).TotalSeconds
$raw = Join-Path $Out "$Name-raw.txt"
if (-not (Test-Path $log)) { throw "no log was written at $log -- the run produced nothing" }
Copy-Item $log $raw -Force

# The exit code is reported rather than asserted: a deliberate close is the normal ending here, and a
# crash is visible in the log (and in the code below, which is non-zero for anything but a clean exit).
$code = if ($process.HasExited) { $process.ExitCode } else { -1 }
$rawLines = (Get-Content $raw -Encoding UTF8 | Measure-Object).Count
Write-Host ("[demo] {0}: {1} s, exit {2}, {3} line(s) -> {4}" -f $Name, $elapsed, $code, $rawLines, $raw)
