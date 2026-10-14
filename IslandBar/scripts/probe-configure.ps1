$ErrorActionPreference = 'Continue'

# Configure the build inside the MSVC environment.
#
# The configure step does NOT need this, but cmake/WinRt.cmake locates the
# C++/WinRT projection headers through $ENV{WindowsSdkDir}, which only vcvars
# sets. Configured outside that environment it warns that the media controls
# will not compile -- which is what happened the first time this was run by
# hand, and it looked like a missing SDK rather than a missing environment.

# Derived from this script's own location, never hardcoded (see START-HERE rule 1).
$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

. (Join-Path $Root 'scripts\env.ps1') | Out-Null

Write-Output ("[probe] WindowsSdkDir = [" + $env:WindowsSdkDir + "]")

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { Write-Output "[probe] cmake not on PATH"; exit 1 }
Write-Output ("[probe] cmake        = " + $cmake)

$buildDir = Join-Path $Root 'build\Debug'
$srcDir   = $Root

# Force a fresh configure so the SDK path is re-evaluated in this environment.
Remove-Item -Force -Recurse -ErrorAction SilentlyContinue (Join-Path $buildDir "CMakeCache.txt")
Remove-Item -Force -Recurse -ErrorAction SilentlyContinue (Join-Path $buildDir "CMakeFiles")

$out = & $cmake -S $srcDir -B $buildDir -G "Visual Studio 17 2022" -A x64 2>&1
$out | Select-Object -Last 10 | ForEach-Object { Write-Output ("  " + $_) }
Write-Output ("[probe] configure exit = " + $LASTEXITCODE)
