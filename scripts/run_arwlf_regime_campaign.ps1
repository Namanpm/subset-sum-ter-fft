# Runs ARWLF regime-campaign steps (see ARWLF_REGIME_CAMPAIGN.md): one fresh
# ter_arwlf_campaign process per (snapshot, phase), in the order given.
#
# Before the first step it freezes the build: a SHA-256 manifest of every
# source the executable is built from, the executable hash, the generated
# project file (compiler flags) and the CMake cache flags.  Every step appends
# a provenance record (arguments, git HEAD and dirty state, CPU, power plan, AC
# state before and after, exit code).  Refuses to start a step on battery.
# Stops at the first step that exits non-zero (an exactness failure is 4).
#
#   pwsh scripts/run_arwlf_regime_campaign.ps1 -Out results/arwlf/regime-X `
#        -Steps "snap.snap|census|calibration", "snap.snap|thin-qx|calibration"
# A step is "snapshot|phase|role[|cases]" (cases: comma list for confirm).

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string[]]$Steps,
    [string]$Exe = "build-full/Release/ter_arwlf_campaign.exe",
    [string]$Campaign = ""
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if ($Campaign -eq "") { $Campaign = Split-Path -Leaf $Out }

function Get-PowerState {
    $battery = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue |
        Select-Object -First 1
    [ordered]@{
        power_online = if ($null -ne $battery) { [bool]$battery.PowerOnline } else { $null }
        power_plan   = ((powercfg /getactivescheme) -join " ").Trim()
        timestamp    = (Get-Date).ToString("o")
    }
}

# --- build freeze (once per campaign directory) ------------------------------
$manifest = Join-Path $Out "source_manifest.txt"
if (-not (Test-Path $manifest)) {
    $files = @(Get-ChildItem -Recurse -File bench, src, include, tests, scripts) + @(Get-Item CMakeLists.txt)
    $lines = $files | Sort-Object FullName | ForEach-Object {
        $rel = Resolve-Path -Relative $_.FullName
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant(), $rel
    }
    $lines | Set-Content -Encoding utf8 $manifest
    $project = "build-full/ter_arwlf_campaign.vcxproj"
    if (Test-Path $project) { Copy-Item $project (Join-Path $Out "ter_arwlf_campaign.vcxproj") }
    $cache = Get-Content build-full/CMakeCache.txt |
        Where-Object { $_ -match "^(CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE|CMAKE_CXX_COMPILER|TER_NATIVE_OPT|CMAKE_GENERATOR)[:=]" }
    $freeze = [ordered]@{
        campaign            = $Campaign
        frozen_at           = (Get-Date).ToString("o")
        executable          = (Resolve-Path $Exe).Path
        executable_sha256   = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLowerInvariant()
        source_manifest_sha256 = (Get-FileHash -Algorithm SHA256 $manifest).Hash.ToLowerInvariant()
        git_commit          = (git rev-parse HEAD).Trim()
        git_dirty           = [bool](git status --porcelain)
        cmake_cache         = $cache
        cpu                 = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        msvc                = (Get-Content build-full/CMakeCache.txt | Where-Object { $_ -match "^CMAKE_CXX_COMPILER:" })
    }
    $freeze | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $Out "build_freeze.json")
}
$frozen = Get-Content (Join-Path $Out "build_freeze.json") | ConvertFrom-Json
$exeHash = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLowerInvariant()
if ($exeHash -ne $frozen.executable_sha256) {
    throw "Refusing to run: $Exe differs from the executable frozen for this campaign."
}

# --- steps -------------------------------------------------------------------
foreach ($step in $Steps) {
    $parts = $step -split "\|"
    if ($parts.Count -lt 3) { throw "bad step: $step" }
    $snapshot, $phase, $role = $parts[0], $parts[1], $parts[2]
    $before = Get-PowerState
    if ($before.power_online -eq $false) { throw "Refusing to start: the machine is on battery." }
    $name = [System.IO.Path]::GetFileNameWithoutExtension($snapshot)
    $arguments = @("--snapshot=$snapshot", "--out-dir=$Out", "--label=$name", "--campaign=$Campaign",
                   "--phase=$phase", "--role=$role", "--pin=p-core")
    if ($parts.Count -ge 4 -and $parts[3] -ne "") { $arguments += "--cases=$($parts[3])" }
    $log = Join-Path $Out "$name.$phase.console.log"
    & $Exe @arguments 2>&1 | Tee-Object -FilePath $log -Append
    $code = $LASTEXITCODE
    $record = [ordered]@{
        step            = $step
        arguments       = $arguments
        snapshot_sha256 = (Get-FileHash -Algorithm SHA256 $snapshot).Hash.ToLowerInvariant()
        executable_sha256 = $exeHash
        git_commit      = (git rev-parse HEAD).Trim()
        git_dirty       = [bool](git status --porcelain)
        power_before    = $before
        power_after     = Get-PowerState
        exit_code       = $code
    }
    ($record | ConvertTo-Json -Depth 5 -Compress) | Add-Content -Encoding utf8 (Join-Path $Out "provenance.jsonl")
    if ($code -ne 0) { throw "Step '$step' exited $code; campaign stopped." }
}
