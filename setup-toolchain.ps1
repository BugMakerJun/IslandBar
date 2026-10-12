# =====================================================================
#  IslandBar toolchain setup  (ASCII-only on purpose)
#
#  Installs: MSVC v143 + Windows 11 SDK 26100 + CMake + Ninja
#  Compiler/IDE portion -> C:\TOOL\VSBuildTools
#  Windows SDK           -> C:\Program Files (x86)\Windows Kits\10
#                           (SDK path is registry-bound; cannot be relocated)
#
#  RUN IN AN ELEVATED POWERSHELL:
#    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\PROJECT\Taskbar\setup-toolchain.ps1"
# =====================================================================

$ErrorActionPreference = 'Stop'

$InstallPath  = 'C:\TOOL\VSBuildTools'
$Bootstrapper = 'C:\TOOL\vs_BuildTools.exe'
$Winget       = "$env:LOCALAPPDATA\Microsoft\WindowsApps\winget.exe"

# --- 0. Preconditions ------------------------------------------------
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[FAIL] Administrator rights required. Re-open PowerShell with Run as administrator.' -ForegroundColor Red
    exit 1
}
Write-Host "[ OK ] Running elevated as $($identity.Name)" -ForegroundColor Green

New-Item -ItemType Directory -Force -Path 'C:\TOOL' | Out-Null

$freeGB = (Get-PSDrive C).Free / 1GB
Write-Host ('[INFO] Free space on C: {0:N1} GB' -f $freeGB)
if ($freeGB -lt 15) {
    Write-Host '[FAIL] Need at least 15 GB free.' -ForegroundColor Red
    exit 1
}

# --- 1. Fetch the VS Build Tools bootstrapper ------------------------
$needDownload = $true
if (Test-Path $Bootstrapper) {
    if ((Get-Item $Bootstrapper).Length -gt 1MB) { $needDownload = $false }
}

if ($needDownload) {
    Write-Host '[INFO] Downloading Visual Studio 2022 Build Tools bootstrapper ...'
    $urls = @(
        'https://aka.ms/vs/17/release/vs_BuildTools.exe',
        'https://download.visualstudio.microsoft.com/download/pr/6e0a0b48-2c9d-4d2b-9c5f-8b2e6a9a0d3f/vs_BuildTools.exe'
    )
    $ok = $false
    foreach ($u in $urls) {
        try {
            Invoke-WebRequest -Uri $u -OutFile $Bootstrapper -UseBasicParsing -TimeoutSec 300
            if ((Test-Path $Bootstrapper) -and (Get-Item $Bootstrapper).Length -gt 1MB) { $ok = $true; break }
        } catch {
            Write-Host "[WARN] Source failed: $u" -ForegroundColor Yellow
        }
    }
    if (-not $ok) {
        Write-Host '[FAIL] Could not download the bootstrapper. Check network/proxy.' -ForegroundColor Red
        exit 1
    }
}
Write-Host ('[ OK ] Bootstrapper ready ({0:N1} MB)' -f ((Get-Item $Bootstrapper).Length / 1MB)) -ForegroundColor Green

# --- 2. Assemble the component list ----------------------------------
# A component id that does not exist for your VS channel is skipped with a
# warning; the run does not abort. To list ids available to you right now:
#   vs_BuildTools.exe --layout C:\TOOL\layout --lang en-US
$addArgs = @(
    '--add', 'Microsoft.VisualStudio.Workload.VCTools'
    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100'
    '--add', 'Microsoft.VisualStudio.Component.VC.CMake.Project'
)

# NOTE on flags -- two mistakes already cost us two failed runs, do not repeat:
#
#   1. --log does not exist in setup.exe's parser. The bootstrapper accepts it and
#      forwards it, then setup.exe aborts argument parsing with exit 87.
#      VS writes %TEMP%\dd_*.log by itself, so the option is useless anyway.
#
#   2. --cache / --nocache are BOOLEAN switches, not paths, and they are mutually
#      exclusive. "--cache C:\path" makes C:\path a stray positional argument AND
#      conflicts with --nocache. To relocate the cache you must set the CachePath
#      registry policy before the first install; --path cache= is not reliable.
#      We use --nocache only, which also means nothing lands in C:\TOOL.
#
# Also: the bootstrapper forwards arguments SPACE-SEPARATED and UNQUOTED, so a
# path containing spaces would split. The "=" form below is a single token and
# cannot be mis-split. Documented as equivalent to --installPath.
$installArgs = @(
    '--quiet', '--wait', '--norestart'
    "--path", "install=$InstallPath"
    '--nocache'
) + $addArgs

Write-Host ''
Write-Host '[INFO] Installing now. Roughly 4-6 GB to download; 20-60 min depending on link speed.'
Write-Host "       installPath  : $InstallPath"
Write-Host '       package cache: disabled (--nocache), nothing is kept after install'
Write-Host "       installer log: $env:TEMP\dd_*.log"
Write-Host ''

$proc = Start-Process -FilePath $Bootstrapper -ArgumentList $installArgs -Wait -PassThru
$code = $proc.ExitCode

switch ($code) {
    0     { Write-Host '[ OK ] Visual Studio Build Tools installed.' -ForegroundColor Green }
    3010  { Write-Host '[ OK ] Installed, but a reboot is required to finish.' -ForegroundColor Yellow }
    default {
        Write-Host "[FAIL] Installer exit code $code. Inspect the newest logs in $env:TEMP (dd_*.log)." -ForegroundColor Red
        exit $code
    }
}

# --- 3. CMake and Ninja ----------------------------------------------
Write-Host ''
Write-Host '[INFO] Installing CMake and Ninja ...'
if (Test-Path $Winget) {
    try {
        & $Winget install --id Kitware.CMake --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
        & $Winget install --id Ninja-build.Ninja --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
        Write-Host '[ OK ] CMake / Ninja requested.' -ForegroundColor Green
    } catch {
        Write-Host '[WARN] CMake/Ninja install failed. Install manually, or use the CMake bundled with VS.' -ForegroundColor Yellow
    }
} else {
    Write-Host '[WARN] winget not found. Install CMake and Ninja manually.' -ForegroundColor Yellow
}

# --- 4. Verify -------------------------------------------------------
Write-Host ''
Write-Host '=== Verification ===' -ForegroundColor Cyan

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    Write-Host '--- VS instances ---'
    & $vswhere -all -products * -format value -property installationPath
    Write-Host '--- Relevant components ---'
    $pkgs = & $vswhere -all -products * -format value -property packages
    $hits = $pkgs | Select-String -Pattern 'VC\.Tools|Windows11SDK'
    if ($hits) { $hits | ForEach-Object { Write-Host "  $_" } }
    else { Write-Host '  (none matched - the component ids may differ on your channel)' -ForegroundColor Yellow }
} else {
    Write-Host '[WARN] vswhere.exe missing; the install may not have completed.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '--- Windows SDK Include versions ---'
$sdkInclude = 'C:\Program Files (x86)\Windows Kits\10\Include'
if (Test-Path $sdkInclude) {
    Get-ChildItem -LiteralPath $sdkInclude | Select-Object -ExpandProperty Name | ForEach-Object { Write-Host "  $_" }
} else {
    Write-Host '  [MISSING] SDK include root not found' -ForegroundColor Red
}

Write-Host ''
Write-Host '--- Required headers ---'
$hdrs = @('dcomp.h','d2d1_3.h','dwrite_3.h','shobjidl_core.h','d3d11_4.h')
foreach ($h in $hdrs) {
    $found = $null
    if (Test-Path $sdkInclude) {
        $found = Get-ChildItem -LiteralPath $sdkInclude -Recurse -Filter $h -ErrorAction SilentlyContinue |
                 Select-Object -First 1
    }
    if ($found) { Write-Host "  [ OK ] $h" -ForegroundColor Green }
    else        { Write-Host "  [MISS ] $h" -ForegroundColor Red }
}

# --- 5. Persist environment -----------------------------------------
Write-Host ''
Write-Host '[INFO] Writing persistent build environment ...'

$cmakeBin = 'C:\Program Files\CMake\bin'
$ninjaDir = "$env:LOCALAPPDATA\Microsoft\WinGet\Links"
$extra    = @($cmakeBin, $ninjaDir) | Where-Object { Test-Path $_ }

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not $userPath) { $userPath = '' }
$parts = $userPath.Split(';') | Where-Object { $_ -ne '' }
$missing = $extra | Where-Object { $parts -notcontains $_ }
if ($missing) {
    $newPath = (@($parts) + @($missing)) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    Write-Host "[ OK ] Appended to user PATH: $($missing -join ', ')" -ForegroundColor Green
} else {
    Write-Host '[ OK ] PATH already contains the tool directories.'
}

# Make VS discoverable by CMake in future non-VS shells
[Environment]::SetEnvironmentVariable('IslandBarVSBuildTools', $InstallPath, 'User')
Write-Host "[ OK ] Recorded VS path in user env: IslandBarVSBuildTools=$InstallPath"

Write-Host ''
Write-Host '=== DONE ===' -ForegroundColor Cyan
Write-Host 'Open a NEW PowerShell so PATH applies, then run:'
Write-Host '  cmake --version'
Write-Host '  ninja --version'
Write-Host ('  "{0}\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --version' -f $InstallPath)
