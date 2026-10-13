# Recorded AddressSanitizer run of the staged-solve differential test (the one that exceeds CTest's 1 h limit).
# Run ONLY on a quiet machine, after the n=108 solve has finished or stopped.  Expect roughly 1 to 2.5 hours.
#
#   pwsh -NoProfile -File paper/runs/run_asan_differential_recorded.ps1
#
# Records: executable SHA-256, exact command, start and end UTC, exit code, and the complete output,
# into results/arwlf/asan-differential-<date>/ (record.json + output.log).
param([string]$Out = "")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))   # repository root

$exe = "build-full-asan/Debug/ter_arwlf_solve_tests.exe"
if (-not (Test-Path $exe)) { throw "missing $exe (cmake --build build-full-asan --config Debug --target ter_arwlf_solve_tests)" }
$busy = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "ter_*" }
if ($busy) { throw "Refusing to run: $($busy[0].ProcessName) is running; wait for it to finish." }
$runtime = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "Hostx64\\x64" } | Select-Object -First 1
if (-not $runtime) { throw "ASan runtime DLL not found under Visual Studio" }
if ($Out -eq "") { $Out = "results/arwlf/asan-differential-" + (Get-Date -Format "yyyyMMdd") }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$scratch = Join-Path $Out "scratch"
$env:PATH = "$($runtime.DirectoryName);$env:PATH"

$started = (Get-Date).ToUniversalTime()
$command = "$exe $scratch"
& $exe $scratch 2>&1 | Tee-Object -FilePath (Join-Path $Out "output.log")
$code = $LASTEXITCODE
$ended = (Get-Date).ToUniversalTime()
$log = Get-Content (Join-Path $Out "output.log") -Raw
[ordered]@{
    command            = $command
    executable_sha256  = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
    asan_runtime       = $runtime.FullName
    started_utc        = $started.ToString("o")
    ended_utc          = $ended.ToString("o")
    elapsed_s          = [math]::Round(($ended - $started).TotalSeconds, 1)
    exit_code          = $code
    sanitizer_report   = [bool]($log -match "AddressSanitizer")
    summary_line       = ($log -split "`n" | Where-Object { $_ -match "checks," } | Select-Object -Last 1)
    git_commit         = (git rev-parse HEAD).Trim()
    git_dirty          = [bool](git status --porcelain)
} | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Out "record.json")
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
Get-Content (Join-Path $Out "record.json")
if ($code -ne 0) { exit $code }
