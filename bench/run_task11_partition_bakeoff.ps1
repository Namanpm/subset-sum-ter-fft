param(
    [string]$OutputRoot = "results\partition\task11-final-20260825"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$exe = Join-Path $repo "build-root-v2\Release\ter_root_partition_bench.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Release benchmark executable does not exist: $exe"
}

$output = [System.IO.Path]::GetFullPath((Join-Path $repo $OutputRoot))
if (Test-Path -LiteralPath $output) {
    throw "refusing to overwrite existing campaign directory: $output"
}
New-Item -ItemType Directory -Path $output | Out-Null
$manifest = Join-Path $output "command-manifest.jsonl"
$exeHashBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant()

function Write-Manifest([hashtable]$Entry) {
    Add-Content -LiteralPath $manifest -Encoding utf8 -Value ($Entry | ConvertTo-Json -Compress)
}

function Assert-CampaignChild([string]$Path) {
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $prefix = $output.TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "campaign path escaped output root: $resolved"
    }
}

function Remove-CampaignChild([string]$Path) {
    Assert-CampaignChild $Path
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Invoke-CampaignCell(
    [string]$Label,
    [string[]]$ArgumentList,
    [string]$OutputPath
) {
    Assert-CampaignChild $OutputPath
    $partial = "$OutputPath.partial"
    $fullArguments = @($ArgumentList) + @("--output", $partial)
    $quoted = $fullArguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }
    $command = '& "' + $exe + '" ' + ($quoted -join ' ')
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $diagnosticLines = @(& $exe @fullArguments 2>&1)
    $exitCode = $LASTEXITCODE
    $timer.Stop()
    foreach ($line in $diagnosticLines) {
        Write-Host $line
    }
    $diagnostic = ($diagnosticLines | ForEach-Object { $_.ToString() }) -join "`n"
    Write-Manifest @{
        type = "benchmark"
        label = $Label
        command = $command
        exit_code = $exitCode
        wall_seconds = $timer.Elapsed.TotalSeconds
        diagnostic = $diagnostic
        output = [System.IO.Path]::GetRelativePath($repo, $OutputPath)
    }
    if ($exitCode -ne 0) {
        if (Test-Path -LiteralPath $partial) {
            Remove-Item -LiteralPath $partial -Force
        }
        return $false
    }
    if (-not (Test-Path -LiteralPath $partial -PathType Leaf)) {
        throw "successful benchmark did not create output: $partial"
    }
    Move-Item -LiteralPath $partial -Destination $OutputPath
    return $true
}

Write-Manifest @{
    type = "campaign"
    executable = [System.IO.Path]::GetRelativePath($repo, $exe)
    executable_sha256_before = $exeHashBefore
    seed = 6840143426475650816
    warmup = 5
    repeat = 30
}

$sizes = @(20000, 32496, 64000, 500000, 1000000, 2000000, 5000000)
$threads = "1,2,4,8,16"
$distributions = @("uniform", "hot", "alternating", "sorted", "reverse", "duplicates")
$blocks = @(1024, 2048, 4096)
$syntheticRoot = Join-Path $output "synthetic"
New-Item -ItemType Directory -Path $syntheticRoot | Out-Null

foreach ($size in $sizes) {
    $temporaryN = Join-Path $syntheticRoot ("N-{0}.partial" -f $size)
    $completedN = Join-Path $syntheticRoot ("N-{0}" -f $size)
    New-Item -ItemType Directory -Path $temporaryN | Out-Null
    $nCompleted = $true
    foreach ($backendCase in @("current", "block-1024", "block-2048", "block-4096")) {
        $caseDirectory = Join-Path $temporaryN $backendCase
        New-Item -ItemType Directory -Path $caseDirectory | Out-Null
        foreach ($distribution in $distributions) {
            $backend = if ($backendCase -eq "current") { "current-scatter" } else { "block-inplace" }
            $arguments = @(
                "--backend", $backend,
                "--n", $size.ToString(),
                "--bits", "8",
                "--shift", "0",
                "--threads", $threads,
                "--distribution", $distribution,
                "--seed", "6840143426475650816",
                "--warmup", "5",
                "--repeat", "30"
            )
            if ($backend -eq "block-inplace") {
                $blockBytes = [int]($backendCase.Substring("block-".Length))
                $arguments += @("--block-bytes", $blockBytes.ToString())
            }
            $outputPath = Join-Path $caseDirectory ("{0}.jsonl" -f $distribution)
            $label = "synthetic N=$size backend=$backendCase distribution=$distribution"
            if (-not (Invoke-CampaignCell $label $arguments $outputPath)) {
                $nCompleted = $false
                break
            }
        }
        if (-not $nCompleted) {
            break
        }
    }
    if ($nCompleted) {
        Move-Item -LiteralPath $temporaryN -Destination $completedN
        Write-Manifest @{ type = "synthetic-N"; N = $size; status = "Completed" }
    } else {
        Remove-CampaignChild $temporaryN
        Write-Manifest @{
            type = "synthetic-N"
            N = $size
            status = "Omitted"
            reason = "at least one required backend/distribution command failed; all rows for this N were removed"
        }
    }
}

$capturedTemporary = Join-Path $output "captured-root.partial"
$capturedCompleted = Join-Path $output "captured-root"
New-Item -ItemType Directory -Path $capturedTemporary | Out-Null
$captureCompleted = $true
foreach ($backendCase in @("current", "block-1024", "block-2048", "block-4096")) {
    $backend = if ($backendCase -eq "current") { "current-scatter" } else { "block-inplace" }
    $arguments = @(
        "--dataset", "captured-root",
        "--backend", $backend,
        "--bits", "8",
        "--shift", "0",
        "--threads", $threads,
        "--warmup", "5",
        "--repeat", "30",
        "--root-n", "72",
        "--root-seed", "19797034556212562",
        "--root-mode", "full",
        "--list", "all"
    )
    if ($backend -eq "block-inplace") {
        $blockBytes = [int]($backendCase.Substring("block-".Length))
        $arguments += @("--block-bytes", $blockBytes.ToString())
    }
    $outputPath = Join-Path $capturedTemporary ("{0}.jsonl" -f $backendCase)
    if (-not (Invoke-CampaignCell "captured-root backend=$backendCase" $arguments $outputPath)) {
        $captureCompleted = $false
        break
    }
}
if ($captureCompleted) {
    Move-Item -LiteralPath $capturedTemporary -Destination $capturedCompleted
    Write-Manifest @{ type = "captured-root"; status = "Completed" }
} else {
    Remove-CampaignChild $capturedTemporary
    Write-Manifest @{
        type = "captured-root"
        status = "Omitted"
        reason = "at least one required backend command failed; all captured-root rows were removed"
    }
}

$exeHashAfter = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant()
Write-Manifest @{
    type = "campaign-complete"
    executable_sha256_after = $exeHashAfter
    executable_unchanged = ($exeHashAfter -eq $exeHashBefore)
}
if ($exeHashAfter -ne $exeHashBefore) {
    throw "benchmark executable changed during campaign"
}

$hashManifest = Join-Path $output "data-hashes.jsonl"
Get-ChildItem -LiteralPath $output -Recurse -File -Filter "*.jsonl" |
    Where-Object { $_.FullName -ne $hashManifest } |
    Sort-Object FullName |
    ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        Add-Content -LiteralPath $hashManifest -Encoding utf8 -Value ((@{
            path = [System.IO.Path]::GetRelativePath($repo, $_.FullName)
            sha256 = $hash
        } | ConvertTo-Json -Compress))
    }

Write-Host "Task 11 campaign complete: $output"
