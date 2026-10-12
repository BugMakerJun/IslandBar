# =====================================================================
#  env.ps1 -- dot-source this to get the build environment in a session
#      . C:\PROJECT\Taskbar\IslandBar\scripts\env.ps1
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file:
#  Windows PowerShell 5.1 reads .ps1 files using the system ANSI codepage
#  unless they carry a UTF-8 BOM, so a non-ASCII literal would be mangled.
#  Paths are therefore derived at runtime from this script's own location.
# =====================================================================

# --- 0. Derive the workspace root from this script's location --------
#   <Root>\scripts\env.ps1  ->  <Root>
$global:IslandBarRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

# --- 1. Locate vcvars64.bat -----------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
}
$global:IslandBarVsPath = $vsPath

# --- 2. Locate cmake (VS-bundled fallback) --------------------------
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $global:IslandBarCMake = $cmakeCmd.Source
} elseif ($vsPath) {
    $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $candidate) { $global:IslandBarCMake = $candidate }
}

# --- 3. Locate ninja (optional) -------------------------------------
$ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
$global:IslandBarNinja = if ($ninjaCmd) { $ninjaCmd.Source } else { $null }

# --- 4. Report -------------------------------------------------------
Write-Host "[env] IslandBarRoot : $global:IslandBarRoot"
Write-Host "[env] VS instance   : $(if ($global:IslandBarVsPath) { $global:IslandBarVsPath } else { 'NOT FOUND' })"
Write-Host "[env] cmake         : $(if ($global:IslandBarCMake) { $global:IslandBarCMake } else { 'NOT FOUND' })"
Write-Host "[env] ninja         : $(if ($global:IslandBarNinja) { $global:IslandBarNinja } else { 'absent (MSBuild generator will be used)' })"

# --- 5. Import the MSVC environment into this session ---------------
# Extracts the env vars that vcvars64.bat sets, by running it and diffing.
function Import-MsvcEnvironment {
    param([string]$VsPath, [string]$Arch = 'x64')

    $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        Write-Host "[env] vcvars64.bat not found at $vcvars" -ForegroundColor Red
        return
    }

    $before = @{}
    Get-ChildItem Env: | ForEach-Object { $before[$_.Name] = $_.Value }

    # Capture the environment vcvars produces. The paths here are ASCII, so the
    # OEM codepage is safe.
    $lines = cmd.exe /c "`"$vcvars`" $Arch >nul 2>&1 && set" 2>$null
    foreach ($line in $lines) {
        $idx = $line.IndexOf('=')
        if ($idx -lt 1) { continue }
        $name  = $line.Substring(0, $idx)
        $value = $line.Substring($idx + 1)
        if ($before.ContainsKey($name) -and $before[$name] -eq $value) { continue }
        Set-Item -Path "Env:$name" -Value $value -ErrorAction SilentlyContinue
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($cl) { Write-Host "[env] cl.exe        : $($cl.Source)" -ForegroundColor Green }
    else     { Write-Host "[env] cl.exe still not on PATH" -ForegroundColor Red }
}

if ($global:IslandBarVsPath) { Import-MsvcEnvironment -VsPath $global:IslandBarVsPath }
