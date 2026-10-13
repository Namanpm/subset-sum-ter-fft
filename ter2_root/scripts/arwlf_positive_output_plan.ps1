# The ARWLF positive-output experiment (ARWLF_REGIME_CAMPAIGN.md, Amendment 2):
# phase `plant` on the three calibration n=108 roots, in the preregistered
# order.  Safe to rerun at any time: every finished case is skipped, so an
# interrupted run resumes where it stopped.  Run it in its own terminal:
#
#   pwsh -NoProfile -File ter2_root/scripts/arwlf_positive_output_plan.ps1
#
# Ends with summary.md (section "Positive-output experiment").

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))  # repository root
$out = "results/arwlf/positive-20261003"
$snap = "results/root-queue-generalization/snapshots"
$roots = @("$snap/n108_s0x1001_a0.snap", "$snap/n108_s0x3007_a0.snap", "$snap/n108_s0x2003_a0.snap")

$steps = @($roots | ForEach-Object { "$_|plant|calibration" })
& ./ter2_root/scripts/run_arwlf_regime_campaign.ps1 -Out $out -Campaign positive-20261003 -Steps $steps

python -X utf8 ter2_root/scripts/summarize_arwlf_regime_campaign.py $out | Out-Null
Write-Host "Positive-output experiment complete: $out/summary.md"
