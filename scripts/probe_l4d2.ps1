<#
.SYNOPSIS
Inspects an installed Left 4 Dead 2 and its tools without loading game DLLs.
.DESCRIPTION
Reads Steam manifests, PE headers, interface-version strings and gameinfo.txt.
Interface strings are evidence only: they do not prove CreateInterface support
or binary compatibility. The script never starts the game or modifies Steam.
#>
[CmdletBinding()]
param(
    [string]$GameRoot,
    [string]$OutputPath,
    [string]$SdkPublic,
    [switch]$IncludeHashes
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$maxBinaryScanBytes = 16 * 1024 * 1024

function Read-Text([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [IO.File]::ReadAllText($Path)
    }
    return ''
}

function Get-KvValue([string]$Text, [string]$Key) {
    $pattern = '(?im)^\s*"?' + [regex]::Escape($Key) + '"?\s+(?:"([^"\r\n]*)"|([^\s{}"/]+))'
    $match = [regex]::Match($Text, $pattern)
    if (!$match.Success) { return '' }
    if ($match.Groups[1].Success) { return $match.Groups[1].Value }
    return $match.Groups[2].Value
}

function Get-SteamLibraries {
    $roots = New-Object 'System.Collections.Generic.List[string]'
    if ($env:STEAMPATH) { $roots.Add($env:STEAMPATH) }
    try {
        $registry = Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction Stop
        if ($registry.SteamPath) { $roots.Add($registry.SteamPath) }
    } catch { }
    foreach ($programFiles in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($programFiles) { $roots.Add((Join-Path $programFiles 'Steam')) }
    }
    $libraries = New-Object 'System.Collections.Generic.List[string]'
    foreach ($root in @($roots | Select-Object -Unique)) {
        if (!(Test-Path -LiteralPath $root -PathType Container)) { continue }
        $libraries.Add([IO.Path]::GetFullPath($root))
        $text = Read-Text (Join-Path $root 'steamapps\libraryfolders.vdf')
        foreach ($match in [regex]::Matches($text, '(?im)^\s*"(?:path|\d+)"\s+"([^"\r\n]+)"')) {
            $path = $match.Groups[1].Value.Replace('\\', '\')
            if (Test-Path -LiteralPath $path -PathType Container) {
                $libraries.Add([IO.Path]::GetFullPath($path))
            }
        }
    }
    return @($libraries | Select-Object -Unique)
}

function Get-Manifest([string]$Library, [int]$AppId) {
    $path = Join-Path $Library ('steamapps\appmanifest_' + $AppId + '.acf')
    $text = Read-Text $path
    if (!$text) { return $null }
    return [ordered]@{
        app_id = $AppId
        manifest = $path
        name = Get-KvValue $text 'name'
        install_dir = Get-KvValue $text 'installdir'
        build_id = Get-KvValue $text 'buildid'
        state_flags = Get-KvValue $text 'StateFlags'
    }
}

function Get-BinaryEvidence([string]$Path, [bool]$Required) {
    $result = [ordered]@{
        path = $Path; required = $Required; exists = $false; bytes = 0
        pe_valid = $false; machine = $null; architecture = $null
        interface_strings = @(); sha256 = $null; error = $null
    }
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $result }
    $result.exists = $true
    try {
        $result.bytes = (Get-Item -LiteralPath $Path).Length
        if ($IncludeHashes) { $result.sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
        if ($result.bytes -gt $maxBinaryScanBytes) {
            throw 'Binary exceeds the 16 MiB PE/interface inspection limit; scan skipped.'
        }
        $bytes = [IO.File]::ReadAllBytes($Path)
        if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
            throw 'Missing DOS/PE header.'
        }
        $pe = [BitConverter]::ToInt32($bytes, 60)
        if ($pe -lt 0 -or $pe -gt $bytes.Length - 24 -or
            [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) {
            throw 'Invalid PE signature or offset.'
        }
        $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
        $result.machine = ('0x{0:X4}' -f $machine)
        $result.architecture = switch ($machine) { 0x014c { 'x86' } 0x8664 { 'x64' } default { 'unknown' } }
        $result.pe_valid = $true
        $ascii = [Text.Encoding]::ASCII.GetString($bytes)
        $pattern = '(?<![A-Za-z0-9_])(?:VFileSystem|VMaterialSystem|VStudioRender|MDLCache|VDataCache|VEngineCvar|MaterialSystemHardwareConfig|VMaterialSystemConfig|ShaderApi|VPhysics|VPhysicsCollision|VPhysicsSurfaceProps|StudioDataCache)\d{3}(?!\d)'
        $result.interface_strings = @([regex]::Matches($ascii, $pattern) |
            ForEach-Object { $_.Value } | Sort-Object -Unique)
    } catch { $result.error = $_.Exception.Message }
    return $result
}

function Get-SearchPaths([string]$Text, [string]$Root, [string]$GameDir) {
    $inPaths = $false
    $order = 0
    foreach ($line in ($Text -split '\r?\n')) {
        $clean = ($line -replace '//.*$', '').Trim()
        if (!$inPaths) {
            if ($clean -match '^"?SearchPaths"?\s*\{?$') { $inPaths = $true }
            continue
        }
        if ($clean -eq '}') { break }
        $match = [regex]::Match($clean, '^(?:"([^"\r\n]+)"|([^\s{}"]+))\s+(?:"([^"\r\n]*)"|([^\s{}"]+))')
        if (!$match.Success) { continue }
        $id = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
        $location = if ($match.Groups[3].Success) { $match.Groups[3].Value } else { $match.Groups[4].Value }
        $resolved = $location
        if ($location.StartsWith('|gameinfo_path|', [StringComparison]::OrdinalIgnoreCase)) {
            $resolved = Join-Path $GameDir $location.Substring(15)
        } elseif (![IO.Path]::IsPathRooted($location) -and !$location.Contains('|')) {
            $resolved = Join-Path $Root $location
        }
        if (!$resolved.Contains('*') -and !$resolved.Contains('|')) {
            $resolved = [IO.Path]::GetFullPath($resolved)
        }
        [ordered]@{
            order = $order; path_id = $id; location = $location
            resolved = $resolved; exists = (Test-Path -LiteralPath $resolved)
        }
        $order++
    }
}

function Get-SdkDefinition([string]$RelativePath, [string]$Macro) {
    if (!$SdkPublic) { return $null }
    $text = Read-Text (Join-Path $SdkPublic $RelativePath)
    $match = [regex]::Match($text, '(?m)^\s*#define\s+' + [regex]::Escape($Macro) + '\s+"([^"]+)"')
    if ($match.Success) { return $match.Groups[1].Value }
    return $null
}

$libraries = @(Get-SteamLibraries)
$manifests = @()
foreach ($library in $libraries) {
    foreach ($appId in @(550, 563)) {
        $manifest = Get-Manifest $library $appId
        if ($null -ne $manifest) { $manifests += $manifest }
        if (!$GameRoot -and $null -ne $manifest -and $appId -eq 550) {
            $GameRoot = Join-Path (Join-Path $library 'steamapps\common') $manifest.install_dir
        }
    }
}
if (!$GameRoot) { throw 'Left 4 Dead 2 was not found in Steam libraries; pass -GameRoot.' }
$GameRoot = [IO.Path]::GetFullPath($GameRoot)
$gameDir = Join-Path $GameRoot 'left4dead2'
$gameInfoPath = Join-Path $gameDir 'gameinfo.txt'
$gameInfo = Read-Text $gameInfoPath
if (!$gameInfo) { throw ('gameinfo.txt not found: ' + $gameInfoPath) }

if (!$SdkPublic -and $env:SOURCE_SDK_PUBLIC) { $SdkPublic = $env:SOURCE_SDK_PUBLIC }
if (!$SdkPublic) {
    foreach ($cache in @('build\CMakeCache.txt', 'build64\CMakeCache.txt')) {
        $match = [regex]::Match((Read-Text (Join-Path $workspace $cache)), '(?m)^SOURCE_SDK_PUBLIC:PATH=(.+)\r?$')
        if ($match.Success) { $SdkPublic = $match.Groups[1].Value.Trim(); break }
    }
}
if (!$SdkPublic) {
    $candidate = Join-Path $workspace 'thirdparty\source-sdk-2013\src\public'
    if (Test-Path -LiteralPath $candidate -PathType Container) { $SdkPublic = $candidate }
}

$binaryNames = @('tier0.dll', 'vstdlib.dll', 'filesystem_stdio.dll', 'materialsystem.dll',
    'shaderapidx9.dll', 'studiorender.dll', 'datacache.dll', 'vphysics.dll', 'stdshader_dx9.dll', 'hlmv.exe')
$binaries = @()
foreach ($name in $binaryNames) {
    $binaries += Get-BinaryEvidence (Join-Path (Join-Path $GameRoot 'bin') $name) ($name -ne 'hlmv.exe')
}
$checks = @(
    @('filesystem_stdio.dll', 'filesystem.h', 'FILESYSTEM_INTERFACE_VERSION', 'VFileSystem'),
    @('materialsystem.dll', 'materialsystem\imaterialsystem.h', 'MATERIAL_SYSTEM_INTERFACE_VERSION', 'VMaterialSystem'),
    @('studiorender.dll', 'istudiorender.h', 'STUDIO_RENDER_INTERFACE_VERSION', 'VStudioRender'),
    @('materialsystem.dll', 'materialsystem\materialsystem_config.h', 'MATERIALSYSTEM_CONFIG_VERSION', 'VMaterialSystemConfig'),
    @('vstdlib.dll', 'icvar.h', 'CVAR_INTERFACE_VERSION', 'VEngineCvar')
)
$comparison = @()
foreach ($check in $checks) {
    $binary = @($binaries | Where-Object { [IO.Path]::GetFileName($_.path) -eq $check[0] })[0]
    $expected = Get-SdkDefinition $check[1] $check[2]
    $observed = @($binary.interface_strings | Where-Object { $_ -match ('^' + $check[3] + '\d{3}$') })
    $comparison += [ordered]@{
        binary = $check[0]; sdk_macro = $check[2]; sdk_header = $check[1]
        sdk_expected = $expected; observed_strings = $observed
        expected_string_present = ($null -ne $expected -and $observed -contains $expected)
    }
}
$report = [ordered]@{
    schema_version = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    game_root = $GameRoot
    steam_libraries = $libraries
    steam_manifests = $manifests
    steam_app_id = Get-KvValue $gameInfo 'SteamAppId'
    tools_app_id = Get-KvValue $gameInfo 'ToolsAppId'
    gameinfo = $gameInfoPath
    search_paths = @(Get-SearchPaths $gameInfo $GameRoot $gameDir)
    binaries = $binaries
    sdk_public = $SdkPublic
    sdk_comparison = $comparison
    required_binaries_are_x86 = (@($binaries | Where-Object { $_.required -and (!$_.pe_valid -or $_.architecture -ne 'x86') }).Count -eq 0)
    native_preview_verified = $false
    max_binary_scan_bytes = $maxBinaryScanBytes
    evidence_limit = 'PE headers and strings only; no DLL was loaded, no CreateInterface was called, and no rendering or ABI compatibility was verified. A matching version string does not establish compatible vtables, signatures or structures.'
}
$json = $report | ConvertTo-Json -Depth 10
if ($OutputPath) {
    $destination = [IO.Path]::GetFullPath($OutputPath)
    $steamRoots = @($libraries) + @($GameRoot)
    foreach ($steamRoot in $steamRoots) {
        $prefix = [IO.Path]::GetFullPath($steamRoot).TrimEnd('\', '/')
        if ($destination.Equals($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            $destination.StartsWith($prefix + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'OutputPath must be outside Steam installations and game libraries.'
        }
    }
    if (!$destination.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputPath must be inside this workspace.'
    }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    [IO.File]::WriteAllText($destination, $json, (New-Object Text.UTF8Encoding($false)))
}
$json
