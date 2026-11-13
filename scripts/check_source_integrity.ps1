<#
.SYNOPSIS
    Checks source/resource wiring without configuring or compiling the project.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$checkedSources = 0

# Direct CMake source entries must resolve. Entries with CMake variables and
# generated files are deliberately left to the actual configure/build checks.
Get-ChildItem -LiteralPath $repoRoot -Directory | Where-Object {
    $_.Name -in @('ui', 'vmt_core', 'vtf_reader', 'engine_bridge', 'game_profiles', 'renderer', 'tests')
} | ForEach-Object {
    $moduleRoot = $_.FullName
    $cmakeFile = Join-Path $moduleRoot 'CMakeLists.txt'
    foreach ($line in (Get-Content -LiteralPath $cmakeFile)) {
        if ($line -match '^\s+([A-Za-z0-9_./-]+\.(?:cpp|h))\s*$') {
            $sourcePath = Join-Path $moduleRoot $Matches[1]
            if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                throw "Missing CMake source: $sourcePath"
            }
            $checkedSources++
        }
    }
}

$testCmake = Get-Content -LiteralPath (Join-Path $repoRoot 'tests/CMakeLists.txt') -Raw
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'tests') -Filter '*tests.cpp' -File | ForEach-Object {
    if ($testCmake -notmatch ('(?m)^\s+' + [regex]::Escape($_.Name) + '\s*$')) {
        throw "Test source is not registered in CMake: $($_.Name)"
    }
}

$qrcPath = Join-Path $repoRoot 'ui/resources/vmtstudio.qrc'
[xml]$resourceDocument = Get-Content -LiteralPath $qrcPath -Raw
$resourceRoot = Split-Path -Parent $qrcPath
$schemaResources = @()
$seenResourceNames = @{}
$seenShaders = @{}
foreach ($group in @($resourceDocument.RCC.qresource)) {
    foreach ($entry in @($group.file)) {
        $relativePath = $entry.InnerText
        $resourceFile = [System.IO.Path]::GetFullPath((Join-Path $resourceRoot $relativePath))
        if (-not (Test-Path -LiteralPath $resourceFile -PathType Leaf)) {
            throw "Missing Qt resource: $resourceFile"
        }
        $resourceName = [string]$group.prefix + '/' + [string]$entry.alias
        if ($seenResourceNames.ContainsKey($resourceName)) {
            throw "Duplicate Qt resource name: $resourceName"
        }
        $seenResourceNames[$resourceName] = $true
        if ($group.prefix -eq '/schemas') {
            $schema = Get-Content -LiteralPath $resourceFile -Raw | ConvertFrom-Json
            $shaderName = [string]$schema.shader
            if ([string]::IsNullOrWhiteSpace($shaderName) -or $seenShaders.ContainsKey($shaderName)) {
                throw "Missing or duplicate shader name in resource: $resourceFile"
            }
            $seenShaders[$shaderName] = $true
            $schemaResources += $resourceFile
        }
    }
}

$schemaFiles = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'data/schema') -Filter '*.json' -File)
if ($schemaResources.Count -ne $schemaFiles.Count) {
    throw 'The bundled shader resource count differs from data/schema.'
}
foreach ($schemaFile in $schemaFiles) {
    if ($schemaFile.FullName -notin $schemaResources) {
        throw "Shader schema is not bundled: $($schemaFile.Name)"
    }
}

Write-Output "PASS: $checkedSources CMake source entries exist; all test sources are registered."
Write-Output "PASS: $($seenResourceNames.Count) Qt resources exist, including all $($schemaFiles.Count) shader schemas."
Write-Output 'No configuration, compilation, packaging or C++ test execution was performed.'
