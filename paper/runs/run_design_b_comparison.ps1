# Design B (the classical solver's production root) against the frozen root backends on identical
# dimension-72 attempts.  Run ONLY on a quiet machine, after the n=108 solve has finished or stopped.
#
#   pwsh -NoProfile -File paper/runs/run_design_b_comparison.ps1
#
# For each (seed, attempt) the same instance and attempt are used by both sides:
#   classic : ter_full_bench --mode=attempt-cost --seed=<instance seed> --attempt-limit=<attempt+1>, 3 warmups + 7 timed.
#             The bench builds the instance from --seed, so the seed must be the instance seed, not seed + attempt
#             (the first version of this script did that and timed a different instance for attempts > 0).
#             Each solve runs attempts 0..attempt; l1_to_l0_ns is the median root time of the terminal attempt
#             (design B, streaming tiled merge). The script checks that the terminal attempt index, attempt seed,
#             status and level-one list sizes equal the staged capture before accepting the row.
#             ter_full_bench prints one row whose stage times are medians of the timed solves (no min/max).
#   staged  : ter_root_snapshot_capture once per attempt, then ter_arwlf_solve_attempt, 7 runs per backend,
#             P-core pinned; root_ms is the backend pass (the campaign's timed region: sink reset + call).
# Stage boundaries differ (the classic stage includes its own root-list preparation and authority checks;
# the staged root_ms excludes snapshot load and key/index preparation), so report both as measured and
# state the boundaries; do not present the ratio as a like-for-like kernel comparison.
param(
    [string]$Out = "",
    [int]$Repeat = 7,
    [int]$Warmup = 3
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))   # repository root

$bench = "build-full/Release/ter_full_bench.exe"
$capture = "build-full/Release/ter_root_snapshot_capture.exe"
$solve = "build-full/Release/ter_arwlf_solve_attempt.exe"
foreach ($exe in @($bench, $capture, $solve)) { if (-not (Test-Path $exe)) { throw "missing $exe (build Release first)" } }
$busy = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "ter_*" }
if ($busy) { throw "Refusing to run: $($busy[0].ProcessName) is running; wait for it to finish." }

$cases = @(@{ seed = 0x7201; attempt = 0 }, @{ seed = 0x7201; attempt = 10 }, @{ seed = 0x7201; attempt = 21 }, @{ seed = 0x7203; attempt = 0 })
$backends = @("k2", "k2_wide", "arwlf", "dispatch")
# Never write into an existing record: the recorded runs are design-b-n72-compare-v2 (valid) and
# history/design-b-n72-compare (invalid first run).
if ($Out -eq "") { $Out = "results/arwlf/design-b-n72-compare-" + (Get-Date -Format "yyyyMMdd-HHmmss") }
if (Test-Path $Out) { throw "Refusing to overwrite existing $Out; pass a new -Out directory." }
New-Item -ItemType Directory -Path $Out | Out-Null
$rows = New-Object System.Collections.Generic.List[object]

foreach ($c in $cases) {
    $label = "s0x{0:x}_a{1}" -f $c.seed, $c.attempt
    # classic design B
    $raw = @(& $bench --n=72 --mode=attempt-cost --seed=$($c.seed) --attempt-limit=$($c.attempt + 1) --warmup=$Warmup --repeat=$Repeat 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { throw "ter_full_bench failed for ${label}: $($raw -join ' | ')" }
    $raw | Set-Content -Encoding utf8 (Join-Path $Out "$label.classic.csv")
    $data = @($raw | Where-Object { $_ -and -not $_.ToString().StartsWith("#") } | ConvertFrom-Csv)
    if ($data.Count -ne 1) { throw "ter_full_bench produced $($data.Count) data rows for $label, expected 1" }
    $bc = $data[0]
    if ([int]$bc.terminal_attempt_index -ne $c.attempt -or [uint64]$bc.terminal_attempt_seed -ne ($c.seed + $c.attempt)) {
        throw "classic terminal attempt $($bc.terminal_attempt_index) seed $($bc.terminal_attempt_seed) is not attempt $($c.attempt) of $label" }
    # staged capture of the same (instance seed, attempt)
    $snap = Join-Path $Out "$label.snap"
    & $capture --n=72 --seed=$($c.seed) --attempt=$($c.attempt) --out=$snap | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "capture failed for $label" }
    $cap = Get-Content "$snap.json" -Raw | ConvertFrom-Json
    if (($cap.level1_sizes -join ":") -ne $bc.L1_sizes -or [uint64]$cap.attempt_seed -ne [uint64]$bc.terminal_attempt_seed) {
        throw "classic and staged level-one lists differ for ${label}: $($bc.L1_sizes) vs $($cap.level1_sizes -join ':')" }
    $rows.Add([pscustomobject]@{ case = $label; reporter = "design_B_classic($($bc.terminal_attempt_status))"; ms_median = [double]$bc.l1_to_l0_ns / 1e6; ms_min = ""; ms_max = ""; samples = $Repeat })
    foreach ($b in $backends) {
        $jsonl = Join-Path $Out "$label.$b.jsonl"
        if (Test-Path $jsonl) { Remove-Item $jsonl }
        for ($i = 0; $i -lt $Repeat + 1; $i++) { & $solve --snapshot=$snap --out=$jsonl --backend=$b --pin=p-core | Out-Null; if ($LASTEXITCODE -ne 0) { throw "solve failed: $label $b" } }
        $recs = @(Get-Content $jsonl | ForEach-Object { $_ | ConvertFrom-Json })
        $t = @($recs | Select-Object -Skip 1 | ForEach-Object { [double]$_.root_ms })   # first run is the warmup
        $statuses = @($recs | ForEach-Object { $_.status } | Sort-Object -Unique)
        $expected = if ($bc.terminal_attempt_status -eq "Found") { "Found" } else { "NoWitnessThisAttempt" }
        if ($statuses.Count -ne 1 -or $statuses[0] -ne $expected) { throw "staged status $($statuses -join ',') disagrees with classic $($bc.terminal_attempt_status) for $label $b" }
        $ran = ($recs | Select-Object -Last 1).dispatch_ran
        $name = if ($b -eq "dispatch") { "dispatch(ran $ran)" } else { $b }
        $rows.Add([pscustomobject]@{ case = $label; reporter = $name; ms_median = ($t | Sort-Object)[[int](($t.Count - 1) / 2)]; ms_min = ($t | Measure-Object -Minimum).Minimum; ms_max = ($t | Measure-Object -Maximum).Maximum; samples = $t.Count })
    }
    Remove-Item $snap, "$snap.json" -ErrorAction SilentlyContinue
}
$rows | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $Out "summary.csv")
$rows | Format-Table -AutoSize | Out-String | Tee-Object -FilePath (Join-Path $Out "summary.txt")
[ordered]@{ finished = (Get-Date).ToString("o"); git_commit = (git rev-parse HEAD).Trim(); git_dirty = [bool](git status --porcelain)
            exe_sha256 = [ordered]@{ bench = (Get-FileHash -Algorithm SHA256 $bench).Hash; capture = (Get-FileHash -Algorithm SHA256 $capture).Hash; solve = (Get-FileHash -Algorithm SHA256 $solve).Hash } } |
    ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Out "provenance.json")
