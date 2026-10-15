# =====================================================================
#  snapshot.ps1 -- copy the sources somewhere safe before a risky change
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\snapshot.ps1
#      ... -Label before-pointer-refactor
#
#  THIS FILE IS NO LONGER THE BASELINE, AND ITS ORIGINAL REASON IS GONE. It was written when this machine
#  appeared to have no version control -- git is installed, the repository has been under it for a long time,
#  and the rule is now "commit locally before a risky change" (see ROADMAP.md and AGENTS.md). Corrected here
#  rather than deleted, because a script that claims something false about the machine is worse than no
#  script: the next session reads this comment and believes it.
#
#  What it is still good for: a copy of the SOURCES on disk, outside the repository, for the case where a
#  working tree gets mangled in a way git cannot recover from (a bad bulk rewrite that was never committed).
#  Committing first is the cheaper reflex -- use that one first.
#
#  Copies the SOURCE only -- not build output, which is large and regenerable.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Label = 'snapshot',
    [int]$Keep = 10
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$SafeLabel = ($Label -replace '[^A-Za-z0-9_-]', '-')
$Dest = Join-Path $Root "backups\$Stamp-$SafeLabel"

Write-Host "[snapshot] root: $Root"
Write-Host "[snapshot] dest: $Dest"

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# The things that cannot be regenerated: sources, build files, scripts, docs.
$items = @('src', 'tools', 'setup', 'cmake', 'scripts', 'CMakeLists.txt',
           'README.md', 'README.zh-CN.md')

$copied = 0
foreach ($item in $items) {
    $source = Join-Path $Root $item
    if (-not (Test-Path $source)) { continue }
    # Exclude the backups folder: a snapshot that contained every earlier snapshot would grow
    # without bound.
    $copied_items = if ($item -eq 'src' -or $item -eq 'scripts') {
        Get-ChildItem $source -Recurse -File
    } else {
        Get-ChildItem $source
    }
    $target = Join-Path $Dest (Split-Path -Leaf $source)
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    foreach ($file in $copied_items) {
        $relative = $file.FullName.Substring($source.Length).TrimStart('\')
        $out = Join-Path $target $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
        Copy-Item $file.FullName -Destination $out -Force -ErrorAction SilentlyContinue
    }
    $copied++
    Write-Host "[snapshot]   $item"
}

# The handoff, the feature list and the quick reference live at the top of the WORKSPACE,
# one level above this project: they describe the session, not the build.
$workspace = Split-Path -Parent $Root
foreach ($doc in @('HANDOFF.md', 'FEATURES.md', 'START-HERE.txt')) {
    $source = Join-Path $workspace $doc
    if (Test-Path $source) {
        Copy-Item $source -Destination $Dest -Force -ErrorAction SilentlyContinue
        $copied++
        Write-Host "[snapshot]   $doc"
    }
}

$files = Get-ChildItem $Dest -Recurse -File | Measure-Object
Write-Host ("[snapshot] {0} items, {1} files, {2:N0} bytes" -f `
    $copied, $files.Count, ($files | Select-Object -ExpandProperty Sum -ErrorAction SilentlyContinue))
Write-Host "[snapshot] done: $Dest" -ForegroundColor Green

# Keep the newest few and drop the rest, so this does not grow without bound.
$all = Get-ChildItem (Join-Path $Root 'backups') -Directory |
       Sort-Object Name -Descending
if ($all.Count -gt $Keep) {
    $all | Select-Object -Skip $Keep | ForEach-Object {
        Write-Host "[snapshot] pruning $($_.Name)"
        Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
}
