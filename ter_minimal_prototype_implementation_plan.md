# TER Minimal Prototype Implementation Plan

Status: T0 baseline implemented and verified as of 2026-08-14.

Current implementation stop line: only the authentic child generation, exact audit,
cubic tiny oracle, pair-materialized scalar baseline, packed equivalent, tests,
telemetry, and benchmark described by the T0 milestone are implemented. The direct
bucket and NTT sections below are retained solely as future research design; they
are not authorized or implemented in the current milestone.

Revision: 2026-08-13. This version incorporates the external design review concerning hot-entry size, contiguous residue indexes, exact NTT scope, validator separation, realistic TER shapes, lower-level MITM fixture generation, dynamic orientation, output policies, and paired benchmark methodology.

## 1. Decision and research question

Build one authentic TER level-2 to level-1 merge experiment before implementing full TER.

The experiment must answer:

> Can an exact one-universe NTT support prefilter save enough residue-probe work to beat an optimized direct-bucket TER merge after all transform, recovery, structural-validation, and output-construction costs are included?

The NTT is not expected to reduce the number of genuine modular candidate triples. For a fixed input snapshot:

```text
exact_candidate_triples_direct == exact_candidate_triples_ntt
structural_checks_direct        == structural_checks_ntt
```

Its only proposed advantage is bulk detection of impossible pair-sum residue classes, thereby eliminating fruitless residue-level probes before exact recovery.

This is a local, empirical optimization study. It does not claim an improved TER exponent or a faster complete subset-sum solver.

## 2. Hard scope boundary

### Included

- Density-1 random subset-sum instance generation.
- Genuine ternary entries over `{-1, 0, +1}`.
- Authentic level-2 digit distributions and balanced half support inherited from TER's lower MITM construction.
- Authentic nested power-of-two modular targets.
- Three genuine level-2 child lists.
- One real level-2 to level-1 ternary merge.
- Exact level-1 parent construction and validation.
- A tiny cubic correctness oracle.
- A practical pair-materialized baseline.
- An optimized direct-bucket implementation.
- A one-universe Boolean-support NTT prefilter with exact recovery.
- Deterministic fixtures, differential tests, telemetry, and paired benchmarks.
- Both correctness-oriented and paper-like finite TER shape families.

### Excluded

- The full `18 -> 9 -> 3 -> 1` TER pipeline.
- Final root search or subset-sum solution recovery.
- A full asymptotic parameter optimizer.
- Other TER depths.
- Schroeppel-Shamir integration or modification.
- Multiple TER filtering moduli, CRT filtering, or two-modulus experiments.
- Sparse convolution, FFT, FWHT, Bloom filters, neural routing, or adaptive mixtures of experimental filters.
- Parallel execution in V1.
- Quantum TER and decoding extensions.
- Publication, novelty, or improved-exponent claims.

## 3. Success and veto conditions

Correctness is a hard veto. Every completed strategy must produce exactly the same TER result semantics.

```text
raw_valid_triples(cubic_or_pair_baseline)
    == raw_valid_triples(direct_bucket)
    == raw_valid_triples(ntt_hybrid)

unique_parent_list(cubic_or_pair_baseline)
    == unique_parent_list(direct_bucket)
    == unique_parent_list(ntt_hybrid)
```

The NTT path is scientifically useful only if it beats direct buckets, not merely the pair-materialized baseline.

Proceed toward full classical TER only when all of the following hold:

1. No output mismatch or false negative occurs in any completed correctness case.
2. NTT total time includes index construction, support encoding, transforms, recovery, exact validation, deduplication, and parent-list construction.
3. The median paired ratio `T_ntt / T_bucket` is at most `0.80` on each of the three largest feasible primary configurations.
4. The bootstrap 95% confidence interval for the paired ratio lies below `1.0` on those configurations.
5. The advantage is present across at least ten independent snapshots/seeds, not produced by repeated timing of one snapshot.
6. The advantage persists or improves as list sizes grow.
7. It appears on `PAPER_ROUNDED`, not only `FINITE_SANITY`.
8. Peak memory is measured and remains within the declared benchmark ceiling.

Interpretation rules:

- NTT beats the pair baseline but loses to direct buckets: modular indexing is useful; NTT is not.
- NTT and buckets are approximately tied: stop unless a future batched or repeated-target experiment is explicitly authorized.
- NTT loses because residue support saturates: preserve the negative result.
- NTT clearly and repeatedly beats direct buckets on `PAPER_ROUNDED`: design the complete classical TER pipeline next and test whether the local gain survives end to end.

## 4. Authenticity contract

The prototype is small in scope but must preserve the actual semantics of the selected TER stage.

For a ternary vector distribution `D^n[alpha, beta]`:

```text
negative_count = alpha * n
positive_count = (alpha + beta) * n
zero_count     = (1 - 2*alpha - beta) * n
```

The level-2 child domain is `D^n[w2, 1/18]`. The level-1 parent domain is `D^n[w1, 1/6]`.

Every level-2 entry must also be constructible as the concatenation of two valid half-supported level-3 entries. Consequently, its negative and positive counts must split evenly across the first and second halves.

An entry is not authentic merely because it has a plausible residue. It must carry a real ternary vector, satisfy its exact digit counts, satisfy its assigned modular target, and pass the balanced-half rules.

## 5. Finite TER shape families

Use two explicit families so clean correctness cases are not confused with performance realism.

### 5.1 `FINITE_SANITY`

Purpose:

- Unit tests.
- Exhaustive generation.
- Planted witnesses.
- Cubic-oracle comparisons.
- Differential debugging.

Require `n` to be divisible by 36.

For each complete level-2 child:

```text
child_negative_count = 2
child_positive_count = 2 + n/18
```

Each half therefore contains:

```text
half_negative_count = 1
half_positive_count = 1 + n/36
```

The target level-1 parent contains:

```text
parent_negative_count = 2
parent_positive_count = 2 + n/6
```

This family deliberately prioritizes exact integer structure and easy planted decompositions. It is a correctness authority, not the primary performance distribution.

Initial sanity sizes:

```text
n in {36, 72, 108, 144}
```

### 5.2 `PAPER_ROUNDED`

Purpose:

- Primary performance benchmarks.
- Residue occupancy measurements.
- Structural-survivor measurements.
- Direct bucket versus NTT conclusions.

Use the published optimized values:

```text
w1 = 0.0156
w2 = 0.0059
```

Require `n` to be divisible by 36. Derive finite counts deterministically:

```text
child_negative_count = nearest feasible even integer to w2*n
child_positive_count = child_negative_count + n/18

parent_negative_count = nearest feasible integer to w1*n
parent_positive_count = parent_negative_count + n/6
```

Tie-breaking rounds downward. `child_negative_count` must be even because it is split across the two level-3 halves. All counts must be nonnegative and sum to at most `n`.

The shape builder must run an exact feasibility check for a three-child decomposition into the requested parent distribution. Model coordinate types as the 27 ordered triples in `{-1,0,+1}^3`; solve for nonnegative integer pattern counts satisfying all child marginals and parent marginals. A deterministic integer-flow or bounded integer-feasibility routine is acceptable. If no solution exists, return `InvalidShape`; never silently alter counts.

Suggested starting sizes:

```text
n in {360, 720, 1080}
```

The requested list size is independent of the theoretical full TER cardinality. The benchmark samples authentic entries without claiming to materialize asymptotic lists.

Every result row must name its shape family and exact integer counts. No result may be reported merely as "TER-like."

## 6. Arithmetic and nested-target model

Generate a density-1 instance:

```text
a_i in Z_N
N = 2^n
```

A planted binary vector of weight `n/2` may define the root target for reproducibility, although the root is not searched in this prototype.

The selected merge uses nested powers of two:

```text
M2 = 2^b2
M1 = 2^b1
Q  = M1 / M2 = 2^(b1-b2)

0 < b2 < b1 < n
```

`M2` is the modular constraint already established in each child list. `M1` is the stronger level-1 parent constraint. `Q` is the remaining quotient-residue universe used by buckets and NTT.

Choose child targets `tA2`, `tB2` and parent target `t1`. Derive:

```text
tC2 = (t1 - tA2 - tB2) mod M2
```

For child-list `J` with target `tJ2`, every entry must satisfy:

```text
inner_product(a, entry) == tJ2 mod M2
```

Let `x` be the entry's inner-product residue modulo `M1`. Define:

```text
delta = (x - tJ2) mod M1
q     = delta / M2 mod Q
```

The division is exact because the child already satisfies its `M2` target.

Define:

```text
tau = (t1 - (tA2 + tB2 + tC2)) / M2 mod Q
```

Then the parent arithmetic condition is exactly equivalent to:

```text
qA + qB + qC == tau mod Q
```

### Power-of-two hot-path operations

Centralize all arithmetic in one module and use identical operations in every strategy:

```text
maskQ  = Q - 1
maskM1 = M1 - 1

delta    = (x - child_target) & maskM1
q        = delta >> b2
pair_sum = (qX + qY) & maskQ
required = (tau - qX - qZ) & maskQ
```

No merge implementation may independently redefine signed or modular normalization.

## 7. Reference and hot data representations

### 7.1 Reference representation

Use for correctness work:

```cpp
ReferenceTernaryVector = contiguous int8_t digits
```

Each coordinate is exactly `-1`, `0`, or `+1`.

Reference entries retain enough sidecar data to audit generation and arithmetic, but the merge-facing entry remains minimal.

### 7.2 Hot representation

After the reference baseline is proven, introduce:

```text
PackedTernary
    positive_mask[]
    negative_mask[]
```

Per coordinate:

```text
positive=0, negative=0 ->  0
positive=1, negative=0 -> +1
positive=0, negative=1 -> -1
positive=1, negative=1 -> invalid
```

The packed representation must mask unused high bits in the final machine word. Conversion to and from the reference representation must be deterministic and differentially tested.

### 7.3 Compact `TerEntry`

The merge hot path stores only:

```cpp
struct TerEntry {
    EntryId id;
    QuotientResidue q;
    PackedTernary digits;
};
```

During the initial reference phases, `digits` is the `int8_t` representation instead.

Do not duplicate these values in every entry:

- The list's `M2` target.
- Source-list identity.
- Original `M1` residue.
- Generation provenance.
- Diagnostic hashes.

They belong in list metadata or optional debug sidecars keyed by `EntryId`.

IDs use 32 bits in V1. A configuration requiring more than `2^32-1` entries is unsupported.

## 8. Core types and ownership

### `FiniteTerShape`

Owns exact child-half, full-child, and parent digit counts plus family and feasibility evidence.

### `Instance`

Owns the master seed, `n`, `N`, full input numbers, hot residues modulo `M1`, optional planted root vector, root target, and instance fingerprint.

Full `n`-bit arithmetic may use `boost::multiprecision::cpp_int` during generation and audit. The merge path uses fixed-width residues after configuration validation.

### `TerList`

Owns immutable entries and list-level metadata:

- List role.
- `M2` target.
- Shape.
- Optional debug sidecars.

### `MergeSpec`

Owns `M2`, `M1`, `Q`, target values, `tau`, shape, output policy, validator policy, and resource ceilings.

### `InputSnapshot`

Owns the instance, merge specification, and immutable lists A, B, and C. Algorithms receive read-only views such as `std::span<const TerEntry>`.

The snapshot is hashed once before timing. An optional debug integrity hash is computed after all algorithms finish. Hashing is never inside a measured algorithm region.

### `DebugSidecar`

Optional correctness-only records keyed by entry ID:

- Original residue modulo `M1`.
- Generation provenance.
- Half-entry IDs.
- Diagnostic fingerprints.

### `ParentRecord`

Contains the exact packed parent vector and its audited residue. Provenance is attached only under `CorrectnessMode`.

## 9. Authentic child-list generators

All generator modes must emit entries satisfying the same shape, balanced-half, modular-target, and uniqueness invariants.

### 9.1 `EXHAUSTIVE_REFERENCE`

For tiny sanity cases:

1. Enumerate every first-half vector with the exact half distribution.
2. Enumerate every second-half vector with the exact half distribution.
3. Concatenate matching pairs.
4. Retain entries satisfying the child target modulo `M2`.
5. Sort canonically and remove duplicate full vectors.

This generator is simple and authoritative, not scalable.

### 9.2 `REJECTION_REFERENCE`

For moderate diagnostic cases:

1. Uniformly sample negative and positive positions independently within each half while preserving disjointness and exact counts.
2. Form the complete level-2 vector.
3. Compute its arithmetic residue.
4. Retain it only when the assigned `M2` target matches.
5. Deduplicate by full vector.

Record attempts, acceptances, rejection causes, and observed acceptance rate. Exceeding the attempt budget returns `GenerationIncomplete`.

### 9.3 `MITM_PERFORMANCE`

Primary performance-fixture generator:

1. Generate a deterministic pool of authentic first-half vectors.
2. Compute each first-half arithmetic residue modulo `M2` and build a flat residue index.
3. Generate an independent deterministic pool of authentic second-half vectors.
4. Compute and index their residues modulo `M2`.
5. For each first-half residue, identify the complementary second-half residue required by the child target.
6. Compute the population size of all compatible half pairs.
7. Uniformly sample compatible pair indices without replacement using deterministic global pair ranks and prefix counts.
8. Concatenate the selected halves, compute `q`, deduplicate full vectors, and audit every emitted entry.
9. Grow half pools deterministically when the requested unique list size is not reached, stopping at the declared resource limit.

This samples from the compatible pairs in the generated lower-list pools and mirrors TER's lower MITM structure. It must not be described as uniform over the complete theoretical level-2 domain unless that has been separately established.

### 9.4 `PLANTED_MITM`

Use the finite-shape feasibility solution to construct one authentic valid parent decomposition. Insert each child's two halves into the appropriate lower pools, derive compatible targets, run the same MITM assembly path, and verify that the planted entries survive.

Filler entries are produced using `MITM_PERFORMANCE`. Failure to retain the planted triple is `InvariantViolation`.

### Generator result semantics

- `Completed`: exact requested list sizes produced.
- `InvalidShape`: finite distributions or planted decomposition infeasible.
- `GenerationIncomplete`: attempt/pool limit reached before requested size.
- `ResourceLimit`: declared memory/work ceiling reached.
- `InvariantViolation`: an emitted entry fails audit.

Shorter-than-requested lists are never silently benchmarked.

## 10. Flat contiguous residue indexes

Use a CSR-style index for all entry buckets:

```cpp
struct ResidueIndex {
    std::vector<uint32_t> offsets;   // size Q+1
    std::vector<EntryId> ids;        // size number_of_entries
    std::vector<Residue> nonempty;   // sorted occupied residues
};
```

Residue `r` owns the contiguous range:

```text
ids[offsets[r] .. offsets[r+1])
```

Build it with a deterministic two-pass count/prefix/fill algorithm. Preserve input entry order within a residue bucket.

Do not use `vector<vector<EntryId>>` or hash tables in primary performance code.

### Pair-materialized CSR index

The practical baseline stores pair IDs as packed 64-bit values:

```text
high 32 bits = first entry ID
low  32 bits = second entry ID
```

Use the same count/prefix/fill construction for pair-sum residue buckets. Pair storage exceeding the declared ceiling returns `ResourceLimit` before partial output can be mistaken for completion.

## 11. Validators and exact parent semantics

All strategies feed candidates through one validation interface. There are two implementations with identical acceptance semantics.

### 11.1 Snapshot prevalidation

Before timed benchmark execution, audit every child entry once:

- Valid ternary encoding.
- Exact child distribution.
- Exact balanced-half distribution.
- Exact `M2` target.
- Correct stored quotient residue `q` derived from the audited `M1` residue.
- Unique vector identity within its list.

Any failure invalidates the snapshot.

### 11.2 `AuditValidator`

Correctness authority:

1. Recheck the quotient-residue equation.
2. Add all three vectors coordinate by coordinate.
3. Reject a coordinate sum outside `{-1,0,+1}`.
4. Count exact parent negatives, positives, and zeros.
5. Require the exact target parent distribution.
6. Recompute the full parent inner product modulo `M1`.
7. Require the parent target.
8. Emit the exact parent vector and provenance triple.

### 11.3 `FastValidator`

Performance path:

1. Recheck the quotient-residue equation.
2. Use packed masks, exact Boolean logic, and `popcount` to reject invalid coordinate sums and derive the parent masks.
3. Require the exact parent distribution.
4. Audit the full inner product once when a unique parent is first emitted.

Prevalidated child arithmetic plus the quotient equation makes a full inner-product recomputation unnecessary for every duplicate candidate. Every unique emitted parent is still independently audited once.

The packed structural logic must be derived from the complete 27-case ternary truth table and differentially tested against coordinatewise `int8_t` addition.

### 11.4 Parent list semantics

The TER output list is the set of unique exact parent vectors. Candidate provenance multiplicity is separate diagnostic information.

Use a shared output finalizer for every strategy. It must:

- Intern or collect exact parent vectors.
- Produce a deterministic canonical order.
- Remove duplicate parent vectors.
- Preserve raw valid-triple counts.
- Optionally preserve full provenance under correctness policy.

Output construction and deduplication are included in timed merge totals because they are part of producing the real parent list.

## 12. Output policies

### `CorrectnessMode`

Store:

- Every valid `(a,b,c)` provenance triple.
- Every parent vector.
- Exact multiplicity per parent.
- Canonical parent set.
- Diagnostic fingerprints.

Use only for bounded differential cases.

### `BenchmarkMode`

Store:

- The actual unique TER parent list.
- Raw valid-triple count.
- Unique-parent count.
- Optional rolling candidate fingerprint that does not replace the parent list.

Do not materialize every provenance triple. Canonical output hashing for cross-strategy comparison occurs outside the timed region after the real output list has been constructed.

## 13. Merge orientation planning

Do not hard-code `A+B -> C`.

Use one shared orientation vocabulary, canonical permutation rules, and deterministic tie-breaking. Each algorithm may use its own declared cost model because its unavoidable work differs. Orientation planning overhead is included in measured preprocessing.

### 13.1 Pair-materialized baseline planner

Choose materialized pair `(X,Y)` and lookup list `Z` minimizing:

```text
entry_count(X) * entry_count(Y)
```

Tie-break lexicographically by list roles.

### 13.2 Direct-bucket planner

Choose two occupied-residue lists to loop and the third to look up. Minimize the exact probe estimate:

```text
occupied(X) * occupied(Z)
```

where each `(rX,rZ)` probe computes the required `rY` and checks that bucket. Evaluate all three lookup choices and both equivalent loop-name orderings only for deterministic tie-breaking; do not double-count symmetric work.

### 13.3 NTT planner

For candidate convolution pair `(X,Y)` and filtered list `Z`, estimate pair-sum support density from occupied counts:

```text
estimated_pair_support = Q * (1 - (1 - occupied(X)/Q)^occupied(Y))
estimated_active_Z     = min(occupied(Z),
                             occupied(Z) * estimated_pair_support / Q)
estimated_recovery     = min(occupied(X), occupied(Y)) * estimated_active_Z
```

Choose the orientation minimizing:

```text
estimated_transform_cost(Q) + estimated_recovery
```

Compute only the selected convolution in the primary path. Computing all three convolutions and charging only one would be invalid. If an experimental all-three planner is added, all three forward/product/inverse costs must be included.

After convolution, recover using the smaller occupied side of `(X,Y)` against the active residues of `Z`, looking up the other side.

Record predicted and actual active-residue counts so planner quality can be evaluated.

### 13.4 Locked-orientation diagnostic

In addition to each algorithm's optimized primary result, optionally run a diagnostic where direct buckets and NTT use the same explicitly fixed orientation. This isolates the NTT filtering mechanism from orientation quality. It is not a substitute for the optimized head-to-head result.

## 14. Merge algorithms

### 14.1 Cubic oracle

For tiny correctness cases only:

```text
for a in A
  for b in B
    for c in C
      exact modular check
      AuditValidator
```

This is the independent enumeration authority, not the performance baseline.

### 14.2 Pair-materialized practical baseline

1. Choose orientation.
2. Count every selected pair sum modulo `Q`.
3. Build the flat pair-sum CSR index.
4. For every entry in the lookup list, compute the required pair residue.
5. Recover every stored pair in that bucket.
6. Send the resulting triple through the selected exact validator.
7. Build the real unique parent list through the shared output finalizer.

All pair construction, storage, lookup, validation, and output costs are timed.

### 14.3 Direct modular buckets

1. Build flat CSR indexes for A, B, and C.
2. Choose the lowest estimated residue-probe orientation.
3. For every occupied residue in the first loop list:
4. For every occupied residue in the second loop list:
5. Compute the only required residue in the lookup list.
6. If its bucket is nonempty, enumerate the Cartesian product of the three entry buckets.
7. Send every recovered triple through the selected exact validator.
8. Build the real unique parent list.

One `direct_residue_probe` is one occupied-residue pair for which the required lookup residue is computed and tested.

### 14.4 One-universe NTT hybrid

1. Build the same flat CSR indexes.
2. Choose one convolution orientation using the declared NTT cost model.
3. Build Boolean support arrays for the selected pair:

   ```text
   SX[r] = 1 iff bucketX[r] is nonempty
   SY[r] = 1 iff bucketY[r] is nonempty
   ```

4. Compute their exact cyclic convolution over length `Q`.
5. Mark lookup residue `rZ` active iff:

   ```text
   convolution[(tau - rZ) mod Q] != 0
   ```

6. Recover exact entries using the same CSR bucket ranges as the direct method, but probe only active `Z` residues.
7. Send every recovered modular triple through the same exact validator.
8. Build the same real unique parent list.

The NTT never emits a witness and never replaces exact recovery.

### 14.5 NTT exactness contract

Use Boolean inputs, not entry-count histograms. With cyclic length `Q`, each integer convolution coefficient is at most `Q`.

For prime `998244353`:

```text
Q must be a supported power of two
Q <= 2^23
Q < 998244353
```

Therefore a positive coefficient cannot wrap to zero in the transform field. Unsupported lengths return `UnsupportedNttLength`; floating-point FFT fallback is prohibited.

A zero coefficient is safe negative evidence for that residue class. A nonzero coefficient is only support evidence and never witness evidence.

## 15. Telemetry contract

### Common identity and configuration

- Instance seed and snapshot ID.
- Shape family and exact integer counts.
- `n`, number bit width, and measured density.
- `M2`, `M1`, `Q`, `b2`, `b1`, and `tau`.
- Generator mode and planted/natural workload.
- Input list sizes.
- Terminal status.
- Validator and output policies.
- Chosen orientation and lookup list.
- Orientation planning time and predicted cost.

### Occupancy

- Occupied residues for A, B, and C.
- Mean, median, p95, and maximum nonempty-bucket occupancy.
- Empty-bucket fraction.
- Optional occupancy histogram.

### Candidate conservation

- Modular candidate triples recovered.
- Structural checks.
- Structurally valid raw triples.
- Unique parent outputs.
- Duplicate-parent count.

For completed direct and NTT runs on one snapshot:

```text
modular_candidate_triples_direct == modular_candidate_triples_ntt
structural_checks_direct          == structural_checks_ntt
```

Any violation is an invariant failure.

### Pair baseline

- Entry pairs examined.
- Pair records materialized.
- Nonempty pair-sum buckets.
- Pair-index bytes.
- Pair-build, recovery, validation, output, and total times.

### Direct buckets

- CSR build time and bytes.
- `direct_residue_probes`.
- Compatible residue triples.
- Recovery, validation, output, and total times.

### NTT

- CSR build time and bytes.
- Support-encoding time.
- Forward-transform time.
- Pointwise-product time.
- Inverse-transform time.
- Pair-sum support size.
- Occupied lookup residues before filtering.
- Active lookup residues after filtering.
- `ntt_residue_probes`.
- `residue_probes_eliminated_by_ntt`.
- Predicted versus actual active residues.
- Recovery, validation, output, and total times.

### Resource accounting

- Peak working set where available.
- Internal owned-byte estimate by component.
- Pair-index, CSR, NTT scratch, candidate, and output bytes.
- Allocation count in debug/performance-instrumented builds where practical.

## 16. Timing and benchmark boundaries

### Excluded from algorithm timing

- Instance and snapshot generation.
- Reference-to-packed conversion shared by all algorithms.
- Snapshot prevalidation.
- Initial snapshot hash.
- Post-run integrity hash.
- Cross-strategy canonical output comparison.
- Process startup and result-file serialization.

### Included in each algorithm timing

- Its orientation planning.
- Its index construction.
- All algorithm-specific preprocessing.
- Pair materialization or NTT transforms.
- Exact recovery.
- Fast structural validation.
- Unique-parent auditing.
- Parent-list construction and deduplication.

The benchmark must not reuse a prebuilt CSR index unless a separately labeled reuse experiment charges the common construction consistently to all applicable methods.

## 17. Benchmark design

### 17.1 Correctness lane

Use `FINITE_SANITY`, `CorrectnessMode`, the reference representation, `AuditValidator`, and list sizes small enough for the cubic oracle.

Cover:

- Natural and planted snapshots.
- Empty and nonempty outputs.
- Sparse and dense quotient occupancy.
- Multiple `Q` values.
- At least 20 deterministic seeds per bounded configuration.

Require exact provenance-multiset and unique-parent equality.

### 17.2 Packed/common-core validation lane

On bounded snapshots, run every algorithm using both:

- Reference `int8_t` vectors plus `AuditValidator`.
- Packed vectors plus `FastValidator`.

Require exact equality before packed structures may be used for primary timing.

### 17.3 Primary performance lane

Use:

- `PAPER_ROUNDED`.
- `MITM_PERFORMANCE` and `PLANTED_MITM` snapshots.
- Packed vectors.
- Compact hot entries.
- Flat CSR indexes.
- `FastValidator`.
- `BenchmarkMode`.
- Single-threaded execution.

Sweep independently:

- `n`.
- Requested child-list size.
- `Q`.
- `M2`.
- Natural versus planted snapshots.
- Residue occupancy regimes.

Start conservatively and increase sizes until a declared time or memory boundary is reached. The pair-materialized baseline may return `ResourceLimit` at sizes where direct and NTT continue; report this rather than weakening its storage semantics.

### 17.4 Execution order

For each snapshot:

1. Construct, prevalidate, pack, and hash it outside timing.
2. Generate a deterministic permutation of `baseline`, `bucket`, and `ntt` from the snapshot seed.
3. Perform one untimed warm-up per applicable algorithm.
4. Run measured algorithms in interleaved deterministic order.
5. Rotate order across repetitions when repetitions are used.
6. Compute the optional final integrity hash after all runs.
7. Compare outputs outside timing and invalidate the row on mismatch.

Independent snapshots/seeds are the primary samples. Repetitions on one snapshot estimate timing noise only.

Recommended minimum:

```text
10 independent snapshots per primary configuration
3 measured repetitions per algorithm per snapshot
```

Increase repetitions only when within-snapshot noise materially affects conclusions.

### 17.5 Statistics

For each snapshot compute paired ratios:

```text
rho_ntt_bucket   = median(T_ntt) / median(T_bucket)
rho_bucket_pair  = median(T_bucket) / median(T_pair_baseline)
rho_ntt_pair     = median(T_ntt) / median(T_pair_baseline)
```

Across snapshots report:

- Median paired ratio.
- Interquartile range.
- Bootstrap 95% confidence interval.
- Count of seeds won/lost/tied.
- Absolute median times.
- Minimum only as a diagnostic.

Do not treat repeated runs of one snapshot as independent samples.

## 18. Required tests

### Arithmetic

- Power-of-two configuration validation.
- Mask and shift operations equal general modulo/division reference operations.
- Nested-target derivation.
- Quotient equivalence to the full `M1` equation.
- Wraparound near `0`, `M2-1`, `M1-1`, and `Q-1`.

### Shapes

- Exact `FINITE_SANITY` counts.
- Deterministic `PAPER_ROUNDED` rounding and tie-breaking.
- Balanced-half feasibility.
- Exact 27-pattern planted-decomposition feasibility.
- Infeasible shapes return `InvalidShape`.

### Representations

- `int8_t` to packed to `int8_t` round trip.
- Unused high-bit masking.
- Invalid overlapping positive/negative bits rejected.
- Packed truth-table validation equals coordinatewise reference validation.

### Generators

- Every emitted entry satisfies exact full and half distributions.
- Every entry satisfies its `M2` target and stored `q`.
- Exhaustive direct generation equals explicit lower MITM on tiny domains.
- Rejection and MITM modes obey the same output invariants.
- Planted triple survives `PLANTED_MITM`.
- Duplicate full vectors are removed deterministically.
- Attempt and memory exhaustion return explicit non-completed states.

### CSR indexes

- Offsets are monotone and end at `ids.size()`.
- Bucket contents equal a simple reference grouping.
- Nonempty residue list is exact and sorted.
- Stable input order within buckets.
- Packed pair IDs encode/decode correctly.

### Validators and outputs

- Coordinate sums `-3`, `-2`, `2`, and `3` are rejected.
- Exact parent distribution enforced.
- Audit and fast validators accept identical candidates.
- Every unique emitted parent passes independent arithmetic audit.
- Duplicate parents preserve correct raw count and unique-list semantics.
- Correctness and benchmark output policies produce the same unique parent list.

### Orientations

- Every legal orientation produces identical outputs.
- Deterministic tie-breaking.
- Direct planner chooses the minimum declared probe estimate.
- NTT planner records predicted and actual active residues.
- Locked-orientation direct and NTT equality.

### NTT

- Forward/inverse round trip.
- Cyclic convolution equals an `O(Q^2)` integer reference.
- Empty, singleton, wraparound, and fully dense supports.
- Positive Boolean-support coefficient never becomes zero.
- Unsupported transform lengths fail explicitly.

### End-to-end differential

- Cubic oracle equals pair baseline.
- Pair baseline equals direct buckets.
- Direct buckets equal NTT.
- Raw provenance multiplicity equality in correctness mode.
- Unique-parent equality in both output modes.
- Candidate-conservation telemetry equality.
- Deterministic replay from snapshot seed and configuration.

### Terminal semantics

- `Completed` with zero parents remains distinct from all failure states.
- Partial work after cancellation or resource exhaustion is never compared or reported as complete.

## 19. Result and failure semantics

- `Completed`: exact parent list is complete for the supplied snapshot.
- `InvalidConfiguration`: modulus, size, or policy contract is invalid.
- `InvalidShape`: finite TER counts or planted decomposition are infeasible.
- `GenerationIncomplete`: fixture generator did not reach requested list sizes.
- `UnsupportedNttLength`: transform length is unsupported.
- `ResourceLimit`: declared work or memory ceiling was reached.
- `Cancelled`: externally interrupted.
- `InvariantViolation`: arithmetic, structural, conservation, or immutability assertion failed.

A completed empty parent list says only that this local merge produced no valid parent. It does not prove the original subset-sum instance UNSAT.

## 20. Proposed files and responsibilities

The workspace currently has no source or build conventions. The provisional implementation target is C++20 with CMake.

```text
CMakeLists.txt

include/ter/
    types.hpp                  core IDs, statuses, policies, results
    shape.hpp                  FINITE_SANITY and PAPER_ROUNDED
    arithmetic.hpp             power-of-two modular helpers
    reference_vector.hpp       int8_t ternary representation
    packed_vector.hpp          positive/negative masks
    instance.hpp               density-1 instance and targets
    generator.hpp              all fixture generator modes
    residue_index.hpp          flat CSR entry index
    validator.hpp              audit/fast validator interface
    output.hpp                 canonical parent-list finalizer
    orientation.hpp            method-specific cost planners
    telemetry.hpp              stable counters and timings

include/ter/merge/
    cubic_oracle.hpp
    pair_baseline.hpp
    direct_bucket.hpp
    ntt.hpp
    ntt_hybrid.hpp
    recovery.hpp               shared CSR exact-recovery helpers

src/ter/
    shape.cpp
    arithmetic.cpp
    reference_vector.cpp
    packed_vector.cpp
    instance.cpp
    generator.cpp
    residue_index.cpp
    validator.cpp
    output.cpp
    orientation.cpp
    telemetry.cpp

src/ter/merge/
    cubic_oracle.cpp
    pair_baseline.cpp
    direct_bucket.cpp
    ntt.cpp
    ntt_hybrid.cpp
    recovery.cpp

tests/
    arithmetic_tests.cpp
    shape_tests.cpp
    representation_tests.cpp
    generator_tests.cpp
    residue_index_tests.cpp
    validator_tests.cpp
    orientation_tests.cpp
    ntt_tests.cpp
    differential_tests.cpp
    determinism_tests.cpp
    terminal_state_tests.cpp

bench/
    benchmark_cases.hpp
    benchmark_cases.cpp
    ter_merge_bench.cpp
```

Responsibilities must remain separated:

- Merge strategies do not implement their own TER structural rules.
- Validators do not choose orientations.
- NTT code does not recover witnesses.
- Generator code is outside measured merge timing.
- Output policies do not change the mathematical unique-parent result.

## 21. Implementation sequence

### Phase 1: correctness foundation

Implement:

- Core types and terminal states.
- `FINITE_SANITY`.
- Density-1 instance and nested-target arithmetic.
- Reference `int8_t` vectors.
- Minimal `TerEntry`.
- `EXHAUSTIVE_REFERENCE` and planted witness construction.
- Snapshot prevalidation.
- `AuditValidator`.
- Cubic oracle.

Gate: exhaustive small cases and planted witnesses pass independent audits.

### Phase 2: authentic practical baseline

Implement:

- Pair orientation planner.
- Flat pair CSR buckets.
- Pair-materialized baseline.
- Shared canonical output finalizer.
- Correctness telemetry.

Gate: pair baseline equals the cubic oracle, including provenance multiplicity.

### Phase 3: direct buckets

Implement:

- Flat entry `ResidueIndex`.
- Direct orientation planner.
- Direct CSR recovery.
- Residue-probe telemetry.

Gate: direct buckets equal the pair baseline on every completed correctness case.

### Phase 4: optimized common core

Implement:

- Packed ternary masks.
- Exact packed structural logic and `popcount` counting.
- Compact hot `TerEntry`.
- Debug sidecars.
- `FastValidator`.
- `CorrectnessMode` and `BenchmarkMode`.

Gate: packed/fast results equal reference/audit results for all strategies on bounded cases.

### Phase 5: NTT

Implement:

- Boolean support encoding.
- Exact cyclic NTT.
- NTT orientation planner.
- Active lookup-residue filtering.
- Shared CSR recovery and validator use.
- NTT-specific cost and probe telemetry.

Gate: NTT equals direct buckets, including candidate conservation and parent outputs.

### Phase 6: realistic fixtures

Implement:

- `PAPER_ROUNDED` and exact feasibility checks.
- `REJECTION_REFERENCE`.
- `MITM_PERFORMANCE`.
- `PLANTED_MITM`.

Gate: every fixture passes prevalidation; bounded fixtures agree across generator modes where comparison is feasible.

### Phase 7: fair benchmark harness

Implement:

- Deterministic interleaved algorithm order.
- Explicit timing boundaries.
- Paired cross-seed statistics.
- Memory accounting.
- CSV and JSON result output.
- Go/no-go summary generation.

Gate: dry-run benchmark fixtures reproduce identical outputs and stable counters before performance conclusions are accepted.

## 22. Prohibited shortcuts

- Do not start full TER before this experiment passes its go/no-go gate.
- Do not use random residue arrays in place of authentic entries.
- Do not call a modular NTT hit a witness.
- Do not prune on a nonzero coefficient; only an exact zero support result can reject a residue class.
- Do not omit exact recovery or structural validation.
- Do not compare NTT only with the pair-materialized baseline.
- Do not use nested vectors or hash tables for primary residue buckets.
- Do not leave redundant arithmetic/provenance fields in hot entries.
- Do not begin performance conclusions using only `FINITE_SANITY`.
- Do not use rejection sampling as the sole performance generator.
- Do not hash snapshots inside measured regions.
- Do not materialize all provenance triples in large benchmark runs.
- Do not compute three NTT orientations and charge only the selected one.
- Do not use floating-point FFT fallback.
- Do not add threads before the single-threaded comparison is understood.
- Do not convert timeout, cancellation, generation failure, or resource exhaustion into an empty valid output.
- Do not claim whole-TER or exponent speedup from one local merge.

## 23. Remaining uncertainties and escalation triggers

1. The workspace currently contains no repository, source conventions, or master TER specification. File layout and C++20/CMake remain provisional until real project context appears.
2. `PAPER_ROUNDED` is a declared finite approximation of published asymptotic distributions, not an exact reproduction of asymptotic list populations.
3. The lower MITM performance generator samples from finite generated half pools. Its sampling population and any truncation must be recorded.
4. The NTT planner's occupancy model may predict active support poorly. Predicted versus actual telemetry determines whether to revise only the planner, not the correctness mechanism.
5. Direct and NTT methods should recover exactly the same modular candidate triples. If NTT appears to reduce exact candidate count, treat that as a correctness bug rather than a speedup.
6. If packed validation dominates all methods, optimize the shared representation only after reference equivalence remains proven.
7. If output size dominates runtime, report that as the local stage's limiting cost; do not credit or blame residue filtering alone.

Any implementation discovery requiring a change to entry semantics, parent multiplicity semantics, modular nesting, or finite shape definitions must return to design review before coding around it.

## 24. Final experimental decision table

| Observed result | Conclusion | Next action |
|---|---|---|
| Any output mismatch | NTT/direct implementation unsound | Stop, diagnose, and rerun correctness gates |
| Buckets beat pair baseline; NTT loses to buckets | Basic modular indexing is the useful optimization | Preserve result; do not build full TER for NTT |
| NTT and buckets tie | No one-shot transform advantage established | Stop unless a batched/repeated-target study is separately approved |
| NTT wins only on `FINITE_SANITY` | Distribution-specific artifact | Do not proceed to full TER |
| NTT wins isolated transform time but loses total time | Recovery/preprocessing dominates | Record negative result |
| NTT consistently beats buckets on large `PAPER_ROUNDED` snapshots | Local research hypothesis survives | Design and implement full classical TER next |

The plan is complete when an implementation agent can build each phase without inventing TER semantics or benchmark policy. Full TER remains explicitly deferred.
