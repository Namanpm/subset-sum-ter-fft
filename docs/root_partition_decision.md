# TER root partition backend decision

Date: 2026-08-25

## Decision

**C. BlockInPlace remains experimental; CurrentScatter remains the default.**

This is a finite, single-machine decision about the timed `partition_chunk`
operation. It is not a whole-attempt, whole-root-merge, or whole-solver speed
claim. `Adaptive` remains `NotApplicable`; this task installs no threshold.

## Gates fixed before applying the measurements

The required alternatives were applied as follows:

1. **Speed default (A):** on representative captured-root pairs, the lower
   paired-bootstrap 95% confidence bound for
   `current elapsed / candidate elapsed` must exceed `1.05`. The tail is
   acceptable only when
   `p95_candidate_over_current_elapsed_ratio <= 1.10`, both in aggregate and
   in every claimed thread regime.
2. **Memory backend (B):** the captured-root median
   `candidate/current scratch_peak_bytes` ratio must be at most `0.25` (at
   least a 4x reduction), and geometric-mean candidate/current elapsed time
   must be at most `1.10`.
3. **Otherwise (C):** BlockInPlace remains experimental and CurrentScatter
   stays the default.

The p95 criterion above was fixed before reading the final analyzer output. It
uses the required measured candidate/current time-ratio direction.

## Build and executable identity

```text
Git HEAD:                 3eac235f871c2e4cf07a73152644e6113eaf39e1
Branch:                   main
Generator:                Visual Studio 18 2026
Compiler:                 MSVC 19.51.36252.0
Configuration:            Release
TER_NATIVE_OPT:            ON
Benchmark executable:     build-root-v2/Release/ter_root_partition_bench.exe
Executable SHA-256 before campaign:
  964f202cb9b4d9438dca258ee0335254ebd2394938ed6f6f16d2444e78485fab
Executable SHA-256 after campaign:
  964f202cb9b4d9438dca258ee0335254ebd2394938ed6f6f16d2444e78485fab
```

The executable was not rebuilt or changed during the paired campaign.

## Exact campaign and commands

The campaign was launched once with:

```powershell
cd "C:\Users\naman\Desktop\subset-sum-ter-fft"
& .\bench\run_task11_partition_bakeoff.ps1 `
  -OutputRoot 'results\partition\task11-final-20260825'
```

The driver executes the following exact synthetic forms for every listed `N`
and distribution, serially and in separate files:

```powershell
$exe = '.\build-root-v2\Release\ter_root_partition_bench.exe'
$sizes = 20000,32496,64000,500000,1000000,2000000,5000000
$distributions = 'uniform','hot','alternating','sorted','reverse','duplicates'

& $exe --backend current-scatter --n <N> --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --distribution <distribution> `
  --seed 6840143426475650816 --warmup 5 --repeat 30 `
  --output '...\N-<N>\current\<distribution>.jsonl'

& $exe --backend block-inplace --n <N> --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --distribution <distribution> `
  --seed 6840143426475650816 --warmup 5 --repeat 30 `
  --block-bytes 1024 `
  --output '...\N-<N>\block-1024\<distribution>.jsonl'

& $exe --backend block-inplace --n <N> --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --distribution <distribution> `
  --seed 6840143426475650816 --warmup 5 --repeat 30 `
  --block-bytes 2048 `
  --output '...\N-<N>\block-2048\<distribution>.jsonl'

& $exe --backend block-inplace --n <N> --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --distribution <distribution> `
  --seed 6840143426475650816 --warmup 5 --repeat 30 `
  --block-bytes 4096 `
  --output '...\N-<N>\block-4096\<distribution>.jsonl'
```

The four genuine captured-root commands were:

```powershell
& $exe --dataset captured-root --backend current-scatter --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --warmup 5 --repeat 30 --root-n 72 `
  --root-seed 19797034556212562 --root-mode full --list all `
  --output '...\captured-root\current.jsonl'

& $exe --dataset captured-root --backend block-inplace --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --warmup 5 --repeat 30 --root-n 72 `
  --root-seed 19797034556212562 --root-mode full --list all `
  --block-bytes 1024 --output '...\captured-root\block-1024.jsonl'

& $exe --dataset captured-root --backend block-inplace --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --warmup 5 --repeat 30 --root-n 72 `
  --root-seed 19797034556212562 --root-mode full --list all `
  --block-bytes 2048 --output '...\captured-root\block-2048.jsonl'

& $exe --dataset captured-root --backend block-inplace --bits 8 --shift 0 `
  --threads 1,2,4,8,16 --warmup 5 --repeat 30 --root-n 72 `
  --root-seed 19797034556212562 --root-mode full --list all `
  --block-bytes 4096 --output '...\captured-root\block-4096.jsonl'
```

Every one of the 172 expanded benchmark commands, including its output path,
exit code, wall time, and diagnostic, is preserved verbatim in
`results/partition/task11-final-20260825/command-manifest.jsonl`.

Strict analyses used:

```powershell
python bench\analyse_root_partition.py --group-by-threads `
  --output results\partition\task11-final-20260825\analysis\synthetic-block-<block>.json `
  <all sorted CurrentScatter synthetic JSONL files> `
  <all sorted block-<block> synthetic JSONL files>

python bench\analyse_root_partition.py --group-by-threads `
  --output results\partition\task11-final-20260825\analysis\captured-block-<block>.json `
  results\partition\task11-final-20260825\captured-root\current.jsonl `
  results\partition\task11-final-20260825\captured-root\block-<block>.jsonl
```

`<block>` was run separately as `1024`, `2048`, and `4096`; no analysis mixed
candidate block sizes.

## Matrix coverage

| Dataset | Logical workload cells per backend | Repetitions per cell | Rows per backend | Omitted cells |
| --- | ---: | ---: | ---: | ---: |
| Synthetic: 7 N x 6 distributions x 5 threads | 210 | 30 | 6,300 | 0 |
| Captured A/B/C x 5 threads | 15 | 30 | 450 | 0 |

The four synthetic backend/block regimes produced 25,200 rows in 168 files.
The four captured regimes produced 1,800 rows in four files. All required N,
including 5,000,000, completed. No machine constraint was declared or used to
drop a cell.

All four genuine runs reported:

```text
capture_status=Completed
capture_solver_status=AttemptLimitReached
A=19109, B=32496, C=33505, r1=25, Q=47
```

The first-capture generation measurements were 1,774,838,100 ns (current),
1,800,480,700 ns (1024), 1,796,849,300 ns (2048), and 1,785,393,800 ns
(4096). Capture generation and the deterministic replay capture were outside
the timed partitions and outside pair identity.

## Measured results

All speedups are `current elapsed / candidate elapsed`; values below one mean
BlockInPlace is slower. Scratch ratios are `candidate/current`.

### Captured-root aggregate

| Block bytes | Pairs | Geometric speedup | 95% CI | Median speedup | p95 candidate/current time | Median scratch ratio | Worst scratch ratio | Geomean candidate/current time |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1024 | 450 | 0.5542 | [0.5346, 0.5740] | 0.5480 | 3.2744 | 5.1712 | 20.5806 | 1.8045 |
| 2048 | 450 | 0.5146 | [0.4986, 0.5313] | 0.4895 | 3.1663 | 9.9417 | 40.6082 | 1.9431 |
| 4096 | 450 | 0.4520 | [0.4371, 0.4670] | 0.4449 | 3.7818 | 19.4841 | 80.6644 | 2.2124 |

### Captured-root result by block size and thread regime

| Block | Threads | Geometric speedup | 95% CI | Median speedup | p95 candidate/current | Median / worst scratch ratio | Gate result |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1024 | 1 | 0.6111 | [0.5608, 0.6667] | 0.5494 | 2.6995 | 2.0417 / 3.4334 | C |
| 1024 | 2 | 0.6286 | [0.5906, 0.6711] | 0.6084 | 2.3158 | 3.1417 / 5.1712 | C |
| 1024 | 4 | 0.5674 | [0.5179, 0.6195] | 0.5848 | 3.1433 | 4.9571 / 8.0064 | C |
| 1024 | 8 | 0.4857 | [0.4504, 0.5242] | 0.4805 | 3.5918 | 8.2825 / 12.9319 | C |
| 1024 | 16 | 0.4937 | [0.4647, 0.5257] | 0.4680 | 3.2788 | 13.9271 / 20.5806 | C |
| 2048 | 1 | 0.5765 | [0.5419, 0.6133] | 0.5508 | 2.6927 | 4.0339 / 6.7856 | C |
| 2048 | 2 | 0.5475 | [0.5077, 0.5907] | 0.5092 | 2.9952 | 6.0388 / 9.9417 | C |
| 2048 | 4 | 0.5596 | [0.5224, 0.6019] | 0.5431 | 2.7920 | 9.6477 / 15.5841 | C |
| 2048 | 8 | 0.4621 | [0.4306, 0.4960] | 0.4485 | 3.4669 | 16.2582 / 25.3865 | C |
| 2048 | 16 | 0.4423 | [0.4197, 0.4671] | 0.4275 | 3.2227 | 27.4791 / 40.6082 | C |
| 4096 | 1 | 0.4374 | [0.4123, 0.4635] | 0.4264 | 3.6511 | 8.0199 / 13.4914 | C |
| 4096 | 2 | 0.5016 | [0.4629, 0.5439] | 0.4813 | 3.1625 | 11.8345 / 19.4841 | C |
| 4096 | 4 | 0.5118 | [0.4776, 0.5507] | 0.4781 | 3.2437 | 19.0302 / 30.7408 | C |
| 4096 | 8 | 0.4098 | [0.3759, 0.4455] | 0.4279 | 4.1428 | 32.2110 / 50.2968 | C |
| 4096 | 16 | 0.4098 | [0.3858, 0.4367] | 0.3934 | 3.6600 | 54.5843 / 80.6644 | C |

Every thread regime fails A because its entire confidence interval is below
one and its p95 tail is above 1.10. Every regime fails B because scratch is
higher, not 4x lower, and candidate time is well above the 1.10 ceiling.

### Synthetic scaling context

| Block bytes | Pairs | Geometric speedup | 95% CI | Median speedup | p95 candidate/current time | Median / worst scratch ratio |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1024 | 6,300 | 0.6350 | [0.6283, 0.6417] | 0.6447 | 3.2734 | 0.5734 / 19.9463 |
| 2048 | 6,300 | 0.6268 | [0.6193, 0.6348] | 0.6613 | 4.0576 | 1.1287 / 39.3567 |
| 4096 | 6,300 | 0.6171 | [0.6095, 0.6247] | 0.6587 | 3.8327 | 2.2406 / 78.1783 |

The synthetic sweep also makes no speed case. Its scratch ratios vary strongly
with N and thread count because fixed per-worker block buffers dominate small
inputs. This context does not override the representative captured-root gate.

## Gate evaluation and global result

- **A fails for every block size and thread regime.** The best captured lower
  CI is 0.5906, not greater than 1.05. The best captured p95 ratio is 2.3158,
  not at most 1.10.
- **B fails for every block size and thread regime.** Captured median scratch
  ratios range from 2.0417 to 54.5843; none is at most 0.25. Aggregate
  candidate time is 1.8045x to 2.2124x CurrentScatter by geometric mean.
- Therefore **C is the only gate-consistent result.** BlockInPlace remains an
  experimental backend and CurrentScatter remains the default.

No `Adaptive` threshold is recommended from these data, and none is installed.

## What was and was not measured

Measured:

- verified `partition_chunk` wall time on identical input records;
- backend scratch telemetry;
- exact paired fingerprints, status, and effective thread counts;
- synthetic finite-N behavior and genuine captured A/B/C behavior.

Not charged to timed partitions:

- synthetic generation and copying;
- independent reference verification and JSON I/O;
- genuine root capture generation and deterministic replay capture.

Not measured or proven:

- any change in root pair-work;
- L1-to-L0 root-merge end-to-end time;
- whole-attempt or whole-solver speedup;
- an asymptotic improvement;
- a modeled crossover. No modeled sensitivity was used in this decision.

## Strict validation and rerun

The analyzer accepted all three 6,300-pair synthetic comparisons and all three
450-pair captured comparisons. Acceptance proves matching pair identity,
input fingerprint, canonical output fingerprint, completed status, capture
metadata, and effective thread count for every compared row.

A post-campaign cell was selected with the platform cryptographic RNG:

```text
N=32496, distribution=reverse, threads=2, block_bytes=2048
```

CurrentScatter and BlockInPlace were each rerun with warmup 5 and repeat 30.
All 30 semantic rows per backend matched the original rows exactly after
excluding timing-only fields. Executable SHA-256 was unchanged before/after
the rerun.

Final Release validation:

```powershell
python -m py_compile bench\analyse_root_partition.py
python bench\analyse_root_partition.py --self-test
cmake --build build-root-v2 --config Release --parallel
ctest --test-dir build-root-v2 -C Release --output-on-failure --timeout 300
```

Results: analyzer self-test passed; full build passed; CTest passed 13/13 in
16.85 seconds.

## Artifact hashes and actual analyzer JSON

```text
command-manifest.jsonl SHA-256:
  5fc7ab9b2b0f800d66a5175562209f7b5ab4c7fa5a91cd1d3864dc0a9801be2a
data-hashes.jsonl SHA-256 (173 per-file raw-data/command entries):
  9b252c8ce9d126ff9d937456b4e84e95daf306a57cfcd643990982c0677f8958

analysis/captured-block-1024.json:
  62e512632806d39d783d33194fab1ad48b4ecbdf1604755b900333f88c371c53
analysis/captured-block-2048.json:
  1ca365def0648553a743ddedf7174c58216d2dc24cc5011265b4d2dfb27a1a5e
analysis/captured-block-4096.json:
  904a3a16746cf5f7b5b8b7ad9f007075344e82ec73671fb005faf32c1e13e299
analysis/synthetic-block-1024.json:
  04cc7e2b84d4ef5290d505c65e5ac8100c28a2dbfae11cc9bea1a2dc0947bc02
analysis/synthetic-block-2048.json:
  4c503fbde70fe9f88f9798b17f4093af60646048913caf1b7ca1d1011affcedb
analysis/synthetic-block-4096.json:
  1ee29c7f618ee9eb2bfa477100a2afb6a2d93d186cc1ae28b8bdf39597c3aa38

rerun current.jsonl:
  39c477ac4c7ffb402b30aec5eabc1b3f2ebe91f5a5d38e55c973289336fe4c1d
rerun block-2048.jsonl:
  d67d4b4a5dd3ca84dc07dee005078ee8d6b7112f629c3f7509d73e95fd8bcf4e
rerun analysis.json:
  68016612f405a74b896ac0eb5ffae82b453242c2940f289d6fb93dd0ecc6d77d
```

The six named `analysis/*.json` files are the actual analyzer JSON, including
the aggregate and all `by_threads_requested` results. They are retained beside
the raw files; the tables above are rounded renderings of those exact values,
not separately recomputed estimates.
