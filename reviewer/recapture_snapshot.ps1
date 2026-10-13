# Recaptures an authentic FullTer root snapshot from its seed and checks it is the snapshot the
# campaign measured. The .snap files are not in git; this shows they are a deterministic function of
# the seed and the code, and it produces the file the timing script needs.
#
#   pwsh -NoProfile -File reviewer/recapture_snapshot.ps1 -List
#   pwsh -NoProfile -File reviewer/recapture_snapshot.ps1 -Root n72_anchor
#   pwsh -NoProfile -File reviewer/recapture_snapshot.ps1 -Root n108_s0x1001_a0 -Install
#
# Roots: n72_anchor and n72_s0x72{01,03,07}_auto (about 4 MB each, about a second);
# n108_s0x{1001,2003,3007,4001,5003,6007}_a0 (about 1 GB each; the recorded captures took about
# 160 s and 1.6 GB of memory). Needs build-full/Release/ter_root_snapshot_capture.exe
# (reviewer/build_and_test.ps1). The capture runs the recorded command; only --out is redirected.
#
# What is compared. Header bytes 352-423 of a snapshot (ter2_root/frozen/ter_root_snapshot.hpp,
# layout 1) are run metadata: the capture process's peak working set, its elapsed time, the git
# commit and the header checksum over them. A recapture therefore never has the measured file's
# whole-file SHA-256 (results/arwlf/regime-20261001/snapshots.json). The payload digest is the
# SHA-256 of every other byte: all three root lists and every deterministic header field (sizes,
# seeds, targets, per-list checksums). reviewer/snapshot_payload_digests.json records it for the
# ten measured snapshots; -RecordDigests regenerates that file from the local snapshots, after
# checking each against its recorded whole-file SHA-256.
#
# Without -Install the capture goes to a temporary directory and is deleted after the comparison.
# With -Install a matching capture is moved to the recorded path, where the timing script reads it.
param(
    [string]$Root = "",
    [switch]$Install,
    [switch]$List,
    [switch]$RecordDigests
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
. "$PSScriptRoot/snapshot_digest.ps1"
$snaps = (Get-Content -Raw results/arwlf/regime-20261001/snapshots.json | ConvertFrom-Json).snapshots
$digestFile = $SnapshotDigestFile

if ($RecordDigests) {
    $records = foreach ($s in $snaps) {
        $path = $s.path_not_in_git
        if (-not (Test-Path -LiteralPath $path)) { throw "missing $path; every measured snapshot is needed to record digests" }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() -ne $s.sha256) { throw "$path does not match its recorded SHA-256" }
        [ordered]@{ root = $s.root; file_sha256 = $s.sha256; file_bytes = $s.file_bytes; payload_sha256 = Get-SnapshotPayloadDigest $path }
    }
    [ordered]@{
        about     = "SHA-256 of each measured root snapshot excluding header bytes 352-423 (run metadata: peak working set, elapsed time, git commit, header checksum; layout 1). Computed from the measured files, each first checked against file_sha256 from results/arwlf/regime-20261001/snapshots.json."
        generator = "reviewer/recapture_snapshot.ps1 -RecordDigests"
        snapshots = @($records)
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $digestFile
    Write-Host "wrote $digestFile ($(@($records).Count) snapshots)"
    return
}

if ($List -or $Root -eq "") {
    $snaps | ForEach-Object {
        $state = if (Test-Path -LiteralPath $_.path_not_in_git) { "present" } else { "absent" }
        "{0,-18} {1,-12} {2,14} bytes  {3}" -f $_.root, $_.role, $_.file_bytes, $state
    }
    return
}
$s = @($snaps | Where-Object { $_.root -eq $Root })
if ($s.Count -ne 1) { throw "unknown root '$Root'; run with -List" }
$s = $s[0]
$expected = @((Get-Content -Raw $digestFile | ConvertFrom-Json).snapshots | Where-Object { $_.root -eq $Root })[0].payload_sha256

$exe = "build-full/Release/ter_root_snapshot_capture.exe"
if (-not (Test-Path $exe)) { throw "missing $exe (run reviewer/build_and_test.ps1 first)" }
if (Test-Path bench.lock) { throw "bench.lock exists: a guarded benchmark is running; wait for it to finish." }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ter-recapture-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
$out = Join-Path $tmp "$Root.snap"
try {
    $arguments = @(($s.capture_command -split ' ' | Select-Object -Skip 1) | ForEach-Object {
        if ($_ -like "--out=*") { "--out=$out" } else { $_ }
    })
    Write-Host "> $exe $($arguments -join ' ')"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "capture exited $LASTEXITCODE" }
    Write-Host ("capture took {0:N1} s" -f $timer.Elapsed.TotalSeconds)

    $bytes = (Get-Item -LiteralPath $out).Length
    $digest = Get-SnapshotPayloadDigest $out
    Write-Host "measured  payload $expected  $($s.file_bytes) bytes"
    Write-Host "recapture payload $digest  $bytes bytes"
    if ($digest -ne $expected -or $bytes -ne $s.file_bytes) {
        Write-Host "FAIL  $Root recapture differs from the measured snapshot" -ForegroundColor Red
        exit 1
    }
    Write-Host "PASS  $Root recapture equals the measured snapshot in every byte except the run metadata" -ForegroundColor Green
    if ($Install) {
        if (Test-Path -LiteralPath $s.path_not_in_git) {
            Write-Host "kept the existing $($s.path_not_in_git)"
        } else {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $s.path_not_in_git) | Out-Null
            Move-Item -LiteralPath $out -Destination $s.path_not_in_git
            Write-Host "installed $($s.path_not_in_git) (its whole-file SHA-256 differs from snapshots.json only through the run metadata)"
        }
    }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
