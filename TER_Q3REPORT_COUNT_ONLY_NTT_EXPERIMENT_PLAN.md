# TER-Q3REPORT Count-Only NTT Experiment Plan

## Status and decision

**Status:** implemented and benchmarked. This document remains the frozen
scope contract for the experiment. The first native `n=36` measurements fail
the cheap kill gate in every supplied family, so witness recovery is **not**
authorized by this result.

**Decision to test:** whether exact cyclic NTT pair counts remove enough real L2-to-L1 TER arithmetic work to justify a later witness-recovery prototype.

This experiment is not an NTT TER solver. It does not claim an improved TER exponent.

## Scope lock

Implement only:

```text
frozen authentic L2-to-L1 snapshots
snapshot audit
shared quotient arithmetic
independent cubic oracle
direct exact baseline
scalar cyclic convolution
exact cyclic NTT counts
telemetry and benchmark runner
GO/NO-GO decision gates
```

Explicitly out of scope:

```text
witness recovery
moment/Prony recovery
sparse convolution
WPRJ/refinement trees
full NTT merge dispatcher
TER parameter re-optimization
new asymptotic claims
```

## Proposed module boundaries

```text
include/ter/q3report_audit.hpp
    Public snapshot, report, configuration, and result types.

src/q3report_audit.cpp
    Snapshot audit, direct baseline, scalar convolution, NTT count path,
    telemetry, and decision gates.

tests/q3report_audit_tests.cpp
    Arithmetic, oracle, NTT, safety, and differential tests.

bench/ter_q3report_bench.cpp
    Reproducible benchmark and report output.
```

Update `CMakeLists.txt` only to add these test and benchmark targets.

The existing FullTer L2 entries and targets are private implementation state. To
produce genuine `AUTHENTIC_FULLTER` inputs without duplicating FullTer's stage
construction, the implementation may make the smallest necessary addition to
`include/ter/full_ter.hpp` and `src/full_ter.cpp`: a deterministic, read-only
capture function that exports one frozen native-width L2-to-L1 merge as an
`InputSnapshot`. It must reuse FullTer's actual target generation and L3-to-L2
merge logic, return an explicit non-success status when a native-width capture
is impossible, and must not change solver execution or solver decisions.

## System boundary

The input is one real level-2 to level-1 merge:

```text
A, B, C                 actual level-2 TER child lists
sA, sB, sC              child targets modulo 2^r2
sP                      parent target modulo 2^r1
r2 < r1
Q = 2^(r1-r2)
required parent digit distribution
instance coefficients and deterministic provenance metadata
```

The experiment reports arithmetic selectivity and exact TER output statistics. It must not change solver semantics.

## Frozen snapshot

`FrozenL2MergeSnapshot` stores:

```text
A, B, C entries
instance coefficients
sA, sB, sC, sP
r2, r1, Q
required L1 distribution
seed, parameter family, execution mode, fingerprint
```

Snapshot families are labelled exactly:

```text
AUTHENTIC_FULLTER
    Complete genuine TER lists, whenever feasible.

DISTRIBUTION_MATCHED
    Controlled TER-like scaling lists. Never labelled authentic FullTer.

FIXED_Q_MICROBENCH
    NTT kernel measurements only. Never used for TER scaling claims.
```

## Independent snapshot audit

Before a direct merge, an oracle, or an NTT allocation, independently verify:

```text
- every packed vector decodes to {-1,0,+1}^n;
- every entry has the exact required L2 digit distribution;
- EntryIds are valid and unique within the source list;
- recomputed <instance coefficients, entry vector> equals the stored full residue;
- each recomputed residue meets its child target modulo 2^r2;
- sA+sB+sC = sP modulo 2^r2;
- r2 < r1 and Q is a representable power of two;
- the requested parent distribution is valid.
```

Metadata is never trusted as a substitute for recomputation.

## Failure semantics

The experiment exposes its own result status rather than overloading solver
success/failure semantics:

```text
Completed
NotApplicable
InvalidSnapshot
InvalidConfiguration
ResourceLimit
ArithmeticSafetyFailure
InternalInvariantFailure
```

`NotApplicable`, unsupported NTT length, unsafe coefficient reconstruction,
and resource failure never mean "zero candidates". An empty parent list never
means subset-sum UNSAT. A report is complete only when every requested phase
completed; otherwise any partial telemetry is explicitly marked as partial.

## Shared quotient arithmetic

The direct baseline and NTT share the following canonical arithmetic.

For an entry `x` from child list `X`:

\[
q_X(x)=\left(\frac{((\langle a,x\rangle\bmod 2^{r_1})-s_X)}{2^{r_2}}\right)\bmod Q.
\]

For the parent target:

\[
\tau=\left(\frac{s_P-s_A-s_B-s_C}{2^{r_2}}\right)\bmod Q.
\]

Rules:

```text
- Canonicalize child targets into [0, 2^r2) and parent target into [0, 2^r1).
- Compute the tau numerator in signed, widened arithmetic.
- Require exact divisibility by 2^r2 before division.
- Canonicalize tau into [0,Q).
- Never substitute x/2^r2 or x mod Q for the offset-aware quotient residue.
```

The exact upper-level arithmetic condition is:

\[
q_A(a)+q_B(b)+q_C(c)=\tau\pmod Q.
\]

## Independent cubic oracle

The cubic oracle is only for tiny snapshots and must not call `quotient_residue()` or `derive_tau()`.

```text
for every a in A:
  for every b in B:
    for every c in C:
      recompute all inner products from vectors and instance coefficients;
      test residue(a)+residue(b)+residue(c) = sP mod 2^r1;
      test ternary structural validity and exact parent distribution;
      record raw child-ID provenance and deduplicated parents.
```

This protects against a shared arithmetic error making every comparator agree on the same wrong answer.

## Direct exact baseline

The direct comparator:

```text
enumerates A x B arithmetic candidates;
looks up the required C quotient bucket;
applies exact structural validation;
constructs and deduplicates parents.
```

It records:

```text
A x B pairs examined
C-bucket probes
raw arithmetic triples
structurally valid raw triples
distinct parent vectors
provenance multiplicity
direct arithmetic time
structural-validation time
deduplication time
peak memory
```

For tiny snapshots, direct output, raw provenance, parent multiplicity, and fingerprint must exactly equal the independent cubic oracle.

## Exact count-only cyclic NTT

Build frequencies:

```text
freqA[q] = number of A entries with quotient residue q
freqB[q] = number of B entries with quotient residue q
```

Compute the cyclic convolution:

\[
\operatorname{pairCount}[t]=\sum_q \operatorname{freqA}[q]\operatorname{freqB}[(t-q)\bmod Q].
\]

For each `c`:

```text
requiredTarget(c) = tau - qC(c) mod Q
```

Interpretation:

```text
pairCount[requiredTarget(c)] = 0
    Exact arithmetic impossibility for c.

pairCount[requiredTarget(c)] > 0
    At least one arithmetic A+B pair exists. No witness is recovered.
```

Use an integer NTT only for exact decisions. A floating-point FFT is benchmark-only unless certified rounding proves it cannot cause an unsafe zero result.

## NTT applicability gate

Before allocating transform buffers, return `NotApplicable` if any condition fails:

```text
- Q is unsupported by transform length or available NTT prime(s);
- checked index arithmetic cannot represent all array sizes;
- input, output, twiddle, bit-reversal, CRT, and scratch buffers exceed memory budget;
- coefficient reconstruction cannot be proven exact;
- Q or vector indexes exceed implementation limits.
```

`NotApplicable` leaves the direct baseline authoritative. It never authorizes pruning.

## Exact coefficient reconstruction

For every cyclic coefficient:

\[
0\le\operatorname{pairCount}[t]\le |A||B|.
\]

One NTT prime `p` is allowed only when:

\[
|A||B| < p.
\]

Otherwise CRT is allowed only when:

\[
\prod_i p_i > |A||B|.
\]

All products and allocation computations require overflow checks. If the exactness proof fails, no NTT zero may prune any candidate.

## Telemetry

Report selected targets in both forms:

```text
Unique-target view
    distinct residues in {tau-qC(c)}.

C-entry-weighted view
    one observation per actual c entry.
```

Required measurements:

```text
|A|, |B|, |C|, Q
|A| x |B|
lambda = |A| x |B| / Q

all-residue support fraction
predicted support fraction = 1-exp(-lambda)
all-residue count histogram

selected-target zero rate
selected-target count 1/2/4 thresholds
selected-target p50/p90/p99/max
total raw arithmetic triples

direct arithmetic time
NTT frequency-build time
NTT core-transform time
CRT reconstruction time
selected-target lookup time

snapshot-audit time
full-histogram/report time
scalar-comparison time
structural-validation time
deduplication time

direct peak memory
NTT persistent memory
NTT scratch memory
```

NTT core timing and audit/report timing remain separate. A full scan over Q coefficients for experiment histograms is not charged as mandatory production work.

The report also records the structural collapse already exposed by the direct
baseline:

```text
raw arithmetic triples
    -> structurally valid raw triples
    -> distinct parent vectors
```

It is telemetry only and must not trigger witness-recovery implementation.

## Required tests

```text
1. Exhaust tiny r1/r2/target cases:
   quotient equation equals original modulo-2^r1 equation.

2. Signed tau:
   negative intermediate numerator, target wraps, and boundary targets.

3. Snapshot audit:
   corrupt stored residue, target, digit count, EntryId, and packed vector.
   Every case fails before merge or NTT.

4. Scalar cyclic convolution:
   every coefficient equals explicit pair enumeration.

5. NTT:
   every coefficient equals scalar cyclic convolution.

6. Wraparound:
   sums near 0 and Q-1.

7. Duplicate quotient residues:
   multiplicities count exactly.

8. CRT/non-wrap:
   unsafe one-prime cases are rejected.

9. Differential semantics:
   cubic oracle equals direct baseline on tiny snapshots.

10. No false prune:
    NTT zero exactly equals scalar pair count zero.

11. Conservation:
    sum over C entries of pairCount[tC] equals direct raw arithmetic triple count.

12. Determinism:
    a fixed snapshot, configuration, and build mode reproduce logical counts,
    fingerprints, and classification; timing is allowed to vary.
```

For practical tiny domains, deterministic property tests additionally vary
target normalization, cyclic wrap, duplicate quotient residues, snapshot
corruption, and NTT reconstruction.

## NTT implementation constraints

```text
- Use integer NTT only for exact decisions.
- The transform must be cyclic in Z_Q; any linear internal transform must fold
  the two halves before interpretation.
- Every prime/root pair is a fixed, documented mathematical choice whose
  two-adicity supports Q. Do not select constants from memory or dynamically
  from unverified data.
- CRT, if used, is solely for exact pair-count reconstruction.
- No floating-point FFT participates in pruning.
```

## Benchmark protocol

Use deterministic frozen snapshots, at least one warm-up, multiple measured
repetitions, a median, and dispersion. Every record carries snapshot family,
fingerprint, seed, dimensions, TER parameter family, execution mode, list
sizes, r1/r2/Q, lambda, compiler/version/flags/build mode, CPU, thread count,
source revision, memory budget, and NTT prime/CRT strategy. Compare only runs
with identical snapshot, semantics, thread count, and warm-up policy.

## Cheap kill gate

Compare fresh runs on the same snapshot, with the same repetitions.

```text
T_direct-arithmetic
    A x B pair enumeration and residue lookup only;
    excludes structural validation and deduplication.

T_NTT-count-only
    frequency build, transforms, exact reconstruction,
    and required-target lookup only.
```

If:

\[
T_{\mathrm{NTT\ count-only}}\ge T_{\mathrm{direct\ arithmetic}},
\]

reject NTT for that regime immediately. Witness recovery has non-negative cost, so it cannot rescue a count-only path that already loses.

## GO / NO-GO

Proceed only to a small, separately authorized witness-recovery prototype if all are true:

```text
- NTT counts are exact;
- count-only NTT passes the cheap kill gate;
- a meaningful C-entry-weighted fraction has zero count, or surviving counts are mostly low;
- the result persists across authentic and distribution-matched scaling snapshots.
```

Reject support-only NTT as the main route if:

```text
- selected-target zero rate is near zero;
- selected-target counts are broadly high;
- count-only NTT loses to direct arithmetic;
- pair support follows saturation prediction as snapshots grow.
```

## Implementation order after explicit authorization

```text
1. Frozen snapshot and snapshot audit
2. Shared quotient arithmetic
3. Independent cubic oracle
4. Direct exact baseline and telemetry
5. Scalar cyclic convolution
6. Exact count-only cyclic NTT
7. Differential tests
8. Benchmark/report runner
9. Run benchmark families and apply GO/NO-GO
```

No witness-recovery code is authorized by this plan.

## Implemented realization

The frozen implementation follows this document as a deliberately narrow
experiment:

```text
include/ter/q3report_audit.hpp
    Q3-only status model, frozen snapshot, explicit work/memory ceilings,
    exactness and structural-collapse telemetry.

src/q3report_audit.cpp
    independent snapshot audit; signed tau arithmetic; direct target-aware
    baseline; tiny original-modulus cubic oracle; scalar cyclic reference;
    exact one-prime cyclic NTT at 998244353; no recovery.

include/ter/full_ter.hpp + src/full_ter.cpp
    one read-only capture seam for a complete native-width Full TER L2 group.
    It reuses the actual L3-to-L2 construction and target generation. It is
    unavailable rather than silently truncated outside the native Q3 range.

tests/q3report_audit_tests.cpp
    deterministic differential tests, genuine Full TER capture/replay,
    signed negative tau, conservation, malformed snapshots, and resource caps.

bench/ter_q3report_bench.cpp
    single-thread CSV runner with one warm-up, repeated medians, transform
    dispersion, separate audit/report timings, and all three snapshot labels.
```

The transform is an exact in-place cyclic NTT using

```text
p = 998244353 = 119 * 2^23 + 1
primitive root = 3
supported Q <= 2^23
```

The implementation uses one prime only if `|A|*|B| < p`. It does **not** add
CRT as a dormant placeholder: an unsafe coefficient bound returns
`ArithmeticSafetyFailure`, so no wrapped NTT coefficient can be interpreted
as an exact zero.

Run the native benchmark from the repository root:

```powershell
cmake --build build-native --config Release --parallel
ctest --test-dir build-native -C Release --output-on-failure
.\build-native\Release\ter_q3report_bench.exe 7
```

`AUTHENTIC_FULLTER` is a complete native `n=36` guided Full TER capture.
`DISTRIBUTION_MATCHED` is generated T0 input and is never used as Full TER
evidence. `FIXED_Q_MICROBENCH` projects the genuine vectors into a smaller
quotient ring and is a transform-size measurement only.
