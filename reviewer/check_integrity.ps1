# Integrity of the recorded evidence. No build, no timing; finishes in seconds.
#
#   pwsh -NoProfile -File reviewer/check_integrity.ps1               # code + records
#   pwsh -NoProfile -File reviewer/check_integrity.ps1 -Snapshots    # + hash local .snap files (~6 GB read)
#
# Checks:
#   1. The 29 frozen root-backend sources (K2, K2-WIDE, QUEUE-DENSE, ARWLF + dispatcher) match
#      ter2_root/frozen_root_backends.sha256 (the same CMake script CTest runs).
#   2. Every frozen paper input in paper/data/ matches the SHA-256 in paper/data/input_manifest.json.
#   3. Where that manifest names a raw record under results/ (or paper/working/) as the origin, the
#      raw record has the same SHA-256, i.e. the paper's copy is byte-identical to the run's output.
#   4. Every file moved to results/arwlf/history/ matches RELOCATION_MANIFEST.json.
#   5. With -Snapshots: every root snapshot present on disk matches the SHA-256 and size recorded in
#      results/arwlf/regime-20261001/snapshots.json, or, for a recapture, the payload digest in
#      reviewer/snapshot_payload_digests.json (see reviewer/snapshot_digest.ps1). The .snap files are
#      not in git; a missing one is reported, not failed (reviewer/recapture_snapshot.ps1 makes it).
# Exit code 0 only if no check failed.
param([switch]$Snapshots)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$failures = 0

function Test-Sha256([string]$path, [string]$expected) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -eq $expected.ToUpperInvariant()
}
function Write-Result([string]$check, [int]$checked, [string[]]$bad) {
    if ($bad.Count -eq 0) { Write-Host "PASS  $check ($checked checked)" -ForegroundColor Green; return }
    Write-Host "FAIL  $check ($($bad.Count) of $checked)" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "      $_" }
    $script:failures += $bad.Count
}

# 1. frozen backends
& cmake "-DROOT=$($repo -replace '\\', '/')" -P ter2_root/scripts/check_frozen_root_backends.cmake
$bad = @(); if ($LASTEXITCODE -ne 0) { $bad += "see the CMake message above" }
Write-Result "frozen root-backend sources" 29 $bad

# 2 and 3. paper inputs and their raw origins
$inputs = (Get-Content -Raw paper/data/input_manifest.json | ConvertFrom-Json).inputs
$bad = @(); $originBad = @(); $originChecked = 0
foreach ($e in $inputs) {
    $copy = Join-Path paper/data $e.file
    if (-not (Test-Path -LiteralPath $copy -PathType Leaf)) { $bad += "missing $copy"; continue }
    if (-not (Test-Sha256 $copy $e.sha256)) { $bad += "hash differs: $copy" }
    # The origin field starts with a path; descriptions follow it. Script origins (.py) generate the
    # file rather than hold it, and an origin absent from a fresh clone (paper/working/) is skipped.
    $origin = ($e.origin -split ' ')[0]
    if ((Test-Path -LiteralPath $origin -PathType Leaf) -and [IO.Path]::GetExtension($origin) -ne ".py") {
        $originChecked++
        if (-not (Test-Sha256 $origin $e.sha256)) { $originBad += "paper/data/$($e.file) differs from $origin" }
    }
}
Write-Result "paper/data inputs against input_manifest.json" $inputs.Count $bad
Write-Result "paper/data copies byte-identical to their raw records" $originChecked $originBad

# 4. relocated historical results
$moved = (Get-Content -Raw results/arwlf/history/RELOCATION_MANIFEST.json | ConvertFrom-Json).files
$bad = @()
foreach ($f in $moved) {
    if (-not (Test-Path -LiteralPath $f.current -PathType Leaf)) { $bad += "missing $($f.current)"; continue }
    if (-not (Test-Sha256 $f.current $f.sha256)) { $bad += "hash differs: $($f.current)" }
    if (Test-Path -LiteralPath $f.original) { $bad += "still at original path: $($f.original)" }
}
Write-Result "results/arwlf/history relocation" $moved.Count $bad

# 5. root snapshots (optional)
if ($Snapshots) {
    . "$PSScriptRoot/snapshot_digest.ps1"
    $snaps = (Get-Content -Raw results/arwlf/regime-20261001/snapshots.json | ConvertFrom-Json).snapshots
    $payload = @{}
    (Get-Content -Raw $SnapshotDigestFile | ConvertFrom-Json).snapshots | ForEach-Object { $payload[$_.root] = $_.payload_sha256 }
    $bad = @(); $present = 0
    foreach ($s in $snaps) {
        $path = $s.path_not_in_git
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Write-Host "      absent (not in git): $path"; continue }
        $present++
        if ((Get-Item -LiteralPath $path).Length -ne $s.file_bytes) { $bad += "size differs: $path"; continue }
        if (Test-Sha256 $path $s.sha256) { continue }
        # A recapture installed by recapture_snapshot.ps1 differs only in the run metadata.
        if ((Get-SnapshotPayloadDigest $path) -eq $payload[$s.root]) { Write-Host "      recaptured (payload matches): $path"; continue }
        $bad += "hash differs: $path"
    }
    Write-Result "root snapshots against snapshots.json" $present $bad
}

if ($failures -gt 0) { Write-Host "$failures integrity failure(s)." -ForegroundColor Red; exit 1 }
Write-Host "All integrity checks passed."
