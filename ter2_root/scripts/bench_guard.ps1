# Benchmark guard: protects and audits a running TER root benchmark
# (ter_arwlf_campaign / ter_arwlf_whole_root).  Start it in a second PowerShell
# window (as Administrator for full effect) next to the benchmark; stop it with
# Ctrl+C after the benchmark is done.  It changes nothing about the benchmark's
# computation.
#
#   pwsh -NoProfile -File ter2_root/scripts/bench_guard.ps1                 # protect + log
#   pwsh -NoProfile -File ter2_root/scripts/bench_guard.ps1 -SuspendHeavy   # + pause compilers/tests
#   pwsh -NoProfile -File ter2_root/scripts/bench_guard.ps1 -Observe        # log only, change nothing
#
# Every interval, while a benchmark process is running:
#   - raises it to High priority (never Realtime) and locks its working set in
#     RAM (hard minimum = its private bytes) so Windows cannot trim or compress it;
#   - moves every other process it may touch off the reserved core (the
#     benchmark's pinned P-core, -ReservedCpu, default 23 as recorded in every
#     run's pinned_cpu);
#   - with -SuspendHeavy, suspends compilers / build tools / test runners / other
#     ter_* programs until no benchmark is running.
# Always: holds bench.lock at the repository root (heavy work must not start
# while it exists), keeps the machine awake, and appends one row per interval
# to results/arwlf/bench_guard.csv (UTC time, foreign CPU in cores (per process,
# plus whole-machine busy cores, which also covers protected processes), the top
# foreign processes, heavy processes present, the benchmark's working set and
# page faults, CPU performance %, free memory, AC state, actions taken).
# On exit (Ctrl+C included) it restores every affinity it changed, resumes what
# it suspended, releases the working-set lock and removes bench.lock.
# It never changes the power plan or Defender settings (that would change the
# conditions the earlier results were measured under); it logs the power plan.

param(
    [int]$IntervalSeconds = 5,
    [int]$ReservedCpu = 23,
    [switch]$SuspendHeavy,
    [switch]$Observe,
    [int]$MaxIntervals = 0,  # 0 = until Ctrl+C (testing only)
    [string]$LogPath = ""    # default results/arwlf/bench_guard.csv
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$lock = Join-Path $root "bench.lock"
$log = if ($LogPath) { $LogPath } else { Join-Path $root "results/arwlf/bench_guard.csv" }
$benchPattern = '^ter_arwlf_(campaign|whole_root)$|^ter_.*_bench$'
$heavyPattern = '^(cl|link|lld-link|clang|clang-cl|clang\+\+|MSBuild|cmake|ctest|ninja|devenv|vctip|mspdbsrv)$|^ter_'
$reservedBit = [int64]1 -shl $ReservedCpu

Add-Type -Namespace TerGuard -Name Native -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr handle);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr handle);
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
[StructLayout(LayoutKind.Sequential)] public struct PowerStatus {
  public byte ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag; public int BatteryLifeTime, BatteryFullLifeTime;
}
[DllImport("kernel32.dll")] public static extern bool GetSystemPowerStatus(out PowerStatus status);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetProcessWorkingSetSizeEx(IntPtr process, UIntPtr minimum, UIntPtr maximum, uint flags);
'@
$ES_CONTINUOUS = [uint32]"0x80000000"; $ES_SYSTEM_REQUIRED = [uint32]1
$QUOTA_LIMITS_HARDWS_MIN_ENABLE = [uint32]1; $QUOTA_LIMITS_HARDWS_MIN_DISABLE = [uint32]2
$QUOTA_LIMITS_HARDWS_MAX_DISABLE = [uint32]8

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$mode = if ($Observe) { "observe" } else { "protect" }

# --- bench.lock --------------------------------------------------------------
if (Test-Path $lock) {
    $owner = (Get-Content $lock -ErrorAction SilentlyContinue | Select-Object -First 1) -as [int]
    if ($owner -and (Get-Process -Id $owner -ErrorAction SilentlyContinue)) {
        throw "Another bench_guard (pid $owner) already holds $lock."
    }
}
"$PID`n$((Get-Date).ToUniversalTime().ToString('o'))" | Set-Content -Encoding utf8 $lock

$me = Get-Process -Id $PID
$me.PriorityClass = "BelowNormal"
$changedAffinity = @{}   # "pid|startTicks" -> @{ process; original }
$suspended = @{}         # same key -> process
$wsLocked = @{}          # same key -> process
$lastCpu = @{}           # same key -> total processor seconds
$lastTime = Get-Date
$header = "utc,mode,admin,interval_s,benchmarks,bench_cpu_cores,bench_ws_gib,bench_private_gib,bench_page_faults_per_s," +
          "foreign_cpu_cores,machine_busy_cores,guard_cpu_cores,top_foreign,heavy,suspended,reserved_cpu_perf_pct,total_perf_pct," +
          "free_mem_gib,on_ac,affinity_moved,affinity_failed,ws_locked,power_plan"
if (-not (Test-Path $log)) { New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null; $header | Set-Content -Encoding utf8 $log }
$plan = ((powercfg /getactivescheme) -join " ") -replace '.*\(([^)]*)\).*', '$1'

function Get-Key($p) { try { "$($p.Id)|$($p.StartTime.Ticks)" } catch { $null } }

function Restore-All {
    foreach ($entry in $changedAffinity.Values) {
        try { if (-not $entry.process.HasExited) { $entry.process.ProcessorAffinity = $entry.original } } catch {}
    }
    foreach ($p in $suspended.Values) {
        try { if (-not $p.HasExited) { [void][TerGuard.Native]::NtResumeProcess($p.Handle) } } catch {}
    }
    foreach ($p in $wsLocked.Values) {
        try {
            if (-not $p.HasExited) {
                # Back to Windows' default soft limits.  Never pass (-1, -1): that empties the working set.
                [void][TerGuard.Native]::SetProcessWorkingSetSizeEx($p.Handle, [UIntPtr]::new([uint64]204800),
                    [UIntPtr]::new([uint64]1413120), $QUOTA_LIMITS_HARDWS_MIN_DISABLE -bor $QUOTA_LIMITS_HARDWS_MAX_DISABLE)
            }
        } catch {}
    }
    $changedAffinity.Clear(); $suspended.Clear(); $wsLocked.Clear()
}

Write-Host "bench_guard: mode=$mode admin=$admin reserved cpu=$ReservedCpu suspend-heavy=$([bool]$SuspendHeavy) log=$log"
if (-not $admin -and -not $Observe) {
    Write-Host "  (not elevated: system processes cannot be moved; run as Administrator for full effect)"
}
[void][TerGuard.Native]::SetThreadExecutionState($ES_CONTINUOUS -bor $ES_SYSTEM_REQUIRED)
foreach ($p in Get-Process) {
    $key = Get-Key $p
    if ($key) { try { $lastCpu[$key] = $p.TotalProcessorTime.TotalSeconds } catch {} }
}
$lastTime = Get-Date
$intervals = 0
try {
    while ($MaxIntervals -eq 0 -or $intervals -lt $MaxIntervals) {
        Start-Sleep -Seconds $IntervalSeconds
        $intervals++
        $now = Get-Date
        $elapsed = ($now - $lastTime).TotalSeconds
        $lastTime = $now
        $all = Get-Process
        $bench = @($all | Where-Object { $_.ProcessName -match $benchPattern })
        $benchIds = @($bench | ForEach-Object { $_.Id })
        $benchCpu = 0.0; $foreignCpu = 0.0; $guardCpu = 0.0
        $foreign = @()
        $seen = @{}
        foreach ($p in $all) {
            if ($p.Id -eq 0 -or $p.Id -eq 4) { continue }
            $key = Get-Key $p
            if (-not $key) { continue }
            $seen[$key] = $true
            $cpu = $null
            try { $cpu = $p.TotalProcessorTime.TotalSeconds } catch {}
            if ($null -ne $cpu) {
                $delta = if ($lastCpu.ContainsKey($key)) { [math]::Max(0.0, $cpu - $lastCpu[$key]) / $elapsed } else { 0.0 }
                $lastCpu[$key] = $cpu
                if ($benchIds -contains $p.Id) { $benchCpu += $delta }
                elseif ($p.Id -eq $PID) { $guardCpu += $delta }
                else { $foreignCpu += $delta; if ($delta -gt 0.02) { $foreign += [pscustomobject]@{ n = $p.ProcessName; c = $delta } } }
            }
        }
        foreach ($k in @($lastCpu.Keys)) { if (-not $seen.ContainsKey($k)) { $lastCpu.Remove($k) } }
        $heavy = @($all | Where-Object { $_.ProcessName -match $heavyPattern -and $benchIds -notcontains $_.Id })

        $moved = 0; $failed = 0
        if (-not $Observe -and $bench.Count -gt 0) {
            foreach ($b in $bench) {
                try { if ($b.PriorityClass -ne "High") { $b.PriorityClass = "High" } } catch { $failed++ }
                $key = Get-Key $b
                if ($key -and -not $wsLocked.ContainsKey($key)) {
                    $min = [uint64]$b.PrivateMemorySize64 + 256MB
                    if ([TerGuard.Native]::SetProcessWorkingSetSizeEx($b.Handle, [UIntPtr]::new($min), [UIntPtr]::new($min * 2),
                            $QUOTA_LIMITS_HARDWS_MIN_ENABLE -bor $QUOTA_LIMITS_HARDWS_MAX_DISABLE)) {
                        $wsLocked[$key] = $b
                    } else { $failed++ }
                }
            }
            foreach ($p in $all) {
                if ($benchIds -contains $p.Id -or $p.Id -eq 0 -or $p.Id -eq 4) { continue }
                try {
                    $mask = [int64]$p.ProcessorAffinity
                    if (($mask -band $reservedBit) -ne 0 -and ($mask -band -bnot $reservedBit) -ne 0) {
                        $key = Get-Key $p
                        if ($key -and -not $changedAffinity.ContainsKey($key)) { $changedAffinity[$key] = @{ process = $p; original = $p.ProcessorAffinity } }
                        $p.ProcessorAffinity = [IntPtr]($mask -band -bnot $reservedBit)
                        $moved++
                    }
                } catch { $failed++ }
            }
            if ($SuspendHeavy) {
                foreach ($h in $heavy) {
                    $key = Get-Key $h
                    if ($key -and -not $suspended.ContainsKey($key)) {
                        try { [void][TerGuard.Native]::NtSuspendProcess($h.Handle); $suspended[$key] = $h } catch { $failed++ }
                    }
                }
            }
        }
        if ($bench.Count -eq 0 -and ($suspended.Count -gt 0 -or $changedAffinity.Count -gt 0 -or $wsLocked.Count -gt 0)) {
            Restore-All   # benchmark finished: give everything back
        }

        $benchWs = 0.0; $benchPrivate = 0.0; $faults = ""
        foreach ($b in $bench) { $b.Refresh(); $benchWs += $b.WorkingSet64 / 1GB; $benchPrivate += $b.PrivateMemorySize64 / 1GB }
        $reservedPerf = ""; $totalPerf = ""; $machineBusy = ""; $free = ""
        try {
            $paths = @("\Processor Information(0,$ReservedCpu)\% Processor Performance",
                       "\Processor Information(_Total)\% Processor Performance",
                       "\Processor(_Total)\% Processor Time",
                       "\Memory\Available MBytes")
            if ($bench.Count -gt 0) { $paths += "\Process($($bench[0].ProcessName))\Page Faults/sec" }
            $samples = (Get-Counter -Counter $paths -MaxSamples 1 -ErrorAction Stop).CounterSamples
            $reservedPerf = [math]::Round($samples[0].CookedValue, 1)
            $totalPerf = [math]::Round($samples[1].CookedValue, 1)
            $machineBusy = [math]::Round($samples[2].CookedValue / 100.0 * [Environment]::ProcessorCount, 3)
            $free = [math]::Round($samples[3].CookedValue / 1024.0, 2)
            if ($samples.Count -gt 4) { $faults = [math]::Round($samples[4].CookedValue, 0) }
        } catch {}
        # No WMI/CIM in the loop: WMI queries wake wmiprvse, itself a CPU load.
        $power = New-Object TerGuard.Native+PowerStatus
        $onAc = if ([TerGuard.Native]::GetSystemPowerStatus([ref]$power)) { if ($power.ACLineStatus -eq 255) { "" } else { [int]$power.ACLineStatus } } else { "" }
        $top = ($foreign | Sort-Object c -Descending | Select-Object -First 3 | ForEach-Object { "{0}:{1:N2}" -f $_.n, $_.c }) -join " "
        $row = @(
            $now.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"), $mode, [int]$admin, [math]::Round($elapsed, 2),
            (($bench | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join " "), [math]::Round($benchCpu, 3),
            [math]::Round($benchWs, 2), [math]::Round($benchPrivate, 2), $faults, [math]::Round($foreignCpu, 3),
            $machineBusy, [math]::Round($guardCpu, 3), "`"$top`"", "`"$((($heavy | ForEach-Object { $_.ProcessName }) | Sort-Object -Unique) -join ' ')`"",
            $suspended.Count, $reservedPerf, $totalPerf, $free, $onAc, $moved, $failed, $wsLocked.Count, "`"$plan`"")
        Add-Content -Encoding utf8 $log ($row -join ",")
        Write-Host ("{0}  bench={1} cpu {2:N2}  foreign {3:N2} cores  heavy=[{4}]  ws {5:N2}/{6:N2} GiB  free {7} GiB" -f
            $now.ToString("HH:mm:ss"), $bench.Count, $benchCpu, $foreignCpu,
            ((($heavy | ForEach-Object { $_.ProcessName }) | Sort-Object -Unique) -join ' '), $benchWs, $benchPrivate, $free)
    }
} finally {
    Restore-All
    [void][TerGuard.Native]::SetThreadExecutionState($ES_CONTINUOUS)
    Remove-Item -Force $lock -ErrorAction SilentlyContinue
    Write-Host "bench_guard: restored affinities, resumed suspended processes, released bench.lock"
}
