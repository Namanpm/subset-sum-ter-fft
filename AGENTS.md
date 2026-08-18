# AGENTS.md

## Repository mission

This repository is correctness-critical research code for classical Ternary Representation Technique (TER) algorithms for random density-one Subset Sum, primarily based on Yang Li's depth-3 construction.

It currently contains:

- the frozen minimal/T0 TER merge baseline;
- a complete classical depth-3 TER implementation with topology `18 -> 9 -> 3 -> 1`;
- exact finite TER parameter derivation;
- scalar/reference correctness authorities;
- packed high-performance ternary representations;
- deterministic generators and fixtures;
- exact power-of-two modular arithmetic;
- tests, audits, telemetry, and single-threaded benchmarks;
- an isolated TER-Q3REPORT exact count-only cyclic NTT experiment for L2->L1 arithmetic analysis.

Primary priorities, in order:

1. mathematical correctness;
2. reproducibility;
3. scientific honesty;
4. native performance;
5. minimal unnecessary complexity;
6. preserving trustworthy baselines for research comparison.

A negative experimental result is valid research output. Never alter an experiment merely to make a proposed technique look useful.

---

## Read before editing

Before any non-trivial change:

1. inspect the repository tree;
2. inspect the relevant headers and implementation files;
3. inspect tests for the affected subsystem;
4. inspect the relevant design/research documents;
5. search before introducing a new API, type, helper, abstraction, or mathematical formula;
6. identify the current correctness authority.

Do not code from a prompt alone when the repository already defines the semantics. Do not assume an older planning document still matches the implementation.

---

## Source priority

Unless a task explicitly supplies a newer frozen specification, use:

```text
1. Explicit current task specification / frozen experiment plan
2. AGENTS.md
3. Current validated implementation semantics and differential tests
4. FULL_TER_COMPLIANCE.md for FullTer semantics
5. Dedicated current experiment/design document
6. Existing public repository contracts and headers
7. ter_minimal_prototype_implementation_plan.md
8. ter_ss_modular_convolution_master_spec.md
9. Published TER paper where the repository leaves a point unresolved
```

Do not silently resolve a genuine mathematical conflict by choosing the easiest implementation. Inspect the authorities, preserve currently validated behavior unless the task explicitly changes it, and report the discrepancy.

---

## Important documents

```text
AGENTS.md
README.md
FULL_TER_COMPLIANCE.md
ter_minimal_prototype_implementation_plan.md
ter_ss_modular_convolution_master_spec.md
TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md
```

`FULL_TER_COMPLIANCE.md` governs the classical FullTer solver contract. `TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md` governs the separately authorized count-only NTT experiment. Historical statements saying NTT is absent/out of scope refer to solver integration, not the already implemented isolated Q3REPORT experiment.

Q3REPORT does **not** authorize NTT integration into FullTer.

---

## Repository layout

Production/research code lives mainly in `include/ter/` and `src/`.

Key modules:

```text
arithmetic.*        exact power-of-two modular arithmetic
instance.*          density-one Subset Sum instance generation/storage
shape.*             finite TER shape construction
generator.*         T0/input snapshot generation and deterministic generator modes
reference_vector.*  obvious scalar ternary representation
packed.*            packed ternary representation and bit operations
validator.*         exact validation authorities
merge.*             frozen/minimal T0 merge
full_ter.*          complete depth-3 TER implementation
q3report_audit.*    isolated count-only TER-Q3REPORT arithmetic/NTT experiment
```

Current tests:

```text
tests/shape_tests.cpp
tests/core_tests.cpp
tests/generator_tests.cpp
tests/merge_tests.cpp
tests/packed_tests.cpp
tests/full_ter_tests.cpp
tests/q3report_audit_tests.cpp
```

Current benchmarks:

```text
bench/ter_baseline_bench.cpp
bench/ter_full_bench.cpp
bench/ter_q3report_bench.cpp
```

Do not reorganize the repository without a concrete task requiring it.

---

## FullTer semantics are strict

`FullTer` means complete classical TER execution with complete level-3 sets under the implemented finite parameters. It must never silently sample, truncate, cap, or substitute reduced lists.

If authentic complete materialization exceeds implementation or resource limits, return `ResourceLimit` or the appropriate explicit failure state. Never silently fall back to `FixtureTer`.

`FixtureTer` is intentionally reduced for correctness, pipeline, and controlled benchmark work. Never describe it as complete TER or full Algorithm-2 execution.

The implemented topology is:

```text
18 logical L3 lists
        -> 9 L2 lists via binary merges
        -> 3 L1 lists via ternary merges
        -> 1 L0 list via ternary root merge
```

The implementation may share immutable physical L3 bases where mathematically equivalent while preserving the logical 18-list semantics.

---

## Correctness authorities

Optimized code is not self-certifying.

Where enabled and feasible:

```text
L3 -> L2: optimized binary MITM checked against scalar/reference logic
L2 -> L1: optimized ternary merge checked against a cubic ternary oracle
L1 -> L0: optimized ternary merge checked against the root scalar oracle
final solution: independently verified against the original full Subset Sum target
```

Do not remove independent authorities simply to improve benchmark numbers. If optimized code disagrees with an independent oracle, treat the optimized code as wrong until proven otherwise.

---

## Exact arithmetic

TER arithmetic in this repository is exact and currently targets power-of-two moduli.

Never:

- use floating point for an exact solver decision;
- rely on undefined signed overflow;
- perform an unchecked shift that may equal/exceed the type width;
- replace target-aware quotient arithmetic with a superficially similar shift/mask without proving equivalence;
- duplicate critical arithmetic formulas across optimized paths when a canonical helper exists.

Independent test oracles should intentionally avoid sharing transformed formulas when shared logic could make implementation and oracle fail identically.

---

## Ternary representation

TER vectors represent `{-1,0,+1}^n`.

The scalar/reference representation is a correctness authority. Packed vectors use disjoint positive and negative masks:

```text
positive=0 negative=0 ->  0
positive=1 negative=0 -> +1
positive=0 negative=1 -> -1
positive=1 negative=1 -> invalid
```

Mask overlap is an invariant violation. Material packed changes require differential testing against scalar behavior.

---

## Parent semantics

A valid ternary parent must satisfy every required stage invariant, including:

- arithmetic target;
- every coordinate remaining in `{-1,0,+1}`;
- exact required ternary digit distribution;
- any stage-specific TER structural constraints.

Do not weaken checks for speed. Optimize their implementation instead.

Keep these concepts distinct:

```text
raw arithmetic triples
structurally valid raw triples
unique parent vectors
```

Audit modes may retain provenance and multiplicities; production semantics need the deduplicated logical parent set required by the solver.

---

## Failure semantics

Never conflate:

```text
Completed with empty output
GenerationIncomplete
AttemptLimitReached
ResourceLimit
Cancelled
InvalidConfiguration
InvalidShape
InvariantViolation
NotApplicable
ArithmeticSafetyFailure
```

In particular:

```text
AttemptLimitReached != UNSAT
NoWitnessThisAttempt != UNSAT
empty parent list != global Subset Sum UNSAT
NTT NotApplicable != zero candidate count
ResourceLimit != mathematical failure
```

A solution may be reported only after independent exact verification. Partial results must never masquerade as complete exact results. Fail closed.

---

## Determinism

Given the same seed, configuration, shape, execution mode, and input instance, logical outputs must reproduce: generated lists, targets, residues, parent sets, counters, fingerprints, and experiment classifications. Timing may vary.

Do not make correctness depend on nondeterministic container iteration.

---

## Generator semantics

The standalone snapshot generator supports reference, rejection, MITM performance, and planted MITM modes.

Do not describe generated data more strongly than its origin permits:

- a T0/generated snapshot is not automatically an authentic FullTer L2 snapshot;
- a planted fixture is not a random-target FullTer benchmark;
- a reduced fixture is not complete TER.

Preserve family, mode, seed, and list-size semantics in reports. Generation failure must remain explicit. Never silently emit fewer entries than requested and call generation complete.

---

## Q3REPORT count-only NTT experiment

The repository contains a separately scoped selected-target L2->L1 arithmetic experiment.

Its purpose is to test whether exact cyclic NTT pair counts remove enough TER arithmetic work to justify later witness-recovery research.

It is **not**:

- an NTT TER solver;
- witness recovery;
- proof of a better TER exponent;
- permission to integrate NTT/FFT into FullTer.

Current components include:

```text
FrozenL2MergeSnapshot
independent snapshot audit
target-aware quotient arithmetic
independent original-modulus cubic oracle
direct exact baseline
scalar cyclic convolution
exact count-only cyclic NTT
telemetry and decision gates
```

Snapshot families are semantically distinct:

```text
AUTHENTIC_FULLTER
    complete captured native FullTer L2->L1 data when feasible

DISTRIBUTION_MATCHED
    controlled/generated TER-like data, not complete FullTer evidence

FIXED_Q_MICROBENCH
    transform/kernel measurement only
```

Never relabel one family as another.

---

## Q3REPORT exactness and safety

An NTT coefficient may be interpreted as an exact integer count only when coefficient wraparound has been ruled out mathematically.

For a single prime `p`, require strictly:

```text
|A| * |B| < p
```

If exact reconstruction cannot be established, no NTT coefficient may authorize a zero prune. `NotApplicable` and `ArithmeticSafetyFailure` remain explicit states.

Do not introduce floating-point FFT into an exact pruning path without a complete, independently tested certification argument.

The current finite Q3REPORT benchmark results fail the cheap-kill gate in the tested regimes and do not authorize witness recovery. This is a finite-regime result, not a proof that every possible NTT/FFT formulation is impossible.

When extending the experiment:

- preserve the frozen existing results;
- distinguish authentic FullTer snapshots from generated scaling data;
- use multiple independent seeds/instances for empirical claims;
- measure crossover instead of selecting only favorable points;
- preserve negative results;
- add new benchmark regimes rather than rewriting old ones after seeing outcomes.

---

## Research integrity

This repository may support publishable research.

Never:

- fabricate or cherry-pick benchmark results;
- omit failed seeds because they are inconvenient;
- claim a test passed without running it;
- claim sanitizer cleanliness without running the sanitizer;
- claim an optimization is faster without before/after Release measurements;
- compare different inputs as if identical;
- call FixtureTer FullTer;
- call distribution-matched data authentic;
- hide mandatory transform/setup/reconstruction/memory costs;
- silently change mathematical semantics;
- infer asymptotic improvement from finite constant-factor benchmarks;
- claim an exponent improvement without a derivation/proof.

Label finite-n evidence as finite-n. Label heuristics as heuristics. Report inconclusive experiments as inconclusive.

---

## Performance philosophy

Correctness first, then optimization:

```text
simple/reference path
-> differential correctness
-> optimized implementation
-> differential correctness again
-> Release benchmark
-> profile
-> measured optimization
```

Once correctness is established, optimize aggressively but measurably.

Prefer:

- contiguous storage;
- flat arrays;
- compact hot records;
- preallocation/exact-size allocation;
- checked integer arithmetic;
- predictable iteration;
- cache locality;
- bit-parallel operations;
- simple compiler-friendly loops;
- sort + unique where appropriate.

Avoid in hot paths without evidence:

- pointer-heavy object graphs;
- `std::list` / `std::map`;
- per-candidate heap allocation;
- virtual dispatch;
- `std::function`;
- generic strategy/plugin frameworks;
- speculative abstraction.

Do not cargo-cult SIMD, intrinsics, or templates. Use them when profiling and benchmarks justify them.

---

## No premature abstraction / no hallucinated APIs

Before declaring or calling a new API:

1. search for existing functionality;
2. inspect the declaration;
3. inspect semantics/tests;
4. inspect nearby call sites;
5. create only the smallest concrete API required.

Do not invent factories, plugin systems, generic backends, strategy hierarchies, dispatch frameworks, or future hooks unless the current task genuinely requires them.

The repository is the authority, not an agent's conversational memory.

---

## Scope discipline

Modify the smallest reasonable set of files. Do not refactor unrelated code while implementing an experiment.

Do not introduce parallelism, GPU code, sparse convolution, Schroeppel-Shamir, witness recovery, parameter re-optimization, or new solver-dispatch architecture unless the current task explicitly authorizes it.

Existing experimental code may of course be maintained and tested.

---

## Testing requirements

Material correctness changes must add or update tests.

Current CMake tests:

```text
ter_shape_tests
ter_core_tests
ter_generator_tests
ter_merge_tests
ter_packed_tests
ter_full_tests
ter_q3report_audit_tests
```

Run the narrowest relevant target during iteration, then all relevant regression tests before completion.

If shared arithmetic/types change, run the full suite. If FullTer changes, run FullTer plus shared lower-level suites. If Q3REPORT changes, run Q3REPORT differential tests plus shared arithmetic/FullTer regressions.

Never delete, weaken, skip, or rewrite a failing correctness test merely to make a task green. Differential mismatches are blockers.

---

## Standard build/test commands

Windows/MSVC Release:

```powershell
cmake -S . -B build-full -DTER_NATIVE_OPT=ON
cmake --build build-full --config Release --parallel
ctest --test-dir build-full -C Release --output-on-failure
```

Do not report performance from Debug builds.

AddressSanitizer where supported:

```powershell
cmake -S . -B build-asan -DTER_ENABLE_ASAN=ON
cmake --build build-asan --config Debug --parallel
ctest --test-dir build-asan -C Debug --output-on-failure
```

Adapt to the active compiler/toolchain rather than blindly assuming MSVC. If a sanitizer is unsupported, state that instead of claiming it passed.

---

## Compiler/warning policy

The project is C++20. New code must compile cleanly under the strict warning configuration already defined in `CMakeLists.txt`.

Avoid undefined behavior, unchecked narrowing/size multiplication, invalid shifts, dangling spans/references, references invalidated by vector reallocation, uninitialized reads, and accidental signed/unsigned conversions in correctness-critical arithmetic.

Treat meaningful warnings as defects.

---

## Benchmark rules

Benchmarks are scientific measurements.

Use Release builds, deterministic workloads, warm-up where appropriate, repeated measurements, and medians/dispersion where useful.

Record enough configuration to reproduce the workload. Separate core algorithm time from audit/reference/reporting/fixture-generation time unless the benchmark explicitly measures end-to-end cost.

Do not hide mandatory algorithm costs or charge optional research telemetry as mandatory production work.

Compare identical input, semantics, thread count, build mode, optimization policy, and warm-up policy.

Current benchmark executables:

```text
ter_baseline_bench
ter_full_bench
ter_q3report_bench
```

`ter_full_bench` measures FullTer where the chosen finite instance is actually feasible. `ter_q3report_bench` is an experimental comparison harness, not solver throughput.

---

## Memory/resource limits

Resource limits are part of semantics.

Use checked arithmetic before large allocations where practical. Do not rely on allocation failure when preflight can prove impossibility.

If an authentic complete case is too large, return `ResourceLimit`. Do not silently reduce it. Reduced fixtures must be explicitly requested and labelled.

---

## Mathematical changes

For material changes to TER mathematics:

1. state the invariant being changed;
2. identify the current correctness authority;
3. add a simple independent reference where feasible;
4. implement the change;
5. run differential tests;
6. test boundary/wrap cases;
7. run regressions;
8. benchmark only after exact agreement.

Document non-obvious identities near the code or in the relevant research document. Do not leave correctness dependent on an undocumented clever trick.

---

## Working style for coding agents

Operate autonomously on routine repository work. Do not stop after every small step for approval. Do not spend context on ceremonial progress narration.

Use this workflow:

```text
inspect -> understand -> plan minimally
edit -> build narrow target -> test -> debug
full relevant verification -> inspect diff -> concise evidence report
```

Ask the user only when a genuinely blocking ambiguity cannot be resolved from the repository or supplied task.

---

## Preserve user work / Git discipline

Never discard unrelated user modifications.

Do not reset unrelated changes, mass-format unrelated code, delete research artifacts, or commit generated build outputs.

Do not commit, stage, push, reset, or rewrite history unless explicitly requested. If a commit is requested, inspect the diff, run required verification, exclude generated artifacts/secrets, and include only intended changes.

---

## Documentation discipline

When living implementation semantics change, update the relevant living documentation. Do not rewrite a frozen historical experiment plan merely because results changed. Prefer a result/addendum or clearly marked implementation section.

Keep terminology exact:

```text
FullTer
FixtureTer
AUTHENTIC_FULLTER
DISTRIBUTION_MATCHED
FIXED_Q_MICROBENCH
```

These labels carry scientific meaning.

---

## Completion requirements

Do not claim completion merely because the code compiles.

Before completion:

- run all tests relevant to modified semantics;
- inspect actual outputs;
- run the full suite when shared/core code changed;
- run sanitizers when required and supported;
- run Release benchmarks before performance claims;
- inspect the final diff;
- verify failure semantics remain correct;
- verify no accidental scope creep occurred.

If verification could not be performed, state exactly what was not run and why.

---

## Final agent report

Keep final responses concise and evidence-dense. For implementation tasks report:

```text
Files changed
What changed
Important design/correctness decisions
Tests/builds actually run
Benchmarks actually measured, if relevant
Known limitations or blockers
Git/commit status when relevant
```

Do not paste whole source files unless asked. Do not repeat the task specification. Do not claim success without evidence.

---

## Non-negotiable rules

1. Correctness beats speed.
2. Scientific honesty beats a favorable result.
3. FullTer never silently truncates.
4. FixtureTer is never called complete FullTer.
5. Attempt exhaustion is never UNSAT.
6. Every reported solution receives independent exact verification.
7. Exact solver decisions do not rely on unsafe floating-point assumptions.
8. Never weaken tests to make an optimization pass.
9. Never invent APIs without inspecting the repository.
10. Never silently change TER mathematics.
11. Never silently broaden task scope.
12. Never claim performance improvement without measured Release evidence.
13. Never claim asymptotic improvement from finite benchmarks.
14. Preserve independent correctness authorities.
15. Preserve reproducibility and deterministic semantics.
16. Negative experimental results are legitimate results.
17. Do not implement future recovery/solver work merely because an experiment hints at it.
18. Do not commit or push unless explicitly requested.
19. Do not discard unrelated user work.
20. Re-read these rules before declaring a substantial task complete.
