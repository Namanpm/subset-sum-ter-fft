# TER Root v2 / Stage-C P2 Baseline

Recorded before source changes for `implementation_plan.md`.

## Repository state

- Repository HEAD: `3eac235f871c2e4cf07a73152644e6113eaf39e1`.
- Branch: `main...origin/main`.
- Pre-existing staged file preserved without modification: `ssp_oracle.cpp`.
- Unique local radix donor: `C:\Users\naman\Desktop\subset-sum-ter-fft\ssp_oracle.cpp` (687,834 bytes).
- Read-only IPS2Ra donor: `C:\Users\naman\Desktop\ips2ra`, HEAD `ef3407632348f58a6ab75832a749cde99f3b9ad8`.

## Pre-edit validation

Command:

```powershell
cmake -S . -B build-baseline
cmake --build build-baseline --config Release --parallel
ctest --test-dir build-baseline -C Release --output-on-failure
```

Result: Release build succeeded and all 12 CTest tests passed.

## FullTer n=72 baseline

Command:

```powershell
.\build-baseline\Release\ter_full_bench.exe --n=72
```

Environment: MSVC 19.51, Release, native optimization off, one thread, `FullTer`, random target, finite-sanity family, audit off.

| Field | Observed value |
|---|---:|
| Terminal status | `AttemptLimitReached` |
| `r1`, `r2` | 25, 7 |
| L1 sizes A:B:C | 19,109 : 32,496 : 33,505 |
| Logical root pair probes | 620,966,064 |
| Root pair records | 0 |
| Root quotient bits | 47 |
| Root low-prefilter bits | 24 |
| Low-prefilter candidates | 1,238,960 |
| Full-quotient rejects | 1,238,960 |
| Root tiled scan | 622,690,700 ns |
| Total solver time | 769,630,200 ns |
| Root tiled auxiliary bytes | 3,383,100 |
| Full solver peak working bytes | 14,783,716 |

This is a baseline measurement, not evidence for a Stage-C speedup. `AttemptLimitReached` remains an incomplete solver outcome, not UNSAT.

## Source transplant map

| Donor | Donor source/function | TER destination | Modification | Why | Form |
|---|---|---|---|---|---|
| Local `ssp_oracle.cpp` | `RadixWorkerState` | `src/root_partition_current_scatter.cpp::PartitionWorkerState` | Change value-only buffers to `TerPartitionEntry` movement buffers. | TER must preserve `{low_k, EntryId}` records. | Adapted copy. |
| Local `ssp_oracle.cpp` | `RadixWorkerPool` | `src/root_partition_current_scatter.cpp::PartitionWorkerPool` | Remove Oracle ownership only; retain generation, condition-variable, and synchronous-pass mechanics. | TER has no Oracle object. | Adapted copy. |
| Local `ssp_oracle.cpp` | `radix_sort_unsigned_values` | `partition_current_scatter_serial` | Stop after one requested digit; retain histogram, exclusive prefix, cursor scatter. | Stage C is a partition, not a complete sort. | Adapted copy. |
| Local `ssp_oracle.cpp` | `parallel_radix_sort_unsigned_values` | `partition_current_scatter_parallel` | Classify `entry.low_k`, move complete records, accept 8/12/16 bits. | Preserve donor's bucket-major/worker-major disjoint scatter. | Adapted copy. |
| Local `ssp_oracle.cpp` | `radix_worker_state_bytes` and timing updates | partition workspace accounting/stats | Rename telemetry and report record/capacity bytes. | TER's `PartitionStats` is the public telemetry contract. | Adapted copy. |
| IPS2Ra `partitioning.hpp` | `Sorter::partition` phase order | `src/root_partition_block_inplace.cpp` orchestration | Remove recursive sort and scheduler calls. | TER needs exactly one physical digit. | Adapted copy. |
| IPS2Ra `local_classification.hpp` | `classifyLocally`, `sequentialClassification` | BlockInPlace local classification | Replace generic classifier with `low_k` digit extraction and TER records. | The root partition key is fixed by the public contract. | Adapted copy. |
| IPS2Ra `block_permutation.hpp` | `computeOverflowBucket`, `classifyAndReadBlock`, `swapBlock`, `permuteBlocks` | BlockInPlace block permutation | Retain displaced-block chain control flow; remove radix recursion. | Required one-level in-place redistribution machinery. | Adapted copy. |
| IPS2Ra `cleanup_margins.hpp` | `saveMargins`, `writeMargins` | BlockInPlace margin cleanup | Retain partial-buffer/overflow repair; remove base-case sort. | Stability and full sorting are outside Stage C. | Adapted copy. |
| IPS2Ra `bucket_pointers.hpp` | `BucketPointers` | TER `BucketBlockPointers` | Use serial pointers first; use the donor's mutex-compatible portable branch for parallel coordination. | MSVC/Ter must not require `unsigned __int128` atomics. | Adapted copy. |
| IPS2Ra `buffers.hpp` / `config.hpp` | `Block`, `Buffers`, block geometry | TER block buffers/geometry helpers | Fix at one requested radix-8 digit; use TER's 8-byte record. | Required per-bucket buffering and 2,048-byte default. | Adapted copy. |
| IPS2Ra `memory.hpp` | aligned buffer-storage concept | TER workspace-owned buffers | Exclude upstream TBB queues/schedulers; use TER-owned standard storage only. | The normative specification forbids a TBB dependency. | Portability adaptation; no replacement algorithm. |

All IPS2Ra-derived TER source will retain the BSD-2-Clause provenance notice and the full license will be copied to `third_party/licenses/ips2ra-BSD-2-Clause.txt` before derived source is added.

## Task 6 frozen CurrentScatter partition baseline

Measured on 2026-08-24 before any BlockInPlace/IPS2Ra source entered the
repository. This is a CurrentScatter baseline only. It contains no candidate
measurements and supports no speedup or winner claim.

### Build and machine identity

- Worktree: `C:\Users\naman\Desktop\subset-sum-ter-fft`.
- Repository HEAD: `3eac235f871c2e4cf07a73152644e6113eaf39e1` on branch `main` with the
  Task 1-5 worktree changes still uncommitted.
- Build: `build-root-v2`, CMake 4.3.3, Visual C++ 19.51.36252.0, Release,
  `TER_NATIVE_OPT=ON`, Windows SDK 10.0.26100.0.
- Executable SHA-256:
  `f0eef96d1b92ef1e786b929589e19e5e52c3b97449117e37c3f35a5bdfbe9a9f`.
- Machine: `NAMANSPC`, Intel Core Ultra 9 275HX, 24 cores / 24 logical
  processors, Windows 11 Pro Insider Preview 10.0.26300 build 26300.

Configuration and build commands:

```powershell
cmake -S . -B build-root-v2 -DTER_NATIVE_OPT=ON
cmake --build build-root-v2 --config Release --target ter_root_partition_bench --parallel
```

### Generator and timing contract

Schema version 1 uses deterministic generators. Uniform projects SplitMix64
keys; hot places exactly 90% (integer floor) in bucket `seed & digit_mask` and
excludes that bucket from the cold 10%; alternating uses digits zero and the
maximum digit; sorted/reverse use monotone digit runs; duplicates repeats
eight deterministic full `low_k` values while keeping `EntryId` unique.

For each generated input, an independent per-bucket vector directory is built
before timing. After each call it verifies CSR boundaries, each record's digit,
exact bucket counts, and sorted full `(low_k, EntryId)` membership. Input reset,
reference construction/checking, canonical fingerprints, and JSON I/O are
outside `elapsed_ns`; only `partition_chunk` is inside it.

### Exact frozen matrix command

The executable accepts one width and distribution per process, so the exact
matrix was run as 18 slices and byte-concatenated in the displayed loop order:

```powershell
$bench = '.\build-root-v2\Release\ter_root_partition_bench.exe'
$partDir = Join-Path (Get-Location) 'results\partition\task6-parts'
foreach ($bits in 8,12,16) {
  foreach ($distribution in 'uniform','hot','alternating','sorted','reverse','duplicates') {
    $output = Join-Path $partDir ("baseline-{0}-{1}.jsonl" -f $bits,$distribution)
    & $bench --backend current-scatter --n 20000,32496,64000,500000,1000000 `
      --bits $bits --shift 0 --threads 1,2,4,8,16 --distribution $distribution `
      --seed 6840143426475650816 --warmup 5 --repeat 30 --output $output
  }
}
```

Result artifact (intentionally untracked):
`results/partition/current_scatter_baseline.jsonl`.

- Rows: 13,500 measured, covering all 450
  `N/bits/threads/distribution` cells with 30 repetitions each.
- Warmups: 5 per cell; warmups are not emitted.
- Seed: `6840143426475650816`; shift: 0; requested/effective threads:
  `1,2,4,8,16`.
- File size: 8,259,470 bytes.
- SHA-256:
  `56e07ec728d484f51b7568d1e2137196c60174fecb189b728f5c0aec143ad4ba`.
- Validation: every row parsed as JSON; exact schema ordering present; every
  status was `Completed`; all elapsed times were positive; `records_classified`
  and `records_written` equalled `N`; fingerprints were stable for every
  logical dataset across repetitions and thread requests.
- Observed slice wall times summed to approximately 859.7 seconds. This includes
  untimed setup and independent verification and is not a partition benchmark
  metric.

The raw JSONL is the measurement authority. No BlockInPlace row was produced:
requesting that backend terminated with exit code 2 and `NotApplicable` before
opening an output file or taking a timing.

## Task 8 serial BlockInPlace correctness addendum

On 2026-08-25 the serial radix8 BlockInPlace correctness reference was added
after the frozen CurrentScatter measurements above. This does not alter or
reinterpret any Task 6 baseline row.

The new backend is a genuine bounded-buffer, one-digit in-place block
partition. For `B = block_bytes / sizeof(TerPartitionEntry)`, its minimum
algorithm-owned payload is:

```text
514 * block_bytes + 2 * floor(N / B) + 12,296 bytes
```

This consists of per-bucket partial and one-block overflow-margin storage, two
swap blocks, one 16-bit owner per physical block, and fixed radix metadata. It
does not allocate an N-record scratch array. Nonzero caps use actual retained
vector capacities, and switching backends releases the inactive backend's
retained storage before admission.

The final correctness matrix made 61 BlockInPlace-vs-CurrentScatter comparisons
covering N=0/1, 255/256/257, 511/512/513, deterministic 10k random, one bucket,
99.99% hot, alternating, sorted/reverse, randomized unique EntryIds, shifts 0
and 8, and block sizes 8/64/2,048 bytes. It required exact CSR offsets, exact
digit membership, and exact per-bucket `(low_k, EntryId)` multisets. Focused
Release passed `1/1`; Debug MSVC ASan passed `1/1` with no sanitizer diagnostic;
the full Release suite passed `13/13`.

No candidate timing was added in Task 8. The Task 6 harness remains frozen and
still rejects BlockInPlace before timing, so the historical JSONL remains a
CurrentScatter-only baseline. There is no measured speedup, winner, default,
or production-root claim in this addendum.
