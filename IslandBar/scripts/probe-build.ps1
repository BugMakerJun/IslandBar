$ErrorActionPreference = 'Continue'

# Build ONLY the GSMTC probe.
#
# The bar's own main.cpp is mid-edit by another session and does not compile, so
# a normal build of the solution fails before reaching anything else. Building the
# single target and switching project references off is what makes the probe
# usable while that is true.

. "C:\PROJECT\Taskbar\IslandBar\scripts\env.ps1" | Out-Null
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source

$out = & $cmake --build "C:\PROJECT\Taskbar\IslandBar\build\Debug" --config Debug `
                 --target gsmtc_probe -- /p:BuildProjectReferences=false 2>&1

$out | Select-String -Pattern 'error|warning C|\.exe$|Build succeeded|FAILED|-> ' |
    Select-Object -First 25 | ForEach-Object { Write-Output ("  " + $_.Line.Trim()) }
Write-Output ("[probe] build exit = " + $LASTEXITCODE)

$exe = "C:\PROJECT\Taskbar\IslandBar\build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe"
if (Test-Path $exe) {
    Write-Output ("[probe] built: " + $exe)
} else {
    Write-Output "[probe] NOT built"
}
