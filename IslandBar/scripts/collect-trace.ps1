# =====================================================================
#  collect-trace.ps1 -- run the bar and collect its diagnostic trace
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\collect-trace.ps1
#
#  Move the pointer onto an application icon, wait a moment, then close the bar.
#
#  The trace is written to trace.txt next to this script. Open it and send the lines
#  beginning "anim:", "row:", "title:" or "icon hover:".
#
#  Why this exists: the bar prints its trace to its own stdout, and the log file it
#  writes lives under the %TEMP% of whichever shell started it -- which differs between
#  shells. Reading a stale log from another shell is what made two rounds of diagnosis
#  go the wrong way, so this script always captures the output of the run it starts.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string[]]$AppArgs = @(),
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Exe = Join-Path $Root 'build\Debug\src\Debug\island_bar.exe'

if (-not (Test-Path $Exe)) {
    Write-Host "[trace] no executable at $Exe" -ForegroundColor Red
    Write-Host '[trace] build it first: scripts\build.ps1'
    exit 1
}

if (-not $Out) { $Out = Join-Path $Root 'trace.txt' }

# Refuse to be misleading about which build is running.
$exeTime = (Get-Item $Exe).LastWriteTime
$newest = Get-ChildItem (Join-Path $Root 'src') -Recurse -Include '*.cpp', '*.h' |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newest -and $newest.LastWriteTime -gt $exeTime) {
    Write-Host "[trace] STALE: $($newest.Name) is newer than the executable." -ForegroundColor Red
    Write-Host '[trace]        Build first, or the trace describes the old code.'
    exit 1
}

Write-Host "[trace] executable: $Exe"
Write-Host "[trace] built:      $exeTime"
Write-Host ''
Write-Host '  Move the pointer onto an APPLICATION ICON beside the island and hold it there.'
Write-Host '  Then close the island window.'
Write-Host ''

$stdout = Join-Path $env:TEMP 'island_bar-trace.out'
Remove-Item $stdout -Force -ErrorAction SilentlyContinue

# An EMPTY array is rejected by -ArgumentList ("the argument is null or empty"), so the parameter
# is omitted entirely when there is nothing to pass. Passing @() looks harmless and fails at the
# call, which is how this was found.
$startArgs = @{
    FilePath               = $Exe
    PassThru               = $true
    NoNewWindow            = $true
    RedirectStandardOutput = $stdout
}
if ($AppArgs.Count -gt 0) { $startArgs['ArgumentList'] = $AppArgs }

$p = Start-Process @startArgs
if (-not $p) { throw 'the program could not be started' }
$p.WaitForExit()

# Copied before reading: the program holds its own log open, but its stdout is done with.
Copy-Item $stdout $Out -Force -ErrorAction SilentlyContinue

$lines = @()
if (Test-Path $Out) { $lines = Get-Content $Out -ErrorAction SilentlyContinue }

$interesting = $lines | Where-Object { $_ -match 'anim:|row:|title:|icon hover:|state:' }

Write-Host ''
Write-Host "[trace] full output: $Out" -ForegroundColor Cyan
Write-Host ("[trace] {0} lines, {1} interesting" -f $lines.Count, $interesting.Count) -ForegroundColor Cyan
Write-Host ''
if ($interesting.Count -eq 0) {
    Write-Host '[trace] no trace lines. That means the pointer was never seen on an icon,' -ForegroundColor Yellow
    Write-Host '[trace] which is itself the answer -- send this whole message.'
} else {
    $interesting | Select-Object -Last 40 | ForEach-Object { Write-Host "  $_" }
}
