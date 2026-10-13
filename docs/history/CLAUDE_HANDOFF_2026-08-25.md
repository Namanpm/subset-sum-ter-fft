# TER Stage-C/P2 Handoff for Claude

**Repository:** `C:\Users\naman\Desktop\subset-sum-ter-fft`  
**Snapshot checked:** 2026-08-25  
**Purpose:** continue from the actual live state without repeating a very expensive benchmark campaign or assuming that Stage C changes the TER root-search algorithm.

## Executive truth

The **Stage-C/P2 root-partition substrate is implemented, benchmarked, and validated** in the working tree. It is **not committed**.

The Stage-C implementation is a one-digit partition operation over exact TER records:

```text
{ low_k quotient residue, EntryId }
        -> partition_chunk(...)
        -> CSR bucket offsets + same complete records rearranged by one radix digit
```

It does **not** implement a new full TER root algorithm, reduce the number of L1-to-L0 pair probes, recover witnesses, or replace the production root merge. `Region2Path` and `Adaptive` exist only as enum values and deliberately return `Status::NotApplicable`.

The candidate `BlockInPlace` backend lost the paired benchmark. **Keep `CurrentScatter` as the default. Do not install an adaptive policy or claim a solver speedup.**

## Current Git state

At the snapshot:

```text
HEAD:   3eac235 Implement bounded tiled TER root output policies and telemetry
branch: main...origin/main
```

The Stage-C work is present but uncommitted/untracked as follows:

| State | Files |
|---|---|
| Modified, unstaged | `CMakeLists.txt`, `include/ter/limitations.hpp`, `include/ter/types.hpp`, `tests/limitations_tests.cpp` |
| New, untracked implementation | `include/ter/root_partition.hpp`, `src/root_partition.cpp`, `src/root_partition_internal.hpp`, `src/root_partition_current_scatter.cpp`, `src/root_partition_block_inplace.cpp`, `tests/root_partition_tests.cpp` |
| New, untracked benchmark/support | `bench/ter_root_partition_bench.cpp`, `bench/ter_root_capture_adapter.hpp`, `bench/ter_root_capture_adapter.cpp`, `bench/analyse_root_partition.py`, `bench/run_task11_partition_bakeoff.ps1`, `results/` |
| Pre-existing staged donor; do **not** blindly commit | `ssp_oracle.cpp` |

`ssp_oracle.cpp` was explicitly preserved as a donor/reference input. The Stage-C source contains adapted mechanics, not a requirement to add the entire 17,672-line donor file.

## What was implemented

### 1. Public exact partition API

`include/ter/root_partition.hpp` defines:

- `TerPartitionEntry { QuotientResidue low_k; EntryId entry_id; }`, fixed at 8 bytes;
- `PartitionBackend`: `CurrentScatter`, `BlockInPlace`, `Region2Path`, `Adaptive`;
- `PartitionSpec`, `PartitionStats`, and reusable `PartitionWorkspace`;
- `partition_chunk(...)`, which mutates the complete entries and returns CSR bucket offsets.

Supported behavior:

- `CurrentScatter`: physical digit widths 8, 12, or 16 bits;
- `BlockInPlace`: physical digit width 8 only, with power-of-two record-aligned block size;
- `Region2Path` and `Adaptive`: explicitly return `NotApplicable`;
- invalid ranges/caps/configuration return the project `Status` rather than silently doing something else.

### 2. CurrentScatter baseline

`src/root_partition_current_scatter.cpp` implements the frozen baseline:

- serial histogram -> exclusive prefix -> complete-record scatter;
- parallel worker-pool variant with disjoint output regions;
- exact `EntryId` preservation and complete-record movement;
- scratch-cap accounting and detailed timing/memory telemetry.

### 3. BlockInPlace candidate

`src/root_partition_block_inplace.cpp` implements an IPS2Ra-derived, portable, one-digit radix-8 in-place partition candidate:

- local classification;
- block permutation;
- margin/partial-record cleanup;
- serial and portable parallel execution;
- bounded block-buffer-based accounting; no N-record scratch array.

It is correct as a partition implementation, but it is slower and consumes more scratch on the representative captured-root workload.

### 4. Tests, benchmark harness, and genuine TER adapter

- `tests/root_partition_tests.cpp` performs exact CSR, digit, and full `(low_k, EntryId)` membership comparisons between the backends, including boundaries, hot/one-bucket inputs, shifts, block sizes, and parallel cases.
- `bench/ter_root_partition_bench.cpp` benchmarks only the timed `partition_chunk` call; independent reference construction, input reset, verification, capture, and JSON output remain outside the timed interval.
- `bench/ter_root_capture_adapter.*` replays genuine captured Full TER root A/B/C lists from a deterministic n=72 FullTer attempt.
- `bench/analyse_root_partition.py` pairs backend rows and reports bootstrap confidence intervals, timing ratios, and scratch ratios.
- `bench/run_task11_partition_bakeoff.ps1` drives the full fixed Task-11 matrix and protects against overwriting output or executable changes mid-campaign.

## Validation actually available

### Current check performed at this handoff

```powershell
ctest --test-dir build-root-v2 -C Release -R '^ter_root_partition_tests$' --output-on-failure
```

Result: **1/1 passed** in 4.81 seconds.

### Previously recorded final validation

The existing decision record reports:

```powershell
python -m py_compile bench\analyse_root_partition.py
python bench\analyse_root_partition.py --self-test
cmake --build build-root-v2 --config Release --parallel
ctest --test-dir build-root-v2 -C Release --output-on-failure --timeout 300
```

Recorded result: analyzer self-test passed; Release build passed; **13/13 CTest tests passed** in 16.85 seconds. This is documented evidence from the existing decision record; it was not re-run in full during this handoff.

An earlier correctness addendum also records a Debug MSVC ASan `1/1` focused root-partition pass. Treat that as historical evidence; rerun ASan if changing the partition code.

## Full paired benchmark: completed result

Artifacts are under:

```text
results/partition/task11-final-20260825/
```

Campaign configuration:

- synthetic sizes: 20,000; 32,496; 64,000; 500,000; 1,000,000; 2,000,000; 5,000,000;
- distributions: uniform, hot, alternating, sorted, reverse, duplicates;
- requested threads: 1, 2, 4, 8, 16;
- warmups: 5; measured repetitions: 30;
- candidate block sizes: 1024, 2048, 4096 bytes;
- genuine captured TER replay: A/B/C lists from n=72, with sizes A=19,109, B=32,496, C=33,505; r1=25; Q=47.

All required 5,000,000-entry synthetic cells completed. The command manifest preserves 172 expanded commands with their exit codes and output paths. The executable SHA-256 stayed unchanged throughout the campaign.

### Decision

Speedup direction is:

```text
CurrentScatter elapsed / BlockInPlace elapsed
```

So **below 1.0 means BlockInPlace is slower**.

| Captured-root block size | Geometric speedup | 95% CI | Candidate time relative to CurrentScatter | Median candidate/current scratch |
|---:|---:|---:|---:|---:|
| 1024 bytes | 0.5542x | [0.5346, 0.5740] | 1.8045x slower | 5.1712x |
| 2048 bytes | 0.5146x | [0.4986, 0.5313] | 1.9431x slower | 9.9417x |
| 4096 bytes | 0.4520x | [0.4371, 0.4670] | 2.2124x slower | 19.4841x |

Therefore:

```text
Decision: BlockInPlace remains experimental.
Default:  CurrentScatter.
Adaptive: remains NotApplicable.
```

This is a **partition-kernel decision only**. It does not say the complete TER solver is faster or slower, because Stage C does not change root pair-work.

## Important existing documents

- `implementation_plan.md`: the Stage-C/P2 implementation plan. It has 12 tasks and explicitly stops after Stage C.
- `docs/root_v2_baseline.md`: pre-edit source audit, FullTer n=72 baseline, Task-6 CurrentScatter baseline, and BlockInPlace correctness addendum.
- `docs/root_partition_decision.md`: complete campaign methodology, raw-artifact hashes, gate rules, and final backend decision.
- `TER_Hybrid_Partition_Backend_v2.0_CODEX.pdf` and `TER_L1_to_L0_Radix_Refined_Root_Architecture_v2.1_CODEX.pdf`: architectural source documents.

## What is not done, and must not be misrepresented as done

1. **No production-root replacement.** `full_ter.cpp` still uses its existing root path.
2. **No root pair-space reduction.** Partitioning improves record arrangement only; it does not turn the 621-million-probe n=72 root merge into fewer mathematical pairs.
3. **No `Region2Path`.** It is an enum placeholder returning `NotApplicable`.
4. **No `Adaptive` backend/threshold.** The data explicitly rejects promotion of BlockInPlace, so no automatic selection rule should be installed from this campaign.
5. **No witness-recovery, exact region-pruning, carry-refinement, BBF integration, or new later-stage root algorithm** was implemented as part of this work.
6. **No commit or push was created for Stage C.** The changes must be reviewed and selectively staged before committing. In particular, do not accidentally include the pre-existing staged `ssp_oracle.cpp`.

## Recommended Claude continuation

Do this before any new code:

```powershell
Set-Location 'C:\Users\naman\Desktop\subset-sum-ter-fft'
git status --short --branch
git diff -- CMakeLists.txt include/ter/limitations.hpp include/ter/types.hpp tests/limitations_tests.cpp
git diff --no-index -- NUL include/ter/root_partition.hpp
ctest --test-dir build-root-v2 -C Release -R '^ter_root_partition_tests$' --output-on-failure
```

Then choose one of two paths deliberately:

1. **Land Stage C cleanly:** review the uncommitted files, run the full Release suite and ASan if changes occur, stage only Stage-C files plus results/docs that are intentionally versioned, and keep `ssp_oracle.cpp` out unless explicitly desired.
2. **Begin a new research stage:** write a fresh spec first. It must state how it reduces root pair-work or recovers witnesses and must be benchmarked end-to-end against the existing root path. Do not re-run the 5M partition bake-off merely to rediscover that BlockInPlace loses.

## Plain-English status

The work that was actually finished is a correct, tested, and extensively benchmarked **root-record partition component**. The experiment answered its own question: the fancy in-place candidate loses to the existing scatter backend on genuine TER root lists. The TER solver itself remains unchanged by this component. The next meaningful work is either a careful commit of Stage C or a separately specified root-search algorithm that reduces pair-work; it is not more tuning of the rejected BlockInPlace candidate.
