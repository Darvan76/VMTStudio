<#
.SYNOPSIS
    Automated Release Packaging Script for VMT-Studio.

.DESCRIPTION
    Packages VMT-Studio (x64) frontend, HLMV backend (x86), VTFLib runtimes,
    deployed Qt 6 libraries and plugins, shader schemas, and documentation
    into a distributable directory and ZIP archive.

.PARAMETER Version
    Release version string (default: 0.2.0).

.PARAMETER BuildDir
    Root build directory for 64-bit target (default: build64).

.PARAMETER OutDir
    Destination directory for staging and release zip (default: dist).

.EXAMPLE
    .\scripts\package_release.ps1
    .\scripts\package_release.ps1 -Version "0.2.0" -BuildDir "build64" -OutDir "dist"
#>
[CmdletBinding()]
param(
    [string]$Version = "0.2.0",
    [string]$Config = "Release",
    [string]$BuildDir = "build64",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"

# Repository root (one level above this script)
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# Auto-detect version from root CMakeLists.txt if default or not specified
if (-not $PSBoundParameters.ContainsKey('Version')) {
    $CMakePath = Join-Path $RepoRoot "CMakeLists.txt"
    if (Test-Path $CMakePath) {
        $CMakeContent = Get-Content $CMakePath -Raw
        if ($CMakeContent -match '(?ms)project\s*\([^)]*VERSION\s+([0-9]+(?:\.[0-9]+)*)') {
            $Version = $matches[1]
        }
    }
}

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
$StagingDirName = "vmtstudio-v$Version-win64$ConfigSuffix"
$StagingDir = Join-Path $ResolvedOutDir $StagingDirName
$ZipPath = Join-Path $ResolvedOutDir "$StagingDirName.zip"

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
    Remove-Item -Recurse -Force $StagingDir
}
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null

# -----------------------------------------------------------------------------
# 1. Copy vmtstudio.exe
# -----------------------------------------------------------------------------
$VmtStudioCandidates = @(
    (Join-Path $ResolvedBuildDir "bin/$Config/vmtstudio.exe"),
    (Join-Path $ResolvedBuildDir "bin/Release/vmtstudio.exe"),
    (Join-Path $ResolvedBuildDir "bin/Debug/vmtstudio.exe"),
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
    (Join-Path $RepoRoot "build/bin/Release/hlmv_backend.exe"),
    (Join-Path $ResolvedBuildDir "bin/Release/hlmv_backend.exe"),
    (Join-Path $RepoRoot "build/bin/Debug/hlmv_backend.exe"),
    (Join-Path $ResolvedBuildDir "bin/Debug/hlmv_backend.exe"),
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
# 3. Copy VTFLib runtime DLLs (64-bit and 32-bit if applicable)
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

# 32-bit VTFLib (if present in build/ or thirdparty)
$Vtf32Candidates = @(
    (Join-Path $RepoRoot "build/bin/$Config/VTFLib++_d.dll"),
    (Join-Path $RepoRoot "build/bin/$Config/VTFLib++.dll"),
    (Join-Path $RepoRoot "build/bin/$Config/VTFLib.dll"),
    (Join-Path $RepoRoot "build/bin/Release/VTFLib++.dll"),
    (Join-Path $RepoRoot "build/bin/Release/VTFLib.dll"),
    (Join-Path $RepoRoot "build/bin/Debug/VTFLib++_d.dll"),
    (Join-Path $RepoRoot "build/bin/Debug/VTFLib++.dll"),
    (Join-Path $RepoRoot "build/bin/Debug/VTFLib.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib/bin/$Config/VTFLib++_d.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib/bin/$Config/VTFLib++.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib/bin/$Config/VTFLib.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib/bin/VTFLib++_d.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib/bin/VTFLib++.dll"),
    (Join-Path $RepoRoot "thirdparty/vtflib/build_vtflib/bin/VTFLib.dll")
)
$Vtf32Dll = $null
foreach ($cand in $Vtf32Candidates) {
    if (Test-Path $cand) {
        $Vtf32Dll = $cand
        break
    }
}
if ($Vtf32Dll) {
    Write-Host "Copying 32-bit VTFLib runtime from $Vtf32Dll..."
    $X86Dir = Join-Path $StagingDir "x86"
    if (-not (Test-Path $X86Dir)) {
        New-Item -ItemType Directory -Path $X86Dir -Force | Out-Null
    }
    $Vtf32BaseName = Split-Path -Leaf $Vtf32Dll
    Copy-Item -Path $Vtf32Dll -Destination (Join-Path $X86Dir $Vtf32BaseName) -Force
    if ($Vtf32BaseName -ne "VTFLib.dll") {
        Copy-Item -Path $Vtf32Dll -Destination (Join-Path $X86Dir "VTFLib.dll") -Force
    }
    if ($Vtf32BaseName -ne "VTFLib++.dll" -and $Vtf32BaseName -ne "VTFLib++_d.dll") {
        Copy-Item -Path $Vtf32Dll -Destination (Join-Path $X86Dir "VTFLib++.dll") -Force
    }
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

    # VTFLib PDBs if present
    if ($Vtf64Dll) {
        $Vtf64Pdb = [System.IO.Path]::ChangeExtension($Vtf64Dll, ".pdb")
        if (Test-Path $Vtf64Pdb) {
            Write-Host "  Copying $(Split-Path -Leaf $Vtf64Pdb)..."
            Copy-Item -Path $Vtf64Pdb -Destination (Join-Path $StagingDir (Split-Path -Leaf $Vtf64Pdb)) -Force
        }
    }
    if ($Vtf32Dll) {
        $Vtf32Pdb = [System.IO.Path]::ChangeExtension($Vtf32Dll, ".pdb")
        if (Test-Path $Vtf32Pdb) {
            Write-Host "  Copying 32-bit $(Split-Path -Leaf $Vtf32Pdb)..."
            $X86Dir = Join-Path $StagingDir "x86"
            Copy-Item -Path $Vtf32Pdb -Destination (Join-Path $X86Dir (Split-Path -Leaf $Vtf32Pdb)) -Force
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
    & $Windeployqt $DeployMode --no-translations (Join-Path $StagingDir "vmtstudio.exe")
} else {
    Write-Host "windeployqt not found; relying on pre-deployed Qt files from build folder."
}

# Copy any additional runtime DLLs from build directory
$ReleaseBinDir = Split-Path -Parent $VmtStudioExe
$AdditionalDllPatterns = @(
    "Qt6*.dll",
    "D3Dcompiler_47.dll",
    "dxcompiler.dll",
    "dxil.dll",
    "opengl32sw.dll"
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
# 5. Copy data/schema directory
# -----------------------------------------------------------------------------
$SchemaSrc = Join-Path $RepoRoot "data/schema"
if (Test-Path $SchemaSrc) {
    Write-Host "Copying schema definitions..."
    $SchemaDstData = Join-Path $StagingDir "data/schema"
    if (-not (Test-Path $SchemaDstData)) {
        New-Item -ItemType Directory -Path $SchemaDstData -Force | Out-Null
    }
    Copy-Item -Path (Join-Path $SchemaSrc "*") -Destination $SchemaDstData -Recurse -Force

    # Also provide materials/schema beside the executable
    $SchemaDstMaterials = Join-Path $StagingDir "materials/schema"
    if (-not (Test-Path $SchemaDstMaterials)) {
        New-Item -ItemType Directory -Path $SchemaDstMaterials -Force | Out-Null
    }
    Copy-Item -Path (Join-Path $SchemaSrc "*") -Destination $SchemaDstMaterials -Recurse -Force
} else {
    Write-Warning "Schema directory not found at '$SchemaSrc'."
}

# -----------------------------------------------------------------------------
# 5b. Copy models directory
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
$DocFiles = @("LICENSE", "README.md", "README.es.md")
foreach ($doc in $DocFiles) {
    $srcDoc = Join-Path $RepoRoot $doc
    if (Test-Path $srcDoc) {
        Copy-Item -Path $srcDoc -Destination (Join-Path $StagingDir $doc) -Force
    }
}

# -----------------------------------------------------------------------------
# 6b. Staging directory hygiene cleanup
# -----------------------------------------------------------------------------
Write-Host "Sanitizing staging directory..."
$JunkFilters = @("*.ilk", "*.exp", "*.obj", "*.iobj", "*.ipdb", "*.tmp", "*.temp", "*.log", "*.dmp", "*.mdmp", "*.stackdump", "*.core")
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
Compress-Archive -Path $StagingDir -DestinationPath $ZipPath -Force

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
