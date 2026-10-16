# =====================================================================
#  watch-settings-popup.ps1 -- catch the moment Settings opens by itself
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\watch-settings-popup.ps1
#      ... [-Out <file>] [-Minutes 0] [-Shot]
#
#  WHY THIS EXISTS. On 2026-10-16 the user twice reported that the Settings window appeared with no
#  action of theirs (18:15 and 19:29). What was measured at 19:29:10: SystemSettings.exe was created
#  with its parent being the DcomLaunch svchost -- a BROKERED activation, so the parent does not name
#  the caller -- and two SoftLanding tasks (the update-restart prompt machinery) had run at 19:26.
#
#  The bar is not the caller: every tile it presses writes `tile N 'X' -> ms-settings:... (result N)`,
#  and the log has no such line at either time. But "not us" is not an answer to "who then", and the
#  event logs do not keep the caller of a brokered activation.
#
#  So this watches for the next one and records everything around it: the process, its parent and
#  command line, the foreground window, EVERY process created in the ten seconds before it, the tail
#  of the bar's own log, and (with -Shot) a picture of the screen.
#
#  It writes one block per sighting, flushed immediately, so a sighting survives the window closing.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Out = '',
    [int]$Minutes = 0,
    [switch]$Shot
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)   # ...\IslandBar
if (-not $Out) { $Out = Join-Path $Root 'captures\settings-popup.txt' }
$captures = Split-Path -Parent $Out
New-Item -ItemType Directory -Force -Path $captures | Out-Null

$started = Get-Date
$seen = @{}

function Write-Block([string[]]$lines) {
    $lines | Out-File -FilePath $Out -Encoding UTF8 -Append
    Write-Host $lines[0]
}

Write-Block @("=== watching from $($started.ToString('yyyy-MM-dd HH:mm:ss')) (out: $Out) ===")

$deadline = if ($Minutes -gt 0) { $started.AddMinutes($Minutes) } else { [DateTime]::MaxValue }

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1500
    $now = Get-Date

    $procs = @(Get-CimInstance Win32_Process -Filter "Name='SystemSettings.exe'" -ErrorAction SilentlyContinue)
    foreach ($p in $procs) {
        $created = $p.CreationDate
        if ($created -lt $started) { continue }
        $key = "$($p.ProcessId)@$created"
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true

        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($p.ParentProcessId)" -ErrorAction SilentlyContinue
        $newest = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
                    Where-Object { $_.CreationDate -and $_.CreationDate -gt $now.AddSeconds(-10) } |
                    Sort-Object CreationDate |
                    ForEach-Object { "    {0}  {1} (pid {2})  {3}" -f $_.CreationDate.ToString('HH:mm:ss.fff'), $_.Name, $_.ProcessId, $_.CommandLine })
        $barLog = Join-Path $env:USERPROFILE 'IslandBar\island_bar.log'
        $barTail = @()
        if (Test-Path $barLog) { $barTail = @(Get-Content $barLog -Encoding UTF8 -Tail 8 | ForEach-Object { "    $_" }) }

        $block = @()
        $block += "--- SIGHTING $($now.ToString('yyyy-MM-dd HH:mm:ss')) ---"
        $block += "  SystemSettings pid=$($p.ProcessId) created=$($created.ToString('HH:mm:ss.fff'))"
        $block += "  parent=$($parent.Name) (pid $($p.ParentProcessId)) cmd=[$($parent.CommandLine)]"
        $block += "  own cmd=[$($p.CommandLine)]"
        $block += "  processes created in the last 10 s:"
        $block += $newest
        $block += "  bar log tail:"
        $block += $barTail
        Write-Block $block

        if ($Shot) {
            try {
                Add-Type -AssemblyName System.Drawing
                $name = "settings-popup-$($now.ToString('HHmmss')).png"
                $bmp = New-Object System.Drawing.Bitmap 1400, 700
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                $g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 1400, 700))
                $bmp.Save((Join-Path $captures $name), [System.Drawing.Imaging.ImageFormat]::Png)
                $g.Dispose(); $bmp.Dispose()
                Write-Block @("  screenshot: $name")
            } catch { Write-Block @("  screenshot failed: $($_.Exception.Message)") }
        }
    }
}

Write-Block @("=== watching stopped at $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) ===")
