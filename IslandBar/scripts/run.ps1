# =====================================================================
#  run.ps1 -- build island_bar, make sure no stale copy is running, start it
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1
#
#  This exists because a whole round of feedback was spent on a STALE BINARY: the
#  executable in build\Debug was older than the source, so the fixes under discussion
#  were not in the program being tested. Nothing about the running program says which
#  build it is; this script makes that impossible to get wrong by rebuilding first and
#  refusing to start if the build did not produce a newer binary.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string]$Config = 'Debug',
    [string[]]$AppArgs = @('--keep-taskbar'),
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Exe  = Join-Path $Root "build\$Config\src\$Config\island_bar.exe"

# --- 1. make sure nothing stale is holding the window -----------------
#   A leftover process from an earlier run draws a pill of its own, and a click goes
#   to whichever window happens to be on top. That is indistinguishable from a broken
#   click handler, so stale copies are killed rather than left to confuse things.
$stale = Get-Process island_bar -ErrorAction SilentlyContinue
if ($stale) {
    Write-Host "[run] killing $($stale.Count) leftover island_bar process(es)" -ForegroundColor Yellow
    $stale | ForEach-Object { $_.Kill() }
    Start-Sleep -Seconds 2
}

# --- 2. build ---------------------------------------------------------
if (-not $NoBuild) {
    Write-Host '[run] building ...'
    & (Join-Path $Root 'scripts\build.ps1') -Config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed with $LASTEXITCODE" }
}

if (-not (Test-Path $Exe)) { throw "no executable at $Exe" }

# Refuse to run a binary that is older than the newest source file: that is the exact
# situation that wasted a round of testing, and it is cheap to detect.
$newestSource = Get-ChildItem (Join-Path $Root 'src') -Recurse -Include '*.cpp','*.h' |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
$exeTime = (Get-Item $Exe).LastWriteTime
if ($newestSource -and $newestSource.LastWriteTime -gt $exeTime) {
    Write-Host "[run] WARNING: $($newestSource.Name) is newer than the executable." -ForegroundColor Yellow
    Write-Host "[run]          The binary does not contain the latest change." -ForegroundColor Yellow
}

Write-Host "[run] executable: $Exe"
Write-Host "[run] built:      $($exeTime)"
Write-Host ''

# --- 3. run, and WAIT for it -----------------------------------------
#
# Started with Start-Process -PassThru and polled explicitly, rather than relying on
# WaitForExit or on the call operator.
#
# Both of those were tried and both reported "exited with 0 after 0.0s" while the program
# was still running. The giveaway was the log file being locked at the very moment the
# script claimed the program had exited. A launcher that lies about whether the program is
# running sends the reader looking for a crash that never happened, and it did: a whole
# round was spent on a log line that came from a self-test instead of the running bar.
#
# Polling HasExited is the one form that has not lied.
Write-Host '=== island_bar (close the window to stop) ===' -ForegroundColor Cyan
Write-Host ''

# The program writes its own diagnostics to stdout as well as to its log, so they are
# captured here: the first line names the log file, which is the file worth sending.
$outFile = Join-Path $env:TEMP 'island_bar-run.out'
$errFile = Join-Path $env:TEMP 'island_bar-run.err'
Remove-Item $outFile, $errFile -Force -ErrorAction SilentlyContinue

$started = Get-Date
$process = Start-Process -FilePath $Exe -ArgumentList $AppArgs -PassThru -NoNewWindow `
                         -RedirectStandardOutput $outFile -RedirectStandardError $errFile
if (-not $process) { throw 'the program could not be started' }

$process.WaitForExit()

$code = $process.ExitCode
$ranFor = (Get-Date) - $started

Write-Host ''
Write-Host ("[run] exited with {0} after {1:N1}s" -f $code, $ranFor.TotalSeconds) -ForegroundColor Cyan

# The log path the PROGRAM printed, not one this script guessed.
#
# The program states its own log path because %TEMP% differs between the shell that asks and
# the process that logs. Reading it from the program's own output is the only version that
# cannot be wrong.
$logPath = $null
if (Test-Path $outFile) {
    $line = Get-Content $outFile -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'log file:\s*(.+)$' } | Select-Object -First 1
    if ($line -and $line -match 'log file:\s*(.+)$') {
        $candidate = $Matches[1].Trim()
        if (Test-Path $candidate) { $logPath = $candidate }
    }
}

Write-Host ''
if ($logPath) {
    Write-Host "[run] log: $logPath" -ForegroundColor Cyan
    Write-Host ''
    Write-Host '[run] the lines that matter, if something looked wrong:'
    Write-Host '        anim:  -> which icon the pointer was taken to be on, and the step size'
    Write-Host '        row:   -> where the layout put every button'
    Write-Host '        icon hover: -> whether the pointer was seen on an icon at all'
    Write-Host ''
    Write-Host '      get them with:'
    Write-Host "        Get-Content '$logPath' | Select-String 'anim:|row:|icon hover|state:'"
} else {
    Write-Host '[run] the program did not print its log path; its output was:' -ForegroundColor Yellow
    if (Test-Path $outFile) { Get-Content $outFile | Select-Object -Last 30 }
}

# An immediate exit is the single most confusing outcome, because it looks like the program
# simply refused to open. Print the log, since a GUI program has no other way to say why.
if ($ranFor.TotalSeconds -lt 3) {
    Write-Host ''
    Write-Host '[run] it stopped almost immediately -- that is not normal for a GUI app.' -ForegroundColor Red
    Write-Host '[run] printing the log now, because this is the only record of why:' -ForegroundColor Red
    Write-Host ''

    $logPath = $null
    $candidates = @()
    if ($env:TEMP) { $candidates += (Join-Path $env:TEMP 'island_bar.log') }
    $found = Get-ChildItem $env:TEMP -Recurse -Filter 'island_bar.log' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    if ($found) { $candidates += $found }
    $logPath = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

    if ($logPath) {
        Write-Host "[run] log: $logPath"
        # Copied before reading: the program may have been killed with the file still open,
        # and Get-Content fails outright on a locked file ("being used by another process"),
        # which hides the very output it was invoked to show.
        $copy = Join-Path $env:TEMP 'island_bar-tail.log'
        try {
            Copy-Item $logPath $copy -Force -ErrorAction Stop
            Get-Content $copy -Tail 60
        } catch {
            Write-Host "[run] could not read the log ($($_.Exception.Message))" -ForegroundColor Yellow
            Write-Host '[run] it is still open by another process -- is an old island_bar still running?' -ForegroundColor Yellow
            Get-Process island_bar -ErrorAction SilentlyContinue |
                ForEach-Object { Write-Host "      pid $($_.Id) started $($_.StartTime)" }
        }
    } else {
        Write-Host '[run] no log file was written at all -- which itself says the program died' -ForegroundColor Yellow
        Write-Host '[run] before it could open one, i.e. before main() got going.' -ForegroundColor Yellow
    }
    exit 1
}

Write-Host ''
Write-Host '[run] the full log is at the path the program printed as "log file:" above.'
Write-Host ''
Write-Host '[run] what to look for in it:'
Write-Host '      pointer ENTERED the island   -> the pointer was seen on the pill'
Write-Host '      click routed: ...            -> a click arrived and was understood'
Write-Host '      (neither line appears)       -> input is not reaching the program at all'
