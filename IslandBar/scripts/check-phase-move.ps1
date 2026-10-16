# =====================================================================
#  check-phase-move.ps1 -- did the block really only MOVE?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-phase-move.ps1 `
#          -Function FollowSliderDrags -RegionStart 3029 -RegionEnd 3128
#
#  WHY THIS IS A FILE AND NOT A FEW LINES OF SHELL, and it is a measured reason rather than a taste: the
#  ad-hoc version of this check produced a FALSE GREEN three times in one session. Each time the same way --
#  the brace walk started on a line that has no brace (a multi-line signature), so the walk "ended" on its
#  first step, the compared region came back EMPTY, and an empty set compares as identical. The three times
#  it happened the slice was fine; the fourth time it would have been a slice that was not, and the evidence
#  would have said it was. So the walk starts at the opening brace, and an empty region is an ERROR here.
#
#  WHAT IT COMPARES. The block as it stood in `git show <Reference>:<Path>` between RegionStart and RegionEnd,
#  against the body of `Function` as it stands now. Blank lines are dropped and every line is trimmed, because
#  a move out of the frame loop also dedents it (twelve spaces to four) -- whitespace is not the question,
#  the statements are.
#
#  TWO SUBSTITUTIONS, and both are named on the command line rather than hidden here:
#    * `runtime.` is stripped from the current side: phases read cross-frame state through BarRuntime, which
#      did not exist when the reference was written.
#    * -Rename old=new applies to the REFERENCE side: a local that became a parameter changes its name at
#      the call site and in the body. SEVERAL PAIRS GO IN ONE ARGUMENT, comma separated --
#      `-Rename a=b,c=d` -- because PowerShell refuses the same parameter twice.
#      its name at the call site and in the body.
#
#  Exit code: 0 when the only differences are those, 1 when a statement really did change, 2 on bad input.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Function,
    [Parameter(Mandatory = $true)][int]$RegionStart,
    [Parameter(Mandatory = $true)][int]$RegionEnd,
    [string]$Reference = 'HEAD',
    [string]$Path = 'IslandBar/src/main.cpp',
    # One comma-separated string, split below: PowerShell refuses a repeated parameter, and an array-typed
    # parameter binds `a=b,c=d` as ONE element -- both found by running it rather than by reading about it.
    [string]$Rename = ''
)

$ErrorActionPreference = 'Stop'

function Read-Source([string]$text) {
    $lines = $text -split "`r?`n"
    return $lines
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)   # ...\IslandBar -> ...
$repoRoot = Split-Path -Parent $repoRoot                              # the repository root
Push-Location $repoRoot
try {
    $referenceText = & git show ("{0}:{1}" -f $Reference, $Path)
    if ($LASTEXITCODE -ne 0 -or -not $referenceText) { throw "could not read $Reference`:$Path" }
} finally {
    Pop-Location
}
$referenceLines = Read-Source ($referenceText -join "`n")

if ($RegionStart -lt 1 -or $RegionEnd -gt $referenceLines.Count -or $RegionEnd -lt $RegionStart) {
    Write-Host ("check-phase-move: the region {0}..{1} is not inside the reference ({2} lines)" -f $RegionStart, $RegionEnd, $referenceLines.Count)
    exit 2
}
$before = $referenceLines[($RegionStart - 1)..($RegionEnd - 1)]

$currentPath = Join-Path $repoRoot $Path
$current = Read-Source (Get-Content $currentPath -Raw -Encoding UTF8)

# --- locate the function, and walk its braces FROM THE OPENING BRACE -------------------------------
$signaturePattern = '^[A-Za-z_][\w:<>,\s&*]*\s' + [regex]::Escape($Function) + '\s*\('
$signatureLine = ($current | Select-String -Pattern $signaturePattern | Select-Object -First 1).LineNumber
if (-not $signatureLine) { Write-Host ("check-phase-move: no definition of {0} in {1}" -f $Function, $Path); exit 2 }

$openLine = 0
for ($i = $signatureLine; $i -le $current.Count; $i++) {
    if ($current[$i - 1].TrimEnd().EndsWith('{')) { $openLine = $i; break }
}
if (-not $openLine) { Write-Host 'check-phase-move: the function never opens a brace'; exit 2 }

$depth = 0
$closeLine = 0
for ($i = $openLine; $i -le $current.Count; $i++) {
    $depth += ([regex]::Matches($current[$i - 1], '\{')).Count - ([regex]::Matches($current[$i - 1], '\}')).Count
    if ($depth -eq 0) { $closeLine = $i; break }
}
if (-not $closeLine) { Write-Host 'check-phase-move: the function never closes'; exit 2 }
if ($closeLine -le $openLine) { Write-Host 'check-phase-move: the function body is empty -- refusing to compare nothing'; exit 2 }

$bodyLines = $current[$openLine..($closeLine - 2)]     # after the opening brace, before the closing one
if ($bodyLines.Count -lt 1) { Write-Host 'check-phase-move: the function body is empty -- refusing to compare nothing'; exit 2 }

# Split on commas, but a piece WITHOUT an '=' is the continuation of the value before it: a replacement can
# be a whole call, and a whole call has commas in it. MEASURED: `dismissPanel();=DismissPanelSurface(bar,
# systems, active, resting);` came back as four pieces with the naive split.
$renamePairs = @()
$text = ''
$old = ''
foreach ($piece in ($Rename -split ',')) {
    if ($piece.Contains('=')) {
        if ($old -ne '') { $renamePairs += , @($old, $text) }
        $bits = $piece -split '=', 2
        $old = $bits[0]
        $text = $bits[1]
    } else {
        $text += ',' + $piece
    }
}
if ($old -ne '') { $renamePairs += , @($old, $text) }

function Normalize([string[]]$lines, [array]$renames, [bool]$stripRuntime) {
    $out = @()
    foreach ($line in $lines) {
        $text = $line.Trim()
        foreach ($pair in $renames) {
            $text = [regex]::Replace($text, ('(?<![\w\.])' + [regex]::Escape($pair[0]) + '(?![\w])'), $pair[1])
        }
        if ($stripRuntime) { $text = $text -replace 'runtime\.', '' }
        if ($text -ne '') { $out += $text }
    }
    return $out
}

$beforeText = Normalize $before $renamePairs $false
# The body is everything between the function's own braces, and the region is everything between the block's
# -- so neither side gets a brace added: appending the function's closing brace here is what made the first
# version of this tool report a phantom difference of one line.
$afterText = Normalize $bodyLines $renamePairs $true

if ($beforeText.Count -eq 0) { Write-Host 'check-phase-move: the reference region has no statements -- wrong line range?'; exit 2 }

$difference = Compare-Object $beforeText $afterText
if (-not $difference) {
    Write-Host ("check-phase-move: {0} is the same block ({1} statement(s), region {2}..{3} of {4})" -f $Function, $beforeText.Count, $RegionStart, $RegionEnd, $Reference)
    exit 0
}

Write-Host ("check-phase-move: {0} DIFFERS -- {1} line(s); '-' is the reference, '+' is the function now" -f $Function, $difference.Count)
foreach ($entry in $difference) {
    $mark = if ($entry.SideIndicator -eq '<=') { '  - ' } else { '  + ' }
    Write-Host ($mark + $entry.InputObject)
}
exit 1
