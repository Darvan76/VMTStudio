<#
.SYNOPSIS
    Builds a portable executable from the exact versioned distribution ZIP.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^$|^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$Version = '',
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    # CI can reuse the package created in the immediately preceding step.
    [switch]$UseExistingPackage
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$projectText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($projectText -notmatch '(?ms)project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Cannot read the application version from CMakeLists.txt.'
}
$projectVersion = $Matches[1]
if (-not [string]::IsNullOrWhiteSpace($Version) -and $Version -ne $projectVersion) {
    throw "Portable version $Version does not match project version $projectVersion."
}
$Version = $projectVersion

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$cmakeExe = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmakeExe) {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\CMake\bin\cmake.exe'
    )
    $cmakeExe = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $cmakeExe) { throw 'CMake was not found.' }

$distDir = Join-Path $repoRoot 'dist'
$configSuffix = if ($Config -eq 'Debug') { '-Debug' } else { '' }
$zipPath = Join-Path $distDir "vmtstudio-v$Version-win64$configSuffix.zip"
if (-not $UseExistingPackage) {
    & (Join-Path $PSScriptRoot 'package_release.ps1') -Version $Version -Config $Config
}
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    throw "The distribution ZIP was not created: $zipPath"
}
& (Join-Path $PSScriptRoot 'verify_release_package.ps1') `
    -PackageDir (Join-Path $distDir "vmtstudio-v$Version-win64$configSuffix") `
    -ArchivePath $zipPath -Version $Version -Config $Config

$launcherSource = Join-Path $repoRoot 'tools/portable_launcher'
$launcherBuild = Join-Path $repoRoot 'build64/portable_launcher'
& $cmakeExe -S $launcherSource -B $launcherBuild -A x64 `
    "-DVMTSTUDIO_PORTABLE_VERSION:STRING=$Version" "-DVMTSTUDIO_PAYLOAD:FILEPATH=$zipPath"
if ($LASTEXITCODE -ne 0) { throw "Launcher configuration failed ($LASTEXITCODE)." }
& $cmakeExe --build $launcherBuild --config $Config --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Launcher build failed ($LASTEXITCODE)." }

$portableName = "VMT-Studio-v$Version-Portable.exe"
$portableExe = Join-Path $launcherBuild "$Config/$portableName"
if (-not (Test-Path -LiteralPath $portableExe -PathType Leaf)) {
    $portableExe = Join-Path $launcherBuild $portableName
}
if (-not (Test-Path -LiteralPath $portableExe -PathType Leaf)) {
    throw "The built launcher was not found: $portableExe"
}

$distPortableName = "VMT-Studio-v$Version-Portable$configSuffix.exe"
$distExe = Join-Path $distDir $distPortableName
Copy-Item -LiteralPath $portableExe -Destination $distExe -Force
Copy-Item -LiteralPath $portableExe -Destination (Join-Path $repoRoot $distPortableName) -Force
$checksumPath = Join-Path $distDir 'SHA256SUMS'
@($zipPath, $distExe) | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path -Leaf $_)"
} | Set-Content -LiteralPath $checksumPath -Encoding ascii
$item = Get-Item -LiteralPath $distExe
Write-Output "Portable executable: $($item.FullName)"
Write-Output "Size: $($item.Length) bytes; version: $Version; configuration: $Config"
Write-Output "SHA-256 checksums: $checksumPath"
