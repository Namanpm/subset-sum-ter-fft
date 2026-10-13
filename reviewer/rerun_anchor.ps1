# Re-times one preregistered anchor cell of the ARWLF regime campaign and prints it beside the
# recorded result (results/arwlf/regime-20261001/<root>.anchor.console.log).
#
#   pwsh -NoProfile -File reviewer/rerun_anchor.ps1 -List
#   pwsh -NoProfile -File reviewer/rerun_anchor.ps1 -Root n72_anchor
#   pwsh -NoProfile -File reviewer/rerun_anchor.ps1 -Root n108_s0x1001_a0          # the paper's anchor root
#   pwsh -NoProfile -File reviewer/rerun_anchor.ps1 -Root n108_s0x1001_a0 -Smoke   # one pass, no warm-up
#
# Same executable, phase and protocol as the campaign (ter_arwlf_campaign --phase=anchor, final
# protocol: 3 warm-ups and 7 timed passes of K2, K2-WIDE, ARWLF and the dispatcher, pinned to one
# core). Every reported parent is checked against an independent authority; a mismatch exits 4.
# Each run first loads the snapshot and builds the authority (about 1.5 minutes at n=108), then
# times 10 passes per backend: about 15 minutes for n108_s0x1001_a0 and up to about 35 minutes for
# the other n=108 roots on the author's machine; n=72 roots take seconds. -Smoke times one pass
# (rows labelled "smoke"; about 3 minutes for n108_s0x1001_a0) to check the setup.
#
# Absolute times depend on the CPU; compare the K2/ARWLF ratio. The recorded numbers come from an
# Intel Core Ultra 9 275HX on AC power (cpu in results/arwlf/regime-20261001/build_freeze.json).
# Run this on an idle machine on AC power, with nothing else heavy running.
#
# Needs build-full/Release/ter_arwlf_campaign.exe (reviewer/build_and_test.ps1), an AVX2 CPU, and
# the root's snapshot at its recorded path (reviewer/recapture_snapshot.ps1 -Root <root> -Install).
# The snapshot is checked against the measured one before timing. Outputs go to a new temporary
# directory, which is kept and printed.
param(
    [string]$Root = "",
    [switch]$Smoke,
    [switch]$List
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
. "$PSScriptRoot/snapshot_digest.ps1"
$campaign = "results/arwlf/regime-20261001"
$snaps = (Get-Content -Raw "$campaign/snapshots.json" | ConvertFrom-Json).snapshots

function Get-CaseLine([string[]]$lines) { @($lines | Where-Object { $_ -match "^# case anchor:" })[0] }
function Get-Ratio([string]$line) { if ($line -match "K2/ARWLF=([0-9.]+)") { [double]$Matches[1] } }

if ($List -or $Root -eq "") {
    foreach ($s in $snaps) {
        $state = if (Test-Path -LiteralPath $s.path_not_in_git) { "snapshot present" } else { "snapshot absent" }
        $recorded = Get-Ratio (Get-CaseLine (Get-Content "$campaign/$($s.root).anchor.console.log"))
        "{0,-18} {1,-12} recorded K2/ARWLF = {2,6}   {3}" -f $s.root, $s.role, $recorded, $state
    }
    return
}
$s = @($snaps | Where-Object { $_.root -eq $Root })
if ($s.Count -ne 1) { throw "unknown root '$Root'; run with -List" }
$s = $s[0]

$exe = "build-full/Release/ter_arwlf_campaign.exe"
if (-not (Test-Path $exe)) { throw "missing $exe (run reviewer/build_and_test.ps1 first)" }
if (Test-Path bench.lock) { throw "bench.lock exists: a guarded benchmark is running; wait for it to finish." }
$snapshot = $s.path_not_in_git
if (-not (Test-Path -LiteralPath $snapshot)) {
    throw "missing $snapshot; run: pwsh -NoProfile -File reviewer/recapture_snapshot.ps1 -Root $Root -Install"
}
$expected = @((Get-Content -Raw $SnapshotDigestFile | ConvertFrom-Json).snapshots | Where-Object { $_.root -eq $Root })[0].payload_sha256
if ((Get-SnapshotPayloadDigest $snapshot) -ne $expected) { throw "$snapshot is not the measured snapshot (payload digest differs)" }

$out = Join-Path ([IO.Path]::GetTempPath()) ("ter-anchor-$Root-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$arguments = @("--snapshot=$snapshot", "--out-dir=$out", "--label=$Root", "--campaign=reviewer-rerun",
               "--phase=anchor", "--role=$($s.role)", "--pin=p-core")
if ($Smoke) { $arguments += "--protocol=smoke" }
Write-Host "> $exe $($arguments -join ' ')"
$lines = @(& $exe @arguments 2>&1 | ForEach-Object { $line = "$_"; Write-Host $line; $line })
$code = $LASTEXITCODE
if ($code -ne 0) { throw "ter_arwlf_campaign exited $code (4 = exactness failure); outputs in $out" }

$now = Get-CaseLine $lines
$recorded = Get-CaseLine (Get-Content "$campaign/$Root.anchor.console.log")
Write-Host ""
Write-Host "recorded: $recorded"
Write-Host "this run: $now"
Write-Host ("K2/ARWLF  recorded {0}   this run {1}" -f (Get-Ratio $recorded), (Get-Ratio $now))
Write-Host "outputs:  $out"
