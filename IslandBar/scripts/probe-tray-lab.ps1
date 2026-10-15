# =====================================================================
#  probe-tray-lab.ps1 -- build the three tray experiments and put the exe
#  somewhere it is allowed to run.
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-lab.ps1
#
#  WHY THE EXE IS COPIED OUT OF THE PROJECT TREE. Anything whose image lives under
#  C:\PROJECT\Taskbar runs at LOW integrity on this machine, and a GUI binary in that state
#  fails to start at all (0xC0000142, recorded in ARCHITECTURE.md). The probe creates windows,
#  so it has to run from %USERPROFILE%\IslandBar\traylab like the bar itself does.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [switch]$KeepExe
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'probe-tray-lab.cpp'
$outDir = Join-Path $env:USERPROFILE 'IslandBar\traylab'
$outExe = Join-Path $outDir 'probe-tray-lab.exe'

if (-not (Test-Path $source)) { throw "no source at $source" }

Write-Host "[lab] importing the MSVC environment"
. (Join-Path $PSScriptRoot 'env.ps1') | Out-Null
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) { throw "cl.exe is not available -- env.ps1 did not find a Visual Studio install" }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$obj = Join-Path $outDir 'probe-tray-lab.obj'
Write-Host "[lab] compiling $source"
& cl.exe /nologo /W4 /EHsc /O2 /utf-8 /Fo:$obj /Fe:$outExe $source `
    /link user32.lib shell32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib uiautomationcore.lib
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with $LASTEXITCODE" }

Write-Host "[lab] built: $outExe"
Write-Host "[lab] its own log goes beside it: (Get-Content '$outDir\probe-tray-lab.log')"
Write-Host ""
Write-Host "steps (run each from a normal, non-sandboxed shell):"
Write-Host "  & '$outExe' --host 20            # E1: a fake tray host"
Write-Host "  & '$outExe' --client hold        # E1: register, wait, re-register"
Write-Host "  & '$outExe' --crop on            # E2: clip the taskbar to its tray area"
Write-Host "  & '$outExe' --crop status        # E2: what region is applied now"
Write-Host "  & '$outExe' --crop off           # E2: put the whole taskbar back"
Write-Host "  & '$outExe' --reparent           # E3: can the shell's tray window move?"

if (-not $KeepExe) { Remove-Item -LiteralPath $obj -Force -ErrorAction SilentlyContinue }
