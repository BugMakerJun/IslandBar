# =====================================================================
#  package-release.ps1 -- build the release installer and name it after its own version
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
#      ... -NoBuild                      use what is already in build\
#
#  WHAT IT DOES
#
#    1. builds the configuration asked for. That builds island_bar.exe AND
#       IslandBarSetup.exe -- the installer carries the bar inside itself as a resource, so
#       ONE FILE is the whole distribution (see setup/CMakeLists.txt).
#    2. reads the version OUT OF THE INSTALLER'S OWN VERSION RESOURCE. The version is
#       written in exactly three places (setup/Setup.cpp, setup/payload.rc.in,
#       src/island_bar.rc) and in none here: a fourth copy in a script is a copy that goes
#       stale without anything failing.
#    3. copies it to <root>\dist\IslandBarSetup-<version>.exe, prints size and hash, and
#       checks the package does NOT import the MSVC runtime.
#
#  WHY THE RUNTIME CHECK IS PART OF MAKING THE PACKAGE
#
#  The first package this project built was a Debug one and imported
#  MSVCP140D/VCRUNTIME140D/ucrtbased: it installs fine and then cannot start on a machine
#  without Visual Studio, which is the whole point of the static /MT setting in the
#  top-level CMakeLists.txt (HANDOFF 8.69). A wrong-but-green package is not a thing to be
#  left to memory, so the check is here. When dumpbin cannot be found the script says which
#  paths it tried instead of passing quietly.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string]$Config = 'Release',
    [switch]$NoBuild,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Write-Host "[package] root: $Root"

if (-not $NoBuild) {
    Write-Host "[package] building $Config ..."
    & (Join-Path $Root 'scripts\build.ps1') -Config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed with $LASTEXITCODE" }
}

$buildDir = Join-Path $Root "build\$Config"
$installer = Get-ChildItem -Path $buildDir -Recurse -Filter 'IslandBarSetup.exe' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $installer) { throw "IslandBarSetup.exe was not found under $buildDir" }
Write-Host "[package] installer: $($installer.FullName)"

$version = $installer.VersionInfo.ProductVersion
if (-not $version) { throw 'the installer has no version resource: the name would be ambiguous' }

if (-not $OutDir) { $OutDir = Join-Path $Root 'dist' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$package = Join-Path $OutDir "IslandBarSetup-$version.exe"
Copy-Item -LiteralPath $installer.FullName -Destination $package -Force

$size = (Get-Item $package).Length
$hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
Write-Host ''
Write-Host "package : $package" -ForegroundColor Green
Write-Host ("size    : {0} bytes ({1:N2} MB)" -f $size, ($size / 1MB))
Write-Host "version : $version"
Write-Host "sha256  : $hash"
Write-Host ''

# --- does it run on a machine without Visual Studio? -------------------
#
# dumpbin is looked for on PATH first (somebody in this project may have run the developer
# prompt), then inside the newest Visual Studio instance that vswhere knows about. vswhere
# itself is looked for through TWO independent answers for "where is the 32-bit Program
# Files" -- [Environment]::GetFolderPath and the environment variable -- and each candidate
# is checked with Test-Path AND [System.IO.File]::Exists.
#
# THAT REDUNDANCY WAS EARNED, and the way it was earned is worth keeping. The first version
# of this file spelled the path with one backslash too many -- "...\Microsoft\Visual
# Studio\..." instead of "...\Microsoft Visual Studio\..." -- so Test-Path answered false,
# the whole block was skipped, and the check printed "not checked" as if the machine had no
# Visual Studio. Read the trace, and the FIRST reading of it blamed the environment: the
# path had been printed by a hand-retyped line that happened to be correct. What settled it
# was a breakpoint action on the line (`Get-Variable -Scope 1`) printing the variable's real
# value -- 68 characters, the extra one visible in the middle.
#
# The lesson is the one this project keeps paying for: when a check goes quiet, read the
# VALUE, not the source line that is supposed to have produced it -- and make the quiet
# outcome say what it tried.
function Find-Dumpbin {
    $onPath = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    $vswherePaths = @()
    if ($programFilesX86) {
        $vswherePaths += Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    $envAnswer = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($envAnswer) { $vswherePaths += Join-Path $envAnswer 'Microsoft Visual Studio\Installer\vswhere.exe' }
    $vswherePaths = $vswherePaths | Select-Object -Unique

    foreach ($vswhere in $vswherePaths) {
        # Test-Path first, then the .NET check as the tie-breaker: a false from one of them
        # is not an answer about the file, it is an answer about one API.
        $there = (Test-Path -LiteralPath $vswhere) -or ([System.IO.File]::Exists($vswhere))
        if (-not $there) { continue }
        $vs = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
        if (-not $vs) { continue }
        $found = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -like '*Hostx64\x64*' } | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

$dumpbin = Find-Dumpbin
if ($dumpbin) {
    $imports = & $dumpbin /dependents $package |
               Select-String -Pattern '^\s+\S+\.dll' |
               ForEach-Object { $_.Line.Trim() }
    $runtime = $imports | Where-Object { $_ -match '^(MSVC|VCRUNTIME|ucrt|api-ms-win-crt)' }
    if ($runtime) {
        Write-Host 'BAD: this package imports the MSVC runtime -- it will not start elsewhere:' -ForegroundColor Red
        $runtime | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        exit 1
    }
    Write-Host "imports : $($imports.Count) DLL(s), none of the MSVC runtime -- it runs on a clean machine" -ForegroundColor Green
    Write-Host "          (checked with $dumpbin)"
} else {
    Write-Host 'imports : NOT CHECKED -- dumpbin was not found. Confirm the payload is a Release build.' -ForegroundColor Yellow
    $envAnswer = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    Write-Host "          vswhere tried: [$envAnswer] and [$([Environment]::GetFolderPath('ProgramFilesX86'))]"
}

Write-Host ''
Write-Host 'Double-click the package to install for the current user (no administrator needed),' -ForegroundColor Cyan
Write-Host 'or run it with /silent to keep the summary in the log only. Run it from OUTSIDE this' -ForegroundColor Cyan
Write-Host 'project tree: an installer started from inside it runs at low integrity here (8.69).' -ForegroundColor Cyan
exit 0
