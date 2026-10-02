# The whole ARWLF regime campaign (ARWLF_REGIME_CAMPAIGN.md, with Amendment 1),
# in priority order.  Safe to rerun at any time: every finished case is skipped,
# so an interrupted run resumes where it stopped.  Run it in its own terminal:
#
#   pwsh -NoProfile -File ter2_root/scripts/arwlf_regime_campaign_plan.ps1
#
# Stage 1: census, n=72 work, thin-qx crossover sweeps, n=108 anchors, Y ablation.
# Stage 2: the preregistered crossover confirmations (chosen by the summarizer
# from stage-1 data), then the n=108 corner strata.  Ends with summary.md.

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))  # repository root
$out = "results/arwlf/regime-20261001"
$snap = "results/root-queue-generalization/snapshots"; $hold = "results/arwlf/holdout-snapshots"
$c72 = "$snap/n72_anchor.snap"
$h72 = @("$hold/n72_s0x7201_auto.snap", "$hold/n72_s0x7203_auto.snap", "$hold/n72_s0x7207_auto.snap")
$c108 = @("$snap/n108_s0x1001_a0.snap", "$snap/n108_s0x2003_a0.snap", "$snap/n108_s0x3007_a0.snap")
$h108 = @("$hold/n108_s0x4001_a0.snap", "$hold/n108_s0x5003_a0.snap", "$hold/n108_s0x6007_a0.snap")

function Invoke-Steps([string[]]$steps) {
    & ./ter2_root/scripts/run_arwlf_regime_campaign.ps1 -Out $out -Campaign regime-20261001 -Steps $steps
}

# --- stage 1 ------------------------------------------------------------------
$steps = @()
foreach ($s in @($c72) + $c108) { if (-not (Test-Path "$out/$([IO.Path]::GetFileNameWithoutExtension($s))_census.csv")) { $steps += "$s|census|calibration" } }
foreach ($s in $h72 + $h108) { if (-not (Test-Path "$out/$([IO.Path]::GetFileNameWithoutExtension($s))_census.csv")) { $steps += "$s|census|holdout" } }
foreach ($p in @("whole", "anchor", "stratified")) { $steps += "$c72|$p|calibration" }
foreach ($s in $h72) { foreach ($p in @("whole", "anchor", "stratified")) { $steps += "$s|$p|holdout" } }
foreach ($s in $c108) { $steps += "$s|thin-qx|calibration" }
foreach ($s in $c108) { $steps += "$s|anchor|calibration" }
foreach ($s in $h108) { $steps += "$s|anchor|holdout" }
foreach ($s in $c108) { $steps += "$s|thin-y|calibration" }
Invoke-Steps $steps

# --- stage 2 ------------------------------------------------------------------
$confirm = python -X utf8 ter2_root/scripts/summarize_arwlf_regime_campaign.py $out | Select-String -Pattern "^confirm "
$steps = @()
foreach ($line in $confirm) {
    $root, $cases = ($line.Line -replace "^confirm ", "") -split ": ", 2
    $snapshot = @($c108 | Where-Object { $_ -match [regex]::Escape($root) })[0]
    if ($snapshot -and $cases) { $steps += "$snapshot|confirm|calibration|$cases" }
}
foreach ($s in $c108) { $steps += "$s|stratified-corners|calibration" }
foreach ($s in $h108) { $steps += "$s|stratified-corners|holdout" }
Invoke-Steps $steps

python -X utf8 ter2_root/scripts/summarize_arwlf_regime_campaign.py $out | Out-Null
Write-Host "Campaign complete: $out/summary.md"
