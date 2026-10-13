# Release build and the CTest suite, with the commands AGENTS.md prescribes.
#
#   pwsh -NoProfile -File reviewer/build_and_test.ps1                      # full suite (61 tests)
#   pwsh -NoProfile -File reviewer/build_and_test.ps1 -Regex "arwlf|frozen" # a subset
#
# Configures build-full with TER_NATIVE_OPT=ON (MSVC: /arch:AVX2), the configuration recorded in
# results/arwlf/regime-20261001/build_freeze.json, builds Release and runs CTest. On the author's
# machine the 61 tests took 7 minutes, mostly ter_arwlf_solve_tests and ter_arwlf_planted_tests.
# The executables land in build-full/Release/, where the other reviewer scripts expect them.
# Exit code is CTest's.
param(
    [string]$BuildDir = "build-full",
    [string]$Regex = "",
    [switch]$SkipConfigure
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
if (Test-Path bench.lock) { throw "bench.lock exists: a guarded benchmark is running; wait for it to finish." }

function Invoke-Native([string]$exe, [string[]]$arguments) {
    Write-Host "> $exe $($arguments -join ' ')"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe exited $LASTEXITCODE" }
}

if (-not $SkipConfigure) { Invoke-Native cmake @("-S", ".", "-B", $BuildDir, "-DTER_NATIVE_OPT=ON") }
Invoke-Native cmake @("--build", $BuildDir, "--config", "Release", "--parallel")

$ctestArgs = @("--test-dir", $BuildDir, "-C", "Release", "--output-on-failure")
if ($Regex -ne "") { $ctestArgs += @("-R", $Regex) }
Write-Host "> ctest $($ctestArgs -join ' ')"
& ctest @ctestArgs
exit $LASTEXITCODE
