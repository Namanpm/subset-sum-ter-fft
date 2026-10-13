# The ARWLF whole-root run (ARWLF_WHOLE_ROOT.md): every microproblem of the
# n=108 root 0x1001 with one planted final output, frozen backends, pinned.
# Safe to rerun at any time: finished runs and finished K2 chunks are skipped,
# so an interrupted run resumes where it stopped.  Run it in its own terminal:
#
#   pwsh -NoProfile -File ter2_root/scripts/arwlf_whole_root_plan.ps1
#   pwsh -NoProfile -File ter2_root/scripts/arwlf_whole_root_plan.ps1 -IncludeK2Wide   # + ~31 h
#
# Order: ARWLF and DISPATCH (about 1 h each), frozen K2 (about 51 h, 64
# resumable chunks), then ARWLF and DISPATCH again to bracket any drift.
# Ends with summary.md.

param([switch]$IncludeK2Wide)

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))  # repository root
$out = "results/arwlf/whole-root-20261003"
$snapshot = "results/root-queue-generalization/snapshots/n108_s0x1001_a0.snap"
$exe = "build-full/Release/ter_arwlf_whole_root.exe"
$steps = @("arwlf-1|arwlf", "dispatch-1|dispatch", "k2-1|k2", "arwlf-2|arwlf", "dispatch-2|dispatch")
if ($IncludeK2Wide) { $steps += "k2_wide-1|k2_wide" }
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Get-OnAc {
    $battery = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $battery) { return $null }
    return [bool]$battery.PowerOnline
}

# --- build freeze (once): sources, executable, flags --------------------------
$manifest = Join-Path $out "source_manifest.txt"
if (-not (Test-Path $manifest)) {
    $files = @(Get-ChildItem -Recurse -File src, include, ter2_root) + @(Get-Item CMakeLists.txt)
    $files | Sort-Object FullName | ForEach-Object {
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant(), (Resolve-Path -Relative $_.FullName)
    } | Set-Content -Encoding utf8 $manifest
    [ordered]@{
        frozen_at              = (Get-Date).ToString("o")
        executable             = (Resolve-Path $exe).Path
        executable_sha256      = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
        source_manifest_sha256 = (Get-FileHash -Algorithm SHA256 $manifest).Hash.ToLowerInvariant()
        snapshot_sha256        = (Get-FileHash -Algorithm SHA256 $snapshot).Hash.ToLowerInvariant()
        git_commit             = (git rev-parse HEAD).Trim()
        git_dirty              = [bool](git status --porcelain)
        cmake_cache            = Get-Content build-full/CMakeCache.txt |
            Where-Object { $_ -match "^(CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE|CMAKE_CXX_COMPILER|TER_NATIVE_OPT|CMAKE_GENERATOR)[:=]" }
        cpu                    = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    } | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $out "build_freeze.json")
}
$frozen = Get-Content (Join-Path $out "build_freeze.json") | ConvertFrom-Json
if ((Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant() -ne $frozen.executable_sha256) {
    throw "Refusing to run: $exe differs from the executable frozen for this run."
}

# --- steps -------------------------------------------------------------------
foreach ($step in $steps) {
    $run, $backend = $step -split "\|"
    if ((Get-OnAc) -eq $false) { throw "Refusing to start: the machine is on battery." }
    $arguments = @("--snapshot=$snapshot", "--out-dir=$out", "--run=$run", "--backend=$backend", "--pin=p-core")
    $started = (Get-Date).ToString("o")
    & $exe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $out "$run.console.log") -Append
    $code = $LASTEXITCODE
    ([ordered]@{ run = $run; backend = $backend; arguments = $arguments; started = $started;
                 finished = (Get-Date).ToString("o"); exit_code = $code; on_ac_after = (Get-OnAc) } |
        ConvertTo-Json -Compress) | Add-Content -Encoding utf8 (Join-Path $out "provenance.jsonl")
    if ($code -ne 0) { throw "Run '$run' exited $code; stopped (rerun to resume)." }
}

python -X utf8 ter2_root/scripts/summarize_arwlf_whole_root.py $out
if ($LASTEXITCODE -ne 0) { throw "summary reports an exactness failure" }
Write-Host "Whole-root run complete: $out/summary.md"
