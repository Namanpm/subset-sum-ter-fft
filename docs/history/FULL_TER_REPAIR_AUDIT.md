# FullTER Repair Audit — Confirmed Problems, Limitations, and Required Fixes

**Repository:** `Namanpm/subset-sum-ter-fft`  
**Audited branch:** `main`  
**Audited commit:** `6e52c2ba18ee681189a01bfbebd88e8d4898b173`  
**Date of audit:** 2026-08-25  

## Purpose

This document is a repair specification for the current `FullTer` implementation and its benchmark harness.

It is intentionally stricter than a normal bug list. It separates:

1. **confirmed bugs / misleading behavior** that should be fixed,
2. **confirmed scalability cliffs** that preserve correctness but destroy practicality,
3. **intentional incomplete architecture** that must not be mistaken for a finished root-v2 implementation,
4. **non-bugs** that must remain correct, especially `AttemptLimitReached != UNSAT`.

The goal is not to rewrite TER from scratch. Preserve the verified 18 -> 9 -> 3 -> 1 depth-3 TER logic, existing exactness, provenance/audit behavior, Stage-C work, and fail-closed semantics.

Before modifying anything, inspect the current local worktree. Do not assume this document is newer than the local repository if HEAD has advanced.

---

# 0. Non-negotiable safety rules

- Do **not** use `git reset --hard`, `git clean`, destructive checkout/restore, or any command that may erase local work.
- Inspect `git status --short --branch` before edits.
- Preserve the untracked/local donor `ssp_oracle.cpp` if it still exists.
- Preserve the IPS2Ra BSD-2-Clause notice and provenance.
- Do not weaken exactness to obtain speed.
- Do not turn `AttemptLimitReached` into `UNSAT`.
- Do not silently switch `FullTer` to `FixtureTer`.
- Do not call a reduced fixture a full performance result.
- Do not claim speedups without measurements.
- Do not claim memory reductions from ratios whose direction has not been checked.
- Every new fallback must be fail-closed.
- Every performance optimization must have a differential correctness authority.
- Run the math and preflight resource requirements before architecture changes.

---

# 1. Math and measured facts that must be treated as ground truth

## 1.1 Current n=72 root geometry

The committed Stage-C baseline records:

- `A = 19,109`
- `B = 32,496`
- `C = 33,505`
- `r1 = 25`
- root quotient width `Q = n - r1 = 47`

Therefore the logical A x B pair universe is:

```text
19,109 * 32,496 = 620,966,064
```

The existing tiled root avoids materializing an A x B pair-record array, but the logical pair universe is still 620,966,064.

The committed baseline reports roughly:

- root tiled scan: `622,690,700 ns`
- total solver attempt: `769,630,200 ns`
- root low-prefilter candidates: `1,238,960`
- full-quotient rejects: `1,238,960`
- terminal status: `AttemptLimitReached`

That `AttemptLimitReached` is a one-attempt result under current `main`, because the default attempt limit is 1.

## 1.2 Current attempt default

`include/ter/limitations.hpp` currently defines:

```cpp
kDefaultAttemptLimit = 1U;
```

and `bench/ter_full_bench.cpp` assigns that default directly to `config.attempt_limit`.

Therefore, on current `main`:

```text
AttemptLimitReached
```

from the default full benchmark means:

```text
exactly one deterministic random TER attempt was tried and did not produce a witness.
```

It does **not** mean millions of attempts failed.

## 1.3 n=108 exact finite-sanity width cliff

For the current finite-sanity family at `n=108`:

```text
R1 = 879,619,727,485,803,060,256,500
floor(log2(R1)) = 79
r1 = floor(79 / 2) = 39
Q = n - r1 = 108 - 39 = 69
```

The current tiled root supports only:

```text
Q <= 64
```

so `n=108` does **not** use the bounded tiled root. It falls back to the generic exact ternary MITM path, which materializes an A x B pair index.

That is a real scalability cliff, not a theoretical concern.

## 1.4 L2 -> L1 width cliff

The current optional native tiled L2 -> L1 path requires:

```text
r1 < 64
and
r1 - r2 <= 32
```

For finite-sanity dimensions:

| n | r1 | r2 | r1-r2 | root Q=n-r1 |
|---:|---:|---:|---:|---:|
| 36 | 12 | 3 | 9 | 24 |
| 72 | 25 | 7 | 18 | 47 |
| 108 | 39 | 12 | 27 | 69 |
| 144 | 53 | 16 | 37 | 91 |
| 180 | 67 | 21 | 46 | 113 |

So:

- the root-width cliff appears already at `n=108`,
- the L2 -> L1 tiled-width cliff appears by `n=144`,
- by `n=180`, `r1` itself exceeds the native `<64` requirement.

The generic fallbacks remain exact, but they materialize pair structures and are not scalable replacements.

## 1.5 L3 full-set growth

For the current finite-sanity family, the complete physical L3 set per half grows as:

| n | L3 entries per physical half |
|---:|---:|
| 36 | 18 |
| 72 | 630 |
| 108 | 24,804 |
| 144 | 1,028,790 |
| 180 | 43,949,268 |
| 216 | 1,913,554,188 |

This growth is fundamental to the current finite construction. Do not promise that merely fixing quotient width makes arbitrary-thousands `n` practical.

The correct goal of arbitrary-width arithmetic is:

> Large `n` should fail because the mathematical/list workload exceeds declared resources, not because a 64-bit implementation boundary is hit.

## 1.6 Correct interpretation of the Stage-C BlockInPlace measurements

**Important correction: BlockInPlace did NOT beat CurrentScatter on memory for the genuine captured n=72 lists.**

`docs/root_partition_decision.md` defines:

```text
scratch ratio = candidate/current
```

The captured-root aggregate results are:

| Block bytes | current/candidate speedup | candidate/current time | candidate/current median scratch |
|---:|---:|---:|---:|
| 1024 | 0.5542x | 1.8045x slower | 5.1712x more scratch |
| 2048 | 0.5146x | 1.9431x slower | 9.9417x more scratch |
| 4096 | 0.4520x | 2.2124x slower | 19.4841x more scratch |

Therefore on the genuine captured n=72 workload, BlockInPlace is **strictly worse in both measured time and measured scratch**.

The earlier interpretation “5.17x-19.48x less scratch” is wrong because it inverted the ratio direction.

The BlockInPlace implementation can still become memory-favorable at larger `N`, because its fixed block-buffer cost is amortized. The committed baseline gives its minimum algorithm-owned payload as approximately:

```text
M_BIP(N,b) = 514*b + 2*floor(N/(b/8)) + 12,296 bytes
```

while CurrentScatter's main record scratch alone is approximately:

```text
M_CS_record(N) = 8*N bytes
```

Ignoring CurrentScatter metadata, the rough minimum crossover where BIP's modeled payload falls below only CurrentScatter's record buffer is approximately:

| block bytes | rough N crossover |
|---:|---:|
| 1024 | ~67,461 |
| 2048 | ~133,251 |
| 4096 | ~264,834 |

These are **model crossovers, not measured production thresholds**.

The captured root lists (`19k-33.5k`) are below all three, which explains why BIP loses memory there.

Do not implement a RAM-aware `Adaptive` policy on the false assumption that BIP is already the low-memory winner at captured-root sizes. Measure the actual crossover first.

---

# 2. Critical benchmark problems

## P0 — `ter_full_bench` has no current `--attempt-limit` CLI

**Severity:** Critical benchmark-control defect.

### Evidence

Current `bench/ter_full_bench.cpp` parses:

- `--n=`
- `--fixture`
- `--fixture-limit=`
- `--guided`
- `--audit`

It does **not** parse `--attempt-limit`.

It then does:

```cpp
config.attempt_limit = ter::limits::full_ter::kDefaultAttemptLimit;
```

and that default is currently `1`.

### Why this is a problem

Historical commands such as:

```powershell
.\build-root-v2\Release\ter_full_bench.exe --n=36 --attempt-limit=100
```

came from an older source/binary state. Current `main` would reject that option.

This makes it easy to believe a benchmark is exercising retries when it is actually only exercising one attempt.

### Required fix

Add an explicit, validated CLI option:

```text
--attempt-limit=<positive integer>
```

Print it in benchmark metadata.

Do not silently change the solver's API default as a substitute for benchmark configurability.

### Acceptance tests

- `--attempt-limit=0` is rejected.
- `--attempt-limit=1` exactly reproduces current one-attempt semantics.
- `--attempt-limit=100` records up to 100 attempts and can report the successful attempt correctly.
- malformed and overflow values are rejected.

---

## P1 — `AttemptLimitReached` is accepted as a benchmark-success condition without being clearly separated from solver success

**Severity:** Critical interpretation defect.

### Evidence

`limits::benchmark::full_ter_warmup_acceptable()` currently accepts both:

```cpp
Status::Found
Status::AttemptLimitReached
```

The benchmark can finish normally and return process exit code 0 after an `AttemptLimitReached` result.

### Why this is a problem

For a **single-attempt cost benchmark**, accepting `AttemptLimitReached` is legitimate.

For a **time-to-solve benchmark**, it is not a solved instance.

The current harness mixes these meanings.

A shell script can see exit code 0 and incorrectly infer “benchmark/solver succeeded,” even though no witness was found.

### Required fix

Introduce explicit benchmark semantics, for example:

```text
--mode=attempt-cost
--mode=time-to-found
--mode=success-curve
```

or an equivalent clear design.

Add an optional:

```text
--require-found
```

which returns a nonzero process exit code if the terminal solver status is not `Found`.

Do **not** make `AttemptLimitReached` equivalent to `UNSAT`.

---

## P2 — detailed CSV telemetry is hardwired to attempt 0

**Severity:** Critical reporting bug once retries are enabled.

### Evidence

`bench/ter_full_bench.cpp` does:

```cpp
const auto& attempt = warmup.telemetry.attempts[0];
```

and prints L2 sizes, L1 sizes, root counters, root survivors, memory fields, etc. from that first attempt.

### Why this is a problem

If:

```text
attempt 1 -> no witness
...
attempt 12 -> Found
```

the row can say:

```text
status = Found
```

while all detailed root counters describe attempt 1.

This already explains the historical “Found on attempt 12, root survivors shown as 0” contradiction.

### Required fix

For a `Found` result, detailed “terminal attempt” fields must come from the attempt that actually returned `Found`.

Also report:

```text
attempts_used
terminal_attempt_index
terminal_attempt_seed
```

If aggregate metrics are wanted, give them separate names such as:

```text
sum_attempt_ns
mean_attempt_ns
median_attempt_ns
total_root_pair_candidates
```

Never mix terminal-attempt counters with aggregate/attempt-0 counters.

### Acceptance test

Create a deterministic test configuration where attempt 0 misses and a later attempt succeeds, then verify the benchmark reports the successful attempt's exact telemetry.

---

## P3 — repeated benchmark runs replay the same deterministic solver result instead of measuring success probability

**Severity:** Major statistical-design defect.

### Evidence

The benchmark uses one fixed seed and re-runs `solve_full_ter()` with the same configuration for warmup plus timed repetitions.

It explicitly requires the same:

- status,
- witness,
- telemetry fingerprint

on every repetition.

### Why this is a problem

This is useful for **deterministic timing reproducibility**.

It tells us almost nothing about:

```text
P(Found on attempt k)
expected attempts to Found
tail of attempts to Found
success probability by attempt budget
```

Those are exactly the quantities needed to understand whether retries scale.

### Required fix

Keep deterministic timing mode, but add a separate stochastic campaign mode with a declared seed set.

Report at minimum:

```text
number of independent instances/seeds
Found count
AttemptLimitReached count
success fraction
attempts-to-Found distribution
median/p95 attempts to Found
time-to-Found distribution
```

For small `n`, compare against known/independent authorities where feasible.

---

## P4 — large benchmarks automatically perform one warmup plus five full replays

**Severity:** Major usability/resource problem.

### Evidence

`kFullTerTimedRepetitions = 5`.

The benchmark performs:

```text
1 warmup + 5 measured solve_full_ter() calls
```

with identical deterministic inputs/configuration.

### Why this is a problem

A supposedly “one run” large-n experiment can cost roughly six times the wall time the user expects.

For a 10-minute solve, that becomes about an hour.

For a pathological case, this can look like a hang.

### Required fix

Add:

```text
--warmup=<count>
--repeat=<count>
```

with conservative defaults for large/full runs.

Provide a documented one-shot mode:

```text
--warmup=0 --repeat=1
```

or equivalent.

Do not silently perform six giant solves.

---

## P5 — no benchmark wall-clock time limit / cancellation policy

**Severity:** Major reviewer-safety issue.

### Evidence

`include/ter/limitations.hpp` explicitly notes that there is no global wall-clock timeout policy.

The solver supports a cooperative cancellation pointer, but `ter_full_bench` does not expose a useful CLI time limit around it.

### Why this is a problem

A reviewer or user can launch a large full run and have no bounded wall-clock guarantee.

This is especially bad around width fallbacks or unexpectedly large lists.

### Required fix

Add a benchmark wall-clock guard, e.g.:

```text
--time-limit-ms=<N>
```

using the existing cancellation mechanism.

Timeout must return a clear `Cancelled`/timeout classification and must never be reported as UNSAT.

---

## P6 — benchmark memory budget is fixed at 4 GiB and not user-configurable from CLI

**Severity:** Major portability/reviewer-safety issue.

### Evidence

`ter_full_bench` uses:

```cpp
limits::benchmark::kFullTerMemoryBytes
```

currently 4 GiB.

### Why this is a problem

A fixed 4-GiB benchmark budget is not equally safe on:

- an 8-GiB reviewer laptop,
- a 32-GiB workstation,
- a 128-GiB machine.

It can also prevent useful larger-machine experiments.

### Required fix

Expose:

```text
--memory-limit-mib=<N>
```

or an equivalent byte-based option.

Optionally support an `auto` safety policy, but an explicit hard cap must always be available for reproducibility.

Print the effective cap in metadata.

---

## P7 — no preflight/dry-run mode for dangerous dimensions

**Severity:** Major usability issue.

### Required fix

Add a read-only preflight mode which computes and prints, without launching the full merge:

```text
n
family
r1
r2
root Q = n-r1
L2->L1 quotient width = r1-r2
complete L3 cardinality per half
selected L2->L1 backend
selected root backend
declared memory cap
known representability gates
whether pair-materializing fallback would be selected
```

If a pair-materializing fallback is selected, print that prominently.

This would have exposed the `n=108, Q=69` cliff before the run.

---

# 3. FullTER API and naming problems

## P8 — `FullTerConfig` defaults to `FixtureTer`

**Severity:** Major API footgun.

### Evidence

`include/ter/full_ter.hpp` currently initializes:

```cpp
TerExecutionMode execution_mode = TerExecutionMode::FixtureTer;
```

### Why this is a problem

A caller can write:

```cpp
FullTerConfig config;
solve_full_ter(instance, config);
```

and receive reduced fixture semantics from an API named `solve_full_ter`.

`ter_full_bench` explicitly overrides this to `FullTer`, so the current benchmark itself is not silently fixture mode. The danger is the public API default.

### Required fix

Choose one of:

1. default `FullTerConfig::execution_mode` to `FullTer`, or
2. remove the default and require callers to select a mode explicitly.

Update tests and docs so fixture execution can never be mistaken for complete execution.

---

## P9 — “FullTER” currently means complete finite topology, not a complete decision procedure

**Severity:** Documentation/expectation problem, not an arithmetic bug.

The current solver is a randomized representation search with finite retries.

After the attempt budget is exhausted it correctly returns:

```text
AttemptLimitReached
```

with the message that attempts were exhausted without proving UNSAT.

That behavior must remain.

### Required fix

Documentation and benchmark output must explicitly distinguish:

```text
Full TER attempt/topology completeness
```

from:

```text
complete subset-sum decision procedure
```

Do not rename `AttemptLimitReached` to anything implying UNSAT.

---

## P10 — current benchmark hardcodes `FiniteSanity`

**Severity:** Major experimental-labeling limitation.

### Evidence

`bench/ter_full_bench.cpp` assigns:

```cpp
config.family = ShapeFamily::FiniteSanity;
```

### Why this is a problem

The repo also has `PaperRounded`, but the full benchmark does not expose it.

Therefore “FullTER benchmark” currently means:

```text
complete FullTer execution under FiniteSanity shape parameters
```

not “the paper-rounded parameter family.”

### Required fix

Add a family selector, for example:

```text
--family=finite-sanity
--family=paper-rounded
```

Print it in every result.

Keep resource refusals honest: large paper-rounded full L3 sets may be impossible and must return `ResourceLimit`, not silently become fixtures.

---

# 4. Dimension-domain problem

## P11 — `n=100` is currently invalid by construction

**Severity:** Major domain limitation.

### Evidence

`finite_dimension_supported(n)` requires:

```text
n % 36 == 0
```

and `derive_full_ter_parameters_impl()` applies this gate to both `FiniteSanity` and `PaperRounded`.

Therefore:

```text
n=100 -> InvalidConfiguration
```

under current main.

### Why this is a problem

It prevents ordinary benchmark points such as 100 and creates giant jumps:

```text
72 -> 108 -> 144
```

The jump itself can cross width/resource cliffs and makes scaling studies unnecessarily coarse.

### Required fix

Do **not** simply delete the divisibility test.

Implement a mathematically validated finite-rounding/shape-construction policy for non-multiples of 36, preserving:

- integral counts,
- non-negative counts,
- half-support constraints,
- valid decomposition patterns,
- exact target relationships,
- `0 < r2 < r1 < n`.

If exact TER shape cannot be constructed for a requested `n`, return a precise shape status.

### Required tests

At minimum exercise nearby awkward dimensions such as:

```text
n=100
n=101 if odd n remains intentionally unsupported
n=102
n=107
n=108
```

Only enable dimensions for which the finite construction is mathematically valid.

---

# 5. Major scalability cliffs inside FullTER

## P12 — root `Q > 64` falls back to A x B pair materialization

**Severity:** Critical scalability defect.

### Evidence

`limits::full_ter::native_root_quotient_supported(n,r1)` requires:

```text
n-r1 <= 64
```

When false, `solve_full_ter_impl()` calls generic `ternary_mitm()` for L1 -> L0.

For `modulus_bits > 64`, generic `ternary_mitm()` allocates:

```text
PairRecord for every A x B pair
```

and sorts the pair array.

At `n=108`, `Q=69`, so this path is selected.

### Required fix

The production root must remain bounded-memory and exact for arbitrary supported quotient widths.

Do not solve this by truncating to 64 bits.

Use the planned root-v2 strategy:

```text
compact exact metadata
-> Stage C partition
-> Stage D exact region pruning
-> Stage E carry-aware refinement
-> narrowed witness recovery
-> exact full-width verification / word-stream proof
```

No production fallback should require materializing all A x B pair records merely because `Q > 64`.

### Acceptance criterion

A supported `Q=65`, `Q=69`, `Q=128`, etc. must stay on a bounded exact path, or fail early with an explicit resource limit. It must never silently choose the legacy A x B materializer as the only scalable option.

---

## P13 — L2 -> L1 also has a native-width cliff

**Severity:** Critical future scalability defect.

### Evidence

The native tiled L2 -> L1 path requires:

```text
r1 < 64
r1-r2 <= 32
```

When false, `solve_full_ter_impl()` falls back to `ternary_mitm()`.

For finite-sanity:

```text
n=144 -> r1-r2=37
```

so the tiled path is already unavailable even though `r1=53`.

At larger n the generic pair materializer becomes the practical path.

### Required fix

Generalize the exact tiled/partition/refinement idea to remove the hard 32-bit quotient-projection dependency from the production L2 -> L1 path.

Do not conflate low-k hot metadata width with exact quotient width. Low-k can remain compact while exact residue words remain authoritative.

---

## P14 — generic ternary fallbacks are exact but fundamentally pair-materializing

**Severity:** Critical architecture limitation.

Both native and multiword generic ternary MITM paths form an A x B pair index:

```text
PairRecord64    for <=64-bit modulus
PairRecord      for >64-bit modulus
```

This is valid as a tiny/reference/fallback authority.

It is not an acceptable large-list production strategy.

### Required fix

Keep a bounded tiny-reference fallback for differential correctness.

Production-scale paths must avoid O(|A||B|) pair-record storage.

---

## P15 — complete L3 generation materializes scalar vectors before packing them

**Severity:** Medium/major memory-efficiency limitation.

### Evidence

Full L3 generation creates:

```text
vector<ReferenceTernaryVector>
```

and later `materialize_entries()` packs every vector into `BigEntry`.

For large L3 cardinalities this temporarily carries a scalar `n`-byte digit vector representation plus vector-object overhead before packing.

### Why this matters

At `n=144`, each physical half already contains about 1,028,790 L3 entries.

At `n=180`, about 43,949,268 per half.

The exponential growth is fundamental, but avoidable representation overhead should not be added on top.

### Required fix

Consider direct generation into packed TER entries / packed half-support representation when doing so preserves deterministic enumeration and exact audit authority.

Keep scalar generation only as a small-n oracle if useful.

Do not claim this makes huge `n` practical; it only removes avoidable representation overhead.

---

# 6. Stage-C / root-v2 integration problems

## P16 — the new Stage-C partition substrate is not used by production FullTER

**Severity:** Major integration gap, intentional in the Stage-C commit.

### Evidence

The Stage-C implementation plan explicitly forbade replacing the production root.

The new API:

```cpp
partition_chunk(...)
```

exists in `root_partition.*`, tests, and partition benchmarks.

`src/full_ter.cpp` still uses the existing tiled root / legacy ternary fallback and does not call the Stage-C partition substrate.

### Required fix

Do not merely call `partition_chunk()` and declare root-v2 complete.

Integrate Stage C only as part of the exact root-v2 pipeline with the required downstream pruning/refinement semantics and differential tests.

---

## P17 — Stage D exact region pruning is absent

**Severity:** Critical missing performance stage.

Stage C only organizes entries into exact radix buckets.

It does not reduce the logical pair mass.

Stage D is the first stage intended to prove whole A-bucket x B-bucket regions impossible and discard them without opening individual pairs.

### Required fix

Implement an exact no-false-negative region-pruning backend.

Required telemetry:

```text
total pair mass before pruning
pair mass rejected by exact region proof
pair mass surviving/opened
region count tested
region count rejected
region count surviving
fixed setup cost
```

Primary metric:

```text
rho_open = surviving pair mass / (|A|*|B|)
```

Do not report pair-mass reduction as wall-clock speedup.

---

## P18 — Stage E carry-aware refinement is absent

**Severity:** Critical missing performance/correctness stage for deeper exact refinement.

Surviving regions need additional quotient chunks.

The next chunk cannot be tested independently because low chunks generate carries.

Required relation:

```text
a1 + b1 + carry0 = target1 (mod 2^h)
```

not simply:

```text
a1 + b1 = target1
```

### Required fix

Implement carry-aware exact refinement after Stage D.

Add explicit carry-boundary tests, including sums immediately below/at/above `2^k`.

Any refinement that ignores carry is invalid.

---

## P19 — later root-v2 recovery and arbitrary-width proof stages are absent

**Severity:** Major incomplete architecture.

The current repository does not yet have the complete refined root pipeline for:

- narrowed surviving-group descriptors,
- direct / SS-style local witness recovery,
- optional BBF policy,
- exact C-candidate resolution,
- low64 exact rejection,
- full-width fingerprinting,
- final original-residue word-stream/limb proof,
- final structural/root validation integrated into the new path.

The existing BigInt residue representation already supplies arbitrary-width exact state, so do not duplicate full quotient limbs per hot entry unless measurements justify it.

### Required fix

Implement these only after Stage D/E correctness gates pass.

The final exact authority must use the original/full residue words, not a probabilistic fingerprint.

---

# 7. Stage-C backend decision problems

## P20 — BlockInPlace was previously interpreted backwards

**Severity:** Critical decision-data interpretation error.

The current committed decision document is unambiguous:

```text
scratch ratio = candidate/current
```

and captured-root ratios are all greater than 1.

Therefore BlockInPlace uses **more**, not less, scratch on the captured root.

### Required fix

Audit every comment/doc/README/decision summary for the incorrect “BIP uses 5x-19x less memory on n=72 captured roots” claim and remove it if present.

Do not rewrite the raw results.

---

## P21 — BlockInPlace is currently dominated on the representative captured root

**Severity:** Major backend-selection issue.

On captured n=72:

- CurrentScatter is about 1.80x-2.21x faster.
- BlockInPlace uses about 5.17x-19.48x more median scratch depending on block size.

So there is no measured reason to select BIP for those list sizes.

### Required fix

Keep CurrentScatter as default for the measured regime.

Before creating an adaptive memory policy, run a dedicated memory-crossover experiment across N and available thread counts.

The fixed BIP block-buffer overhead means its memory advantage, if any, should occur only after sufficiently large N.

---

## P22 — `Adaptive` and `Region2Path` are enum placeholders only

**Severity:** Incomplete backend architecture, not a bug by itself.

`src/root_partition.cpp` returns `Status::NotApplicable` for:

```text
Region2Path
Adaptive
```

### Required fix

Do not install an Adaptive selector based on assumptions.

If Adaptive is implemented, its selection rules must be based on measured:

```text
time
scratch
N
threads
block size
available/hard memory budget
```

and it must always permit an explicit deterministic backend override.

`Region2Path` should remain deferred unless there is a concrete benchmark/research reason to implement it.

---

## P23 — N=1 partition telemetry is internally asymmetric

**Severity:** Minor telemetry defect.

### Evidence

The trivial path sets:

```text
records_classified = 1
```

but leaves:

```text
records_written = 0
```

even though the single input record is also the single output record.

### Required fix

Define the telemetry contract explicitly.

If `records_written` means “records physically copied by backend,” document 0 for trivial no-move paths.

If it means “records successfully placed in output partition,” set it to 1.

Add a test so the meaning cannot drift.

---

# 8. Memory-accounting defects

## P24 — `estimated_entries_bytes()` undercounts retained outer vector capacity

**Severity:** Critical resource-accounting defect.

### Evidence

`estimated_entries_bytes()` iterates over current entries and charges approximately:

```text
sizeof(BigEntry) per live element
+ nested packed/residue storage
```

It does **not** charge:

```text
entries.capacity() * sizeof(BigEntry)
```

for the outer vector capacity.

This matters because merge outputs can retain capacity greater than size after growth and especially after deduplication.

### Why this is dangerous

The solver uses `estimated_entries_bytes()` to build later resident-memory budgets.

A vector can retain a large allocation after elements are erased by canonicalization, while the declared resident accounting shrinks to the smaller `size()`.

The root path already has a more careful capacity-aware helper (`root_output_live_bytes`), showing that capacity matters.

### Required fix

Create one canonical capacity-aware memory-accounting function for `vector<BigEntry>`.

Charge:

```text
outer vector capacity * sizeof(BigEntry)
+ dynamic payload capacities of live BigEntry elements
```

Use it consistently for resident/peak accounting.

Re-run all resource-limit tests.

---

## P25 — L2 -> L1 tiled path reserves `maximum_outputs` up front

**Severity:** Critical memory-behavior problem.

### Evidence

`ternary_tiled_l2_to_l1_native()` computes:

```text
maximum_outputs = remaining / per_output
```

then performs:

```cpp
result.reserve(maximum_outputs);
```

and, in audit mode:

```cpp
provenance->reserve(maximum_outputs);
```

### Why this is dangerous

`maximum_outputs` is a budget ceiling, not an expected output cardinality.

Reserving that ceiling can retain a very large outer allocation even if only a tiny number of outputs survive.

Combined with P24, later memory telemetry can materially under-report the retained outer capacity.

### Required fix

Use bounded geometric growth / preflight accounting similar to the safer root-output path.

Do not reserve the entire theoretical output budget.

Every reallocation overlap must be included in peak accounting.

---

## P26 — provenance memory is not consistently charged in generic merge output limits

**Severity:** Major audit-mode accounting defect.

### Evidence

`binary_mitm()` derives `maximum_outputs` using `entry_bytes` but can also push `ProvenanceRecord`s when audit/provenance is enabled.

Generic/native `ternary_mitm()` similarly computes output ceilings from entry bytes while provenance may be appended separately.

The tiled L2 -> L1 path partially accounts for provenance in `per_output`, but the accounting policy is inconsistent across merge implementations.

### Required fix

When provenance is enabled, include:

```text
ProvenanceRecord outer storage
packed parent payload
capacity growth/reallocation overlap
```

in output admission and peak accounting.

A memory cap must mean the same thing in audit and non-audit modes.

---

# 9. Performance architecture gaps

## P27 — FullTER remains essentially single-threaded end-to-end

**Severity:** Major performance limitation.

The Stage-C partition substrate has thread-aware backends, but production `FullTer` does not use it yet.

The full benchmark reports:

```text
threads=1
```

and the main 18 -> 9 -> 3 -> 1 pipeline is not parallelized as a production full solve.

### Required fix

Do not parallelize before the root-v2 correctness path is stable.

After exact pruning/refinement is integrated, parallelize independent groups/regions with:

- deterministic output semantics where required,
- bounded per-thread memory,
- no multiplicative scratch explosion,
- explicit thread-count CLI/config,
- measured scaling.

---

## P28 — current root low-bit prefilter does not reduce logical A x B pair mass enough

**Severity:** Critical performance limitation.

At captured n=72:

```text
|A|*|B| = 620,966,064
```

The existing tiled root avoids pair-record materialization but still spends about 622.7 ms scanning the root pair universe.

The low-24 filter produces only ~1.239M low-bit candidates, but the scan work still exists.

### Required fix

This is exactly why Stage D/E exist.

The production objective is not merely “fewer accepted candidates.” It is:

```text
do not open/probe most A x B regions at all.
```

Measure pair-mass rejection separately from final exact candidate count.

---

# 10. Documentation and reproducibility problems

## P29 — historical benchmark commands can silently refer to stale binaries

**Severity:** Major reproducibility issue.

A historical command using:

```text
--attempt-limit=100
```

cannot be produced by the current source because the option is absent.

### Required fix

Every benchmark result must record at minimum:

```text
git commit SHA
dirty/clean state
executable SHA-256
full command line
compiler/build mode
TER_NATIVE_OPT
seed
family
execution mode
attempt limit
memory limit
warmup count
repeat count
```

If the executable does not match current source, say so.

---

## P30 — current README/benchmark naming does not emphasize the one-attempt default enough

**Severity:** Major interpretation/documentation issue.

The README describes the full benchmark and n=72 root exercise, but a reader can reasonably interpret `AttemptLimitReached` as a substantial retry exhaustion rather than “default attempt limit is one.”

### Required fix

Document the default attempt count beside every example command.

Once the CLI is added, examples should specify attempt policy explicitly rather than relying on a hidden default.

---

## P31 — Stage-C success must not be described as a FullTER root speedup

**Severity:** Claim-discipline issue.

The Stage-C commit correctly says the partition substrate:

- does not reduce root pair-work,
- does not replace `src/full_ter.cpp`,
- is a partition-kernel measurement only.

Preserve that discipline in all docs.

Do not turn CurrentScatter-vs-BIP timings into a whole-solver claim.

---

# 11. Things that are NOT bugs and must not be “fixed” incorrectly

## N0 — `AttemptLimitReached` is not UNSAT

Keep:

```text
AttemptLimitReached = incomplete randomized search outcome
```

Never convert it to an UNSAT proof.

## N1 — `FullTer` genuinely implements the 18 -> 9 -> 3 -> 1 topology

Do not replace the verified classical TER structure just because the benchmark harness is bad.

The current implementation genuinely contains:

```text
18 logical L3
-> 9 L2
-> 3 L1
-> 1 L0
```

with exact packed-digit validation and independent final witness verification.

## N2 — generic multiword arithmetic is exact

The problem is not that BigInt/multiword arithmetic is wrong.

The problem is that wide-root dispatch falls back to a pair-materializing strategy.

Preserve arbitrary-width exact residue state.

## N3 — FixtureTer is useful

`FixtureTer` is a valid reduced correctness/testing mode.

The bug is allowing its semantics to be confused with FullTer, not its existence.

## N4 — CurrentScatter should remain the measured Stage-C default for current captured-root sizes

Do not promote BlockInPlace because of the previously inverted scratch interpretation.

---

# 12. Required repair order

Do not attack everything as one unstructured rewrite.

## Gate A — benchmark truthfulness first

Fix:

- P0 attempt-limit CLI,
- P1 benchmark-mode semantics,
- P2 terminal-attempt telemetry,
- P3 independent success campaign,
- P4 warmup/repeat controls,
- P5 timeout,
- P6 memory CLI,
- P7 preflight,
- P29 reproducibility metadata,
- P30 docs.

**Gate:** reproduce current one-attempt n=36/n=72 behavior exactly when configured with `--attempt-limit=1`, while new multi-attempt reporting is correct.

## Gate B — memory-accounting correctness

Fix:

- P24 capacity-aware `BigEntry` vector accounting,
- P25 no reserve-to-budget-ceiling,
- P26 provenance accounting.

**Gate:** resource-limit tests plus targeted capacity-retention tests prove the declared cap is conservative.

## Gate C — dimension policy

Fix P11 only with a mathematically verified finite-shape policy.

**Gate:** `n=100` either becomes a valid exact FullTer shape with tested decomposition invariants or is rejected for a mathematically explicit reason. No hacked divisibility bypass.

## Gate D — arbitrary-width bounded root

Fix P12/P14 using the refined root architecture.

**Gate:** `Q=69` does not select A x B pair materialization.

## Gate E — Stage D exact region pruning

Fix P17/P28.

**Gate:** zero false-negative pruning against an exact authority and measurable pair-mass reduction on at least one genuine root.

## Gate F — Stage E carry-aware refinement

Fix P18.

**Gate:** exhaustive carry-boundary differential tests and further pair-mass reduction.

## Gate G — finish narrowed recovery / full-width exact proof

Fix P19.

**Gate:** recovered witnesses independently verify modulo `2^n`, including quotient widths above 64.

## Gate H — L2 -> L1 wide path

Fix P13.

**Gate:** dimensions whose `r1-r2 > 32` do not automatically regress to full A x B materialization.

## Gate I — backend selection / parallelism

Only after the above:

- investigate P21/P22 BIP crossover and Adaptive,
- address P27 parallelism.

Do not use Adaptive to paper over an unresolved root algorithm.

---

# 13. Benchmark redesign requirements

The final `ter_full_bench` should be able to answer three different questions without conflating them.

## Mode 1 — one-attempt cost

Purpose:

```text
How expensive is one exact TER attempt?
```

Report terminal status even if no witness is found.

## Mode 2 — time to Found under an attempt budget

Purpose:

```text
How long does the randomized solver take to find a witness?
```

Report:

```text
Found / AttemptLimitReached
attempts_used
successful_attempt_index if any
total wall time
terminal-attempt telemetry
aggregate attempt telemetry
```

## Mode 3 — success curve

Purpose:

```text
How does success probability scale with attempt budget and n?
```

Use multiple independent seeds/instances and report:

```text
P(Found by 1)
P(Found by 2)
P(Found by 4)
P(Found by 8)
...
median/p95 attempts to Found
censored runs
```

Do not use five identical deterministic replays as a substitute for this.

---

# 14. Minimum regression matrix after repairs

At minimum:

| Case | Purpose |
|---|---|
| n=36 FullTer random, attempt=1 | historical one-attempt compatibility |
| n=36 FullTer random, attempt=100 | retry reporting / Found telemetry |
| n=36 GuidedPlanted | exact connectivity/witness path |
| n=72 FullTer random | genuine 47-bit root path |
| n=72 captured-root replay | Stage-C/Stage-D differential authority |
| n=108 or equivalent Q=69 fixture/preflight | wide-root bounded path |
| n=144 or synthetic equivalent `r1-r2>32` | wide L2->L1 path |
| n=100 if new finite-shape policy supports it | arbitrary-dimension policy |
| 1-byte/very-low memory cap | fail-closed resource handling |
| timeout/cancel run | fail-closed cancellation |
| audit on/off | provenance + memory accounting |
| Stage-C N=0/1 | telemetry contract |
| CurrentScatter/BIP large-N sweep | real memory crossover |
| ASan Debug root/partition focused suite | memory safety |

For tiny cases, compare optimized output/provenance against scalar exact authorities.

---

# 15. Required outputs from the repair session

When the repair is complete, produce:

1. a concise list of files changed,
2. exact build/test commands,
3. all test results,
4. exact benchmark commands used,
5. before/after n=36 one-attempt result,
6. before/after n=72 one-attempt result,
7. a multi-attempt n=36 retry result proving terminal-attempt telemetry is correct,
8. preflight output for n=108 showing the selected wide-root path,
9. proof that no large production root falls back to O(|A||B|) pair-record materialization merely because Q > 64,
10. memory-cap tests proving capacity-aware accounting,
11. explicit status of n=100 support,
12. explicit statement of which root-v2 stages are now implemented,
13. no whole-solver speedup claim unless a whole-solver benchmark actually measured one.

---

# 16. Final acceptance definition

Do not call the repair complete merely because CTest is green.

The repaired FullTER system should satisfy all of the following:

- Benchmark attempt limits are explicit and controllable.
- `AttemptLimitReached` is never presented as solved/UNSAT.
- Found telemetry comes from the actual successful attempt.
- Independent deterministic timing and stochastic success campaigns are separate.
- Large runs have configurable repetition, time, and memory limits.
- Benchmark provenance is sufficient to identify stale binaries.
- `FullTerConfig` cannot accidentally masquerade as full execution while defaulting to fixture semantics.
- n-domain restrictions are explicit and mathematically justified.
- n=108-class quotient widths do not force full A x B pair materialization.
- L2 -> L1 width growth does not silently restore the same scalability disaster later.
- Stage-C is integrated only as part of an exact downstream pruning/refinement path.
- Stage D and Stage E are exact, carry-correct, and measured by pair-mass reduction.
- Final wide-width verification remains exact.
- Memory accounting uses retained capacities, not only logical vector sizes.
- Audit provenance is included in memory limits.
- BlockInPlace is not called a memory winner on captured n=72 data.
- Adaptive is installed only after a measured crossover exists.
- No reduced fixture is reported as FullTer.
- No optimization claim confuses partition speed, pair-work reduction, root time, attempt time, and whole-solver time.

---

# 17. Source map used for this audit

Inspect these files before repairing:

```text
include/ter/full_ter.hpp
include/ter/types.hpp
include/ter/limitations.hpp
include/ter/root_partition.hpp

src/full_ter.cpp
src/instance.cpp
src/root_partition.cpp
src/root_partition_current_scatter.cpp
src/root_partition_block_inplace.cpp

bench/ter_full_bench.cpp
bench/ter_root_partition_bench.cpp
bench/analyse_root_partition.py

tests/full_ter_tests.cpp
tests/root_partition_tests.cpp
tests/limitations_tests.cpp

README.md
FULL_TER_COMPLIANCE.md
implementation_plan.md
docs/root_v2_baseline.md
docs/root_partition_decision.md
```

Current `main` at audit time is:

```text
6e52c2ba18ee681189a01bfbebd88e8d4898b173
```

If local HEAD differs, diff these areas before applying repairs. Local source is authoritative.

---

# 18. Explicit instruction to the implementation agent

Do not merely “clean up” this list.

For every numbered problem:

```text
P0 ... P31
```

classify it as one of:

```text
FIXED
DEFERRED WITH JUSTIFICATION
NOT REPRODUCIBLE ON CURRENT HEAD
INVALIDATED BY NEWER CODE
```

For every `FIXED` item, cite:

```text
files changed
tests added
commands run
measured result if performance-related
```

Do not silently drop an item.

If a proposed fix changes TER mathematics rather than implementation/benchmark policy, stop and document the exact mathematical change and proof obligation before coding it.

The objective is a FullTER implementation whose name, benchmark, telemetry, resource behavior, and scalable exact root path all describe the same actual system instead of five different interpretations wearing one trench coat.
