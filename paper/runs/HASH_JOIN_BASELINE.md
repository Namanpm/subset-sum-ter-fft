# Hash-join prepared-root baseline

This adds a new scalar hash-join root backend comparison without changing any
frozen K2, K2_WIDE, ARWLF, dispatcher, FullTer production path or captured input.
It is a prepared L1-to-L0 benchmark, not full-solver throughput and not an
exponent-improvement claim. No performance result is asserted by this document.

## Build and run

Build the `ter_hash_join_bench` target in the existing native Release tree:

```powershell
cmake --build build-full --config Release --target ter_hash_join_bench --parallel 1
pwsh -NoProfile -File paper/runs/run_hash_join_baseline.ps1
```

The default script runs all 4096 microproblems of the existing, unmodified
`results/arwlf/holdout-snapshots/n72_s0x720{1,3,7}_auto.snap` roots, with
three warmups and seven matched samples per backend. It never captures or
substitutes a missing snapshot. A fresh timestamp plus random suffix identifies
each output directory under `results/arwlf/hash-join-baseline/`.

```powershell
# Read-only correctness validation: Release/AC/affinity are not required by the runner.
build-full/Release/ter_hash_join_bench.exe --snapshot=PATH --microproblems=1 --validate-only --out=validation.csv
# One-cell timing with explicit protocol and controlled host checks.
build-full/Release/ter_hash_join_bench.exe --snapshot=PATH --microproblems=1 --warmups=3 --samples=7 --pin=p-core --out=samples.csv
# Explicit positive-output variant; all backends see the same planted state.
pwsh -NoProfile -File paper/runs/run_hash_join_baseline.ps1 -Plant
# Discovery only: existing n108 0x1001 attempt 0, c2730, zero warmups, one sample.
pwsh -NoProfile -File paper/runs/run_hash_join_baseline.ps1 -N108Discovery
```

Runner defaults are `--microproblems=4096 --warmups=3 --samples=7 --pin=p-core`,
`--max-witnesses=1000000`, and `--memory-limit-mib=2048`. Only `1` or `4096`
microproblems are accepted. `1` uses the existing deterministic
`select_microproblems` selection and checks that it is c2730. `--pin=none`
is an explicitly unpinned experiment, not a pinned result; timed runs still
require Windows, Release/NDEBUG and verified AC power. Validation-only skips
these timing requirements. The script itself always uses native Release paths
and checks `TER_NATIVE_OPT:BOOL=ON` in the existing build cache.

## Exactness and timing contract

All compared paths share the same workload, full `FamilyKeys`, `RootIndex`,
selected cells and unchanged `RootSink` authority chain. K2_WIDE is the exact
full-Q authority gate. Its count-only first pass allocates no witness vector.
Hash_join must first agree on a count-only pass before any witness collection.
The full-key raw triple count bounds all collected witness vectors, which are
reserved before calls because the frozen sink's `emit_triple` is `noexcept`.
Exceeding `--max-witnesses`, invalid input, a backend non-completion status,
or an audit mismatch stops the run; partial output is not a complete result.
The runner rejects output paths that alias the input snapshot, including
existing hard links. Regression tests exercise this on disposable input.

The untimed gate checks exact sorted EntryId witness **multisets** (after
original-residue authority), residue counts/hashes, structural counts and clean
sink diagnostics for K2, K2_WIDE, ARWLF and hash_join. Hash_join, ARWLF and
K2_WIDE must also agree on full-Q raw triple counts and all three multiset
hashes. K2 emits earlier 25-bit candidates; its raw triple count/hash is not
equated to a full-Q backend's raw digest. Every warmup and timed pass repeats
the exact checks. Zero accepted witnesses means only zero witnesses for this
captured root/selection; it is never a global mathematical UNSAT claim.

Timing is `sink.reset + backend call`: hash-table allocation/build/rebuild and
all ARWLF call-local builds are included in each invocation. Snapshot loading,
optional planting, shared keys/index setup, the initial K2_WIDE/hash count passes, full
correctness gate, sorting/auditing and CSV reporting are excluded. Their
`setup_ms` and `exactness_ms` are disclosed; n108 discovery still pays these
untimed correctness costs. Backend order uses the existing campaign's rotated,
odd-round-reversed `round_order`; no standalone cherry-picked backend timing.

CSV contains one row per accepted sample (or four validation rows), `ms`,
`ns_per_pair`, selected logical pairs, raw/full-key hits, residue and structural
counts, multiset hashes, hash_join low hits/table builds/auxiliary bytes,
the witness storage bound, host AC/CPU/class checks, and peak working set.
`full_key_hits` is the shared audited K2_WIDE full-Q count; K2's own
`raw_triples` remains its earlier filter count. Blank validation times are not
measurements. Blank non-hash auxiliary fields mean not collected, not zero.
Medians/dispersion can be calculated from the sample rows without altering
inputs. On a timing failure, already-written rows are only partial data.

Hash `auxiliary_bytes` reports backend scratch/table storage, not the loaded
snapshot, shared keys/index, authority tables or witness vectors. The hash
memory limit applies to that backend storage, not total process working set.
The CSV's peak working set and explicit witness bound expose the wider process
cost. The scalar hash algorithm enumerates selected Q-by-X pairs; its expected
constant-time hash probes do not make this subquadratic.
Scalar means no explicit SIMD intrinsics or AVX2 probe/dispatch path. The
normal native Release compiler is still used; this is not a promise that the
compiler never vectorizes a memory-initialization or validation operation.

## Provenance, planting and n108 limits

The script refuses to start while any `ter_*` process is active and never
stops another process. It records CPU/OS, git commit plus dirty status, Release
cache hash, executable SHA-256 and snapshot SHA-256, exact command arguments,
UTC start/end times and exit code in `provenance.json` and
`command-manifest.jsonl`. stdout is preserved as CSV and diagnostics separately.
The runner enforces AC and requested affinity before and after every shot;
validation-only is deliberately not labelled a controlled timing result.

Default input is unmodified `AUTHENTIC_FULLTER`. `--plant=1` / `-Plant` is
optional and explicitly labelled `AUTHENTIC_PLANTED_TARGETS`: it uses the
existing `PlantedTargets` helper to append one target at c2730, checks its
expected witness set, and compares all paths on that identical altered state.
These constructed authority targets are not TER vectors or solver solutions.
The on-disk snapshot is never rewritten.

`-N108Discovery` runs **only** existing
`results/root-queue-generalization/snapshots/n108_s0x1001_a0.snap`, one cell,
zero warmups and one matched sample per backend. It does not run an n108 whole
root, generate a missing input or imply a stable performance estimate. Any
n108 whole-root cost prediction obtained by scaling pair counts or one-cell
measurements is **model-only, not measured**; family-size and table-build
variation must remain visible. A one-cell result alone cannot establish
whole-root performance or memory feasibility.

## Verification at implementation handoff (2026-10-10)

Native Release tests pass: 218 core checks plus input/output-alias CLI regression.
MSVC AddressSanitizer core test passes. GCC strict -Werror compilation of the new
reporter passes. Unmodified n72 whole roots for 0x7201, 0x7203 and 0x7207 pass
four-way exactness validation; the n72 anchor additionally passes a one-target
positive-output gate. All 29 frozen-file hashes remain unchanged.

Authentic n108 validation/timing and controlled n72 timing have NOT been run.
UBSan execution is blocked by the existing frozen locator's MinGW
posix_memalign portability issue; the local WSL alternative also cannot start.
No frozen source was modified to work around those environment/build failures.
Do not claim UBSan passed or any speedup was measured. Full evidence and
deviations are in HASH_JOIN_PROGRESS.md.
