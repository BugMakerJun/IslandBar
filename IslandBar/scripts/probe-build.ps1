$ErrorActionPreference = 'Continue'

# Build ONLY the GSMTC probe.
#
# The bar's own main.cpp is mid-edit by another session and does not compile, so
# a normal build of the solution fails before reaching anything else. Building the
# single target and switching project references off is what makes the probe
# usable while that is true.

# Derived from this script's own location, never hardcoded: PowerShell 5.1 reads .ps1 with the ANSI
# codepage, so a path literal that is ever non-ASCII would be mangled on the way in.
$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$buildDir = Join-Path $Root 'build\Debug'

. (Join-Path $Root 'scripts\env.ps1') | Out-Null
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source

$out = & $cmake --build $buildDir --config Debug `
                 --target gsmtc_probe -- /p:BuildProjectReferences=false 2>&1

$out | Select-String -Pattern 'error|warning C|\.exe$|Build succeeded|FAILED|-> ' |
    Select-Object -First 25 | ForEach-Object { Write-Output ("  " + $_.Line.Trim()) }
Write-Output ("[probe] build exit = " + $LASTEXITCODE)

$exe = Join-Path $buildDir 'tools\gsmtcprobe\Debug\gsmtc_probe.exe'
if (Test-Path $exe) {
    Write-Output ("[probe] built: " + $exe)
} else {
    Write-Output "[probe] NOT built"
}
