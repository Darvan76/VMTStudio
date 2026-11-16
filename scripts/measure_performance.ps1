<#
.SYNOPSIS
Measures real core/Qt paths with deterministic large fixtures in fresh processes.
.EXAMPLE
cmake --build build64 --config Release --target performance_benchmarks
powershell -NoProfile -File scripts/measure_performance.ps1 -OutputDirectory scratch/perf05-baseline
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(3, 100)][int]$Samples = 15,
    [string]$OutputDirectory = 'scratch/performance',
    [string]$BinDirectory = '',
    [string]$GameDirectory = ''
)

$ErrorActionPreference = 'Stop'
if ([bool]$BinDirectory -ne [bool]$GameDirectory) { throw 'Provide both BinDirectory and GameDirectory.' }
$repoPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$benchmarkPath = Join-Path $repoPath "build64\bin\$Configuration\performance_benchmarks.exe"
if (-not (Test-Path -LiteralPath $benchmarkPath -PathType Leaf)) {
    throw "Build performance_benchmarks in $Configuration first."
}
$outputPath = [IO.Path]::GetFullPath((Join-Path $repoPath $OutputDirectory))
if (Test-Path -LiteralPath (Join-Path $outputPath 'manifest.json')) {
    throw 'This output directory already contains a run; choose a new OutputDirectory to preserve its evidence.'
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$fixtureTemp = [IO.Path]::GetFullPath((Join-Path $repoPath 'scratch\performance-temp'))
if (-not $fixtureTemp.StartsWith($repoPath + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture directory must remain inside the workspace.'
}
New-Item -ItemType Directory -Path $fixtureTemp -Force | Out-Null

$priorTemp = $env:TEMP
$priorTmp = $env:TMP
$priorPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
try {
    $env:TEMP = $fixtureTemp
    $env:TMP = $fixtureTemp
    $qtEntry = Select-String -LiteralPath (Join-Path $repoPath 'build64\CMakeCache.txt') -Pattern '^Qt6Core_DIR:PATH=(.+)$'
    if ($qtEntry) {
        $qtPrefix = [IO.Path]::GetFullPath((Join-Path $qtEntry.Matches[0].Groups[1].Value '..\..\..'))
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtPrefix 'plugins\platforms'
    }
    $sourceHashes = [ordered]@{}
    foreach ($source in @('tests/performance_benchmarks.cpp', 'ui/src/qt/asset_list_model.cpp',
        'ui/src/qt/asset_preview_panel.cpp', 'ui/src/qt/asset_thumbnail_service.cpp',
        'ui/src/qt/inspector_widget.cpp', 'ui/src/qt/app_updater.cpp',
        'ui/src/asset_index.cpp', 'ui/src/session.cpp', 'vtf_reader/src/vtf_reader.cpp',
        'engine_bridge/src/engine_view.cpp', 'hlmv_backend/src/backend_query.cpp')) {
        $sourceHashes[$source] = (Get-FileHash -LiteralPath (Join-Path $repoPath $source) -Algorithm SHA256).Hash
    }
    $manifest = [ordered]@{
        completed = $false
        recorded_utc = [DateTime]::UtcNow.ToString('o')
        configuration = $Configuration
        samples = $Samples
        benchmark_sha256 = (Get-FileHash -LiteralPath $benchmarkPath -Algorithm SHA256).Hash
        backend_sha256 = (Get-FileHash -LiteralPath (Join-Path $repoPath "build64\bin\$Configuration\hlmv_backend.exe") -Algorithm SHA256).Hash
        source_sha256 = $sourceHashes
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputPath 'manifest.json') -Encoding UTF8

    $workloads = @(@('browser', 10000), @('browser', 50000), @('browser', 150000),
        @('editor', 64), @('editor', 512), @('editor', 2048), @('texture', 4096))
    if ($BinDirectory) { $workloads += ,@('engine', $BinDirectory, $GameDirectory) }
    foreach ($workload in $workloads) {
        $scenario = $workload[0]
        $size = $workload[1]
        $reportName = if ($scenario -eq 'engine') { 'engine' } else { "$scenario-$size" }
        $jsonPath = Join-Path $outputPath "$reportName.json"
        $logPath = Join-Path $outputPath "$reportName.log"
        # A fresh process per workload keeps cumulative peak memory meaningful.
        $nativeArguments = @($scenario) + @($workload[1..($workload.Count - 1)]) + @($Samples)
        $reportText = & $benchmarkPath @nativeArguments 2> $logPath
        if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $scenario $size (see $logPath)" }
        $reportText | Set-Content -LiteralPath $jsonPath -Encoding UTF8
        $report = $reportText | ConvertFrom-Json
        Write-Output ("{0} {1}: {2} measurements saved to {3}" -f $scenario, $size, $report.measurements.Count, $jsonPath)
    }
    $manifest.completed = $true
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputPath 'manifest.json') -Encoding UTF8
} finally {
    $env:TEMP = $priorTemp
    $env:TMP = $priorTmp
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $priorPlatformPath
}
