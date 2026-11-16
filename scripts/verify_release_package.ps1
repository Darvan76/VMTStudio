<#
.SYNOPSIS
    Verifies the runtime payload and, optionally, its exact ZIP contents.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$Version,
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [string]$ArchivePath = ''
)

$ErrorActionPreference = 'Stop'
$packageRoot = (Resolve-Path -LiteralPath $PackageDir).Path
$qtSuffix = if ($Config -eq 'Debug') { 'd' } else { '' }
$vtfName = if ($Config -eq 'Debug') { 'VTFLib++_d.dll' } else { 'VTFLib++.dll' }
$required = @(
    'vmtstudio.exe', 'hlmv_backend.exe', $vtfName,
    "Qt6Core$qtSuffix.dll", "Qt6Gui$qtSuffix.dll", "Qt6Widgets$qtSuffix.dll", "Qt6Network$qtSuffix.dll",
    "platforms/qwindows$qtSuffix.dll",
    'LICENSE', 'README.md', 'README.es.md', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md',
    'licenses/VTFLib-LGPL-2.1.txt', 'licenses/miniz-LICENSE.txt', 'licenses/zstd-LICENSE.txt',
    'licenses/Qt-LGPL-3.0.txt', 'licenses/Qt-GPL-3.0.txt', 'licenses/Compressonator-LICENSE.txt',
    'licenses/Valve-Source-SDK-LICENSE.txt', 'licenses/Valve-thirdpartylegalnotices.txt',
    'licenses/mxToolKit-notice.txt'
)
if ($Config -eq 'Release') {
    $required += @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
}
foreach ($model in @('sphere_model', 'cube_model', 'cylinder_model')) {
    foreach ($extension in @('mdl', 'vvd', 'dx90.vtx')) {
        $required += "models/$model.$extension"
    }
}
foreach ($relative in $required) {
    $path = Join-Path $packageRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Required package file is missing or empty: $relative"
    }
}

function Assert-PeMachine([string]$RelativePath, [uint16]$ExpectedMachine) {
    $stream = [System.IO.File]::OpenRead((Join-Path $packageRoot $RelativePath))
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw "Invalid PE file: $RelativePath"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or ($peOffset + 6) -gt $stream.Length) {
            throw "Invalid PE header offset: $RelativePath"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne $ExpectedMachine) {
            throw "Wrong binary architecture: $RelativePath"
        }
    } finally {
        $reader.Dispose()
    }
}
Assert-PeMachine 'vmtstudio.exe' 0x8664
Assert-PeMachine 'hlmv_backend.exe' 0x014C
Assert-PeMachine $vtfName 0x8664
Assert-PeMachine "platforms/qwindows$qtSuffix.dll" 0x8664

$binaryVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $packageRoot 'vmtstudio.exe')).ProductVersion
if ($binaryVersion -ne $Version) {
    throw "Packaged executable version '$binaryVersion' does not match requested version '$Version'. Rebuild first."
}
$files = @(Get-ChildItem -LiteralPath $packageRoot -Recurse -File)
foreach ($file in $files) {
    if ($file.Name -match '^(tier0|vstdlib|materialsystem|filesystem_stdio|shaderapidx9)\.dll$') {
        throw "Game engine DLL must come from the user's installation: $($file.Name)"
    }
    if ($Config -eq 'Release' -and ($file.Name -match '^Qt6.*d\.dll$|^qwindowsd\.dll$|^VTFLib\+\+_d\.dll$|^(msvcp\d+|vcruntime\d+|ucrtbase)d\.dll$')) {
        throw "Debug runtime in release package: $($file.FullName)"
    }
}

if ($ArchivePath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ArchivePath).Path)
    try {
        $zipFiles = @{}
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name.EndsWith('/')) { continue }
            if ($zipFiles.ContainsKey($name)) { throw "Duplicate ZIP entry: $name" }
            $zipFiles[$name] = $entry
        }
        if ($zipFiles.Count -ne $files.Count) { throw 'ZIP file count does not match the staged payload.' }
        $packageName = Split-Path -Leaf $packageRoot
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($packageRoot.Length).TrimStart('\', '/').Replace('\', '/')
            $name = "$packageName/$relative"
            if (-not $zipFiles.ContainsKey($name) -or $zipFiles[$name].Length -ne $file.Length) {
                throw "ZIP entry is missing or differs from staged payload: $name"
            }
            $zipStream = $zipFiles[$name].Open()
            $diskStream = $file.OpenRead()
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try {
                $zipHash = [Convert]::ToBase64String($sha.ComputeHash($zipStream))
                $diskHash = [Convert]::ToBase64String($sha.ComputeHash($diskStream))
                if ($zipHash -ne $diskHash) { throw "ZIP content differs from staged payload: $name" }
            } finally {
                $zipStream.Dispose()
                $diskStream.Dispose()
                $sha.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
}
Write-Host "Verified $Config v$Version runtime package ($($files.Count) files)."
