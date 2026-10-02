<#
.SYNOPSIS
    Orchestrates the cache-tiled exact Q3REPORT publication evidence campaign.

.DESCRIPTION
    Orchestration only.  This script implements no TER mathematics: every number
    it stores is produced by a repository benchmark executable, and every
    derived statistic is produced by bench/analyse_tiled_q3report.py.

    It never deletes a historical result directory, never touches Git state,
    never retries a benchmark to obtain a better timing, and never drops a seed
    whose row was unfavourable.  A benchmark process that exits nonzero is a
    campaign failure, not a skipped point.

    Restartability: each (L, seed-index) paper-scaling cell and each tile-sweep
    seed base is written to its own file under raw/per-process/.  A rerun reuses
    a file that already exists and is non-empty unless -Force is given, so an
    interrupted campaign resumes without reconstructing the matrix by hand.

.PARAMETER DryRun
    Print the intended command matrix and exit without running any benchmark.

.PARAMETER Force
    Re-execute cells whose part files already exist.

.PARAMETER NoAnalyze
    Run benchmarks and hashes but skip the analysis step.

.PARAMETER AnalyzeOnly
    Skip building and benchmarking; regenerate analysis and hashes from the raw
    data already present.
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Force,
    [switch]$NoAnalyze,
    [switch]$AnalyzeOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --------------------------------------------------------------------------
# Declared campaign matrix.  Fixed before any timing was observed.
# --------------------------------------------------------------------------
$RepoRoot     = Split-Path -Parent $PSScriptRoot
$BuildDir     = Join-Path $RepoRoot 'build-full'
$BinDir       = Join-Path $BuildDir 'Release'
$CampaignDir  = Join-Path $RepoRoot 'results/tiled-q3report-final-20260829'
$RawDir       = Join-Path $CampaignDir 'raw'
# NOTE: not named 'parts' - .gitignore has a `parts/` rule that would
# silently exclude every per-process raw output from the package.
$PartsDir     = Join-Path $RawDir 'per-process'
$AnalysisDir  = Join-Path $CampaignDir 'analysis'
$FiguresDir   = Join-Path $CampaignDir 'figures'

# Headline paper-scaling matrix.  32768 is deliberately excluded: it is a
# declared-but-not-executable point and is not needed for the tiling claim.
$MainListSizes  = @(64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384)
$MainSeedIndex  = @(0, 1, 2, 3, 4)
$TileSeedBases  = @(4097, 8195, 12295)
$TileRepetitions = 5
$FocusedTestRegex = 'ter_tiled_q3report_tests|ter_q3report_paper_crossover_tests|ter_merge_tests|ter_packed_tests|ter_full_tests|ter_limitations_tests|ter_tiled_sweep_cli'

$script:CommandLog = New-Object System.Collections.ArrayList

function Write-Step([string]$Message) {
    Write-Host "[campaign] $Message"
}

function Add-CommandRecord {
    param(
        [string]$Purpose,
        [string]$Exe,
        [string[]]$Arguments,
        [string]$Stdout,
        [string]$Stderr,
        [int]$ExitCode,
        [double]$Seconds,
        [bool]$Reused
    )
    $null = $script:CommandLog.Add([ordered]@{
        purpose      = $Purpose
        executable   = $Exe
        arguments    = $Arguments
        stdout_file  = $Stdout
        stderr_file  = $Stderr
        exit_code    = $ExitCode
        elapsed_s    = [math]::Round($Seconds, 3)
        reused_cache = $Reused
    })
}

# Runs one benchmark process, capturing stdout and stderr to separate files.
# A nonzero exit is fatal: no scientific row is ever silently discarded.
function Invoke-Benchmark {
    param(
        [string]$Purpose,
        [string]$Exe,
        [string[]]$Arguments,
        [string]$StdoutPath,
        [string]$StderrPath,
        [switch]$AllowNonZeroExit
    )
    if ((-not $Force) -and (Test-Path $StdoutPath) -and ((Get-Item $StdoutPath).Length -gt 0)) {
        Write-Step "reuse  $Purpose"
        Add-CommandRecord -Purpose $Purpose -Exe $Exe -Arguments $Arguments `
            -Stdout $StdoutPath -Stderr $StderrPath -ExitCode 0 -Seconds 0 -Reused $true
        return
    }
    Write-Step "run    $Purpose"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath
    $sw.Stop()
    $code = $proc.ExitCode
    Add-CommandRecord -Purpose $Purpose -Exe $Exe -Arguments $Arguments `
        -Stdout $StdoutPath -Stderr $StderrPath -ExitCode $code `
        -Seconds $sw.Elapsed.TotalSeconds -Reused $false
    if ($code -ne 0 -and -not $AllowNonZeroExit) {
        throw "benchmark failed ($Purpose): exit $code. stderr: $StderrPath"
    }
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# --------------------------------------------------------------------------
# Dry run: print the matrix, execute nothing.
# --------------------------------------------------------------------------
if ($DryRun) {
    Write-Host "=== intended campaign command matrix ==="
    Write-Host "cmake -S . -B build-full -DTER_NATIVE_OPT=ON"
    Write-Host "cmake --build build-full --config Release --parallel"
    Write-Host "ctest --test-dir build-full -C Release -R `"$FocusedTestRegex`" --output-on-failure"
    Write-Host "ctest --test-dir build-full -C Release --output-on-failure"
    Write-Host "ter_q3report_paper_crossover_bench.exe --anchors-only"
    foreach ($L in $MainListSizes) {
        foreach ($k in $MainSeedIndex) {
            Write-Host "ter_q3report_paper_crossover_bench.exe --extend-16384 --only=$L --seed-index=$k --no-header"
        }
    }
    foreach ($base in $TileSeedBases) {
        Write-Host "ter_tiled_parameter_sweep.exe --seed-base=$base --repetitions=$TileRepetitions"
    }
    Write-Host "ter_full_bench.exe --n=36 --attempt-limit=1 --warmup=1 --repeat=5"
    foreach ($n in @(72, 108, 144)) {
        Write-Host "ter_full_bench.exe --n=$n --preflight"
    }
    Write-Host "ter_tiled_complete_merge_bench.exe --repetitions=5"
    Write-Host "python bench/analyse_tiled_q3report.py --self-test"
    Write-Host "python bench/analyse_tiled_q3report.py --results-dir results/tiled-q3report-final-20260829"
    Write-Host ""
    Write-Host ("paper-scaling rows: {0} list sizes x {1} seeds = {2}" -f `
        $MainListSizes.Count, $MainSeedIndex.Count, ($MainListSizes.Count * $MainSeedIndex.Count))
    Write-Host ("tile-sensitivity rows: {0} seed bases x 9 widths x 7 tiles = {1}" -f `
        $TileSeedBases.Count, ($TileSeedBases.Count * 63))
    exit 0
}

foreach ($dir in @($CampaignDir, $RawDir, $PartsDir, $AnalysisDir, $FiguresDir)) {
    if (-not (Test-Path $dir)) { $null = New-Item -ItemType Directory -Path $dir -Force }
}

Push-Location $RepoRoot
try {
    # ----------------------------------------------------------------------
    # Step 1: build and correctness gates.
    # ----------------------------------------------------------------------
    if (-not $AnalyzeOnly) {
        Write-Step 'configure + build (Release, TER_NATIVE_OPT=ON)'
        & cmake -S . -B build-full -DTER_NATIVE_OPT=ON | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
        & cmake --build build-full --config Release --parallel | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)" }

        Write-Step 'focused correctness tests'
        & ctest --test-dir build-full -C Release -R $FocusedTestRegex --output-on-failure `
            2>&1 | Tee-Object -FilePath (Join-Path $CampaignDir 'ctest-focused.txt') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "focused ctest failed ($LASTEXITCODE)" }

        Write-Step 'full Release test suite'
        & ctest --test-dir build-full -C Release --output-on-failure `
            2>&1 | Tee-Object -FilePath (Join-Path $CampaignDir 'ctest-full.txt') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "full ctest failed ($LASTEXITCODE)" }
    }

    # ----------------------------------------------------------------------
    # Step 2: environment provenance.
    # ----------------------------------------------------------------------
    if (-not $AnalyzeOnly) {
        Write-Step 'capturing environment provenance'
        $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
        $os  = Get-CimInstance Win32_OperatingSystem
        $cs  = Get-CimInstance Win32_ComputerSystem
        $cacheRows = @()
        foreach ($c in (Get-CimInstance Win32_CacheMemory -ErrorAction SilentlyContinue)) {
            $cacheRows += [ordered]@{ level_code = $c.Level; max_cache_size_kb = $c.MaxCacheSize }
        }
        $gitCommit = (& git rev-parse HEAD).Trim()
        $gitShort  = (& git rev-parse --short HEAD).Trim()
        $gitStatus = (& git status --porcelain --untracked-files=no) -join "`n"
        $gitDiffStat = (& git diff --stat) -join "`n"
        $cmakeVersion = ((& cmake --version) | Select-Object -First 1)
        $pyVersion = (& python --version 2>&1 | Out-String).Trim()
        $mplVersion = $null
        try {
            $mplVersion = (& python -c "import matplotlib;print(matplotlib.__version__)" 2>$null | Out-String).Trim()
            if ([string]::IsNullOrWhiteSpace($mplVersion)) { $mplVersion = $null }
        } catch { $mplVersion = $null }

        $cache = Get-Content (Join-Path $BuildDir 'CMakeCache.txt')
        $compilerPath = ($cache | Select-String '^CMAKE_CXX_COMPILER:' | Select-Object -First 1) -replace '.*=', ''
        $generator    = ($cache | Select-String '^CMAKE_GENERATOR:' | Select-Object -First 1) -replace '.*=', ''
        $nativeOpt    = ($cache | Select-String '^TER_NATIVE_OPT:' | Select-Object -First 1) -replace '.*=', ''
        # cl.exe is not on PATH outside a Visual Studio developer shell, so the
        # compiler identity is taken from what CMake actually detected for this
        # build tree rather than from a shell lookup that can silently differ.
        $compilerId = 'unknown'
        $compilerVersion = 'unknown'
        $compilerInfo = Get-ChildItem -Path (Join-Path $BuildDir 'CMakeFiles') -Recurse `
            -Filter 'CMakeCXXCompiler.cmake' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($compilerInfo) {
            $infoText = Get-Content $compilerInfo.FullName
            $idLine  = $infoText | Select-String 'set\(CMAKE_CXX_COMPILER_ID\s+"([^"]+)"' | Select-Object -First 1
            $verLine = $infoText | Select-String 'set\(CMAKE_CXX_COMPILER_VERSION\s+"([^"]+)"' | Select-Object -First 1
            if ($idLine)  { $compilerId = $idLine.Matches[0].Groups[1].Value }
            if ($verLine) { $compilerVersion = $verLine.Matches[0].Groups[1].Value }
        }

        $exeNames = @(
            'ter_q3report_paper_crossover_bench.exe',
            'ter_tiled_parameter_sweep.exe',
            'ter_full_bench.exe',
            'ter_tiled_complete_merge_bench.exe'
        )
        $exeRecords = @()
        foreach ($name in $exeNames) {
            $p = Join-Path $BinDir $name
            $exeRecords += [ordered]@{
                name     = $name
                path     = (Resolve-Path -Relative $p -ErrorAction SilentlyContinue)
                sha256   = (Get-Sha256 $p)
                size_bytes = if (Test-Path $p) { (Get-Item $p).Length } else { $null }
            }
        }

        $env = [ordered]@{
            timestamp_local        = (Get-Date).ToString('o')
            timestamp_utc          = (Get-Date).ToUniversalTime().ToString('o')
            timezone               = (Get-TimeZone).Id
            git_commit             = $gitCommit
            git_short_commit       = $gitShort
            working_tree_dirty     = -not [string]::IsNullOrWhiteSpace($gitStatus)
            git_status_porcelain   = $gitStatus
            git_diff_stat          = $gitDiffStat
            os_caption             = $os.Caption
            os_version             = $os.Version
            cpu_model              = $cpu.Name
            cpu_physical_cores     = $cpu.NumberOfCores
            cpu_logical_processors = $cpu.NumberOfLogicalProcessors
            cpu_max_clock_mhz      = $cpu.MaxClockSpeed
            # Win32_Processor reports L2/L3 as machine-wide aggregates, not the
            # per-core private capacity.  Stored verbatim and labelled as such;
            # no per-core figure is invented.
            l2_cache_kb_os_aggregate = $cpu.L2CacheSize
            l3_cache_kb_os_aggregate = $cpu.L3CacheSize
            win32_cache_memory       = $cacheRows
            installed_ram_bytes    = $cs.TotalPhysicalMemory
            compiler_identity      = $compilerId
            compiler_path          = $compilerPath
            compiler_version       = $compilerVersion
            cmake_version          = $cmakeVersion
            cmake_generator        = $generator
            build_configuration    = 'Release'
            ter_native_opt         = $nativeOpt
            benchmark_thread_count = 1
            benchmark_executables  = $exeRecords
            python_version         = $pyVersion
            matplotlib_version     = $mplVersion
            matplotlib_available   = ($null -ne $mplVersion)
            hardware_cache_counters_collected = $false
            hardware_cache_counter_reason = 'No already-installed hardware performance-counter tool was available, and the campaign is not permitted to install profiling drivers or services.'
        }
        $env | ConvertTo-Json -Depth 6 |
            Set-Content -Path (Join-Path $CampaignDir 'environment.json') -Encoding UTF8
    }

    # ----------------------------------------------------------------------
    # Step 3: authentic FullTer anchors.
    # ----------------------------------------------------------------------
    if (-not $AnalyzeOnly) {
        Invoke-Benchmark -Purpose 'authentic FullTer anchors' `
            -Exe (Join-Path $BinDir 'ter_q3report_paper_crossover_bench.exe') `
            -Arguments @('--anchors-only') `
            -StdoutPath (Join-Path $RawDir 'authentic-anchors.csv') `
            -StderrPath (Join-Path $RawDir 'authentic-anchors.stderr')

        # ------------------------------------------------------------------
        # Step 4: the 45-row paper-scaling matrix, one process per cell.
        # ------------------------------------------------------------------
        $mainStderr = Join-Path $RawDir 'paper-crossover-tiled.stderr'
        $mainCsv    = Join-Path $RawDir 'paper-crossover-tiled.csv'
        $headerPart = Join-Path $PartsDir 'paper-crossover-header.csv'
        if ($Force -or -not (Test-Path $headerPart)) {
            # The header is emitted once, from the binary itself, by running the
            # first cell with the header enabled.
            Invoke-Benchmark -Purpose 'paper-scaling header + L=64 seed-index=0' `
                -Exe (Join-Path $BinDir 'ter_q3report_paper_crossover_bench.exe') `
                -Arguments @('--extend-16384', '--only=64', '--seed-index=0') `
                -StdoutPath $headerPart `
                -StderrPath (Join-Path $PartsDir 'paper-crossover-header.stderr')
        } else {
            Write-Step 'reuse  paper-scaling header + L=64 seed-index=0'
        }

        foreach ($L in $MainListSizes) {
            foreach ($k in $MainSeedIndex) {
                if ($L -eq 64 -and $k -eq 0) { continue }  # carried by the header part
                $stdout = Join-Path $PartsDir ("paper-crossover-L{0}-s{1}.csv" -f $L, $k)
                $stderr = Join-Path $PartsDir ("paper-crossover-L{0}-s{1}.stderr" -f $L, $k)
                Invoke-Benchmark -Purpose ("paper-scaling L={0} seed-index={1}" -f $L, $k) `
                    -Exe (Join-Path $BinDir 'ter_q3report_paper_crossover_bench.exe') `
                    -Arguments @('--extend-16384', "--only=$L", "--seed-index=$k", '--no-header') `
                    -StdoutPath $stdout -StderrPath $stderr
            }
        }

        # Concatenate the per-process parts in declared matrix order.  No value
        # is edited; only whole files are joined.
        Write-Step 'assembling paper-crossover-tiled.csv from per-process parts'
        $mainLines = [System.Collections.ArrayList]::new()
        $stderrLines = [System.Collections.ArrayList]::new()
        foreach ($line in (Get-Content $headerPart)) { $null = $mainLines.Add($line) }
        $null = $stderrLines.Add('=== paper-scaling header + L=64 seed-index=0 ===')
        foreach ($line in (Get-Content (Join-Path $PartsDir 'paper-crossover-header.stderr'))) {
            $null = $stderrLines.Add($line)
        }
        foreach ($L in $MainListSizes) {
            foreach ($k in $MainSeedIndex) {
                if ($L -eq 64 -and $k -eq 0) { continue }
                $stdout = Join-Path $PartsDir ("paper-crossover-L{0}-s{1}.csv" -f $L, $k)
                $stderr = Join-Path $PartsDir ("paper-crossover-L{0}-s{1}.stderr" -f $L, $k)
                foreach ($line in (Get-Content $stdout)) {
                    if ($line.Trim().Length -gt 0) { $null = $mainLines.Add($line) }
                }
                $null = $stderrLines.Add(("=== L={0} seed-index={1} ===" -f $L, $k))
                foreach ($line in (Get-Content $stderr)) { $null = $stderrLines.Add($line) }
            }
        }
        Set-Content -Path $mainCsv -Value $mainLines -Encoding UTF8
        Set-Content -Path $mainStderr -Value $stderrLines -Encoding UTF8

        # ------------------------------------------------------------------
        # Step 5: tile-sensitivity campaign, three independent seed bases.
        # ------------------------------------------------------------------
        foreach ($base in $TileSeedBases) {
            Invoke-Benchmark -Purpose ("tile sweep seed-base={0} repetitions={1}" -f $base, $TileRepetitions) `
                -Exe (Join-Path $BinDir 'ter_tiled_parameter_sweep.exe') `
                -Arguments @("--seed-base=$base", "--repetitions=$TileRepetitions") `
                -StdoutPath (Join-Path $RawDir ("tile-sweep-seed-{0}.csv" -f $base)) `
                -StderrPath (Join-Path $RawDir ("tile-sweep-seed-{0}.stderr" -f $base))
        }

        # ------------------------------------------------------------------
        # Step 6: FullTer integration evidence and applicability preflights.
        # ------------------------------------------------------------------
        $fullterCsv    = Join-Path $RawDir 'fullter-integration.csv'
        $fullterStderr = Join-Path $RawDir 'fullter-integration.stderr'
        $fullterParts  = @()
        $integrationRuns = @(
            @{ purpose = 'FullTer n=36 attempt-cost'; args = @('--n=36', '--attempt-limit=1', '--warmup=1', '--repeat=5') },
            @{ purpose = 'FullTer n=72 preflight';    args = @('--n=72', '--preflight') },
            @{ purpose = 'FullTer n=108 preflight';   args = @('--n=108', '--preflight') },
            @{ purpose = 'FullTer n=144 preflight';   args = @('--n=144', '--preflight') }
        )
        foreach ($run in $integrationRuns) {
            $tag = ($run.purpose -replace '[^A-Za-z0-9]+', '-').ToLowerInvariant()
            $stdout = Join-Path $PartsDir ("{0}.txt" -f $tag)
            $stderr = Join-Path $PartsDir ("{0}.stderr" -f $tag)
            # A preflight legitimately exits nonzero on an unsupported dimension;
            # its exit code is retained in commands.json either way.
            Invoke-Benchmark -Purpose $run.purpose `
                -Exe (Join-Path $BinDir 'ter_full_bench.exe') -Arguments $run.args `
                -StdoutPath $stdout -StderrPath $stderr -AllowNonZeroExit
            $fullterParts += @{ tag = $run.purpose; stdout = $stdout; stderr = $stderr }
        }
        $fullterLines = [System.Collections.ArrayList]::new()
        $fullterErr   = [System.Collections.ArrayList]::new()
        foreach ($part in $fullterParts) {
            $null = $fullterLines.Add(("=== {0} ===" -f $part.tag))
            foreach ($line in (Get-Content $part.stdout)) { $null = $fullterLines.Add($line) }
            $null = $fullterErr.Add(("=== {0} ===" -f $part.tag))
            foreach ($line in (Get-Content $part.stderr)) { $null = $fullterErr.Add($line) }
        }
        Set-Content -Path $fullterCsv -Value $fullterLines -Encoding UTF8
        Set-Content -Path $fullterStderr -Value $fullterErr -Encoding UTF8

        # ------------------------------------------------------------------
        # Step 7: complete-merge engineering comparison (Section 16 Case B).
        # ------------------------------------------------------------------
        Invoke-Benchmark -Purpose 'complete merge: pair-materializing public baseline vs tiled public merge' `
            -Exe (Join-Path $BinDir 'ter_tiled_complete_merge_bench.exe') `
            -Arguments @('--repetitions=5') `
            -StdoutPath (Join-Path $RawDir 'complete-merge.csv') `
            -StderrPath (Join-Path $RawDir 'complete-merge.stderr')

    }

    # ----------------------------------------------------------------------
    # Step 8: analysis, figures and hashes.
    # ----------------------------------------------------------------------
    if (-not $NoAnalyze) {
        Write-Step 'analyser self-test'
        & python (Join-Path $RepoRoot 'bench/analyse_tiled_q3report.py') --self-test
        if ($LASTEXITCODE -ne 0) { throw "analyser self-test failed ($LASTEXITCODE)" }

        Write-Step 'analysing raw data'
        & python (Join-Path $RepoRoot 'bench/analyse_tiled_q3report.py') --results-dir $CampaignDir
        if ($LASTEXITCODE -ne 0) { throw "analysis failed ($LASTEXITCODE)" }
    }

    Write-Step 'hashing raw and analysis artifacts'
    $hashes = [ordered]@{}
    foreach ($dir in @($RawDir, $AnalysisDir, $FiguresDir)) {
        foreach ($file in (Get-ChildItem -Path $dir -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
            $rel = [System.IO.Path]::GetRelativePath($CampaignDir, $file.FullName) -replace '\\', '/'
            $hashes[$rel] = [ordered]@{
                sha256     = (Get-Sha256 $file.FullName)
                size_bytes = $file.Length
            }
        }
    }
    $hashes | ConvertTo-Json -Depth 4 |
        Set-Content -Path (Join-Path $CampaignDir 'data-hashes.json') -Encoding UTF8

    # ----------------------------------------------------------------------
    # Completion summary.
    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host '=== campaign summary ==='
    $mainCsvPath = Join-Path $RawDir 'paper-crossover-tiled.csv'
    if (Test-Path $mainCsvPath) {
        $rows = (Get-Content $mainCsvPath).Count - 1
        Write-Host ("paper-scaling rows: {0} (expected {1})" -f $rows, ($MainListSizes.Count * $MainSeedIndex.Count))
    }
    foreach ($base in $TileSeedBases) {
        $p = Join-Path $RawDir ("tile-sweep-seed-{0}.csv" -f $base)
        if (Test-Path $p) {
            Write-Host ("tile sweep seed-base={0}: {1} rows (expected 63)" -f $base, ((Get-Content $p).Count - 1))
        }
    }
    $checks = Join-Path $AnalysisDir 'claim-checks.json'
    if (Test-Path $checks) {
        $parsed = Get-Content $checks -Raw | ConvertFrom-Json
        Write-Host ("evidence_package_complete: {0}" -f $parsed.evidence_package_complete)
        Write-Host ("ready_for_manuscript_drafting: {0}" -f $parsed.ready_for_manuscript_drafting)
    }
    Write-Host ("artifacts: {0}" -f $CampaignDir)
}
finally {
    # Written unconditionally: a campaign that fails part way through still has
    # to record exactly which commands ran and what they exited with.
    if ($script:CommandLog.Count -gt 0) {
        $script:CommandLog | ConvertTo-Json -Depth 6 |
            Set-Content -Path (Join-Path $CampaignDir 'commands.json') -Encoding UTF8
    }
    Pop-Location
}
