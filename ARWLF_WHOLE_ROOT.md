# ARWLF whole-root run — preregistration

Written 2026-10-03, before any n=108 whole-root timing. The frozen backends
(FROZEN_ROOT_BACKENDS.md) are unchanged; this adds a runner around them.

## Question

The regime campaign (ARWLF_REGIME_CAMPAIGN.md) timed single microproblems: ARWLF is a geometric-mean 53.1x
faster than frozen K2 on 14 calibration and 52.9x on 15 holdout n=108 microproblems. A root has 4096
microproblems. Does the ratio hold for **one complete n=108 root**, including whatever a whole-root pass adds
(ARWLF building its leaf cache for all 192 families, cache behaviour across thousands of consecutive
microproblems, DISPATCH's probes on the whole selection)? And every campaign case had zero final outputs:
here the root carries exactly **one** final output, which every backend must report.

## Workload

- Root: n=108 seed **0x1001**, attempt 0 (`results/root-queue-generalization/snapshots/n108_s0x1001_a0.snap`),
  the calibration root of the 53x result with the most memory headroom and the fastest K2. All 4096
  microproblems.
- One planted final output: the positive-output construction of ARWLF_REGIME_CAMPAIGN.md Amendment 2 with
  M = 1, seed 0xB1A57001, on the anchor microproblem (cell 2730). It is a constructed C record, not a TER
  vector; the rest of the root is the authentic AUTHENTIC_FULLTER data. Any authentic final output the root
  happens to have is reported too.
- Backends: frozen K2, ARWLF and DISPATCH, called exactly as the campaign's timed rounds call them
  (`ter2_root/campaign/ter_arwlf_whole_root.cpp`); K2_WIDE optional (`-IncludeK2Wide`, about 31 h).
- Same build and host protocol as the campaign: Release, `TER_NATIVE_OPT`, P-core pinned, AC power required.

## Protocol

| Run | Backend | Chunks | Expected time |
|---|---|---|---|
| arwlf-1 | ARWLF | 1 (whole root, one call) | ~1 h |
| dispatch-1 | DISPATCH | 1 | ~1 h |
| k2-1 | K2 | 64 (one per target family u) | ~51 h |
| arwlf-2 | ARWLF | 1 | ~1 h |
| dispatch-2 | DISPATCH | 1 | ~1 h |
| k2_wide-1 (optional) | K2_WIDE | 64 | ~31 h |

Each run is one timed pass, one process (`ter_arwlf_whole_root`). Frozen K2 at about 45 s per microproblem
makes a warmup + repeated K2 protocol (the campaign's 3 + 7) infeasible on a whole root; ARWLF and DISPATCH are
run before and after K2 to bracket machine drift. **Chunking:** frozen K2 and K2_WIDE iterate family-major
(for u, for s); running the 64 cells of each family u as one call, in ascending u, performs the same
microproblems in the same order as one call. Each chunk is a resumable checkpoint (at most one ~48 min chunk
is lost to an interruption). ARWLF and DISPATCH are never chunked. Timed region: the campaign's (sink reset +
backend call). Setup (snapshot load, planting, family keys, index, census) is untimed and recorded per process.

## Exactness (fail closed)

- Every chunk: the planted output is reported by exactly the chunk holding the anchor microproblem and by no
  other; no directory misses or residual mismatches; ARWLF completes. A failure writes the row and stops
  (exit 4).
- Every complete run: the union of its final outputs equals every other complete run's, with equal structural
  counts, and contains the planted output (`summarize_arwlf_whole_root.py`, exit 1 otherwise).
- A working set above 4096 MiB after setup is recorded `RESOURCE_LIMIT` and not timed.

## Prediction (from the campaign's anchor medians on this root, before running)

K2 44,558 ms and ARWLF 842.4 ms per anchor microproblem, and the census's +/-2 % uniformity in pairs across
the 4096 microproblems, predict K2 about 4096 x 44.6 s = **50.7 h** and ARWLF about **57.5 min** for the whole
root: about **53x**. Whatever the measurement is, it is reported.

## Analysis

Per run: whole-root time (sum of its chunks), ns per logical pair, per-chunk min / median / max, off-core
chunks, AC state, peak working set. Speedup = K2 whole-root time / each ARWLF (and DISPATCH) run's, reported
for both bracketing runs, never averaged away. Setup reported separately. No other roots, no reruns chosen
after seeing a result; an interrupted run is resumed, not restarted.

Commands: `pwsh -NoProfile -File ter2_root/scripts/arwlf_whole_root_plan.ps1`; results in
`results/arwlf/whole-root-20261003/` (`runs.jsonl`, `summary.md`, `provenance.jsonl`, `build_freeze.json`).
Tests: CTest `ter_arwlf_whole_root_tests` (n=72 anchor root: chunked K2 / K2_WIDE == one call, every backend
== K2, the planted output found, the workload restored).

## Addendum (2026-10-03, during run arwlf-1; no change to the protocol): interference audit

Run arwlf-1 started at 13:30 local time without a guard. During it, a machine check found ~2-3 cores of
background CPU (Windows, other applications) and the runner's cold pages compressed by Windows (working set
0.68 of 3.14 GiB, 1-3 page faults/s, i.e. the hot set stayed resident). From now on
`ter2_root/scripts/bench_guard.ps1` may run beside the benchmark: it raises the benchmark to High priority,
locks its working set in RAM, keeps other processes off its pinned core, optionally suspends compilers and
test runners, and logs foreign CPU every 5 s to `results/arwlf/bench_guard.csv`. It changes no computation,
power plan or security setting. `summarize_arwlf_whole_root.py` reports, per run, the guarded fraction and
flags each chunk that saw a heavy process or a mean foreign CPU above 1.0 core. Flags are reported; no
measurement is dropped or replaced because of them. Unguarded time (all of arwlf-1 before the guard starts)
is reported as unguarded.
