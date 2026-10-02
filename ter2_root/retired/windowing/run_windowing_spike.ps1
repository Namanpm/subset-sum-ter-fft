param(
    [string]$OutputRoot = 'results\ter2-windowing-20260905',
    [ValidateRange(51, 100000)][int]$Samples = 51
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $taskRepo
$taskOutput = [IO.Path]::GetFullPath((Join-Path $taskRepo $OutputRoot))
if (-not $taskOutput.StartsWith($taskRepo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Results must stay inside the Desktop repository.'
}
if (Test-Path -LiteralPath $taskOutput) { throw 'Choose a new results directory; existing results are preserved.' }
$taskProtected = @(
    Get-ChildItem 'bench\ter_microreport8*' -File
    Get-Item 'tests\microreport8_tests.cpp'
    Get-Item 'bench\ter_root_capture_adapter.cpp','bench\ter_root_capture_adapter.hpp'
    Get-ChildItem 'src','include','results\microreport8-20260902','results\ter2-windowing-20260903' -File -Recurse
)
$taskBefore = @($taskProtected | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
$taskGit = git rev-parse HEAD
$taskStatus = @(git status --porcelain=v1)
cmake -S . -B build-mr8 -DTER_NATIVE_OPT=OFF -DTER_ENABLE_ASAN=OFF
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
cmake --build build-mr8 --config Release --target ter_windowing_probe ter_windowing_tests ter_microreport8_tests --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
ctest --test-dir build-mr8 -C Release -R '^(ter_windowing_tests|ter_microreport8_tests)$' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Correctness tests failed.' }
$taskRaw = Join-Path $taskOutput 'raw'
$taskAnalysis = Join-Path $taskOutput 'analysis'
New-Item -ItemType Directory -Path $taskRaw,$taskAnalysis | Out-Null
$taskExe = Join-Path $taskRepo 'build-mr8\Release\ter_windowing_probe.exe'
$taskArgs = @('--root-n=72','--root-seed=19797034556212562',"--samples=$Samples","--out=$taskRaw")
$taskStarted = [DateTimeOffset]::Now
# Keep stdout outside raw until the executable's nonempty-directory guard passes.
& $taskExe @taskArgs | Tee-Object -FilePath (Join-Path $taskOutput 'probe_stdout.txt')
$taskExit = $LASTEXITCODE
$taskAfter = @($taskProtected | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
$taskDifference = @(Compare-Object $taskBefore $taskAfter -Property Path,Hash)
$taskManifest = [ordered]@{
    schema_version = 1
    started = $taskStarted.ToString('o')
    ended = [DateTimeOffset]::Now.ToString('o')
    repository = $taskRepo
    commit = $taskGit
    initial_status = $taskStatus
    executable = $taskExe
    executable_sha256 = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash
    arguments = $taskArgs
    exit_code = $taskExit
    protected_identity_passed = ($taskDifference.Count -eq 0)
    protected_files_before = $taskBefore
    scope = 'Exhaustive authentic n=72 geometry; sampled source-order query timing; isolated experiment'
}
[IO.File]::WriteAllText((Join-Path $taskRaw 'run_manifest.json'), ($taskManifest | ConvertTo-Json -Depth 6))
if ($taskDifference.Count -ne 0) { throw 'Protected files changed during the experiment.' }
if ($taskExit -ne 0) { throw "Probe failed with exit $taskExit; inspect saved output." }
$taskResult = Get-Content -LiteralPath (Join-Path $taskRaw 'window_geometry.json') -Raw | ConvertFrom-Json
if ($taskResult.geometry.true_postings -ne 619579 -or $taskResult.geometry.routing_disagreements -ne 0) {
    throw 'Result correctness counters failed.'
}
Write-Output "Results: $taskOutput"
