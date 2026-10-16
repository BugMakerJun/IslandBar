# =====================================================================
#  check-duplicate-code.ps1 -- where is the same code written twice?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-duplicate-code.ps1
#      ... [-RunLen 10] [-MinUses 2] [-Root <dir>]
#
#  WHY THIS IS A FILE. "Avoid duplicated code" and "extract the shared part into a component" are claims,
#  and the difference between a claim and a measurement is where the candidates come from. This walks every
#  .cpp/.h in src/, keeps the windows of N consecutive non-trivial lines, and prints the ones that appear
#  more than once -- so the extraction list is read out of the source rather than remembered.
#
#  It found the first component (platform/WorkerThread.h, 2026-10-16): seven readers started a background
#  thread, four of them with a teardown written out identically line for line -- and the copies had already
#  drifted, which is where the leak was.
#
#  WHAT IT DELIBERATELY IGNORES: comments, blank lines, lines that are only a brace, and #include/#pragma.
#  Those repeat everywhere and mean nothing. A window must also carry at least two lines with a call or an
#  assignment, so a run of declarations does not read as a duplicate.
#
#  WHAT IT IS NOT: a similarity metric. It finds text that is IDENTICAL, which is the conservative half --
#  code copied and then edited slightly (the interesting half) does not show up here at all.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    # Named RunLen, NOT Window: PowerShell variable names are case-insensitive, and a local `$window`
    # holding a string array assigned onto an [int] parameter is a runtime cast error.
    [int]$RunLen = 6,
    [int]$MinUses = 2,
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    $Root = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'src'
}

$files = Get-ChildItem $Root -Recurse -Include '*.cpp', '*.h' |
         Where-Object { $_.FullName -notmatch '\\build\\' }

$seen = @{}
foreach ($file in $files) {
    $lines = Get-Content $file.FullName -Encoding UTF8
    $keep = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $text = $lines[$i].Trim()
        if ($text -eq '' -or $text -eq '{' -or $text -eq '}' -or $text -eq '};' -or $text -eq '});') { continue }
        if ($text.StartsWith('//')) { continue }
        if ($text -match '^#(include|pragma)') { continue }
        $keep += [pscustomobject]@{ Text = $text; Line = $i + 1 }
    }
    for ($i = 0; $i + $RunLen -le $keep.Count; $i++) {
        $run = $keep[$i..($i + $RunLen - 1)]
        # A run must carry some meat: at least two lines with a call or an assignment.
        $meat = (@($run | Where-Object { $_.Text -match '\(|=' })).Count
        if ($meat -lt 2) { continue }
        $key = ($run | ForEach-Object { $_.Text }) -join "`n"
        if (-not $seen.ContainsKey($key)) { $seen[$key] = @() }
        $seen[$key] += ("{0}:{1}" -f $file.Name, $run[0].Line)
    }
}

$groups = $seen.GetEnumerator() |
          Where-Object { $_.Value.Count -ge $MinUses } |
          Sort-Object { -$_.Value.Count }

Write-Host ("check-duplicate-code: {0} run(s) of {1} line(s) appear {2}+ times" -f $groups.Count, $RunLen, $MinUses)
Write-Host ("check-duplicate-code: files scanned: {0}" -f $files.Count)
Write-Host ''

$shown = 0
foreach ($group in $groups) {
    $shown++
    Write-Host ("--- {0} use(s) -------------------------------------------------" -f $group.Value.Count)
    Write-Host ("  at: {0}" -f (($group.Value | Sort-Object) -join ', '))
    foreach ($line in ($group.Key -split "`n")) { Write-Host ("  | {0}" -f $line) }
    if ($shown -ge 40) { Write-Host '... (stopping after 40 groups)'; break }
}
