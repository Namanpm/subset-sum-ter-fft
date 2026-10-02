# Runs one A64-CHQR campaign: one fresh ter_a64_chqr_bench process per snapshot.
#
# Records executable and snapshot SHA-256, git commit and dirty state, CPU,
# power plan and AC state before and after every process.  Refuses to start a
# process on battery.  Extra arguments are passed to every benchmark process.
#
#   pwsh scripts/run_a64_chqr_campaign.ps1 -Out results/a64-chqr/probe `
#        -Snapshots "a.snap,b.snap" -- --microproblems=1 --no-timing

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Snapshots,  # comma-separated
    [string]$Exe = "build-full/Release/ter_a64_chqr_bench.exe",
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

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$runs = @()
foreach ($snapshot in ($Snapshots -split ",")) {
    $before = Get-PowerState
    if ($before.power_online -eq $false) {
        throw "Refusing to start: the machine is on battery. Plug in and rerun."
    }
    $name = [System.IO.Path]::GetFileNameWithoutExtension($snapshot)
    $log = Join-Path $Out "$name.console.log"
    $meta = [ordered]@{
        snapshot          = (Resolve-Path $snapshot).Path
        snapshot_sha256   = (Get-FileHash -Algorithm SHA256 $snapshot).Hash.ToLowerInvariant()
        executable        = (Resolve-Path $Exe).Path
        executable_sha256 = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLowerInvariant()
        git_commit        = (git rev-parse HEAD).Trim()
        git_dirty         = [bool](git status --porcelain)
        cpu               = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        arguments         = @("--snapshot=$snapshot", "--out-dir=$Out", "--label=$name") + $BenchArgs
        power_before      = $before
    }
    & $Exe "--snapshot=$snapshot" "--out-dir=$Out" "--label=$name" @BenchArgs 2>&1 |
        Tee-Object -FilePath $log
    $meta.exit_code = $LASTEXITCODE
    $meta.power_after = Get-PowerState
    $runs += $meta
    $runs | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $Out "provenance.json")
}
