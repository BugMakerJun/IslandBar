# =====================================================================
#  bar-log.ps1 -- show the trace from the most recent run of the bar
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\bar-log.ps1
#      ... -All        show every line, not just the trace
#      ... -Follow     keep watching while the bar runs
#
#  WHY THIS DOES NOT ASK THE PROGRAM WHERE ITS LOG IS
#
#  It did, and that was the problem. The program prints its log path to its own stdout,
#  and the log lives under the %TEMP% of whichever shell started it -- so a script that
#  reads the path is reading a path that belongs to another shell's environment, and a
#  search from the wrong shell finds nothing. Worse, redirecting the program's stdout
#  proved unreliable here: the file came back empty, so "the program did not print its log
#  path" was reported for a program that prints it every time.
#
#  So instead of being told, this searches every plausible %TEMP% for island_bar logs --
#  every user, and every dsh-* sandbox directory -- and takes the newest. Finding the file
#  is more reliable than being told where it is.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [switch]$All,
    [switch]$Follow
)

$ErrorActionPreference = 'SilentlyContinue'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

# The log is written BESIDE THE EXECUTABLE, which is a fixed path from every shell.
#
# It used to be under %TEMP%, and %TEMP% is per-shell: the program wrote into its own shell's temp
# directory and anyone looking from elsewhere searched a directory the file was never in. That is
# why this script no longer has to guess -- see LogPath() in src\Log.cpp.
$exeLog = Join-Path $Root 'build\Debug\src\Debug\island_bar.log'

# Kept as a fallback for older builds that still wrote to %TEMP%.
$roots = @()
if ($env:TEMP) { $roots += $env:TEMP }
$roots += (Join-Path $env:SystemDrive 'Users')
$roots = $roots | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

function Find-Logs {
    $found = @()
    if (Test-Path $exeLog) { $found += Get-Item $exeLog }
    foreach ($root in $roots) {
        $found += Get-ChildItem -Path $root -Filter 'island_bar.log' -Recurse `
                                -File -ErrorAction SilentlyContinue
    }
    $found | Sort-Object LastWriteTime -Descending
}

function Show-Lines([string]$path, [switch]$everything) {
    $lines = Get-Content $path -ErrorAction SilentlyContinue
    if (-not $lines) { return }
    if ($everything) {
        $lines
    } else {
        $lines | Where-Object {
            $_ -match 'anim:|row:|title:|icon hover:|state:|pointer |click routed'
        }
    }
}

if ($Follow) {
    Write-Host '[log] watching for island_bar runs -- start the bar now' -ForegroundColor Cyan
    Write-Host '[log] press Ctrl+C to stop'
    Write-Host ''

    $seen = @{}
    while ($true) {
        foreach ($log in (Find-Logs)) {
            $key = $log.FullName
            $length = $log.Length
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = 0
                Write-Host "[log] new file: $key" -ForegroundColor Green
            }
            if ($length -le $seen[$key]) { continue }
            # Read only what is new, so a live run does not reprint its whole history.
            $stream = [System.IO.File]::Open($key, 'Open', 'Read', 'ReadWrite')
            try {
                $stream.Seek($seen[$key], 'Begin') | Out-Null
                $reader = New-Object System.IO.StreamReader($stream)
                $fresh = $reader.ReadToEnd()
                $reader.Close()
            } finally {
                $stream.Close()
            }
            $seen[$key] = $length
            $fresh -split "`r?`n" | Where-Object {
                $_ -match 'anim:|row:|title:|icon hover:|state:'
            } | ForEach-Object { Write-Host "  $_" }
        }
        Start-Sleep -Milliseconds 400
    }
}

$logs = Find-Logs
if (-not $logs) {
    Write-Host '[log] no island_bar.log found anywhere. The bar may never have run, or it' -ForegroundColor Yellow
    Write-Host '[log] cannot write to its temp directory.'
    exit 1
}

Write-Host '[log] most recent runs:' -ForegroundColor Cyan
$logs | Select-Object -First 5 | ForEach-Object {
    Write-Host ("  {0}  {1,8} bytes  {2}" -f $_.LastWriteTime, $_.Length, $_.FullName)
}
Write-Host ''

$newest = $logs[0]
Write-Host "[log] newest: $($newest.FullName)" -ForegroundColor Cyan
Write-Host "[log] written: $($newest.LastWriteTime)"
Write-Host ''

$shown = Show-Lines $newest.FullName -everything:$All
if (-not $shown) {
    Write-Host '[log] that file has no trace lines. That is itself an answer: the pointer' -ForegroundColor Yellow
    Write-Host '[log] was never seen on an icon, so nothing ever changed.'
    Write-Host ''
    Write-Host '[log] the last few lines of the file, whatever they are:'
    Get-Content $newest.FullName -ErrorAction SilentlyContinue | Select-Object -Last 12 |
        ForEach-Object { Write-Host "  $_" }
    exit 0
}

$shown | ForEach-Object { Write-Host "  $_" }
Write-Host ''
Write-Host ("[log] {0} trace lines. Pass -All for everything, -Follow to watch live." -f $shown.Count) -ForegroundColor Cyan
