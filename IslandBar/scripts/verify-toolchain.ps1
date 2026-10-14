# =====================================================================
#  verify-toolchain.ps1
#  Checks whether the C++ build environment is actually usable.
#  Safe to run any time, elevated or not. Changes nothing.
# =====================================================================

$ErrorActionPreference = 'SilentlyContinue'
$script:fail = 0

function Section($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Pass($t)    { Write-Host "  [ OK ] $t" -ForegroundColor Green }
function Fail($t)    { Write-Host "  [MISS] $t" -ForegroundColor Red; $script:fail++ }
function Warn($t)    { Write-Host "  [WARN] $t" -ForegroundColor Yellow }

# --- Visual Studio instance ------------------------------------------
Section 'Visual Studio Build Tools instance'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = $null

if (Test-Path $vswhere) {
    Pass "vswhere: $vswhere"
    $vsPath = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
    if ($vsPath) { Pass "instance at $vsPath" }
    else         { Fail 'no VS instance found by vswhere' }
} else {
    Fail "vswhere.exe not found at $vswhere"
    # fall back to the path the setup script targeted
    if (Test-Path 'C:\TOOL\VSBuildTools') { $vsPath = 'C:\TOOL\VSBuildTools'; Warn "falling back to $vsPath" }
}

# --- MSVC compiler ---------------------------------------------------
Section 'MSVC compiler (cl.exe)'

$cl = $null
if ($vsPath) {
    $cl = Get-ChildItem "$vsPath\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
}
if ($cl) {
    Pass "toolset version $($cl.Name)"
    $clExe = Join-Path $cl.FullName 'bin\Hostx64\x64\cl.exe'
    if (Test-Path $clExe) { Pass "cl.exe -> $clExe" } else { Fail "cl.exe missing under $($cl.FullName)" }
    $libDir = Join-Path $cl.FullName 'lib\x64'
    if (Test-Path $libDir) { Pass 'x64 libraries present' } else { Fail "lib\x64 missing under $($cl.FullName)" }
} else {
    Fail 'no VC\Tools\MSVC toolset found'
}

# --- Windows SDK -----------------------------------------------------
Section 'Windows 11 SDK'

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkInc  = Join-Path $sdkRoot 'Include'
$sdkLib  = Join-Path $sdkRoot 'Lib'

if (Test-Path $sdkInc) {
    $vers = Get-ChildItem $sdkInc -Directory | Where-Object { $_.Name -match '^10\.' } |
            Sort-Object Name -Descending
    if ($vers) {
        Pass "SDK version(s): $(($vers | Select-Object -ExpandProperty Name) -join ', ')"
        $newest = $vers[0].Name
        if (Test-Path (Join-Path $sdkLib $newest)) { Pass "libs present for $newest" }
        else { Fail "Lib\$newest missing" }
    } else {
        Fail "no 10.x version directories under $sdkInc"
    }
} else {
    Fail "SDK include root not found: $sdkInc"
}

# --- Headers and import libs we actually need ------------------------
Section 'Required headers'

$headers = @{
    'dcomp.h'          = 'DirectComposition'
    'd2d1_3.h'         = 'Direct2D 1.3'
    'dwrite_3.h'       = 'DirectWrite 3'
    'd3d11_4.h'        = 'D3D11.4'
    'dxgi1_6.h'        = 'DXGI 1.6'
    'shobjidl_core.h'  = 'Shell item / AppBar'
    'shellscalingapi.h'= 'per-monitor DPI'
    'uiautomation.h'   = 'UI Automation'
    'sqlite3.h'        = '(third-party, expected absent)'
}

if (Test-Path $sdkInc) {
    $newest = (Get-ChildItem $sdkInc -Directory | Where-Object { $_.Name -match '^10\.' } |
               Sort-Object Name -Descending | Select-Object -First 1).FullName
    $um = Join-Path $newest 'um'
    $shared = Join-Path $newest 'shared'

    foreach ($h in $headers.Keys | Sort-Object) {
        $found = @()
        foreach ($d in @($um, $shared)) {
            if (Test-Path (Join-Path $d $h)) { $found += $d }
        }
        if ($found.Count -gt 0) { Pass "$h  ($($headers[$h]))" }
        elseif ($h -eq 'sqlite3.h') { Warn "$h  $($headers[$h])" }
        else { Fail "$h  ($($headers[$h]))" }
    }
}

Section 'Required import libraries'

if (Test-Path $sdkLib) {
    $newest = (Get-ChildItem $sdkLib -Directory | Where-Object { $_.Name -match '^10\.' } |
               Sort-Object Name -Descending | Select-Object -First 1).FullName
    $libs = @('dcomp.lib','d2d1.lib','dwrite.lib','d3d11.lib','dxgi.lib','shcore.lib',
              'shell32.lib','user32.lib','gdi32.lib','dwmapi.lib','uiautomationcore.lib',
              'windowscodecs.lib','propsys.lib','ole32.lib','shlwapi.lib')
    $umLib = Join-Path $newest 'um\x64'
    $missing = @()
    foreach ($l in $libs) {
        if (-not (Test-Path (Join-Path $umLib $l))) { $missing += $l }
    }
    if ($missing.Count -eq 0) { Pass "all $($libs.Count) import libraries present" }
    else { Fail "missing: $($missing -join ', ')" }
}

# --- CMake / Ninja ---------------------------------------------------
Section 'CMake and Ninja'

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { Pass "cmake -> $($cmake.Source)"; & cmake --version | Select-Object -First 1 | ForEach-Object { Write-Host "         $_" } }
else {
    $vsCmake = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) { Warn "cmake only inside VS: $vsCmake" } else { Fail 'cmake not found' }
}

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if ($ninja) { Pass "ninja -> $($ninja.Source)" } else { Warn 'ninja not found (optional; MSBuild generator still works)' }

# --- Summary ---------------------------------------------------------
Write-Host ''
if ($script:fail -eq 0) {
    Write-Host '=== RESULT: environment looks usable ===' -ForegroundColor Green
} else {
    Write-Host "=== RESULT: $script:fail check(s) failed ===" -ForegroundColor Red
    Write-Host 'If a component is missing, its --add id may not exist on your VS channel.' -ForegroundColor Yellow
    Write-Host "Inspect the newest $env:TEMP\dd_setup_*.log for 'not found' component warnings." -ForegroundColor Yellow
}
exit $script:fail
