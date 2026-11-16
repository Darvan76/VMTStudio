<#
.SYNOPSIS
    Automated Release Packaging Script for VMT-Studio.

.DESCRIPTION
    Packages VMT-Studio (x64) frontend, HLMV backend (x86), the x64 VTFLib runtime,
    deployed Qt 6 libraries and plugins, and documentation
    into a distributable directory and ZIP archive.

.PARAMETER Version
    Release version string (must match CMakeLists.txt; detected automatically).

.PARAMETER BuildDir
    Root build directory for 64-bit target (default: build64).

.PARAMETER OutDir
    Destination directory for staging and release zip (default: dist).

.EXAMPLE
    .\scripts\package_release.ps1
    .\scripts\package_release.ps1 -BuildDir "build64" -OutDir "dist"
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$Version,
    [ValidateSet('Release', 'Debug')]
    [string]$Config = "Release",
    [string]$BuildDir = "build64",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"

# Repository root (one level above this script)
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# Refuse to label a binary package with a version different from its source.
$CMakeContent = Get-Content -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($CMakeContent -notmatch '(?ms)project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Cannot read the application version from CMakeLists.txt.'
}
$ProjectVersion = $Matches[1]
if ($PSBoundParameters.ContainsKey('Version') -and $Version -ne $ProjectVersion) {
    throw "Package version $Version does not match project version $ProjectVersion."
}
$Version = $ProjectVersion

# Resolve BuildDir and OutDir against RepoRoot if relative
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $ResolvedBuildDir = Join-Path $RepoRoot $BuildDir
} else {
    $ResolvedBuildDir = $BuildDir
}

if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $ResolvedOutDir = Join-Path $RepoRoot $OutDir
} else {
    $ResolvedOutDir = $OutDir
}

$ConfigSuffix = if ($Config -ieq "Debug") { "-Debug" } elseif ($Config -ine "Release") { "-$Config" } else { "" }
$ResolvedBuildDir = [System.IO.Path]::GetFullPath($ResolvedBuildDir)
$ResolvedOutDir = [System.IO.Path]::GetFullPath($ResolvedOutDir)
$StagingDirName = "vmtstudio-v$Version-win64$ConfigSuffix"
$StagingDir = [System.IO.Path]::GetFullPath((Join-Path $ResolvedOutDir $StagingDirName))
$ZipPath = Join-Path $ResolvedOutDir "$StagingDirName.zip"
$outputPrefix = $ResolvedOutDir.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $StagingDir.StartsWith($outputPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The staging directory must stay inside the selected output directory.'
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Packaging VMT-Studio $Config v$Version" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "Repository Root: $RepoRoot"
Write-Host "Build Directory: $ResolvedBuildDir"
Write-Host "Configuration:   $Config"
Write-Host "Output Directory: $ResolvedOutDir"
Write-Host "Staging Directory: $StagingDir"
Write-Host "Target Archive:    $ZipPath"
Write-Host ""

# Ensure output directory exists
if (-not (Test-Path $ResolvedOutDir)) {
    New-Item -ItemType Directory -Path $ResolvedOutDir -Force | Out-Null
}

# Clean previous staging directory if present
if (Test-Path $StagingDir) {
    Write-Host "Cleaning existing staging directory..."
    Remove-Item -LiteralPath $StagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null

# -----------------------------------------------------------------------------
# 1. Copy vmtstudio.exe
# -----------------------------------------------------------------------------
$VmtStudioCandidates = @(
    (Join-Path $ResolvedBuildDir "bin/$Config/vmtstudio.exe"),
    (Join-Path $ResolvedBuildDir "bin/vmtstudio.exe")
)
$VmtStudioExe = $null
foreach ($cand in $VmtStudioCandidates) {
    if (Test-Path $cand) {
        $VmtStudioExe = $cand
        break
    }
}
if (-not $VmtStudioExe) {
    throw "Could not find vmtstudio.exe in '$ResolvedBuildDir/bin/$Config' or '$ResolvedBuildDir/bin'."
}

Write-Host "Copying vmtstudio.exe from $VmtStudioExe..."
Copy-Item -Path $VmtStudioExe -Destination (Join-Path $StagingDir "vmtstudio.exe") -Force

# -----------------------------------------------------------------------------
# 2. Copy hlmv_backend.exe
# -----------------------------------------------------------------------------
$HlmvCandidates = @(
    (Join-Path $RepoRoot "build/bin/$Config/hlmv_backend.exe"),
    (Join-Path $ResolvedBuildDir "bin/$Config/hlmv_backend.exe"),
    (Join-Path $RepoRoot "build/bin/hlmv_backend.exe"),
    (Join-Path $ResolvedBuildDir "bin/hlmv_backend.exe")
)
$HlmvBackendExe = $null
foreach ($cand in $HlmvCandidates) {
    if (Test-Path $cand) {
        $HlmvBackendExe = $cand
        break
    }
}
if (-not $HlmvBackendExe) {
    throw "Could not find hlmv_backend.exe in 'build/bin/$Config' or 'build/bin/Release'."
}

Write-Host "Copying hlmv_backend.exe from $HlmvBackendExe..."
Copy-Item -Path $HlmvBackendExe -Destination (Join-Path $StagingDir "hlmv_backend.exe") -Force

# -----------------------------------------------------------------------------
# 3. Copy the 64-bit VTFLib runtime DLL
# -----------------------------------------------------------------------------
# 64-bit VTFLib next to vmtstudio.exe
$Vtf64Candidates = @(
    (Join-Path $ResolvedBuildDir "bin/$Config/VTFLib++_d.dll"),
    (Join-Path $ResolvedBuildDir "bin/$Config/VTFLib++.dll"),
    (Join-Path $ResolvedBuildDir "bin/$Config/VTFLib.dll"),
    (Join-Path $ResolvedBuildDir "bin/Release/VTFLib++.dll"),
    (Join-Path $ResolvedBuildDir "bin/Release/VTFLib.dll"),
    (Join-Path $ResolvedBuildDir "bin/Debug/VTFLib++_d.dll"),
    (Join-Path $ResolvedBuildDir "bin/Debug/VTFLib++.dll"),
    (Join-Path $ResolvedBuildDir "bin/Debug/VTFLib.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib64/bin/$Config/VTFLib++_d.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib64/bin/$Config/VTFLib++.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib64/bin/$Config/VTFLib.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib64/bin/VTFLib++_d.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib64/bin/VTFLib++.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib64/bin/VTFLib.dll")
)
$Vtf64Dll = $null
foreach ($cand in $Vtf64Candidates) {
    if ($Config -ieq 'Release' -and ($cand -match '[/\\]Debug[/\\]' -or $cand -like '*_d.dll')) {
        continue
    }
    if (Test-Path $cand) {
        $Vtf64Dll = $cand
        break
    }
}
if (-not $Vtf64Dll) {
    throw "Could not find 64-bit VTFLib DLL in build directory or thirdparty."
}

Write-Host "Copying 64-bit VTFLib runtime from $Vtf64Dll..."
$Vtf64BaseName = Split-Path -Leaf $Vtf64Dll
Copy-Item -Path $Vtf64Dll -Destination (Join-Path $StagingDir $Vtf64BaseName) -Force
# Ensure both VTFLib++.dll and VTFLib.dll exist so standard lookups succeed
if ($Vtf64BaseName -ne "VTFLib.dll") {
    Copy-Item -Path $Vtf64Dll -Destination (Join-Path $StagingDir "VTFLib.dll") -Force
}
if ($Vtf64BaseName -ne "VTFLib++.dll" -and $Vtf64BaseName -ne "VTFLib++_d.dll") {
    Copy-Item -Path $Vtf64Dll -Destination (Join-Path $StagingDir "VTFLib++.dll") -Force
}

# -----------------------------------------------------------------------------
# 3b. Copy Debug Symbols (*.pdb) for Debug configuration
# -----------------------------------------------------------------------------
if ($Config -ieq "Debug") {
    Write-Host "Copying debug symbols (*.pdb)..."

    # 64-bit vmtstudio.pdb
    $VmtStudioPdbCandidates = @(
        [System.IO.Path]::ChangeExtension($VmtStudioExe, ".pdb"),
        (Join-Path $ResolvedBuildDir "bin/$Config/vmtstudio.pdb"),
        (Join-Path $ResolvedBuildDir "bin/Debug/vmtstudio.pdb")
    )
    foreach ($cand in $VmtStudioPdbCandidates) {
        if ($cand -and (Test-Path $cand)) {
            Write-Host "  Copying $(Split-Path -Leaf $cand)..."
            Copy-Item -Path $cand -Destination (Join-Path $StagingDir "vmtstudio.pdb") -Force
            break
        }
    }

    # 32-bit hlmv_backend.pdb
    $HlmvPdbCandidates = @(
        [System.IO.Path]::ChangeExtension($HlmvBackendExe, ".pdb"),
        (Join-Path $RepoRoot "build/bin/$Config/hlmv_backend.pdb"),
        (Join-Path $RepoRoot "build/bin/Debug/hlmv_backend.pdb"),
        (Join-Path $ResolvedBuildDir "bin/$Config/hlmv_backend.pdb")
    )
    foreach ($cand in $HlmvPdbCandidates) {
        if ($cand -and (Test-Path $cand)) {
            Write-Host "  Copying $(Split-Path -Leaf $cand)..."
            Copy-Item -Path $cand -Destination (Join-Path $StagingDir "hlmv_backend.pdb") -Force
            break
        }
    }

    # 64-bit VTFLib PDB if present
    if ($Vtf64Dll) {
        $Vtf64Pdb = [System.IO.Path]::ChangeExtension($Vtf64Dll, ".pdb")
        if (Test-Path $Vtf64Pdb) {
            Write-Host "  Copying $(Split-Path -Leaf $Vtf64Pdb)..."
            Copy-Item -Path $Vtf64Pdb -Destination (Join-Path $StagingDir (Split-Path -Leaf $Vtf64Pdb)) -Force
        }
    }
}

# -----------------------------------------------------------------------------
# 4. Qt 6 runtime dependencies and plugins
# -----------------------------------------------------------------------------
$Windeployqt = Get-Command windeployqt.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $Windeployqt) {
    $CandidateQtPaths = @(
        "$env:QT_ROOT_DIR\bin\windeployqt.exe",
        "C:\Qt\6.8.2\msvc2022_64\bin\windeployqt.exe",
        "C:\Qt\6.8.1\msvc2022_64\bin\windeployqt.exe",
        "C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe",
        "$env:QTDIR\bin\windeployqt.exe"
    )
    foreach ($cand in $CandidateQtPaths) {
        if ($cand -and (Test-Path $cand)) {
            $Windeployqt = $cand
            break
        }
    }
}

if ($Windeployqt) {
    $DeployMode = if ($Config -eq "Debug") { "--debug" } else { "--release" }
    Write-Host "Running windeployqt ($Windeployqt) with $DeployMode..."
    & $Windeployqt $DeployMode --no-translations --no-opengl-sw (Join-Path $StagingDir "vmtstudio.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit code $LASTEXITCODE; the package is incomplete."
    }
} else {
    Write-Host "windeployqt not found; relying on pre-deployed Qt files from build folder."
}

# Copy any additional runtime DLLs from build directory
$ReleaseBinDir = Split-Path -Parent $VmtStudioExe

# Ship the redistributable CRT app-locally: a portable package must not depend
# on Visual Studio or a separately installed VC++ redistributable.
if ($Config -ieq 'Release') {
    $crtDirectory = $null
    $redistRoots = @()
    if ($env:VCToolsRedistDir) {
        $redistRoots += $env:VCToolsRedistDir
    }
    foreach ($edition in @('Community', 'Professional', 'Enterprise')) {
        $redistRoot = Join-Path $env:ProgramFiles "Microsoft Visual Studio/2022/$edition/VC/Redist/MSVC"
        if (Test-Path -LiteralPath $redistRoot -PathType Container) {
            $redistRoots += @(Get-ChildItem -LiteralPath $redistRoot -Directory |
                Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
                Sort-Object { [version]$_.Name } -Descending | ForEach-Object { $_.FullName })
        }
    }
    foreach ($redistRoot in $redistRoots) {
        $candidate = Join-Path $redistRoot 'x64/Microsoft.VC143.CRT'
        if (Test-Path -LiteralPath (Join-Path $candidate 'vcruntime140.dll') -PathType Leaf) {
            $crtDirectory = $candidate
            break
        }
    }
    if ($crtDirectory) {
        Get-ChildItem -LiteralPath $crtDirectory -Filter '*.dll' -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $StagingDir -Force
        }
    } else {
        foreach ($runtimeName in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
            $runtimePath = Join-Path $ReleaseBinDir $runtimeName
            if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
                throw "Cannot package a portable release: missing VC++ runtime $runtimeName."
            }
            Copy-Item -LiteralPath $runtimePath -Destination $StagingDir -Force
        }
    }
}

$AdditionalDllPatterns = @(
    "Qt6*.dll",
    "D3Dcompiler_47.dll",
    "dxcompiler.dll",
    "dxil.dll"
)
foreach ($pattern in $AdditionalDllPatterns) {
    Get-ChildItem -Path $ReleaseBinDir -Filter $pattern -File -ErrorAction SilentlyContinue | ForEach-Object {
        # If Release mode, skip Qt debug libraries (*d.dll)
        if ($Config -ieq "Release" -and $_.Name -match "^Qt6.*d\.dll$") {
            return
        }
        $dest = Join-Path $StagingDir $_.Name
        if (-not (Test-Path $dest)) {
            Copy-Item -Path $_.FullName -Destination $dest -Force
        }
    }
}

# Ensure all plugin folders present in build directory are copied
$PluginFolders = @(
    "generic",
    "iconengines",
    "imageformats",
    "networkinformation",
    "platforms",
    "styles",
    "tls"
)
foreach ($folder in $PluginFolders) {
    $srcFolder = Join-Path $ReleaseBinDir $folder
    if (Test-Path $srcFolder) {
        $dstFolder = Join-Path $StagingDir $folder
        if (-not (Test-Path $dstFolder)) {
            New-Item -ItemType Directory -Path $dstFolder -Force | Out-Null
        }
        Get-ChildItem -Path $srcFolder -File | ForEach-Object {
            $destFile = Join-Path $dstFolder $_.Name
            if (-not (Test-Path $destFile)) {
                Copy-Item -Path $_.FullName -Destination $destFile -Force
            }
        }
    }
}

# -----------------------------------------------------------------------------
# 5. Copy models directory
# -----------------------------------------------------------------------------
$ModelsSrc = Join-Path $RepoRoot "models"
if (Test-Path $ModelsSrc) {
    Write-Host "Copying preview models..."
    $ModelsDst = Join-Path $StagingDir "models"
    if (-not (Test-Path $ModelsDst)) {
        New-Item -ItemType Directory -Path $ModelsDst -Force | Out-Null
    }
    Copy-Item -Path (Join-Path $ModelsSrc "*") -Destination $ModelsDst -Recurse -Force
} else {
    Write-Warning "Models directory not found at '$ModelsSrc'."
}

# -----------------------------------------------------------------------------
# 6. Copy LICENSE and README files
# -----------------------------------------------------------------------------
Write-Host "Copying documentation and license..."
$DocFiles = @("LICENSE", "README.md", "README.es.md", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md")
foreach ($doc in $DocFiles) {
    $srcDoc = Join-Path $RepoRoot $doc
    if (-not (Test-Path -LiteralPath $srcDoc -PathType Leaf)) {
        throw "Required release documentation is missing: $srcDoc"
    }
    Copy-Item -LiteralPath $srcDoc -Destination (Join-Path $StagingDir $doc) -Force
}

# Keep the license texts with both the ZIP and the embedded portable payload.
$LicenseDir = Join-Path $StagingDir 'licenses'
New-Item -ItemType Directory -Path $LicenseDir -Force | Out-Null
$LicenseSources = @{
    'VTFLib-LGPL-2.1.txt' = 'thirdparty/vtflib/LGPL.txt'
    'miniz-LICENSE.txt' = 'thirdparty/vtflib/thirdparty/miniz/LICENSE'
    'zstd-LICENSE.txt' = 'thirdparty/vtflib/thirdparty/zstd/LICENSE'
    'Compressonator-LICENSE.txt' = 'thirdparty/vtflib/thirdparty/compressonator/license/license.txt'
}
foreach ($name in $LicenseSources.Keys) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot $LicenseSources[$name]) -Destination (Join-Path $LicenseDir $name) -Force
}
$AdditionalLicenses = Join-Path $RepoRoot 'thirdparty/licenses'
if (Test-Path -LiteralPath $AdditionalLicenses -PathType Container) {
    Copy-Item -Path (Join-Path $AdditionalLicenses '*') -Destination $LicenseDir -Recurse -Force
}
$PatchDir = Join-Path $LicenseDir 'VTFLib-patches'
Copy-Item -LiteralPath (Join-Path $RepoRoot 'thirdparty/patches') -Destination $PatchDir -Recurse -Force

# -----------------------------------------------------------------------------
# 6b. Staging directory hygiene cleanup
# -----------------------------------------------------------------------------
Write-Host "Sanitizing staging directory..."
$JunkFilters = @("*.ilk", "*.exp", "*.obj", "*.iobj", "*.ipdb", "*.tmp", "*.temp", "*.log", "*.dmp", "*.mdmp", "*.stackdump", "*.core", "opengl32sw.dll")
if ($Config -ine "Debug") {
    $JunkFilters += "*.pdb"
}
foreach ($junkFilter in $JunkFilters) {
    Get-ChildItem -Path $StagingDir -Recurse -Filter $junkFilter -File -ErrorAction SilentlyContinue | Remove-Item -Force
}

# -----------------------------------------------------------------------------
# 7. Create ZIP archive
# -----------------------------------------------------------------------------
if (Test-Path $ZipPath) {
    Remove-Item -Path $ZipPath -Force
}

Write-Host "Compressing archive to $ZipPath..."
& (Join-Path $PSScriptRoot 'verify_release_package.ps1') -PackageDir $StagingDir -Version $Version -Config $Config
Compress-Archive -Path $StagingDir -DestinationPath $ZipPath -Force
& (Join-Path $PSScriptRoot 'verify_release_package.ps1') -PackageDir $StagingDir -ArchivePath $ZipPath -Version $Version -Config $Config

# -----------------------------------------------------------------------------
# 8. Summary output
# -----------------------------------------------------------------------------
$PackagedFiles = Get-ChildItem -Path $StagingDir -Recurse -File
$ZipItem = Get-Item $ZipPath
$ZipSizeMB = [math]::Round($ZipItem.Length / 1MB, 2)
$ZipSizeBytes = $ZipItem.Length

Write-Host ""
Write-Host "================ Release Packaging Summary ================" -ForegroundColor Cyan
Write-Host ("Package Name:      {0}" -f $StagingDirName)
Write-Host ("Version:           {0}" -f $Version)
Write-Host ("Staging Directory: {0}" -f $StagingDir)
Write-Host ("Archive Path:      {0}" -f $ZipPath)
Write-Host ("Archive Size:      {0} MB ({1:N0} bytes)" -f $ZipSizeMB, $ZipSizeBytes)
Write-Host ("Total Files:       {0}" -f $PackagedFiles.Count)
Write-Host "-----------------------------------------------------------"
Write-Host "Packaged Files Breakdown:"
$PackagedFiles | ForEach-Object {
    $relativePath = $_.FullName.Substring($StagingDir.Length).TrimStart('\', '/')
    $sizeKB = [math]::Round($_.Length / 1KB, 1)
    Write-Host ("  {0,-50} {1,10} KB" -f $relativePath, $sizeKB)
}
Write-Host "===========================================================" -ForegroundColor Cyan
Write-Host ""

return [PSCustomObject]@{
    Version      = $Version
    StagingDir   = $StagingDir
    ZipPath      = $ZipPath
    ZipSizeBytes = $ZipSizeBytes
    ZipSizeMB    = $ZipSizeMB
    FileCount    = $PackagedFiles.Count
}
