<#
.SYNOPSIS
    Builds the standalone single-file portable executable for VMT-Studio.
#>
param(
    [string]$Version = "0.2.0",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$DistDir = Join-Path $RepoRoot "dist"
$ZipName = "vmtstudio-v$Version-win64.zip"
$ZipPath = Join-Path $DistDir $ZipName

if (-not (Test-Path $ZipPath)) {
    Write-Host "Release zip not found at $ZipPath. Generating release package first..."
    & "$RepoRoot\scripts\package_release.ps1" -Version $Version -Config $Config -BuildDir "build64" -OutDir "dist"
}

$LauncherSrcDir = Join-Path $RepoRoot "tools/portable_launcher"
$LauncherBuildDir = Join-Path $RepoRoot "build64/portable_launcher"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Building Single-File Portable Executable v$Version" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Copy current payload.zip to launcher source directory for the .rc compilation
Copy-Item -Path $ZipPath -Destination (Join-Path $LauncherSrcDir "payload.zip") -Force

Write-Host "Configuring CMake for portable launcher..."
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
    -S $LauncherSrcDir -B $LauncherBuildDir -A x64

Write-Host "Building portable launcher in $Config..."
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
    --build $LauncherBuildDir --config $Config

$PortableExe = Join-Path $LauncherBuildDir "$Config/VMT-Studio-v$Version-Portable.exe"
if (-not (Test-Path $PortableExe)) {
    $PortableExe = Join-Path $LauncherBuildDir "VMT-Studio-v$Version-Portable.exe"
}

if (-not (Test-Path $PortableExe)) {
    throw "Failed to find built portable launcher at $PortableExe"
}

# Output to dist/ and to project root
$DestDist = Join-Path $DistDir "VMT-Studio-v$Version-Portable.exe"
$DestRoot = Join-Path $RepoRoot "VMT-Studio-v$Version-Portable.exe"

Copy-Item -Path $PortableExe -Destination $DestDist -Force
Copy-Item -Path $PortableExe -Destination $DestRoot -Force

# Clean up temp payload copy in tools/
Remove-Item -Path (Join-Path $LauncherSrcDir "payload.zip") -Force -ErrorAction SilentlyContinue

$Item = Get-Item $DestRoot
$SizeMB = [math]::Round($Item.Length / 1MB, 2)

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Single-File Portable Executable Ready!" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
Write-Host "Output at root: $DestRoot"
Write-Host "Output in dist: $DestDist"
Write-Host "Total Size:     $SizeMB MB ($($Item.Length) bytes)"
Write-Host "Architecture:   x64 Portable Launcher (embeds 64-bit Qt frontend + 32-bit HLMV backend)"
Write-Host "==========================================================" -ForegroundColor Green
