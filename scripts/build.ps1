<#
.SYNOPSIS
    Build script for VMT-Studio (Release, Debug, or Both).

.PARAMETER Config
    Build configuration: "Release", "Debug", or "Both" (default: Release).

.PARAMETER Package
    If specified, packages into dist/*.zip after building.

.PARAMETER RunTests
    If specified, runs the unit test suite after building.
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "Both")]
    [string]$Config = "Release",
    [switch]$Package,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# Locate CMake
$CmakeExe = (Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
if (-not $CmakeExe) {
    $Candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    )
    foreach ($c in $Candidates) {
        if (Test-Path $c) { $CmakeExe = $c; break }
    }
}
if (-not $CmakeExe) {
    throw "CMake was not found in PATH or standard Visual Studio installations."
}

$ConfigsToBuild = if ($Config -eq "Both") { @("Release", "Debug") } else { @($Config) }

foreach ($cfg in $ConfigsToBuild) {
    Write-Host "==========================================================" -ForegroundColor Cyan
    Write-Host " Building VMT-Studio ($cfg)" -ForegroundColor Cyan
    Write-Host "==========================================================" -ForegroundColor Cyan

    $BuildWin32 = Join-Path $RepoRoot "build"
    $BuildX64 = Join-Path $RepoRoot "build64"

    # Auto-configure if build directories do not exist yet
    if (-not (Test-Path $BuildWin32)) {
        Write-Host "Configuring 32-bit Win32 build directory..."
        & $CmakeExe -S $RepoRoot -B $BuildWin32 -A Win32
    }
    if (-not (Test-Path $BuildX64)) {
        Write-Host "Configuring 64-bit x64 build directory..."
        & $CmakeExe -S $RepoRoot -B $BuildX64 -A x64
    }

    # 1. Win32 HLMV backend
    if (Test-Path $BuildWin32) {
        Write-Host "Building 32-bit HLMV backend ($cfg)..."
        & $CmakeExe --build $BuildWin32 --config $cfg --target hlmv_backend
    }

    # 2. 64-bit VMT-Studio frontend
    if (Test-Path $BuildX64) {
        Write-Host "Building 64-bit VMT-Studio ($cfg)..."
        if ($RunTests) {
            & $CmakeExe --build $BuildX64 --config $cfg
        } else {
            & $CmakeExe --build $BuildX64 --config $cfg --target vmtstudio
        }
    }

    # 3. Unit tests if requested
    if ($RunTests) {
        Write-Host "Running unit tests ($cfg)..."
        $CtestExe = Join-Path (Split-Path $CmakeExe) "ctest.exe"
        if (Test-Path $CtestExe) {
            & $CtestExe --test-dir $BuildX64 -C $cfg --output-on-failure
        } else {
            $TestExe = Join-Path $BuildX64 "bin/$cfg/ui_tests.exe"
            if (Test-Path $TestExe) {
                & $TestExe
            }
        }
    }

    # 4. Package if requested
    if ($Package) {
        Write-Host "Packaging $cfg distribution..."
        & (Join-Path $PSScriptRoot "package_release.ps1") -Config $cfg
    }
}

Write-Host "Build operation completed successfully!" -ForegroundColor Green
