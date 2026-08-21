Based on the repo state we just established, I would run the project as a **gated research pipeline**, not as “keep optimizing random things until the code develops consciousness.” 💀

Your paper needs one clean causal story:

[
\boxed{
\text{TER root merge is impractical}
\rightarrow
\text{bounded-memory exact merge}
\rightarrow
\text{adaptive execution}
\rightarrow
\text{end-to-end TER improvement}
}
]

Everything below should serve that story.

# Full pipeline from now → research paper

## Phase 0: Freeze the research question

Before touching more code, write this somewhere in the repo:

> **Can the practical L1→L0 bottleneck of depth-3 TER be replaced by an exact bounded-memory cache-tiled merge whose execution parameters are selected adaptively, substantially reducing memory and runtime on realistic TER workloads?**

That becomes the project filter.

If some future idea does not help answer that question, it is secondary.

Your **primary contribution** should be:

[
\boxed{\text{Adaptive bounded-memory exact ternary merge for TER}}
]

Not NTT.

Not Schroeppel-Shamir.

Not ML.

Not every possible SSP optimization humanity has accumulated since Knuth first became suspicious of computers.

---

# Phase 1: Make the root observable

Right now your L2→L1 path has proper infrastructure, but the root is still buried inside Full TER.

### 1. Add a genuine L1→L0 capture seam

You want something conceptually like:

```cpp
capture_full_ter_l1_root_snapshot(...)
```

It should expose the **actual three L1 lists immediately before the root merge**, along with:

* (n)
* (r_1)
* root target (T)
* child target residues (t_A,t_B,t_C)
* list sizes (A,B,C)
* seed
* TER parameter family
* target mode
* memory budget
* expected root digit distribution

Do **not** reconstruct fake L1 lists afterward.

You want:

[
\boxed{\text{exact state consumed by the real root}}
]

### 2. Add root fingerprinting

Capture enough deterministic metadata to replay a workload exactly.

For every captured root:

```text
seed
n
r1
A_size
B_size
C_size
fingerprint_A
fingerprint_B
fingerprint_C
root_target
```

This becomes critical later when reviewers inevitably ask whether benchmark input changed between variants.

### Gate 1

Do not continue until:

* replay produces identical lists
* old `ternary_mitm` result is identical on replay
* planted root cases still recover the witness
* random-target root snapshots are reproducible

---

# Phase 2: Formalize the root quotient mathematics

For (n=72), your root condition is:

[
r_A+r_B+r_C\equiv T\pmod{2^{72}}
]

but every L1 entry already satisfies:

[
r_g\equiv t_g\pmod{2^{25}}.
]

Therefore define:

[
q_g=
\frac{(r_g-t_g)\bmod 2^{72}}
{2^{25}}
]

and

[
\tau=
\frac{(T-t_A-t_B-t_C)\bmod2^{72}}
{2^{25}}.
]

Then:

[
\boxed{
q_A+q_B+q_C\equiv\tau\pmod{2^{47}}
}
]

This needs to exist as both:

1. documentation/math;
2. independently tested code.

### 3. Add quotient derivation tests

For every captured entry verify:

[
r_g-t_g\equiv0\pmod{2^{r_1}}
]

and reconstruction:

[
r_g
\equiv
t_g+2^{r_1}q_g
\pmod{2^n}.
]

Then for every candidate reported by the quotient merge, independently verify the original 72-bit equation.

### Gate 2

You should be able to say:

> The quotient transformation is exact and loses no root solutions.

And have tests proving it.

---

# Phase 3: Widen the tiled engine

This is where your `uint64_t` change belongs.

### 4. Widen quotient storage

Conceptually:

```cpp
using QuotientResidue = std::uint64_t;
```

But audit all **quotient-bearing values**, not random 32-bit counters.

Keep things such as:

```cpp
EntryId
tile_bits
n
list indices
```

32-bit where appropriate.

### 5. Remove the artificial 32-bit quotient ceiling

Change the policy from:

[
q_{\max}=32
]

to the largest range safely representable by the algorithm.

For your immediate experiment you need only:

[
47.
]

### 6. Audit every shift and mask

This matters disproportionately.

Kill quotient-specific constructs like:

```cpp
1U << bits
```

and use safe 64-bit expressions.

Check:

* quotient mask construction
* (Q=2^w)
* bucket indexes
* pair sums
* `tau`
* doubled lookup keys
* `2Q`
* window boundaries

Because if your arithmetic temporarily evaluates (2Q), supporting “64-bit quotient” does **not** necessarily mean all 64 quotient bits are safe.

You need a documented safe domain.

### Gate 3

Run the existing tiled differential suite at:

```text
q = 1
q = 16
q = 31
q = 32
q = 33
q = 47
```

and appropriate upper boundaries.

Every case should exactly match a trusted baseline on manageable fixtures.

---

# Phase 4: Implement the bounded-memory root merge

Now take the actual root off `ternary_mitm`.

### 7. Create a root-specific exact tiled path

Something conceptually like:

```cpp
ternary_tiled_l1_to_l0(...)
```

Do not prematurely merge L2 and root code into one hideous universal abstraction.

They differ enough that a clean specialization is safer.

Pipeline:

```text
L1 A/B/C
  ↓
derive 47-bit quotients
  ↓
choose orientation
  ↓
tiled quotient scan
  ↓
low/full quotient match
  ↓
structural ternary validation
  ↓
root distribution validation
  ↓
independent full 72-bit verification
  ↓
emit root parent/witness
```

### 8. Eliminate root pair materialization

The invariant should be:

[
\boxed{\texttt{pair_records = 0}}
]

for the new path.

The old path may need:

[
AB
]

records.

The new path should have memory roughly:

[
M_{\text{root}}
===============

M_{\text{tile}}
+
M_{\text{quotients}}
+
M_{\text{bounded output}}.
]

### 9. Preserve old root as correctness reference

Do not delete `ternary_mitm`.

You need it for small differential tests.

On small roots:

[
\text{Old MITM output}
======================

\text{New tiled output}.
]

Compare:

* parent set
* multiplicities
* provenance
* witness
* root fingerprint

### Gate 4

At this point you want:

[
\boxed{\text{real L1→L0 root tiling works correctly}}
]

even if its parameter choice is terrible.

This is the first major paper-worthy checkpoint.

---

# Phase 5: Fix survivor/output memory

Pair materialization is only one half of the memory problem.

### 10. Stop reserving giant survivor buffers blindly

Your tiled visitor currently accumulates accepted candidates before finalization.

At root, that can still become ugly.

Move toward:

```text
candidate located
    ↓
validate immediately
    ↓
full root check immediately
    ↓
if all you need is first witness:
    terminate successfully
```

For SAT search, this is huge.

### 11. Separate output policies

You probably want at least:

```cpp
enum class RootOutputPolicy {
    FirstWitness,
    CountOnly,
    FullCanonicalOutput
};
```

For actual solver execution:

[
\boxed{\texttt{FirstWitness}}
]

should avoid materializing every valid parent when one witness is enough.

For correctness experiments:

[
\boxed{\texttt{FullCanonicalOutput}}
]

can remain available.

This prevents your benchmark code from accidentally measuring a research-artifact-only canonicalization requirement as if it were intrinsic TER cost.

### Gate 5

Show:

[
M_{\text{working}}
\approx
M_{\text{fixed tile}}
]

rather than growing with candidate count in the ordinary first-witness path.

---

# Phase 6: Instrument the hell out of it

Now you need data to explain *why* it behaves as it does.

For every root execution record:

### Workload features

```text
n
r1
quotient_bits
A
B
C
pair orientation
A/B
A/C
B/C
```

### Tiling parameters

```text
w
SB
Q = 2^w
S = 2^SB
bucket_count
window_count
```

### Occupancy

For A/B/C:

```text
nonempty buckets
mean occupancy
maximum occupancy
variance
entropy
target-aligned mass
```

### Work

```text
logical pairs
executed pair probes
nonempty windows
arithmetic candidates
structural checks
structural rejects
distribution rejects
full-root checks
valid roots
```

### Time

```text
quotient construction ns
setup ns
bucketization ns
scan ns
validation ns
final verification ns
total root ns
```

### Memory

```text
quotient bytes
bucket/index bytes
tile bytes
candidate/output peak
total working peak
old MITM estimated pair bytes
```

Do not merely record `total_ns`. That's how you end up with graphs that confidently tell you absolutely nothing.

---

# Phase 7: Characterize `(w, SB)`

Now comes the researchy part.

You already discovered:

[
w=30,\ SB=10
\rightarrow13.87\text{ s}
]

while:

[
w=30,\ SB=20
\rightarrow33\text{ ms}.
]

That means `SB` is structurally important.

### 12. Run controlled sweeps

For each root snapshot:

[
w\in W
]

and for each (w):

[
SB\in S(w).
]

Do not use nonsensical combinations that make billions of microscopic windows.

Use a reasonable grid such as:

[
w\in{12,14,16,\dots,32}
]

with broader coverage around the root optimum.

For each (w), sweep feasible:

[
SB\le w.
]

For the genuine 47-bit root, remember:

* (w) is your prefilter width,
* 47 bits is the **full quotient verification width**.

### 13. Use multiple workload families

At minimum:

**A. Genuine Full TER root captures**

This is the primary evidence.

**B. Uniform synthetic**

For validating analytical expectations.

**C. Generic skew**

For robustness.

**D. Target-resonant skew**

This is important.

Your previous generic hot buckets could actually make the merge easier.

Construct cases where hot buckets satisfy something like:

[
h_A+h_B+h_C
\equiv
\tau
\pmod{2^w}.
]

That tests the adversarial case that actually increases candidate density.

**E. Imbalanced geometry**

Keep:

[
ABC
]

fixed while varying:

[
(A,B,C).
]

You already know equal (ABC) does not imply equal runtime.

### Gate 6

You should now have heatmaps:

[
T(w,SB)
]

for many workloads.

And a clear empirical relationship between optimal tile width and occupancy/list geometry.

---

# Phase 8: Build an analytical cost model

Do this **before ML**.

Start from:

[
T(w,SB)
=======

T_{\text{setup}}
+
T_{\text{windows}}
+
T_{\text{scan}}
+
S_wT_{\text{verify}}.
]

For uniform quotient residues:

[
E[S_w]
======

\frac{ABC}{2^w}.
]

For nonuniform distributions:

[
P_\tau
======

\sum_{i+j+k\equiv\tau}
p_A(i)p_B(j)p_C(k)
]

and therefore:

[
E[S]
====

ABC,P_\tau.
]

### 14. Model bucket occupancy

Define something like:

[
\lambda_A=\frac{A}{2^{w-SB}}
]

and similarly for B/C, depending on exact bucket construction.

Your sweep should reveal an efficient occupancy region.

You may find an approximate rule such as:

[
SB^*
\approx
w-\log_2(A/O^*)
]

or a geometry-aware variant.

Do not force a pretty equation if the data does not support one.

### 15. Build planner V1

The planner should evaluate a small candidate set:

[
\mathcal C
==========

{(w,SB)_1,\dots,(w,SB)_k}
]

and predict:

[
\hat T(w,SB).
]

Then choose:

[
(w^*,SB^*)
==========

\arg\min_{(w,SB)\in\mathcal C}
\hat T(w,SB)
]

subject to:

[
M(w,SB)\le M_{\max}.
]

### Gate 7

Compare planner against:

1. fixed `SB=15`;
2. hand-selected setting;
3. exhaustive oracle-best `(w,SB)`.

Report regret:

[
\text{regret}
=============

\frac{T_{\text{planner}}}{T_{\text{oracle best}}}.
]

A planner that regularly gets within, say, 5 to 20% of exhaustive best would already be quite useful.

---

# Phase 9: Tiny online calibration

Hardware matters.

Your model should not pretend a Ryzen, Intel laptop, server Xeon, and M-series Mac are mystical copies of the same machine.

### 16. Add tiny startup calibration

Measure a few cheap things such as:

* pair probe ns
* bucket scan ns
* candidate validation ns
* memory bandwidth proxy

Then plug calibrated constants into:

[
\hat T.
]

This gives you:

[
\boxed{\text{analytical structure + empirical machine constants}}
]

which is far easier to defend than “the neural network vibes said SB=18.”

---

# Phase 10: Only consider ML if the model leaves meaningful error

If analytical planner regret remains substantial and systematic, then add LightGBM.

But ML becomes:

[
\textbf{residual cost predictor}
]

not:

[
\textbf{correctness oracle}.
]

Features:

```text
A
B
C
w
SB
quotient width
orientation
occupancy
entropy
max bucket
target-resonant mass
estimated survivors
memory budget
calibration constants
```

Predict:

[
T(w,SB)
]

or:

[
\log T(w,SB).
]

Split by **instance/seed**, not individual `(w,SB)` rows.

Otherwise you leak the same workload into train and test and congratulate yourself for inventing memorization.

### Decision gate

Keep ML only if it materially improves:

* planner regret,
* cross-size generalization,
* cross-distribution generalization,
* perhaps cross-machine generalization.

If not:

[
\boxed{\text{remove ML from the main paper}}
]

No emotional attachment to failed components. Academia already has enough of that.

---

# Phase 11: End-to-end Full TER integration

Once root planner works:

### 17. Replace root dispatch

Full TER becomes:

```text
L3 generation
 ↓
L3→L2 exact MITM
 ↓
L2→L1 tiled merge
 ↓
L1→L0 adaptive tiled root
 ↓
independent witness verification
```

Old root MITM remains selectable for controlled baselines.

### 18. Add execution modes

Something like:

```text
LEGACY_ROOT_MITM
FIXED_TILED_ROOT
ADAPTIVE_TILED_ROOT
```

This is essential for experiments.

---

# Phase 12: Re-profile the entire solver

Now finally ask:

> Where did the bottleneck move?

Record:

[
T_{\text{L3→L2}},
T_{\text{L2→L1}},
T_{\text{L1→L0}},
T_{\text{other}}.
]

Before:

[
T_{\text{root}}
]

may dominate.

After:

perhaps:

[
T_{\text{L2→L1}}
]

becomes dominant.

That is **not failure**.

That is literally what successful optimization looks like.

### Gate 8

Compute end-to-end speedup:

[
S_{\text{TER}}
==============

\frac{T_{\text{legacy TER}}}
{T_{\text{adaptive TER}}}.
]

Also peak-memory reduction:

[
R_M
===

\frac{M_{\text{legacy peak}}}
{M_{\text{adaptive peak}}}.
]

---

# Phase 13: Decide whether Schroeppel-Shamir belongs

Only after the main result exists.

Your SSP SS backend can become a **secondary portfolio backend** for ordinary MITM-style exact search where appropriate.

But keep this story separate:

```text
Primary:
adaptive bounded-memory TER ternary merge

Secondary:
memory-aware MITM/SS fallback/backend portfolio
```

If SS adds little:

leave it out.

If it gives a meaningful additional end-to-end improvement:

include it as an extension/ablation.

Do not let it hijack the paper.

---

# Phase 14: Build the definitive experimental matrix

For publication, I would require four categories.

## A. Correctness

Across many small exact instances:

[
\text{legacy}
=============

# \text{tiled}

# \text{adaptive tiled}

\text{oracle}
]

for:

* outputs
* multiplicities
* provenance
* witnesses

Zero mismatches.

---

## B. Memory

Show old pair memory:

[
M_{\text{MITM}}\approx ABR
]

against:

[
M_{\text{tiled}}\approx
M_{\text{quotient arrays}}
+
M_{\text{bucket structures}}
+
M_{\text{bounded output}}.
]

Use real n=72 root figures.

That 76-ish GiB → MiB-scale story is extremely visually strong.

---

## C. Performance

Compare:

```text
Legacy root MITM
Fixed tiled
Best exhaustive tiled
Adaptive analytical planner
Adaptive analytical + calibration
ML planner, only if retained
```

Report:

* median
* maybe geometric mean
* variance
* seeds
* compiler
* CPU
* memory budget
* warm-up policy

No cherry-picked “one run on seed 0xDEADBEEF that happened to look heroic.”

---

## D. Generalization

Test across:

* list sizes
* balanced/imbalanced lists
* quotient widths
* genuine TER workloads
* uniform synthetic
* target-compatible skew
* different memory limits

This is what turns:

> “I tuned tile size on my laptop”

into:

> “I characterized the operating regimes of this algorithm.”

---

# Phase 15: Ablation study

Your paper should answer exactly what each component buys.

Table:

| Variant       | Tiling | Adaptive `w` | Adaptive `SB` | Streaming | Calibration |
| ------------- | ------ | ------------ | ------------- | --------- | ----------- |
| Legacy        | ✗      | ✗            | ✗             | ✗         | ✗           |
| Fixed tiled   | ✓      | ✗            | ✗             | maybe     | ✗           |
| Adaptive SB   | ✓      | ✗            | ✓             | ✓         | ✗           |
| Adaptive w+SB | ✓      | ✓            | ✓             | ✓         | ✗           |
| Final         | ✓      | ✓            | ✓             | ✓         | ✓           |

This may reveal that `w` adaptation contributes little while `SB` matters enormously.

If so, simplify the final algorithm.

Complexity is not automatically sophistication.

---

# Phase 16: Produce the key figures

Before writing prose, make these figures.

### Figure 1

TER tree:

[
18\rightarrow9\rightarrow3\rightarrow1
]

highlight L1→L0.

### Figure 2

Legacy vs tiled architecture.

### Figure 3

Memory scaling:

[
AB
]

materialization versus bounded tiled memory.

### Figure 4

Heatmap:

[
T(w,SB).
]

This could be one of the most important figures.

### Figure 5

Planner predicted optimum versus actual optimum.

### Figure 6

Planner regret distribution.

### Figure 7

End-to-end stage runtime before/after.

### Figure 8

Peak memory before/after.

### Figure 9

Scaling with root list sizes.

You may not need all nine in the paper, but generate them before deciding.

---

# Phase 17: Write the paper around claims, not chronology

Do **not** write:

> First I tried NTT. Then that sucked. Then I tiled. Then I changed SB. Then one Thursday I became angry at uint32_t.

That is your project diary, not a research paper. 😭

The paper should look like:

## 1. Introduction

Problem:

> TER's upper ternary merges provide strong asymptotic performance but can impose severe practical pair-materialization and execution costs.

Contribution:

> An exact cache-tiled bounded-memory merge and adaptive execution policy.

Results:

> X× memory reduction, Y× root speedup, Z× end-to-end TER improvement.

---

## 2. Background

Explain:

* Subset Sum
* TER
* depth-3 tree
* (r_2,r_1)
* ternary merge
* theoretical complexities

Keep this short.

---

## 3. Practical bottleneck

Show exactly why:

[
A\times B
]

materialization is ugly.

Use genuine root measurements.

This section motivates everything.

---

## 4. Quotient-space formulation

Derive:

[
q_A+q_B+q_C\equiv\tau\pmod{2^{n-r_1}}.
]

Prove exactness.

---

## 5. Cache-tiled exact merge

Present:

* bucket partitioning
* windows
* lookup transformation
* exact candidate reporting
* memory bound
* correctness

This is your main technical algorithm section.

---

## 6. Adaptive execution

Present:

[
(w^*,SB^*)
==========

\arg\min T(w,SB).
]

Explain:

* occupancy
* candidate count
* list geometry
* cost model
* calibration

This is probably your second major contribution.

---

## 7. Implementation

Explain:

* C++20
* packed ternary representation
* exact verification
* memory accounting
* instrumentation
* fallback paths

Not 20 pages of code trivia.

---

## 8. Experimental methodology

Document:

* hardware
* compiler
* build flags
* seeds
* warmups
* repetitions
* workload families
* exact baselines
* memory caps

This section needs to be painfully boring. That's how reproducibility works.

---

## 9. Results

Subsections:

### 9.1 Correctness

Zero mismatches.

### 9.2 Memory

Legacy vs tiled.

### 9.3 `(w,SB)` regimes

Heatmaps.

### 9.4 Planner quality

Regret.

### 9.5 Root runtime

Speedups.

### 9.6 End-to-end TER

Actual solver improvement.

### 9.7 Stress/generalization

Target-skew, geometry, etc.

---

## 10. Limitations

Be explicit:

* heuristic/random-instance TER setting
* hardware dependence
* current modulus assumptions
* finite practical range
* possibly single-threaded
* no claim of improving TER's asymptotic exponent unless you actually do
* adaptive merge changes practical constants/runtime and memory, not automatically theoretical (2^{0.2400n})

This is important.

Do **not** accidentally write:

> We improve TER from (2^{0.2400n}).

unless you genuinely prove a new asymptotic algorithm.

You are improving the **practical implementation of the bottleneck**.

---

## 11. Related work

Cover:

* Li TER
* representation techniques
* MITM
* Schroeppel-Shamir
* memory-efficient subset-sum merging
* cache-conscious/bucketed join/merge techniques where relevant

This will need a real literature search later.

---

## 12. Conclusion

Something like:

> The work shows that the theoretically attractive TER merge structure need not imply massive practical pair materialization, and that the resulting bounded-memory merge requires workload-sensitive execution parameters to achieve good performance.

That is a coherent paper.

---

# Phase 18: Reproducibility package

Before submission, freeze:

```text
commit hash
compiler version
CMake flags
hardware
benchmark commands
raw CSV
plot scripts
seed list
config files
```

Create something like:

```text
experiments/
    paper/
        configs/
        raw/
        processed/
        plots/
        run_all.*
```

And a single command, ideally:

```text
run_paper_experiments
```

or at least a documented sequence.

A reviewer should not need archaeological training to reproduce Table 3.

---

# Phase 19: Internal paper audit

Before submitting, audit every major claim.

Create a claim ledger:

| Claim                      | Evidence                    |
| -------------------------- | --------------------------- |
| Exact                      | differential tests          |
| bounded memory             | measured + analytical bound |
| no pair materialization    | telemetry/code invariant    |
| adaptive better than fixed | sweep                       |
| planner near optimum       | regret                      |
| improves TER overall       | full solver benchmark       |
| robust                     | multi-seed/stress results   |

If a claim has no row of evidence:

**delete it or generate the evidence.**

---

# Phase 20: Venue-level manuscript

If this remains primarily **algorithm engineering / experimental algorithms**, the clean first target we had identified is **ACM Journal of Experimental Algorithmics**.

For that style of venue, emphasize:

* experimentally validated algorithm engineering,
* reproducibility,
* workload characterization,
* exactness,
* memory/runtime tradeoffs,
* honest negative results like NTT losing where relevant.

But your paper should be essentially finished **before** bending formatting around any one venue.

---

# Your actual milestone ladder

I would number them like this:

```text
M0  Full TER implementation                       ✅
M1  Exact tiled L2→L1 merge                       ✅
M2  Exact bounded-memory L1→L0 root merge         ← CURRENT CORE
M3  Adaptive (w, SB) root planner
M4  Streaming / bounded survivor execution
M5  End-to-end adaptive TER
M6  Generalization + adversarial characterization
M7  Final experimental campaign
M8  Paper figures + claim ledger
M9  Manuscript
M10 Reproducibility freeze + submission
```

And the most important thing is **not** to mentally jump to M7 while M2 is still unfinished.

---

# Immediate next actions

Right now, in order:

1. **Add genuine L1→L0 capture/replay.**
2. **Widen quotient arithmetic to safe 64-bit support.**
3. **Implement exact 47-bit root tiled path.**
4. **Differential-test it against old root MITM.**
5. **Eliminate pair materialization.**
6. **Implement first-witness/bounded-output execution.**
7. **Instrument occupancy/work/time/memory.**
8. **Run `(w,SB)` sweeps on genuine roots.**
9. **Derive the analytical planner.**
10. **Integrate planner into Full TER.**
11. **Re-profile the complete solver.**
12. **Only then decide whether SS or ML adds enough to belong.**

That is the shortest path from your current repo to a **coherent publishable research result**, rather than a repo containing nineteen individually impressive optimizations all fighting over who gets to be the thesis. 🔥
