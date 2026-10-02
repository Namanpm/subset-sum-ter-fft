# Runs one root-queue generalization campaign with provenance.
#
# Records, next to the benchmark's own CSVs: executable SHA-256, git commit and
# dirty state, power plan, AC line state and CPU, before and after the run.
# Refuses to start on battery: a campaign's power state is part of its
# definition, not a footnote.  Extra arguments are passed to the benchmark.
#
#   pwsh scripts/run_root_queue_generalization.ps1 -Out results/root-queue-generalization/screen -- --samples=7

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Exe = "build/Release/ter_root_queue_generalization_bench.exe",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$BenchArgs
)

$ErrorActionPreference = "Stop"

function Get-PowerState {
    $battery = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue |
        Select-Object -First 1
    [ordered]@{
        power_online = if ($null -ne $battery) { [bool]$battery.PowerOnline } else { $null }
        power_plan   = ((powercfg /getactivescheme) -join " ").Trim()
        timestamp    = (Get-Date).ToString("o")
    }
}

$before = Get-PowerState
if ($before.power_online -eq $false) {
    throw "Refusing to start: the machine is on battery. Plug in and rerun."
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$meta = [ordered]@{
    executable        = (Resolve-Path $Exe).Path
    executable_sha256 = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLowerInvariant()
    git_commit        = (git rev-parse HEAD).Trim()
    git_dirty         = [bool](git status --porcelain)
    cpu               = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    arguments         = @("--out=$Out") + $BenchArgs
    power_before      = $before
}

& $Exe "--out=$Out" @BenchArgs 2>&1 | Tee-Object -FilePath (Join-Path $Out "console.log")
$exit = $LASTEXITCODE

$meta.power_after = Get-PowerState
$meta.exit_code = $exit
$meta | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $Out "provenance.json")
exit $exit
