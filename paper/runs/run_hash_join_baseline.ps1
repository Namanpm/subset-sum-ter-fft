# Existing snapshots only. No capture, input conversion, solver termination or whole-root n108 launch.
[CmdletBinding()]
param(
    [ValidateSet(1, 4096)][int]$Microproblems = 4096,
    [ValidateRange(0, 1000)][int]$Warmups = 3,
    [ValidateRange(1, 1000)][int]$Samples = 7,
    [ValidateRange(0, 2147483647)][int]$MaxWitnesses = 1000000,
    [ValidateRange(1, 1048576)][int]$MemoryLimitMiB = 2048,
    [switch]$N108Discovery,
    [switch]$ValidateOnly,
    [switch]$Plant
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-Location -LiteralPath $repo

function Assert-Quiet {
    $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'ter_*' })
    if ($busy.Count -ne 0) {
        throw "Refusing to run: active ter_* process(es): $($busy.ProcessName -join ', '). No process was stopped."
    }
}

# Must precede hashes, provenance gathering and any benchmark work.
Assert-Quiet
$exe = Join-Path $repo 'build-full/Release/ter_hash_join_bench.exe'
$cache = Join-Path $repo 'build-full/CMakeCache.txt'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Missing $exe. Build the native Release ter_hash_join_bench target first."
}
if (-not (Test-Path -LiteralPath $cache -PathType Leaf) -or
    -not (Select-String -LiteralPath $cache -Pattern '^TER_NATIVE_OPT:BOOL=ON$' -Quiet)) {
    throw 'The build-full cache must record TER_NATIVE_OPT:BOOL=ON. This script does not build or reconfigure.'
}

$snapshots = @(
    'results/arwlf/holdout-snapshots/n72_s0x7201_auto.snap',
    'results/arwlf/holdout-snapshots/n72_s0x7203_auto.snap',
    'results/arwlf/holdout-snapshots/n72_s0x7207_auto.snap'
)
if ($N108Discovery) {
    $snapshots = @('results/root-queue-generalization/snapshots/n108_s0x1001_a0.snap')
    $Microproblems = 1
    $Warmups = 0
    $Samples = 1
}
foreach ($snapshot in $snapshots) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $snapshot) -PathType Leaf)) {
        throw "Missing existing input $snapshot. No capture, substitute or conversion will be attempted."
    }
}

$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$out = Join-Path $repo "results/arwlf/hash-join-baseline/$stamp"
New-Item -ItemType Directory -Path $out -ErrorAction Stop | Out-Null
$manifest = Join-Path $out 'command-manifest.jsonl'
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$cacheHash = (Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash
$commit = (& git rev-parse HEAD).Trim()
$dirty = @(& git status --porcelain)
$provenance = [ordered]@{
    created_utc = [DateTime]::UtcNow.ToString('o')
    repository = $repo
    git_commit = $commit
    git_status_porcelain = $dirty
    executable = $exe
    executable_sha256 = $exeHash
    build_cache_sha256 = $cacheHash
    build_config = 'Release'
    native_opt = $true
    machine = $env:COMPUTERNAME
    os = [Environment]::OSVersion.VersionString
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
    protocol = if ($N108Discovery) { 'n108_ONE_CELL_DISCOVERY_NOT_WHOLE_ROOT' } else { 'n72_EXISTING_HOLDOUT_SNAPSHOTS' }
    microproblems = $Microproblems
    warmups = $Warmups
    samples = $Samples
    validate_only = [bool]$ValidateOnly
    source_kind = if ($Plant) { 'AUTHENTIC_PLANTED_TARGETS_NOT_TER_SOLUTIONS' } else { 'AUTHENTIC_FULLTER_UNMODIFIED' }
    timing_boundary = 'sink.reset + backend call; all backend table builds included'
    untimed_cost = 'snapshot load, keys/index, count-only K2_WIDE and hash_join, complete correctness gate, sorting and reporting'
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8

function Write-Manifest([object]$Record) {
    $Record | ConvertTo-Json -Depth 8 -Compress | Add-Content -LiteralPath $manifest -Encoding utf8
}

foreach ($relativeSnapshot in $snapshots) {
    Assert-Quiet
    $snapshot = Join-Path $repo $relativeSnapshot
    $snapshotHash = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash
    $label = [IO.Path]::GetFileNameWithoutExtension($snapshot)
    $csv = Join-Path $out "$label.samples.csv"
    $stdout = Join-Path $out "$label.stdout.csv"
    $stderr = Join-Path $out "$label.stderr.log"
    $arguments = @(
        "--snapshot=$snapshot", "--microproblems=$Microproblems", "--warmups=$Warmups",
        "--samples=$Samples", '--pin=p-core', "--out=$csv", "--max-witnesses=$MaxWitnesses",
        "--memory-limit-mib=$MemoryLimitMiB"
    )
    if ($ValidateOnly) { $arguments += '--validate-only' }
    if ($Plant) { $arguments += '--plant=1' }
    $started = [DateTime]::UtcNow
    Write-Manifest ([ordered]@{
        event = 'start'; time_utc = $started.ToString('o'); label = $label
        executable = $exe; executable_sha256 = $exeHash; arguments = $arguments
        snapshot = $snapshot; snapshot_sha256 = $snapshotHash
        stdout_csv = $stdout; output_csv = $csv; stderr = $stderr
    })
    Write-Host "Running $label; untimed K2_WIDE/count and exactness gates run before matched timing. Output: $out"
    $exitCode = $null
    $errorText = $null
    try {
        Assert-Quiet
        & $exe @arguments 1> $stdout 2> $stderr
        $exitCode = $LASTEXITCODE
    } catch {
        $errorText = $_.Exception.Message
        throw
    } finally {
        Write-Manifest ([ordered]@{
            event = 'end'; time_utc = [DateTime]::UtcNow.ToString('o'); label = $label
            elapsed_seconds = ([DateTime]::UtcNow - $started).TotalSeconds
            exit_code = $exitCode; error = $errorText
            executable_sha256_after = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
            snapshot_sha256_after = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash
        })
    }
    if ($exitCode -ne 0) { throw "Benchmark failed for $label (exit $exitCode); inspect $stderr. No later case was launched." }
    if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $exeHash -or
        (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash -ne $snapshotHash) {
        throw "Input/executable hash changed during $label; output is invalid."
    }
    $rows = @(Import-Csv -LiteralPath $csv)
    $expectedRows = if ($ValidateOnly) { 4 } else { 4 * $Samples }
    if ($rows.Count -ne $expectedRows -or @($rows | Where-Object { $_.exact -ne '1' }).Count -ne 0) {
        throw "Incomplete or invalid CSV for $label. Output must not be treated as a completed campaign."
    }
}
Write-Host "Completed existing-input campaign: $out"
