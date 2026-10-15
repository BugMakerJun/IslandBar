# =====================================================================
#  check-layers.ps1 -- the layer direction, checked on every build
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-layers.ps1
#
#  THE RULE, in one sentence: a layer may include only the layers BELOW it.
#
#      domain  <- the bottom: pure logic, no <windows.h>, links nothing
#      core        shared plumbing (log, process metrics, the purity check itself)
#      platform    Win32 and shell APIs
#      media       what is playing
#      services    the channels things arrive through
#      shell       the shell's own controls, mirrored
#      render      everything that puts pixels on the composition
#      app         the window and what hangs off it
#      cli         the commands that run instead of the bar
#
#      selftest    the runtime assertions: may include everything (it exercises everything)
#      trayhook    the module that lives in other processes: domain, core, platform only
#
#  WHY A SCRIPT AND NOT JUST THE LINK GRAPH. src/CMakeLists.txt now builds one static library per layer, and
#  the link graph catches a CALL that reaches a layer it does not link. It does NOT catch an INCLUDE: the
#  include root is src/ for every target (the project's include form is "module/File.h"), so a file in shell/
#  that includes render/Renderer.h then calls something in it compiles and links exactly as if nothing had
#  happened. MEASURED 2026-10-16 by doing it on purpose. So the include half of the rule is checked here,
#  on every build, by the build system rather than by hope.
#
#  HOW TO READ A FAILURE: the message names the file and the layer it reached for. The fix is almost never
#  to add the edge to the table below -- it is to move the thing that is being included, or to pass the
#  value through the layer that owns it.
#
#  Exit code: 0 when the tree is clean, 1 when something points upwards.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)   # ...\IslandBar
$Src = Join-Path $Root 'src'
if (-not (Test-Path $Src)) { throw "no src directory under $Root" }

# The chain, bottom first. A layer may include any of the layers before it in this list.
$Order = @('domain', 'core', 'platform', 'media', 'services', 'shell', 'render', 'app', 'cli')

# The layers that are not on the chain. selftest exercises everything; trayhook is a module that runs inside
# other processes and may use the three bottom layers it needs (the wire format, the ring, the log).
$Extra = @{
    'selftest' = @('domain', 'core', 'platform', 'media', 'services', 'shell', 'render', 'app', 'cli')
    'trayhook' = @('domain', 'core', 'platform')
}

$Allowed = @{}
for ($i = 0; $i -lt $Order.Count; $i++) {
    $below = @()
    if ($i -gt 0) { $below = $Order[0..($i - 1)] }
    $Allowed[$Order[$i]] = $below
}
foreach ($layer in $Extra.Keys) { $Allowed[$layer] = $Extra[$layer] }

$checked = 0
$violations = @()

foreach ($layer in $Allowed.Keys) {
    $dir = Join-Path $Src $layer
    if (-not (Test-Path $dir)) { continue }
    $may = $Allowed[$layer]
    $files = Get-ChildItem $dir -Recurse -Include *.h, *.cpp -File
    foreach ($file in $files) {
        $checked++
        $relative = $file.FullName.Substring($Src.Length + 1)
        foreach ($line in [System.IO.File]::ReadAllLines($file.FullName)) {
            if ($line -notmatch '^#include "([a-z0-9_]+)/') { continue }
            $other = $Matches[1]
            if ($other -eq $layer) { continue }
            if (-not $Allowed.ContainsKey($other)) { continue }   # not a layer: an ordinary header
            if ($may -contains $other) { continue }
            $violations += ('{0}: includes {1}/ -- {2} may not include {1}' -f $relative, $other, $layer)
        }
    }
}

if ($violations.Count -eq 0) {
    Write-Host ("layer check: clean ({0} file(s) in {1} layer(s))" -f $checked, $Allowed.Keys.Count)
    exit 0
}

Write-Host 'layer check: these includes point upwards:'
foreach ($violation in $violations) { Write-Host ('  ' + $violation) }
Write-Host ''
Write-Host 'The fix is to move what is being included, or to pass the value through the layer that owns it --'
Write-Host 'not to add the edge here. See ARCHITECTURE.md section 3 for the direction.'
exit 1
