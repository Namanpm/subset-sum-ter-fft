# TER + Schroeppel–Shamir + Modular Convolution
## Standalone Master Research Specification for Random Density-1 Subset Sum

**Document status:** Master standalone specification  
**Supersedes:** `ter_fft_hybrid_research_idea.md` and `ter_ss_ntt_followup_research_notes.md`  
**Scope:** TER, Schroeppel–Shamir, FFT/NTT/sparse convolution, modular filtering, witness recovery, practical optimization, experiment design, and paper framing  
**Current project stance:** We are primarily investigating **practical acceleration**, not assuming a new asymptotic exponent.

---

# 0. How to read this document

This file deliberately separates four kinds of statements:

- **Established:** supported by published literature.
- **Observed:** directly observed in the uploaded Schroeppel–Shamir / SSP source.
- **Proposed:** our research idea or experiment.
- **Parked:** intentionally postponed because it is not needed for the next experiment.

That distinction matters. A research document becomes useless very quickly if published facts, code observations, and shower-thought hypotheses are blended into one confident soup.

---

# 1. Executive summary

The project began with a simple question:

> Can FFT-like convolution help the new ternary representation (TER) algorithm for random density-1 Subset Sum?

After refining the idea, the project now has **two immediate optimization targets** and **one postponed integration target**:

```text
                         SUBSET SUM PROJECT
                                |
              +-----------------+-----------------+
              |                                   |
              v                                   v
     Existing optimized SS                  TER algorithm
              |                                   |
              v                        +----------+----------+
      NTT/FFT experiment               |                     |
                                       v                     v
                                  bottom MITM          ternary merges
                                       |                     |
                                       v                     v
                               optimized SS later      NTT/FFT experiment
```

The central proposal is **not** to replace Subset Sum with FFT.

Instead:

1. Keep the exact algorithmic structure of SS or TER.
2. Compress partial sums into a smaller modular universe.
3. Use **NTT/FFT/sparse convolution** to learn which modular combinations can exist.
4. Skip exact work in regions that are provably impossible under that filter.
5. Recover actual candidates only from surviving regions.
6. Verify them using the original exact algorithm.

For TER, the main target is the **higher ternary merge-and-filter stage**.

For Schroeppel–Shamir, the main target is the **ordered pair-stream search**, using the existing highly optimized implementation as a proving ground.

The project is currently framed as:

\[
\boxed{
\textbf{Can modular convolution reduce practical exact-search work in representation and MITM-style Subset Sum algorithms?}
}
\]

A lower TER exponent would be spectacular, but it is **not** the assumed outcome.

---

# 2. Subset Sum background

Given positive integers

\[
a_1,a_2,\ldots,a_n
\]

and a target \(T\), Subset Sum asks whether there exists a subset of the input values whose sum equals \(T\).

Equivalently, find:

\[
e\in\{0,1\}^n
\]

such that

\[
a\cdot e=T.
\]

The search space contains:

\[
2^n
\]

possible subsets.

That exponential search space is why algorithms such as meet-in-the-middle, Schroeppel–Shamir, and representation techniques exist.

---

# 3. Density

A common informal density measure is:

\[
d\approx\frac{n}{\log_2(\max_i a_i)}.
\]

At density approximately one:

\[
\log_2(\max_i a_i)\approx n.
\]

So for \(n=100\), input values may be roughly 100-bit integers.

This matters because the **numeric sum universe is enormous**.

A pseudo-polynomial algorithm indexed by every possible sum may therefore require an array whose size is exponential in \(n\).

That is the first reason plain dense FFT over the complete sum range is not our approach.

---

# 4. Why representation techniques are useful near density one

Representation algorithms do something subtler than ordinary MITM.

They deliberately create **many different descriptions of the same correct solution**, then use that redundancy to survive aggressive filtering.

The rough exchange is:

\[
\boxed{
\text{representation multiplicity}
\longrightarrow
\text{filtering power}
}
\]

Instead of searching only for one binary representation \(e\), representation algorithms construct alternate signed decompositions of \(e\).

Those alternate representations give the algorithm multiple chances for the correct solution to pass random/modular filters.

---

# 5. TER: the current target representation algorithm

**Established.**

The 2025 paper:

> Yang Li, *From Subset-Sum to Decoding: Improved Classical and Quantum Algorithms via Ternary Representation Technique*

introduces a ternary representation approach for random density-1 Subset Sum.

The paper reports a classical heuristic complexity of approximately:

\[
\widetilde O(2^{0.2400n})
\]

time and:

\[
\widetilde O(2^{0.2221n})
\]

space.

This is a **random density-1 heuristic result**, not a worst-case general Subset Sum bound.

Reference:

- https://doi.org/10.3390/info16100887

---

# 6. What ternary representation means

The important idea is not simply:

> split the item set into three physical chunks.

Instead, a solution vector is represented through signed child vectors.

Conceptually:

\[
e=e_1+e_2+e_3
\]

where children use values such as:

\[
-1,\ 0,\ +1.
\]

A single coordinate can therefore be represented in several ways.

For example:

\[
1=1+0+0
\]

and:

\[
1=1+1-1.
\]

Including permutations, one coordinate may have several valid local decompositions.

Across many coordinates, that becomes exponentially many representations.

TER then filters these representations using modular and structural constraints.

---

# 7. Critical correction: TER is a hybrid algorithm

**Established.**

A major correction from the earliest version of our discussion is:

> TER is not "ternary merges at every level forever."

Different levels use different mechanisms.

For the classical depth-3 construction, the useful mental model is:

```text
                         TOP
                    binary/final work
                          |
                          v
              higher representation structure
                          |
                          v
                TERNARY MERGE/FILTER
                     /      |      \
                    /       |       \
               level-2 representation lists
                    ^       ^       ^
                    |       |       |
             bottom list construction
                    |
                    v
                MITM-style work
                    |
                    v
                 lower lists
```

This immediately changes where an optimization belongs.

---

# 8. TER bottom: MITM-style work

**Established.**

At the bottom of the depth-3 TER construction, lower-level lists are paired and combined using meet-in-the-middle-style work to construct the next level.

So the bottom looks conceptually like:

```text
List A          List B
    \            /
     \          /
       combine
          |
          v
       List C
```

This stage is naturally related to Schroeppel–Shamir.

It is **not** the main target of our original ternary-convolution idea.

---

# 9. TER middle: ternary merge-and-filter

**Established + Proposed target.**

Higher up, TER performs structured three-list merges.

Conceptually:

```text
List A        List B        List C
   \             |             /
    \            |            /
        three-list merge
               |
             filter
               |
               v
          parent list
```

The entries are not merely integers.

They also carry representation structure.

A valid parent may need:

- an arithmetic sum relation;
- a modular relation;
- a signed-vector/weight relation;
- other TER-specific structural conditions.

This is the main TER stage we want to investigate with NTT/FFT/sparse convolution.

---

# 10. TER top: also special

**Established.**

The top is not simply another identical ternary representation layer.

The published construction uses a binary/non-representation-style operation at the top.

Therefore the clean optimization map is:

| TER zone | Native shape | Initial optimization idea |
|---|---|---|
| Bottom | MITM-style | possibly optimized SS later |
| Middle | ternary merge-and-filter | NTT/FFT/sparse convolution |
| Top | binary/final | leave alone initially |

---

# 11. Why plain FFT does not solve density-1 SSP

Subset Sum has a beautiful generating-function encoding:

\[
P(x)=\prod_{i=1}^{n}(1+x^{a_i}).
\]

The coefficient of \(x^T\) is nonzero exactly when some subset sums to \(T\).

So polynomial multiplication is absolutely relevant.

FFT can multiply polynomials quickly.

The problem is the degree.

At density one, values can be roughly \(2^n\) in scale.

A dense polynomial/array covering the whole sum range can therefore require roughly exponential numeric space.

So:

```text
full numeric universe
        |
        v
giant dense FFT
```

is usually hopeless.

Our proposal is instead:

```text
already-structured SS/TER lists
        |
        v
small modular universe
        |
        v
FFT/NTT
```

---

# 12. What FFT is useful for here

For this project, forget the audio-frequency definition.

The useful interpretation is:

> FFT lets us compute additive combination information for many values at once.

Suppose we have lists \(A\) and \(B\).

Construct indicator/count arrays:

\[
F[x]=\#\{a\in A:a=x\}
\]

and:

\[
G[x]=\#\{b\in B:b=x\}.
\]

Their convolution:

\[
H=F*G
\]

satisfies:

\[
H[s]=\#\{(a,b):a+b=s\}.
\]

Instead of checking every pair individually, convolution computes information about all sum buckets together.

---

# 13. Why NTT is currently the preferred transform

**Proposed implementation choice.**

FFT uses complex floating-point arithmetic.

NTT, the Number-Theoretic Transform, performs FFT-like convolution using modular integer arithmetic.

For our problem, exactness is attractive.

With a correct NTT setup:

```text
coefficient = 0
    -> no combination for that modular bucket

coefficient != 0
    -> at least one modular combination exists
```

No floating-point rounding drama.

Therefore:

\[
\boxed{\text{NTT is the preferred V1 transform}}
\]

with ordinary FFT kept as a performance baseline because highly optimized FFT libraries can be extremely fast.

---

# 14. The core modular-convolution idea

Choose a modular universe \(M\).

For each partial sum \(s\), compute:

\[
r=s\bmod M.
\]

Build an array indexed by residues:

```text
0
1
2
...
M-1
```

For list \(A\):

\[
A_r[j]=\#\{a\in A:a\bmod M=j\}.
\]

For list \(B\):

\[
B_r[j]=\#\{b\in B:b\bmod M=j\}.
\]

Compute cyclic convolution:

\[
C=A_r*B_r.
\]

Then:

\[
C[t]=0
\]

means there is no pair whose residues sum to \(t\pmod M\).

That is useful as a safe modular rejection condition.

---

# 15. Bloom-filter analogy

Earlier we compared the transform to a Bloom filter.

That analogy is useful only at the interface level.

Bloom filter:

```text
NO    -> definitely absent
MAYBE -> exact check
```

Our modular convolution filter:

```text
ZERO      -> impossible under modular condition
NON-ZERO  -> investigate exact candidates
```

But there is an important difference.

The NTT result is exact **for the modular question**.

The apparent false positives come from the compression:

\[
a+b\equiv T\pmod M
\]

does not imply:

\[
a+b=T.
\]

So modular equality is only a coarse fingerprint.

---

# 16. The safe-filter contract

For any prefilter we want:

\[
\boxed{\text{zero false negatives for anything we prune}}
\]

Meaning:

```text
filter says impossible
      |
      v
we may safely skip exact work
```

but:

```text
filter says possible
      |
      v
exact algorithm still decides
```

This keeps NTT as an optimization layer rather than part of the correctness proof for SAT.

---

# 17. Why binary search and two pointers still exist

NTT does not magically return the exact representation we want.

It tells us which modular combinations exist.

After that, we still need actual entries.

Possible recovery tools include:

- binary search;
- two pointers;
- hash tables;
- bucket-local enumeration;
- existing SS pair streams;
- sparse-convolution witness recovery.

For a simple exact two-list target problem, sorted two-pointer search can already be excellent.

Therefore:

\[
\boxed{\text{NTT must save enough exact work to justify its own preprocessing cost}}
\]

or it is pointless.

---

# 18. Witness recovery is the central difficulty

A convolution coefficient may say:

```text
bucket 8421 -> 158,392 pair combinations
```

but TER may need:

> Which exact representation vectors created those pairs?

Likewise, SS eventually needs an actual subset witness.

If recovering witnesses forces us to enumerate everything the transform compressed, the speedup evaporates.

So one of the project's deepest questions is:

\[
\boxed{
\text{Can modular convolution reject enough work while keeping witness recovery cheap?}
}
\]

---

# 19. Structural compatibility makes TER harder than plain convolution

TER does not only ask:

\[
a+b+c\equiv r\pmod M.
\]

It may also require the corresponding representation vectors to combine correctly.

A plain one-dimensional convolution forgets most of that vector structure.

Three possible strategies:

### A. Arithmetic-only prefilter

Use NTT only to reject impossible arithmetic residue combinations.

Then run all structural checks normally.

This should be the first implementation.

### B. Small structural classes

Partition entries by a small statistic, such as a weight class, then perform separate convolutions.

State becomes something like:

```text
(structural class, residue)
```

### C. Multidimensional convolution

Encode several properties simultaneously.

Mathematically elegant.

Potentially catastrophic because state size multiplies across dimensions.

Therefore extra dimensions should remain polynomial/subexponential if possible.

---

# 20. Multiple transforms: the "double FFT" idea, corrected

Running the exact same FFT twice gives the exact same information twice.

The useful version is:

> use two independent modular views.

For example:

\[
M_1,\ M_2.
\]

Compute compatibility under each.

```text
                     sums
                  /        \
                 /          \
             mod M1        mod M2
                |             |
              NTT 1         NTT 2
                |             |
                +------AND----+
                       |
                  exact recovery
```

A surviving candidate must be consistent with both.

If \(M_1\) and \(M_2\) are coprime, the pair of residues provides information modulo approximately:

\[
M_1M_2.
\]

This is related to the Chinese Remainder Theorem.

---

# 21. Why two small transforms are tempting

Suppose we want roughly \(b\) bits of residue discrimination.

One large transform might use:

\[
M\approx2^b
\]

and cost roughly:

\[
O(M\log M).
\]

Two smaller transforms might use:

\[
M_1\approx M_2\approx2^{b/2}.
\]

Their combined dense-transform sizes are closer to:

\[
2\cdot 2^{b/2}
\]

rather than:

\[
2^b.
\]

At first glance this looks like a free lunch.

It is not.

---

# 22. CRT is not magic: witness correlation problem

Transform 1 may tell us:

```text
some pair matches residue r1
```

and transform 2 may tell us:

```text
some pair matches residue r2
```

Those may be **different pairs**.

Therefore we cannot blindly combine the two residue facts and assume one common witness exists.

The same actual representation entries must satisfy both modular views.

This can reintroduce substantial recovery work.

The central multiple-modulus question becomes:

\[
\boxed{
\text{Can we get large-modulus filtering power from small transforms without paying large-modulus cost during witness correlation?}
}
\]

---

# 23. Sparse convolution

**Established related technique.**

Dense NTT processes every bucket in a chosen modular universe, even empty buckets.

If only a small fraction of buckets are nonzero, sparse convolution may be more appropriate.

Relevant work:

- Karl Bringmann, Nick Fischer, Vasileios Nakos,  
  *Sparse Nonnegative Convolution Is Equivalent to Dense Nonnegative Convolution*  
  https://arxiv.org/abs/2105.05984

The relevance is conceptual:

```text
huge universe
+
sparse additive support
+
hashing
+
convolution
+
recovery
```

is already a serious algorithmic toolbox.

Sparse convolution is therefore a later candidate after measuring actual TER/SS bucket support.

---

# 24. Hashing + FFT is already established

**Established.**

Using modular/linear hashing together with FFT is not itself novel.

Relevant additive-algorithm work includes:

- Shashwat Kasliwal, Adam Polak, Pratyush Sharma,  
  *3SUM in Preprocessed Universes: Faster and Simpler*  
  https://arxiv.org/abs/2410.16784

- Nick Fischer,  
  *Sumsets, 3SUM, Subset Sum: Now for Real!*  
  https://arxiv.org/abs/2410.21953

So the possible novelty is **not**:

> We used FFT with modular hashing.

The possible novelty is the specific integration with TER or the particular exact-search regime.

---

# 25. FFT-based Subset Sum is already established

**Established.**

FFT/convolution has long been relevant in pseudo-polynomial Subset Sum algorithms.

A major reference is:

- Konstantinos Koiliaris, Chao Xu,  
  *A Faster Pseudopolynomial Time Algorithm for Subset Sum*  
  https://arxiv.org/abs/1507.02318

Again:

\[
\boxed{
\text{our novelty cannot be "FFT for Subset Sum"}
}
\]

The project is about using convolution **inside exponential list/search algorithms** in a different regime.

---

# 26. The existing Schroeppel–Shamir implementation

**Observed in uploaded code.**

The uploaded codebase contains:

```text
ssp_oracle(9).cpp
ssp_oracle(7).dll
ssp_build_info.generated.h
```

The source file is roughly 17.6k lines.

It is not merely a textbook Schroeppel–Shamir implementation.

It is a larger SSP oracle/portfolio containing multiple backends and a substantial SS subsystem.

---

# 27. What the wider SSP oracle contains

**Observed.**

The source includes machinery for:

- flat tables;
- stream-filter modes;
- Bloom filters;
- lazy bucket lookup;
- BuildLight/range-based modes;
- backend portfolio selection;
- Schroeppel–Shamir;
- 64-bit paths;
- 128-bit exact paths;
- memory/time/work budgets;
- witness verification;
- batching;
- caching/memoization;
- parallel execution;
- extensive telemetry.

Therefore:

\[
\boxed{
\texttt{ssp\_oracle(9).cpp} \text{ is an SSP engine containing SS, not just "SS.cpp".}
}
\]

---

# 28. SS-only isolation mode

**Observed.**

The source contains a strict Schroeppel–Shamir-only route.

Its comments explicitly state that this path does not fall back to:

- flat stream filtering;
- small-flat reuse;
- BuildLight;
- other portfolio paths.

This is extremely useful for experiments.

We can benchmark:

```text
SS only
```

rather than accidentally measuring a portfolio that silently selected another method.

---

# 29. Schroeppel–Shamir core structure

SS splits the active items into four quarters:

```text
             items
       /       |       \
      /        |        \
    Q0        Q1        Q2        Q3
```

Each quarter generates a subset-sum list.

Then:

```text
Q0 + Q1 -> AB ordered pair stream
Q2 + Q3 -> CD ordered pair stream
```

One stream advances upward in sum.

The other advances downward.

The algorithm searches for:

\[
AB+CD=T.
\]

The clever part is that it need not materialize every pair sum simultaneously.

---

# 30. Quarter generation

**Observed.**

The uploaded 128-bit path explicitly generates quarter subset sums.

For each quarter, entries contain at least:

```text
sum
local subset mask
```

The mask preserves enough information to reconstruct the selected subset later.

For a quarter of \(q\) items, raw generation has up to:

\[
2^q
\]

subset entries before deduplication.

---

# 31. Quarter sorting and deduplication

**Observed.**

Quarter lists are sorted by sum.

Duplicate sums are compacted.

Conceptually:

```text
generated sums
      |
      v
     sort
      |
      v
 deduplicate
      |
      v
unique quarter list
```

This reduces redundant search states and improves pair-stream behavior.

---

# 32. Heap-generated pair streams

**Observed.**

The source has explicit pair-stream heap nodes with fields equivalent to:

```text
pair_sum
row_index
col_index
```

Heap operations generate pair sums in ordered fashion.

This is important because our NTT idea is not competing against a naive nested loop.

The baseline already uses a strong memory-efficient ordered search.

---

# 33. Two-pointer-style exactness logic

**Observed.**

The multi-target SS comments spell out a two-pointer-like invariant.

For one target:

- if current \(AB+CD\) is too large, move the descending side;
- if it is too small, move the ascending side;
- exact exhaustion can prove no pair exists.

This allows large regions of pair space to be skipped implicitly.

Therefore SS is a harder place for NTT to win than a naive all-pairs merge.

---

# 34. Exact witness reconstruction

**Observed.**

The SS implementation preserves quarter-local masks.

When a target match is found, it reconstructs four quarter witnesses.

The final witness encodes which original items were selected.

So the SS engine can produce an actual subset, not merely a yes/no count.

---

# 35. Independent witness verification

**Observed.**

The 128-bit path independently recomputes the selected sum from original item values and recovered masks.

It does not simply trust a cached pair sum.

SAT is only accepted after the witness is rechecked exactly.

This is an excellent correctness boundary for future NTT integration:

```text
NTT optimization
      |
      v
candidate
      |
      v
existing exact witness recovery
      |
      v
existing exact witness verification
```

---

# 36. 128-bit exact arithmetic

**Observed.**

The source has an unsigned 128-bit exact path using compiler-native `unsigned __int128` internally.

Across the C ABI, values are represented by two 64-bit limbs.

Conceptually:

\[
value=lo+2^{64}hi.
\]

This makes the engine much more useful for large-bit-width SSP experiments than a 64-bit-only solver.

---

# 37. Checked overflow behavior

**Observed.**

The 128-bit implementation performs checked additions.

If arithmetic cannot be represented safely, it rejects/downgrades the operation instead of silently wrapping.

Future transform code should preserve this philosophy.

---

# 38. Resource budgets

**Observed.**

The SS subsystem supports controls for things such as:

- memory cap;
- time budget;
- heap-pop budget;
- heap-push budget;
- pair-candidate budget;
- maximum quarter size;
- batch limits.

This allows controlled benchmark regimes rather than unconstrained searches.

---

# 39. SS admission control

**Observed.**

The surrounding engine can reject an SS attempt before full execution based on predicted resource cost.

Reasons include:

- memory;
- numeric width;
- insufficient heap budget;
- insufficient pair budget;
- predicted search cost;
- time budget;
- SS disabled;
- another strategy preferred.

A future NTT-assisted SS mode could theoretically become another admission candidate, but that is not V1.

---

# 40. SS telemetry

**Observed.**

The source already tracks many useful metrics, including:

- SS calls;
- SAT/UNSAT/UNKNOWN;
- quarter sums generated;
- quarter bytes;
- quarter generation time;
- quarter sorting time;
- heap bytes;
- heap pushes;
- heap pops;
- pair candidates inspected;
- search time;
- total SS time;
- memory estimates;
- peak memory;
- witness successes/failures;
- batch reuse;
- admission outcomes;
- predicted search cost.

This is unusually good for research.

We can measure not only:

> Did runtime improve?

but:

> Which exact work disappeared?

---

# 41. Shared multi-target Schroeppel–Shamir

**Observed.**

The source contains a multi-target group sweep.

One ascending AB stream can be shared across multiple targets.

Each target maintains its own descending CD stream.

Conceptually:

```text
                 shared AB
                    |
        +-----------+-----------+
        |           |           |
     target 1    target 2    target 3
        |           |           |
      CD1         CD2         CD3
```

This prevents us from claiming:

> NTT is useful because it batches targets

without comparison.

The SS baseline already exploits some multi-target structure.

---

# 42. Shared multi-target is not always faster

**Observed.**

The source comments note that the group-sweep mode can increase pair checks/runtime on certain workloads.

That is a useful warning.

Even clever-looking structural reuse can lose in practice.

The same standard applies to NTT.

---

# 43. Caching and memoization

**Observed.**

The 128-bit engine contains quarter-build reuse and memoization machinery.

Tracked events include:

- cache lookup attempts;
- builds retained;
- evictions;
- pinned entries;
- duplicate builds avoided;
- memory accounting;
- single-flight behavior;
- batch pins/releases.

NTT experiments must compare preprocessing fairly.

A cached SS run should not be compared against an NTT run with freshly paid preprocessing, or vice versa.

---

# 44. Parallel machinery

**Observed.**

The source has OpenMP-based parallel paths and parallel probing/sorting infrastructure.

The generated build info shows compilation with:

```text
-std=c++17
-O3
-shared
-pthread
-fopenmp
-static-libgcc
-static-libstdc++
```

with OpenMP requested.

This means the baseline is already serious native code.

An NTT experiment should use comparable thread budgets.

---

# 45. Radix infrastructure

**Observed.**

The wider oracle contains:

- radix-8;
- radix-16;
- automatic radix selection;
- parallel radix-16;
- scratch accounting;
- self-tests.

However, some SS quarter paths still explicitly use `std::sort`.

So the correct statement is:

> The codebase has strong radix infrastructure, but not every SS sorting path is necessarily using it.

This may become another future optimization, but it is not part of the NTT hypothesis.

---

# 46. Bloom filters already exist elsewhere in the oracle

**Observed.**

The wider SSP engine already has Bloom-filter support.

It tracks:

- eager builds;
- lazy promotion;
- bits per key;
- number of hashes;
- probes;
- negative skips;
- maybe-hits;
- exact searches after filtering.

This gives us a strong baseline for the earlier conceptual architecture:

```text
cheap filter
    |
    v
exact lookup
```

Therefore Bloom filtering itself is not novel inside this codebase.

---

# 47. Lazy bucketed search also exists

**Observed.**

The surrounding oracle contains a bucket-lazy mode.

It can:

1. partition sums into buckets;
2. leave buckets unsorted initially;
3. sort a bucket only when queried;
4. binary-search inside the sorted bucket.

This is particularly relevant because our NTT idea also compresses work into modular buckets.

So a fair comparison must include:

\[
\boxed{\text{direct modular bucketing without NTT}}
\]

Otherwise we may discover that the transform is an expensive way to perform a job a bucket index already handles.

---

# 48. Build provenance

**Observed.**

The generated build header records:

- source SHA-256;
- build fingerprint;
- compiler description;
- build flags;
- OpenMP request status.

This should be retained in every benchmark.

Minimum benchmark metadata:

```text
source hash
build fingerprint
compiler
compiler flags
CPU
thread count
memory cap
time/work budgets
algorithm configuration
instance seed
```

That is excellent for reproducibility.

---

# 49. Why SS is a useful NTT proving ground

We already have the engine.

That means we can test convolution without first implementing TER.

The experiment can answer:

> Can NTT reduce meaningful exact-search work even against a heavily optimized Schroeppel–Shamir baseline?

If the answer is no, that is useful.

If the answer is yes, that is much more interesting because the baseline is not weak.

---

# 50. Why SS is also a difficult NTT target

SS already avoids naive pair enumeration.

Its ordered heap streams and two-pointer-style logic skip large regions of pair space.

So an NTT prefilter costs extra preprocessing before an already efficient search.

Therefore the transform must eliminate a large amount of:

- heap work;
- pair candidate work;
- repeated target work;
- stream advancement;

to pay for itself.

This is why the expectation is:

\[
\boxed{\text{SS = excellent testbed, but possibly difficult to improve}}
\]

---

# 51. What NTT could do inside SS

**Proposed.**

A useful NTT integration cannot be:

```text
run NTT
then run exactly the same SS search
```

That simply adds work.

The transform needs to make SS skip something.

Possible targets:

- impossible residue classes;
- impossible target groups;
- portions of AB/CD compatibility space;
- repeated modular checks across batches;
- exact bucket searches.

A conceptual version:

```text
quarter lists
     |
     v
derive modular fingerprints
     |
     v
NTT compatibility map
     |
     v
mark impossible AB/CD residue relations
     |
     v
existing exact heap-stream search
but with some work suppressed
```

The exact mechanism remains open.

---

# 52. TER is the more natural NTT target

TER's ternary merge combines three representation lists and then applies structural filtering.

That produces more explicit combinatorial pressure than SS's already-ordered two-stream search.

So the current expectation is:

```text
SS + NTT
    -> easiest to prototype
    -> harder to beat

TER + NTT
    -> harder to implement
    -> more natural place for bulk convolution filtering
```

This is a hypothesis, not a conclusion.

---

# 53. TER + NTT proposed flow

For one higher TER ternary merge:

```text
TER list A
TER list B
TER list C
      |
      v
extract arithmetic sums / residues
      |
      v
modular bucket encoding
      |
      v
NTT / sparse convolution
      |
      v
identify viable modular combinations
      |
      v
recover original list entries
      |
      v
check TER structural constraints
      |
      v
construct valid parent entries
```

NTT handles arithmetic compatibility.

TER remains responsible for representation correctness.

---

# 54. TER bottom + optimized SS

**Proposed but parked.**

Because TER uses MITM-style work at the bottom, the existing optimized SS machinery may eventually be useful there.

Conceptually:

```text
TER lower construction
        |
        v
optimized SS/MITM machinery
        |
        v
TER level-2 lists
```

But there is a major interface mismatch.

---

# 55. The TER↔SS integration problem

**Parked.**

The current SS API is largely target-query shaped:

```text
Does target T exist?
```

with outputs resembling:

```text
SAT / UNSAT / UNKNOWN
possibly one witness
```

TER's bottom construction may need something closer to:

```text
enumerate/build representation entries
satisfying specific modular and structural rules
```

Those are different APIs.

Therefore we cannot assume:

```text
TER -> call current DLL -> done
```

Internal SS machinery may be reusable, but the current public interface may not expose the list-generation behavior TER needs.

---

# 56. Questions required for TER↔SS integration

Eventually we may need to answer:

- Does TER need all valid bottom entries or only sampled/filtered ones?
- What metadata must every generated representation entry retain?
- Can current quarter masks encode TER's signed-vector representation?
- Would pair-stream enumeration destroy SS's memory advantage if TER needs many outputs?
- Can TER's modular constraints be pushed into SS generation?
- Should the DLL API be extended?
- Should only internal C++ components be extracted and reused?
- Does SS even outperform TER's original bottom MITM for the relevant list shape?

None of these are needed for the first NTT experiment.

So:

\[
\boxed{\text{TER↔SS integration is intentionally postponed}}
\]

---

# 57. The project is now three tracks

## Track A: SS + NTT/FFT

Immediate experimental testbed.

Goal:

\[
\boxed{\text{reduce exact Schroeppel–Shamir search work}}
\]

## Track B: TER ternary merge + NTT/FFT

Main representation-algorithm target.

Goal:

\[
\boxed{\text{reduce explicit ternary merge/filter work}}
\]

## Track C: TER bottom + optimized SS

Later integration.

Goal:

\[
\boxed{\text{reuse the overengineered SS machinery in TER's MITM bottom stage}}
\]

Track C is parked.

---

# 58. Baselines for SS experiments

A proper study should compare:

### S0 — current strict SS-only implementation

The real baseline.

### S1 — direct modular buckets

No transform.

Tests whether simple bucketing already achieves the desired filtering.

### S2 — Bloom-style prefilter

Where relevant.

Tests whether a cheap probabilistic membership structure is enough.

### S3 — one NTT

The first transform-assisted version.

### S4 — two independent NTT moduli

Only after S3.

### S5 — sparse convolution

Only after measuring support sparsity.

---

# 59. Baselines for TER experiments

### T0 — faithful original TER merge

Required baseline.

### T1 — direct modular bucketing

No transform.

### T2 — one NTT

Arithmetic prefilter only.

### T3 — two independent modular transforms

Repeated filtering.

### T4 — sparse convolution

If support is sparse enough.

### T5 — structural-class-aware convolution

Only after arithmetic-only filtering is understood.

---

# 60. Later integration baselines

Not now, but eventually:

### I0 — original TER bottom MITM

### I1 — TER bottom using adapted optimized SS

### I2 — TER bottom using SS + NTT

These belong to Track C.

---

# 61. Metrics for SS

The existing telemetry makes this unusually straightforward.

Collect:

- quarter sums generated;
- quarter unique sums;
- quarter generation time;
- quarter sorting time;
- heap pushes;
- heap pops;
- pair candidates inspected;
- exact search time;
- total wall time;
- cache hits/reuse;
- NTT preprocessing time;
- NTT memory;
- total memory;
- witness verification count;
- SAT/UNSAT/UNKNOWN outcome.

The transform wins only if:

\[
T_{\text{NTT}} + T_{\text{reduced exact search}}
<
T_{\text{baseline exact search}}.
\]

---

# 62. Metrics for TER

Collect:

- child list sizes;
- candidate triples/pairs formed;
- modular survivors;
- structural survivors;
- output list size;
- transform time;
- recovery time;
- verification time;
- peak memory;
- bucket occupancy;
- number of nonzero output buckets;
- true representation count;
- false modular compatibility count.

The key is to identify whether the transform reduces **actual merge materialization**, not merely changes accounting.

---

# 63. Bucket occupancy is a make-or-break metric

For a modulus \(M\), define bucket occupancy.

If list size is \(L\), average occupancy is roughly:

\[
\frac{L}{M}
\]

under uniform assumptions.

If \(M\) is too small:

```text
transform cheap
but buckets huge
recovery terrible
```

If \(M\) is too large:

```text
buckets clean
but transform expensive
```

There may be a sweet spot.

This is one of the first things to measure.

---

# 64. Simple exponent model

Suppose a list has:

\[
|L|=2^{\lambda n}
\]

and transform universe:

\[
M=2^{\mu n}.
\]

Dense NTT cost is approximately:

\[
\widetilde O(2^{\mu n}).
\]

Average bucket occupancy is roughly:

\[
2^{(\lambda-\mu)n}
\]

when \(\lambda>\mu\).

Thus:

### Small \(\mu\)

Cheap transform, crowded buckets.

### Large \(\mu\)

Expensive transform, clean buckets.

The optimization problem is roughly:

\[
\min_\mu
\max(
\text{transform cost},
\text{recovery cost},
\text{remaining algorithm costs}
).
\]

---

# 65. Why we do not expect a new TER exponent by default

TER's reported:

\[
2^{0.2400n}
\]

time comes from exponential list/merge structure.

If our transform only improves:

- constant factors;
- sorting;
- candidate rejection;
- cache behavior;
- practical merge work;

while the same exponential-size objects remain fundamentally necessary, then the exponent probably does not change.

So the current honest goal is:

\[
\boxed{\text{same asymptotic family, lower practical runtime and/or memory}}
\]

If the exponent happens to move, excellent.

But the paper should not be built around assuming that outcome.

---

# 66. What counts as a successful result

## Tier A — global asymptotic improvement

Best case.

A valid total TER exponent below \(0.2400\).

Not assumed.

## Tier B — local asymptotic improvement

A TER merge primitive is asymptotically improved, even if another stage remains globally dominant.

## Tier C — strong practical acceleration

Same asymptotic exponent, materially lower real runtime or memory.

This is the most realistic positive target.

## Tier D — regime characterization

Example:

> NTT wins when bucket occupancy/output support falls below threshold X; direct merging wins above it.

This can be a strong experimental/theoretical contribution.

## Tier E — negative/barrier result

Show that convolution fails because:

- output size dominates;
- witness recovery recreates the original cost;
- transform cost dominates;
- modular filters do not reject enough work.

A careful negative result can still be research.

---

# 67. Current novelty assessment

## Definitely not novel by themselves

- FFT for additive problems;
- NTT;
- FFT/NTT for polynomial convolution;
- pseudo-polynomial FFT-based Subset Sum;
- modular hashing;
- multiple moduli;
- CRT;
- Bloom filters;
- sparse convolution;
- Schroeppel–Shamir;
- ternary representation;
- binary search;
- two-pointer search.

## Potentially novel-looking combination

A preliminary literature scan did **not** locate the exact combination:

\[
\boxed{
\text{TER ternary merge}
+
\text{modular/sparse convolution prefilter}
+
\text{exact representation recovery}
}
\]

Nor did it establish that the exact SS + NTT integration proposed here has already been studied in this form.

That is not proof of novelty.

The correct current wording is:

> A preliminary search did not locate this exact integration.

---

# 68. Why "double FFT" is not the paper framing

"Double FFT" is one experimental variant.

The broader research question is:

> Can convolution-based modular filtering and recovery reduce exact work in representation and MITM-style Subset Sum algorithms?

That framing allows:

- one NTT;
- several NTTs;
- sparse convolution;
- direct hashing;
- adaptive moduli;
- hybrid recovery;
- SS and TER case studies.

This prevents the project from dying if literal two-transform filtering is mediocre.

---

# 69. Possible paper framing

A broad title:

> **Convolution-Assisted Exact Search for Representation and Meet-in-the-Middle Subset Sum Algorithms**

A TER-focused title:

> **Modular Convolution for Ternary Representation Merges in Random Subset Sum**

An SS-focused experimental title:

> **Can Modular Convolution Accelerate Schroeppel–Shamir Search?**

A combined paper would need both case studies to be substantial enough to justify the broader framing.

---

# 70. Candidate abstract framing

Only after results exist:

> We investigate modular convolution as a prefilter for exact exponential-time Subset Sum algorithms. Rather than applying FFT over the full numeric sum universe, we map partial-sum structures into smaller modular domains and use NTT/FFT or sparse convolution to identify additive compatibility before exact witness recovery. We evaluate the approach first against a heavily optimized Schroeppel–Shamir implementation and then against ternary representation merge-and-filter operations for random density-1 Subset Sum. The study characterizes the tradeoff between transform size, bucket occupancy, modular collisions, witness-recovery cost, and exact-search work, identifying the regimes in which convolution provides practical benefit and those in which existing ordered merge methods remain superior.

Do not use that as a claim until the experiments support it.

---

# 71. Correctness rules

The existing SSP engine has a strong correctness philosophy.

Keep it.

## SAT

Only report SAT after an exact witness is recovered and verified.

## UNSAT

Only report UNSAT when exact logic proves exhaustion/impossibility.

A lossy modular filter cannot independently prove full integer UNSAT unless its logic truly covers the exact condition.

## UNKNOWN

If resource limits, transform assumptions, arithmetic limits, or recovery fail:

- return/fall back safely;
- do not convert uncertainty into UNSAT.

---

# 72. NTT correctness issues to handle

Potential implementation concerns:

- coefficient overflow;
- transform modulus size;
- transform-length compatibility;
- cyclic versus non-cyclic convolution;
- accidental wrap in count coefficients;
- multiple NTT primes if coefficient range is too large;
- witness correlation across multiple moduli;
- preserving zero-false-negative pruning.

The easiest first implementation may use Boolean support semantics if counts are not required.

---

# 73. Why FFT may still beat NTT in practice

Even though NTT is cleaner mathematically, FFT implementations are extremely optimized.

Therefore benchmark:

```text
NTT
vs
FFT
```

under the same bucket universe and workload.

If FFT is used, numerical safety needs explicit validation so tiny floating errors never create an unsafe false negative.

---

# 74. FWHT and other algorithms from the earlier comparison

Most other fast algorithms discussed are wrong-shaped for the core arithmetic problem.

### FWHT

Useful for XOR/AND/OR-style convolution.

Could become interesting only if TER structural compatibility is encoded using bitwise subset masks.

Not V1.

### Fast Zeta/Möbius transform

Useful over subset lattices.

Usually requires \(O(n2^n)\)-type state, which is exactly the kind of full subset universe representation algorithms try to avoid.

Potentially useful only on small blocks.

### Karatsuba

Fast multiplication of large integers.

Not the main merge problem.

### Strassen

Matrix multiplication.

No obvious natural role.

### Dijkstra / Union-Find / Aho-Corasick

Different problem families.

No current role.

So the immediate transform toolbox remains:

\[
\boxed{\text{NTT, FFT, sparse convolution}}
\]

with direct modular hashing/bucketing as essential baselines.

---

# 75. Minimal SS prototype

Do not modify the entire 17k-line oracle first.

Start with a sidecar/prototype.

### Input

Use the same quarter-sum lists or extracted pair-stream data.

### Build

Construct modular bucket arrays.

### Transform

Run one NTT.

### Measure

Ask:

- how many target/residue classes can be rejected?
- how many pair candidates would that rejection avoid?
- how much time did the transform cost?

### Only then

Integrate the filter into the exact SS search if the numbers look favorable.

---

# 76. Minimal TER prototype

Do not implement the entire TER solver first.

Implement one faithful ternary merge stage.

Input:

```text
List A
List B
List C
sum metadata
minimum structural metadata
```

Compare:

```text
baseline merge
vs
direct modular buckets
vs
one NTT prefilter
```

Measure:

- combinations considered;
- modular survivors;
- recovery work;
- structural survivors;
- output list equality;
- runtime;
- memory.

This isolates the actual research question.

---

# 77. Recommended experiment order

## Phase 1 — freeze the SS baseline

Record current source hash/build fingerprint.

Run strict SS-only benchmarks.

## Phase 2 — build a simple modular bucket baseline

Before NTT.

If buckets alone solve the problem, NTT has no business being there.

## Phase 3 — one NTT on SS

Measure whether exact stream work can actually be skipped.

## Phase 4 — one faithful TER merge implementation

No fancy transform yet.

## Phase 5 — one NTT on TER

Arithmetic prefilter only.

## Phase 6 — multiple moduli

Only if one transform helps.

## Phase 7 — sparse convolution

Only if support measurements justify it.

## Phase 8 — structural metadata in convolution

Only if arithmetic false positives dominate.

## Phase 9 — TER↔SS integration

Only after the separate tracks show value.

---

# 78. What not to do

Do not immediately:

- rewrite TER;
- rewrite SS;
- add Bloom + NTT + two moduli + binary search simultaneously;
- touch admission-control policy;
- expose new ABI functions before the prototype works;
- claim novelty from absence of one search result;
- claim asymptotic improvement from benchmark curves;
- compare against naive nested loops;
- ignore direct hashing/bucketing;
- ignore cache fairness;
- ignore output size;
- ignore witness recovery;
- treat modular equality as exact equality.

---

# 79. Major failure modes

## Failure A — NTT cost dominates

The modular universe needed for useful filtering is too large.

## Failure B — buckets are crowded

The transform rejects little; recovery remains huge.

## Failure C — SS already skips the same work

Ordered pair streams make NTT redundant.

## Failure D — TER structural checks dominate

Arithmetic filtering is cheap, but almost all arithmetic matches fail structural compatibility anyway.

## Failure E — output size dominates

If the algorithm genuinely needs to produce an enormous valid parent list, no prefilter can avoid outputting it.

## Failure F — multi-modulus witnesses do not correlate

Each modulus finds a compatible combination, but not the same entries.

## Failure G — memory explodes

Transform arrays plus original lists plus recovery indices exceed baseline memory.

## Failure H — stage is not globally dominant

A fast local merge does not improve total algorithm time.

---

# 80. The deepest TER question

\[
\boxed{
\text{Can modular convolution prevent TER from explicitly constructing a large fraction of useless intermediate combinations?}
}
\]

Not merely count them.

Not merely move them into another data structure.

Actually avoid the work.

---

# 81. The deepest SS question

\[
\boxed{
\text{Can modular compatibility information eliminate enough heap-stream advancement to beat an already optimized Schroeppel–Shamir search?}
}
\]

If not, the result still tells us something important about where transforms do and do not fit.

---

# 82. The deepest multiple-modulus question

\[
\boxed{
\text{Can several cheap small transforms provide the filtering power of a large modulus without reintroducing the large cost during exact witness recovery?}
}
\]

That is the real "double FFT" research question.

---

# 83. Research questions

1. How uniform are SS pair-sum residues under candidate moduli?
2. How uniform are TER representation-list residues?
3. What modulus minimizes transform + recovery cost?
4. Does direct bucket lookup beat NTT?
5. Does NTT reduce heap pushes/pops in SS?
6. Does NTT reduce candidate materialization in TER?
7. How sparse are convolution outputs?
8. When does sparse convolution beat dense NTT?
9. How correlated are independent modular filters?
10. Can small TER structural classes be encoded cheaply?
11. Does one transform help SAT instances more than UNSAT?
12. Does batching make NTT substantially more valuable?
13. Does caching change the break-even point?
14. What is the memory tradeoff?
15. Can the same technique generalize beyond density one?

---

# 84. Suggested SS results table

| Instance | n | bit width | targets | baseline time | NTT time | exact search time | total hybrid | heap pops baseline | heap pops hybrid | candidates baseline | candidates hybrid | peak memory |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ... | ... | ... | ... | ... | ... | ... | ... | ... | ... | ... | ... | ... |

---

# 85. Suggested TER merge results table

| level | list sizes | modulus | baseline merge time | transform time | recovery time | baseline candidates | modular survivors | structural survivors | output size | peak memory |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ... | ... | ... | ... | ... | ... | ... | ... | ... | ... | ... |

---

# 86. Suggested multi-modulus table

| M1 | M2 | survive M1 | survive M2 | survive both | expected independent survival | observed correlation | recovery time | total time |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ... | ... | ... | ... | ... | ... | ... | ... | ... |

---

# 87. Research outcome matrix

```text
                       PRACTICAL SPEEDUP?
                    no                 yes
              +----------------+----------------+
ASYMPTOTIC    | barrier /      | strong theory  |
IMPROVEMENT?  | negative study | result         |
              +----------------+----------------+
NO ASYMPTOTIC | useful failure | experimental   |
IMPROVEMENT   | characterization| algorithms    |
              +----------------+----------------+
```

A paper does not require the top-right box.

---

# 88. Reproducibility requirements

For every benchmark record:

- exact source commit/hash;
- build fingerprint;
- compiler and version;
- optimization flags;
- thread count;
- CPU model;
- RAM;
- operating system;
- memory cap;
- time cap;
- pair/heap budgets;
- random seed;
- instance generator;
- density;
- \(n\);
- number bit width;
- SAT/UNSAT/planted status;
- transform modulus;
- transform implementation;
- caching state;
- warm/cold run status.

Without this, performance results become folklore.

---

# 89. Current project status

## Established

- TER exists and reports \(\widetilde O(2^{0.2400n})\) heuristic time for random density-1 SSP.
- TER uses different mechanisms at different levels.
- FFT/NTT convolution is a standard additive tool.
- FFT-based pseudo-polynomial SSP exists.
- hashing + FFT exists in related additive algorithms.
- sparse convolution exists.
- Schroeppel–Shamir is a classical MITM-style exact algorithm.

## Observed in uploaded code

- strict SS-only mode;
- quarter generation;
- quarter sort/dedup;
- heap pair streams;
- exact witness recovery;
- independent witness verification;
- unsigned 128-bit exact path;
- overflow checking;
- resource budgets;
- admission control;
- detailed telemetry;
- shared multi-target SS sweep;
- caching/memoization;
- OpenMP infrastructure;
- radix sorting infrastructure elsewhere;
- Bloom filtering elsewhere;
- lazy bucketed exact lookup elsewhere;
- build provenance.

## Proposed

- NTT prefilter for SS;
- NTT prefilter for TER ternary merges;
- multiple independent modular transforms;
- sparse-convolution variants;
- structural-class-aware convolution;
- eventual use of optimized SS in TER bottom stage.

## Parked

- adapting the current SS API to enumerate TER bottom representation lists;
- changing ABI;
- deep TER↔SS integration;
- aggressive multi-transform architecture;
- asymptotic-exponent claims.

---

# 90. Immediate next step

The cleanest immediate next action is:

\[
\boxed{\text{benchmark the strict SS-only implementation and identify exactly where its search time goes}}
\]

because the code already exposes the necessary telemetry.

Then:

\[
\boxed{\text{test whether a simple modular bucket/NTT prefilter can remove measurable pair-stream work}}
\]

No TER implementation is required for that first experiment.

---

# 91. One-paragraph final description

This project investigates whether modular convolution can accelerate exact exponential-time Subset Sum search without replacing the underlying algorithms. For Schroeppel–Shamir, an existing heavily engineered exact implementation provides a strong experimental baseline featuring four-quarter subset-sum generation, deduplication, ordered heap pair streams, exact 128-bit arithmetic, witness verification, batching, caching, resource admission, and detailed telemetry. The proposed modification maps partial-sum information into smaller modular domains and uses NTT/FFT or sparse convolution to identify additive compatibility before exact stream work, with the goal of skipping provably useless search regions. In parallel, the same concept is applied to the higher ternary merge-and-filter stages of the 2025 TER algorithm for random density-1 Subset Sum, where three representation lists create a more natural bulk-convolution target. Multiple independent moduli, sparse convolution, and structural-class-aware transforms are later variants. The project currently targets practical runtime and memory improvements rather than assuming a lower \(2^{0.2400n}\) TER exponent. The main unresolved issues are witness recovery, bucket occupancy, transform break-even points, structural compatibility, and eventual adaptation of the existing SS machinery to TER's bottom list-construction interface.

---

# 92. One-line version

\[
\boxed{
\textbf{Use exact modular convolution as a safe prefilter so SS or TER performs less expensive exact merge/search work, while leaving final witness recovery and verification to the original algorithm.}
}
\]

---

# 93. Literature anchors

## TER

Yang Li.  
*From Subset-Sum to Decoding: Improved Classical and Quantum Algorithms via Ternary Representation Technique.*  
Information 16(10):887, 2025.  
https://doi.org/10.3390/info16100887

## FFT-based pseudo-polynomial Subset Sum

Konstantinos Koiliaris, Chao Xu.  
*A Faster Pseudopolynomial Time Algorithm for Subset Sum.*  
https://arxiv.org/abs/1507.02318

## Sparse convolution

Karl Bringmann, Nick Fischer, Vasileios Nakos.  
*Sparse Nonnegative Convolution Is Equivalent to Dense Nonnegative Convolution.*  
https://arxiv.org/abs/2105.05984

## Linear hashing + FFT in additive algorithms

Shashwat Kasliwal, Adam Polak, Pratyush Sharma.  
*3SUM in Preprocessed Universes: Faster and Simpler.*  
https://arxiv.org/abs/2410.16784

## Modern additive/sumset context

Nick Fischer.  
*Sumsets, 3SUM, Subset Sum: Now for Real!*  
https://arxiv.org/abs/2410.21953

---

# 94. Local implementation sources

The SS implementation observations in this specification are based on the uploaded project files:

```text
ssp_oracle(9).cpp
ssp_oracle(7).dll
ssp_build_info.generated.h
```

Important observed areas include:

```text
SS-only isolation
quarter generation
quarter sorting/deduplication
pair-stream heaps
multi-target shared sweeps
128-bit witness handling
verification
resource budgets
memoization
parallel infrastructure
Bloom/lazy bucket baselines
build provenance
```

Before publishing any paper, replace informal local filenames with a proper repository commit/hash and public reproducibility package if possible.

---

# 95. Final decision log

### Decision 1
Use **NTT** as the first transform implementation; benchmark FFT later.

### Decision 2
Do **not** use full-range FFT over density-1 sums.

### Decision 3
Treat modular convolution as a **prefilter**, not as the final SSP solver.

### Decision 4
Keep exact witness verification unchanged.

### Decision 5
Test SS first because a strong implementation already exists.

### Decision 6
Treat TER's higher ternary merge as the main representation-algorithm target.

### Decision 7
Do not blindly replace TER's bottom MITM with NTT.

### Decision 8
Potentially reuse optimized SS at the TER bottom later.

### Decision 9
Park TER↔SS API/list-enumeration integration.

### Decision 10
Do not assume the TER exponent will decrease.

### Decision 11
Compare against direct buckets and Bloom-style filtering, not only naive search.

### Decision 12
Only test multiple moduli after one-modulus filtering demonstrates value.

### Decision 13
Only test sparse convolution after measuring actual sparsity.

### Decision 14
A practical speedup, regime characterization, or strong negative result can still constitute a research contribution.

---

# 96. Current research question

The master question is:

\[
\boxed{
\textbf{Can modular convolution remove enough exact search/merge work in optimized Schroeppel–Shamir and ternary representation Subset Sum algorithms to produce a meaningful practical advantage without weakening correctness?}
}
\]

Everything else in this project is now a sub-question of that.
