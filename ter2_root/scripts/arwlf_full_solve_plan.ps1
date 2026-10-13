# End-to-end Full TER solve with the frozen root dispatcher (ARWLF_FULL_SOLVE.md).
# For attempt k = 0, 1, ...: stream the solver's three L1 lists of attempt k to a
# snapshot (ter_root_snapshot_capture, the staged solver path), run the root
# stage on the frozen dispatcher and verify every root parent against the
# original instance (ter_arwlf_solve_attempt), then delete the snapshot.  Stops at
# the first Found; after MaxAttempts it reports AttemptLimitReached, which is
# never a proof of UNSAT.  Safe to rerun: finished attempts are skipped.
#
#   pwsh -NoProfile -File ter2_root/scripts/arwlf_full_solve_plan.ps1
#   pwsh -NoProfile -File ter2_root/scripts/arwlf_full_solve_plan.ps1 -N 72 -Seed 0x7201 -MaxAttempts 16
#
# Run it on a quiet machine (bench_guard.ps1 alongside), never next to another benchmark.

param(
    [int]$N = 108,
    [string]$Seed = "0x8001",    # preregistered fresh seed (ARWLF_FULL_SOLVE.md)
    [int]$MaxAttempts = 32,      # preregistered budget
    [string]$Backend = "dispatch",
    [string]$Out = "",
    [switch]$KeepSnapshots,
    [ValidateSet("p-core", "none")][string]$Pin = "p-core"  # "none" only for correctness runs beside a benchmark
)

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))  # repository root
if ($Out -eq "") { $Out = "results/arwlf/full-solve-n$N-s$Seed" }
$capture = "build-full/Release/ter_root_snapshot_capture.exe"
$solve = "build-full/Release/ter_arwlf_solve_attempt.exe"
$records = Join-Path $Out "attempts.jsonl"
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if (-not ('TerPlan.Power' -as [type])) {
    Add-Type -Namespace TerPlan -Name Power -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct Status { public byte ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag; public int LifeTime, FullLifeTime; }
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool GetSystemPowerStatus(out Status status);
'@
}
function Get-OnAc {  # $null when unknown; no WMI (it wakes wmiprvse)
    $status = New-Object TerPlan.Power+Status
    if (-not [TerPlan.Power]::GetSystemPowerStatus([ref]$status) -or $status.ACLineStatus -eq 255) { return $null }
    return $status.ACLineStatus -eq 1
}

# --- build freeze (once): both executables ------------------------------------
$freezePath = Join-Path $Out "build_freeze.json"
if (-not (Test-Path $freezePath)) {
    [ordered]@{
        frozen_at        = (Get-Date).ToString("o")
        n                = $N
        seed             = $Seed
        max_attempts     = $MaxAttempts
        backend          = $Backend
        capture_sha256   = (Get-FileHash -Algorithm SHA256 $capture).Hash.ToLowerInvariant()
        solve_sha256     = (Get-FileHash -Algorithm SHA256 $solve).Hash.ToLowerInvariant()
        git_commit       = (git rev-parse HEAD).Trim()
        git_dirty        = [bool](git status --porcelain)
        cpu              = Get-ItemPropertyValue "HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0" -Name ProcessorNameString
    } | ConvertTo-Json | Set-Content -Encoding utf8 $freezePath
}
$frozen = Get-Content $freezePath | ConvertFrom-Json
if ($frozen.n -ne $N -or $frozen.seed -ne $Seed -or $frozen.backend -ne $Backend) {
    throw "Refusing to run: $Out was started for n=$($frozen.n) seed=$($frozen.seed) backend=$($frozen.backend)."
}
foreach ($pair in @(@($capture, $frozen.capture_sha256), @($solve, $frozen.solve_sha256))) {
    if ((Get-FileHash -Algorithm SHA256 $pair[0]).Hash.ToLowerInvariant() -ne $pair[1]) {
        throw "Refusing to run: $($pair[0]) differs from the executable frozen for this solve."
    }
}

function Get-Records {
    if (-not (Test-Path $records)) { return @() }
    return @(Get-Content $records | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
}

$final = @("Found", "NoWitnessThisAttempt")
for ($attempt = 0; $attempt -lt $MaxAttempts; $attempt++) {
    $done = Get-Records
    $found = @($done | Where-Object { $_.status -eq "Found" })
    if ($found.Count -gt 0) { break }
    $mine = @($done | Where-Object { $_.attempt -eq $attempt -and $final -contains $_.status })
    if ($mine.Count -gt 0) { continue }
    $stopping = @($done | Where-Object { $final -notcontains $_.status })
    if ($stopping.Count -gt 0) { throw "A recorded attempt ended $($stopping[0].status): $($stopping[0].message). Stopped." }
    if ((Get-OnAc) -eq $false) { throw "Refusing to start: the machine is on battery." }

    $snap = Join-Path $Out "attempt_$attempt.snap"
    $started = Get-Date
    Write-Host "== attempt $attempt : capture (n=$N seed=$Seed) =="
    & $capture --n=$N --seed=$Seed --attempt=$attempt --out=$snap 2>&1 | Tee-Object -FilePath (Join-Path $Out "attempt_$attempt.capture.log")
    if ($LASTEXITCODE -ne 0) { throw "capture of attempt $attempt exited $LASTEXITCODE; stopped (rerun to retry)." }
    $captured = Get-Date
    Write-Host "== attempt $attempt : root stage ($Backend) + verification =="
    & $solve --snapshot=$snap --out=$records --backend=$Backend --pin=$Pin 2>&1 | Tee-Object -FilePath (Join-Path $Out "attempt_$attempt.solve.log")
    $code = $LASTEXITCODE
    ([ordered]@{ attempt = $attempt; started = $started.ToString("o"); captured = $captured.ToString("o");
                 finished = (Get-Date).ToString("o"); capture_s = ($captured - $started).TotalSeconds;
                 solve_exit = $code; on_ac_after = (Get-OnAc) } | ConvertTo-Json -Compress) |
        Add-Content -Encoding utf8 (Join-Path $Out "provenance.jsonl")
    if ($code -ne 0) { throw "root stage of attempt $attempt exited $code; stopped. The snapshot is kept: $snap" }
    if (Test-Path "$snap.json") { Move-Item -Force "$snap.json" (Join-Path $Out "attempt_$attempt.capture.json") }
    if (-not $KeepSnapshots) { Remove-Item -Force $snap }
}

$done = Get-Records
$found = @($done | Where-Object { $_.status -eq "Found" })
if ($found.Count -gt 0) {
    $f = $found[0]
    Write-Host "FOUND at attempt $($f.attempt): $($f.unique_parents) verified parent(s), planted solution among them: $($f.planted_found)"
} else {
    Write-Host "AttemptLimitReached after $($done.Count) attempt(s): no witness yet. This is NOT a proof of UNSAT."
}
