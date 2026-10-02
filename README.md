# subset-sum-ter-fft
Experimental subset-sum solver exploring representation techniques (TER), FFT-based methods, exact pruning, and high-performance search.

## Classical depth-3 TER

The repository contains a single-threaded classical implementation of Yang
Li's depth-3 TER Algorithm 2 with the exact topology `18 -> 9 -> 3 -> 1`.
`FullTer` materializes complete level-3 sets or returns `ResourceLimit`;
`FixtureTer` is an explicitly reduced correctness mode and is never reported as
a complete execution. If the complete leaf cardinality exceeds the 64-bit
telemetry range, fixture mode exposes that overflow explicitly while full mode
returns `ResourceLimit`. The implementation currently supports the canonical
power-of-two modulus `N = 2^n`.

Build and test with MSVC:

```powershell
cmake -S . -B build-full -DTER_NATIVE_OPT=ON
cmake --build build-full --config Release
ctest --test-dir build-full -C Release --output-on-failure
```

### What "Full TER" does and does not mean

`FullTer` means the complete `18 -> 9 -> 3 -> 1` TER attempt topology with
exact arithmetic. It is **not** a complete decision procedure for subset sum.
The solver is a randomized representation search with a finite attempt budget,
so its outcomes are:

| Status | Meaning |
|---|---|
| `Found` | a witness was produced and independently verified |
| `AttemptLimitReached` | the attempt budget was consumed without a witness |
| `Cancelled` | a wall-clock or cooperative cancel stopped the run |
| `ResourceLimit` | a declared memory ceiling refused the work |

`AttemptLimitReached` is an incomplete randomized-search outcome. **It is never
a proof of UNSAT**, and neither are `Cancelled` or `ResourceLimit`.

### Running the benchmark

The default attempt budget is **one attempt**
(`limits::full_ter::kDefaultAttemptLimit == 1`), so a default run that prints
`AttemptLimitReached` means exactly one attempt was tried — not that a long
search failed. Always state the attempt policy explicitly rather than relying
on that default:

```powershell
# One-attempt cost: how expensive is a single exact TER attempt?
.\build-full\Release\ter_full_bench.exe --n=36 --attempt-limit=1 --warmup=1 --repeat=5

# Time to a witness under a retry budget.
.\build-full\Release\ter_full_bench.exe --n=36 --mode=time-to-found --attempt-limit=100 --require-found

# Success probability across independent seeds, not one replayed seed.
.\build-full\Release\ter_full_bench.exe --n=36 --mode=success-curve --seeds=64 --attempt-limit=64
```

The M2 root path at `n=72` (the genuine root quotient is 47 bits):

```powershell
.\build-full\Release\ter_full_bench.exe --n=72 --attempt-limit=1 --warmup=0 --repeat=1
```

The default is one warmup plus five timed replays, so a large `--n` run costs
six full solves unless you pass `--warmup=0 --repeat=1`.

Before launching an unfamiliar dimension, ask what shape and backends it would
select without running the merge:

```powershell
.\build-full\Release\ter_full_bench.exe --n=108 --preflight
```

Both merge stages now stay on a bounded streaming path for every constructible
finite-sanity dimension, so no production run falls back to building an
`|A| x |B|` pair index. Where a quotient no longer fits its machine-word index,
the quotient congruence is demoted to an exact no-false-negative prefilter and
the arbitrary-width residue words decide:

| n | r1 | r2 | root Q | L2→L1 Q | root decision | L2→L1 decision |
|---:|---:|---:|---:|---:|---|---|
| 36 | 12 | 3 | 24 | 9 | quotient decisive | projection decisive |
| 72 | 25 | 7 | 47 | 18 | quotient decisive | projection decisive |
| 108 | 39 | 12 | 69 | 27 | **residue authority** | projection decisive |
| 144 | 53 | 16 | 91 | 37 | **residue authority** | **residue authority** |
| 180 | 67 | 21 | 113 | 46 | **residue authority** | **residue authority** |

A truncated quotient match is never acceptance, and a fingerprint is never
acceptance. The final authority is always the original residue words.

The production root is the generalized bounded streaming path, not the
specified v2.1 Stage-C/D/E funnel; that contradiction is resolved with
measurements in
[docs/root_architecture_resolution.md](docs/root_architecture_resolution.md).

Two limits remain and are documented rather than papered over:

- the finite shape domain is exactly `36 | n` — see
  [docs/ter_dimension_domain.md](docs/ter_dimension_domain.md);
- **the root still performs all `|A| x |B|` streaming probes.** Removing the
  `O(|A||B|)` *memory* is not the same as removing the `O(|A||B|)` *work*, and
  only the memory has been removed. Pair work is unchanged at 620,966,064
  probes for n=72. Exact region pruning does reduce opened pair mass 500x, but
  the cost of enumerating which regions survive currently cancels the gain on
  the balanced list geometry TER produces; see
  [docs/root_pair_space_analysis.md](docs/root_pair_space_analysis.md).

Every benchmark row is preceded by a provenance line carrying the
configure-time commit, working-tree dirty state, build stamp, an executable
SHA-256, the full command line, and every declared limit, so a row can always
be tied to the binary that produced it.

### Cache-tiled exact three-list merge

The exact cache-tiled modular three-list reporter (`src/tiled_q3report.cpp`) is
integrated into the Q3REPORT comparator, both public merges and the FullTer
L2→L1 and root stages. Its measured evidence package lives in
`results/tiled-q3report-final-20260829/`, and
[tiling_implementation_idea.md](tiling_implementation_idea.md) Sections 16-17
carry the current numbers. In short: it preserves exact outputs and the same
worst-case `O(|A||B|)` pair bound, is **slower** than the `Q`-wide direct
comparator below `L = 512`, and reaches a geometric-mean 12.93x over it at
`L = 16384` across five declared seeds. It does not change TER's exponent.

Its appended `l1_to_l0_*` CSV fields distinguish low-bit prefilter candidates
from full-quotient matches. A prefilter match is never accepted without the
full 47-bit check.

For explicitly synthetic adversarial L2-to-L1 merge fixtures, use the separate
stress benchmark. Its rows are never paper-faithful FullTer evidence:

```powershell
.\build-full\Release\ter_stress_bench.exe
```

See [TER_STRESS_GENERATOR.md](TER_STRESS_GENERATOR.md) for the stress API,
invariants, telemetry, and boundary semantics.

The `n=36` benchmark is a complete deterministic random-target performance
execution of the TER attempt topology. Guided targets are reserved for differential pipeline-connectivity
tests and are not substituted for the paper's random-target benchmark.
The paper-rounded `n=360` complete level-3 sets are exponentially large and the
current machine-scale implementation correctly returns `ResourceLimit`; reduced
`n=360` fixtures must not be described as full Algorithm-2 runs.
