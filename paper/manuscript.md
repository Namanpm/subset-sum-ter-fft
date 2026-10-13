# Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting

Naman Menasagi

## Abstract

At the final merge of a Ternary Representation Technique (TER) algorithm for random Subset Sum, short arithmetic projections can admit billions of candidate pairs even when the full relation has no solutions. The Adaptive Radix Wide-Leaf Filter (ARWLF) is an exact root reporter that routes each source pair to a unique target leaf with explicit carry and filters it with independent byte-occupancy predicates before any candidate is recovered. We prove that ARWLF reports exactly the full quotient matches with posting multiplicity intact, and give a cost model that charges survivor, resolver and output work explicitly. On authentic dimension-108 roots, a single-core AVX2 implementation is a geometric-mean 53.1 times faster than the frozen tile-sweep baseline (K2) on calibration microproblems, 52.9 times on campaign-holdout roots, and about 32 times faster than a full-width strengthening of K2. On a complete prepared root of 25 trillion logical pairs it takes 57–60 minutes against 54.8 hours, a measured 55.2–57.4-fold reduction with identical output. Sparse dimension-72 roots favor a queued reporter; controlled thinning and an occupancy model locate the crossover and the target-leaf saturation that limits the fixed geometry. In a staged composition with the classical solver, ARWLF agrees differentially on 33 attempts and recovers the same independently verified dimension-72 witness; no end-to-end solver speedup is claimed. The gain is a finite-size constant factor; TER's asymptotic exponent is unchanged.

Keywords: Subset Sum; ternary representations; experimental algorithmics; modular joins; SIMD; exact filtering.

## 1 Introduction

The complexity of a representation algorithm is determined by more than the sizes of its final lists. Intermediate arithmetic matches must be discovered, checked for coordinate compatibility, and recovered as original entries. A merge that is cheap in terms of accepted parents can therefore be expensive on the machine: a short projection may generate many candidates, and nearly all of them can fail the remaining conditions. This distinction has appeared in the analysis of representation methods themselves. Becker, Coron, and Joux explicitly account for the intermediate list before filtering, rather than charging only the final filtered list [@bcj2011]. It also creates a practical optimization question: when many inexpensive pair tests are unavoidable, how much work should accompany each test?

This paper addresses that question at the final ternary root merge of a classical Ternary Representation Technique (TER) construction. Li's depth-three algorithm forms eighteen logical level-three lists, then nine level-two lists, three level-one lists, and a final root list [@li2025]. The root arithmetic is a modular three-list reporting problem. Its large source families admit billions of logical pairs per family cell at the evaluated dimension. Testing every pair with an expensive membership operation, or recovering every match of a short projection before checking the remaining bits, places the cost in the wrong part of the computation.

The Adaptive Radix Wide-Leaf Filter changes this cost distribution. Low residual bits route a pair to exactly one target leaf. Independent occupancy maps for the next bytes reject a pair whenever a required byte is absent. The maps are deliberately weaker than full equality: they summarize target bytes without retaining their correlations. That loss of information is useful because a small, register-friendly test can reject most pairs before original entry identities or wide residues are processed. Exact comparison remains on the survivor path.

The central implementation choice is to organize the scan around a target leaf. Its occupancy tables stay fixed while source rows traverse their compatible partner leaves. Carry handling becomes two contiguous ranges, and the hot operation adds one source value to sixteen sixteen-bit lanes. Additional bytes and all remaining quotient bits are checked only after the initial lane mask has been extracted. The design has one terminal radix level; it does not search for a different radix at runtime.

The evaluation separates exactness, the gain on authentic prepared roots, its source in the geometry, its size against a stronger full-width baseline, dispatcher behavior across regimes, and agreement with the original solver after integration; these questions have different answers. Dense dimension-108 microproblems show large and reproducible relative gains, including approximately 32-fold gains over the strengthened baseline. Sparse dimension-72 roots favor a queued position-and-tag reporter. The frozen dispatcher works well on the dense authentic cells but incurs substantial overhead on tiny cells and excludes some controlled cases where ARWLF would win.

The contributions are fourfold. First, an exact formulation of the TER root as 4,096 carry-coupled three-list cells, with a routing lemma and a byte-rejection lemma that together prove that ARWLF reports exactly the full quotient matches, with posting multiplicity preserved (Theorem 1). Second, a cost model that charges rows, overflow blocks, survivors, resolver scans and outputs explicitly, and therefore predicts where the method loses as well as where it wins. Third, a faithful vector realization with explicit carry, overflow, padding and identity semantics. Fourth, an evaluation on authentic captured roots: preregistered calibration and campaign-holdout roots, a complete dimension-108 root measured against the baseline, occupancy and target-density controls, a staged integration with the classical solver checked differentially against it, and a closed-form occupancy model checked against both measured dimensions. The speedups are engineering evidence for the formulation, measured at finite dimension on one processor; the paper makes no claim about Subset Sum exponents. The improvement comes from changing when detailed candidate work occurs, not from weakening the mathematical decision.

## 2 The root reporting problem

### 2.1 Subset Sum and ternary representations

For positive integer weights $a_1,\ldots,a_n$ and target $T$, integer Subset Sum asks for a binary vector $z$ satisfying $\sum_i a_i z_i=T$. The evaluated implementation uses the balanced modular variant: $\sum_i a_i z_i\equiv T\pmod {2^n}$ with $\sum_i z_i=n/2$. Its final verifier establishes this modular relation and balance, rather than unreduced integer equality. A ternary intermediate vector $e\in\{-1,0,1\}^n$ has residue $R(e)=\sum_i a_i e_i\pmod {2^n}$. A valid merge must satisfy both its arithmetic relation and its required digit distribution and coordinate constraints.

The workloads studied here are captured immediately before a depth-three TER root merge. They contain complete level-one lists produced by the native construction, rather than lists reduced for a pipeline fixture. Capturing a pre-root state establishes its origin; it does not establish that this attempt contains a solution. The term TER² is used by the implementation to identify this root-backend research line. We use TER for the published representation method and do not treat TER² as a separate published complexity result.

[FIGURE:topology]

### 2.2 Exact quotient reduction

Let $A,B,C$ be the three level-one lists. Each entry retains its original identity, vector, and residue. Suppose the inherited modulus is $2^{r_1}$ and each list has a fixed child target $c_A,c_B,c_C$. Valid prepared inputs satisfy $R_L\equiv c_L\pmod {2^{r_1}}$ for every entry in list $L$, and $T\equiv c_A+c_B+c_C\pmod {2^{r_1}}$. Define $Q=n-r_1$ and

$$
q_L=\frac{(R_L-c_L)\bmod 2^n}{2^{r_1}},\qquad
\tau=\frac{(T-c_A-c_B-c_C)\bmod 2^n}{2^{r_1}}.
$$

The divisions are exact because of the inherited alignment. Quotients lie in $\mathbb Z/2^Q\mathbb Z$. With $d_C=\tau-q_C\pmod {2^Q}$, the root arithmetic becomes

$$
q_A+q_B\equiv d_C\pmod {2^Q}. \tag{1}
$$

This reduction is target-aware. Shifting an unadjusted residue would not be an equivalent definition. Equation (1) follows by subtracting the three child targets from the original root congruence and dividing the resulting multiple of $2^{r_1}$.

### 2.3 Family cells and retained key widths

The low six quotient bits partition each source and target list into sixty-four families. For a source-family pair $(s,t)$, define $u=(s+t)\bmod 64$ and $\kappa=(s+t-u)/64$, the carry out of the six-bit sum. The target family is $u$, and the family carry is $\kappa\in\{0,1\}$. There are 4,096 family cells, called microproblems below.

Let $F=\min(Q,64)$ and $W=F-6$. Within a cell, write the low-$F$ source and target keys as $s+64r_A$, $t+64r_B$, and $u+64r_Y$. Equation (1) then requires

$$
r_A+r_B+\kappa\equiv r_Y\pmod {2^W}. \tag{2}
$$

If $Q>64$, the additional $Q-F$ bits must also match, with the carry out of the low-$F$ sum. They are retained separately. In the measured dimension-72 roots, $(r_1,Q,W)=(25,47,41)$; in the dimension-108 roots they are $(39,69,58)$. The latter therefore require five bits beyond the low sixty-four. No equality test may discard those bits.

Arithmetic equality is still only one level of acceptance. The common authority recomputes original residue equality modulo $2^n$ and applies an attached coordinate-closure and digit-count predicate. Original identities are preserved even when values coincide. Reporting arithmetic triples, satisfying that attached predicate, deduplicating parent vectors, and verifying a final binary witness are distinct operations. The principal exactness claim in this paper is full quotient and original-residue reporting, with the residue-exact identity multiset used as a differential gate.

The benchmark's attached structural predicate is a finite test-parent predicate, not the production binary-root predicate. Its negative, zero, and positive counts are $(2,56,14)$ at dimension 72 and $(2,86,20)$ at dimension 108; production roots require $(0,n/2,n/2)$. Authentic level-one vectors each have signed digit sum $n/6$, so a triple has signed digit sum $n/2$, whereas the attached predicate requires $n/6$. Authentic triples therefore cannot satisfy that predicate. Structural counters in the experiments are reported only with this meaning. This boundary does not change the arithmetic join being timed, but it rules out interpreting those counters as production-root certification. The integrated solve path of Section 8.7 does not use the attached predicate: it accepts a residue-exact triple only when the digit sum of its three entries is binary with weight $n/2$, the production rule, and then verifies every resulting parent against the original instance.

## 3 Baselines and the dense candidate problem

### 3.1 The projected tile-sweep reporter

The baseline, K2, evaluates source pairs in eight-by-eight tiles. It retains a nineteen-bit residual projection after the six family bits. For a projected sum $v$, its vector kernel tests a coarse bitmap whose bit represents a block of eight possible residual values. A passing lane is drained to scalar code, which recomputes $v$, performs exact membership in the nineteen-bit target bitmap, and expands every original target posting with that projected value. The shared sink then performs the remaining arithmetic and structural checks.

This is an exact arithmetic reporter when the downstream authorities are attached. Its early test is a necessary condition, not the final mathematical decision. The baseline is useful precisely because it preserves the original identity and multiplicity contract while exposing the cost of a short projection. Each target family uses a 64 KiB exact bitmap and an 8 KiB coarse bitmap. Across sixty-four families those two bitmap classes occupy 4.5 MiB, excluding postings, indexes, and authority data.

When large families populate the nineteen-bit residual space densely, the projected match rate can be substantial even though equality across forty-seven or sixty-nine quotient bits is rare. Every projected match invokes target-posting recovery and detailed downstream work. The bottleneck is not evidence that the vector kernel is poorly implemented; it is a consequence of the information retained by the early projection and the point at which discarded information is restored.

### 3.2 A stronger full-width baseline

K2-WIDE uses the same tile sweep and projected target directory, but compares the full retained residual and any extra high bits before calling the shared sink. It therefore tests a narrower hypothesis than the original baseline comparison: are gains explained solely by avoiding sink calls for low-width false candidates? K2-WIDE removes many such calls, yet still visits the target postings in each surviving projected group and retains the original coarse pair-processing path.

The wide residual addition is safe in the supported domain because $W\leq58$; two residuals and one carry fit in fifty-nine bits. The carry out of the $W$-bit result is added to the separately stored high quotient parts. Both K2-WIDE and ARWLF use this full-width comparison. Their reported acceptance therefore agrees at the same downstream authority level even though early candidate counters differ from K2's.

### 3.3 The queued sparse reporter

QUEUE-DENSE splits the nineteen-bit projection into a thirteen-bit position and a six-bit tag. For one source query it intersects translated target-position occupancy with source-position occupancy, compacts nonzero bitmap words, then resolves tags by a carry-aware cyclic rotation of sixty-four-bit tag masks. Group directories expand the matching relation to original postings. Full-width residue and structural decisions remain downstream.

The position space is fixed at 8,192 slots. The implementation's distinct-residual and hit capacity is 65,536 per relevant family; the dispatcher checks that capacity before admitting the reporter. The dimension-72 roots satisfy it, while the evaluated dimension-108 roots do not. QUEUE-DENSE is thus an informative legal alternative in the sparse regimes, rather than a missing timing entry that can be treated as zero runtime.

## 4 Wide leaf filtering

### 4.1 Terminal partition and carry routing

ARWLF partitions each family once, by the low thirteen residual bits. Let $H=2^{13}$. Write $r=\ell+Hm+\text{higher terms}$, where $\ell\in[0,H)$ is the leaf digit and $m$ contains the next $b=\min(32,W-13)$ bits. Each family retains leaf offsets, contiguous mids, and posting indexes back to the full keys and original identities. Target leaf $k$ stores four independent 256-bit occupancy maps, one per byte position of the mid.

For source leaves $i,j$ in a cell with carry $\kappa$, define

$$
k=(i+j+\kappa)\bmod H,\qquad
c=(i+j+\kappa-k)/H. \tag{3}
$$

For fixed $k$ and $i$, the only compatible partner is $j=(k-i-\kappa)\bmod H$. The carry is zero when $i\leq k-\kappa$ and one otherwise. This makes leaf-sorted source rows two contiguous carry ranges. When $k=0$ and $\kappa=1$, the zero-carry range is empty; all rows belong to the carry-one range. Keeping this seam explicit avoids an unsigned-boundary shortcut that would change the mathematics.

The schedule visits only nonempty target leaves. A pair routed to an empty target leaf cannot satisfy Equation (2), so it needs no byte test. Within a nonempty target leaf, all assigned source rows run consecutively. The target summary stays fixed throughout that sweep.

[FIGURE:pipeline]

### 4.2 Independent byte occupancy

For target leaf $k$ and byte position $g$, define $B_{k,g}$ to contain every byte value occurring at position $g$ in a target mid of that leaf. For a routed pair, compute

$$
m_*=(m_A+m_B+c)\bmod 2^b. \tag{4}
$$

The pair passes the wide leaf filter if and only if every checked byte of $m_*$ belongs to the corresponding $B_{k,g}$. A true target equality contributes all of its bytes to those maps, so it cannot be rejected. The converse does not hold. Byte values may be present because of different target entries. ARWLF intentionally keeps these false positives and resolves them later.

The default checks all available mid bits: four bytes at $b=32$, and four chunks with a restricted last chunk at $b=28$. If a diagnostic configuration declares a smaller checked width, only complete bytes wholly inside that width are used; a partial last byte is safe when the entire available mid is declared, since the unused high bits are zero. The final full-key comparison always remains necessary. The reported timing campaign uses the default full mid width.

An elementary occupancy model explains why this test is attractive without making a distributional assumption part of correctness. Suppose a leaf contains $d$ independently sampled target mids, with $h$ independent uniform full bytes in each draw, and the query mid is independent of all target draws. Then

$$
\Pr[\text{byte filter passes}]=\left(1-\left(\frac{255}{256}\right)^d\right)^h. \tag{5}
$$

For one byte, the absence probability is $(255/256)^d$. Under the stated model the target draws for distinct byte positions are independent, so the per-byte presence probabilities multiply. Equation (5) is an explanatory model, not a measured false-positive rate or a guarantee for TER data. Correlation, duplicates, partial bytes, and constructed targets can change the observed rate. In particular, increasing target occupancy makes the maps saturate and weakens rejection.

### 4.3 One source row and sixteen lanes

The optimized operation pairs one $A$ entry with sixteen $B$ entries in its compatible leaf. The low-sixteen-bit mids form one leaf-sorted dense array with sixteen global tail-padding entries. A short leaf load may also read entries from a neighboring leaf; a precomputed valid mask excludes every lane outside the current leaf. The $A$ preparation stores the two low-sixteen-bit addends for carry zero and carry one, each duplicated across the two sixteen-bit halves of a thirty-two-bit broadcast.

One AVX2 addition forms sixteen mid sums modulo $2^{16}$. Byte lookup tables represent the first two target occupancy maps in a shuffle-friendly layout. Lane-local byte shuffles retrieve membership bytes, bit tests identify missing values, and a byte movemask summarizes failures. The two byte failures for lane $i$ are combined; bit $2i$ of the resulting mask is set exactly when both bytes pass and the lane is valid. A row tentatively writes its source index and pass mask to contiguous pending scratch, advancing the logical length only when at least one lane survives.

The pending masks are drained outside the flat row loop. Full thirty-two-bit mids are reconstructed for surviving lanes, and any remaining byte chunks are checked there. Leaves containing more than sixteen $B$ entries receive additional sixteen-lane blocks in a separate overflow pass, with a valid mask on the final block. Consequently, the phrase sixteen-by-sixteen packet describes an occupancy accounting unit, not a machine instruction that computes 256 sums at once. The hardware operation is one row against sixteen lanes.

### 4.4 Exact recovery and multiplicity

Pairs surviving all byte maps are buffered for one target leaf. Each is compared with every target posting in that leaf. The resolver first checks the declared mid projection, then all $W$ residual bits, then the additional $Q-F$ high bits with the propagated carry. Only a full quotient match reaches the common sink.

The sink independently checks a low-thirty-two-bit quotient relation, recomputes equality of the original residue sum and root target modulo $2^n$, and applies the attached vector predicate described in Section 2.3. A residue-exact triple can fail that predicate. The residue-exact multiset used for differential comparison is captured before the structural decision; it is not a list of verified binary Subset Sum solutions. For constructed records, even consistency between coordinate data and stored residues is an additional property that these checks do not establish.

Every posting remains distinct. If equal-valued entries have different identities, the resolver enumerates their separate matches. The reporter performs no canonical-parent deduplication. This is necessary both for equivalence with the baselines and for charging output-heavy cases correctly.

### 4.5 Semantic algorithm

Algorithm 1 abstracts the row intrinsics while retaining the executed routing and authority semantics. VECTOR-BYTE-TEST returns precisely the valid lanes passing the first two byte maps; it makes no full-equality claim.

```text
Algorithm 1  ARWLF on a selected family cell
Prepare leaf partitions and target byte occupancy maps.
For each nonempty target leaf k in ascending order:
    Clear this leaf's survivor buffer.
    Split A rows into carry-zero and carry-one ranges.
    For each carry range:
        Clear pending row scratch for this range.
        For each source row a in that range:
            j <- (k - leaf(a) - family_carry) mod 8192
            mask <- VECTOR-BYTE-TEST(a, first 16 B_j lanes, carry)
            Record nonzero masks in pending row scratch.
        Drain pending masks using this range's carry and test remaining mid bytes.
    Sweep the additional B_j blocks for overflow leaves.
    For each surviving original source pair (a,b):
        For every target posting y in leaf k:
            If declared mid bits and all quotient bits match:
                Emit the original identity triple to the authority.
```

Preparation is lazy by family and cached within one backend call. The semantic algorithm assumes full target-leaf coverage and successful completion. Prefix probes and an aborted guarded execution do not constitute complete reporting.

## 5 Correctness

**Lemma 1 (unique compatible routing).** For a selected family cell, each source pair belongs to exactly one leaf relation in Equation (3). Any target satisfying Equation (2) belongs to that target leaf, with the stated carry.

**Proof.** Euclidean division of $i+j+\kappa$ by $H$ uniquely determines remainder $k$ and quotient $c$. Since $0\leq i,j<H$ and $\kappa\leq1$, the quotient is zero or one. Solving for $j$ gives $(k-i-\kappa)\bmod H$. If $k-i-\kappa\geq0$, no wrap is needed and $c=0$; otherwise the representative adds $H$ and $c=1$. This proves the two-range rule, including the empty zero-carry range at $k=0,\kappa=1$. Reducing Equation (2) modulo $H$ forces exactly this leaf relation.

**Lemma 2 (safe byte rejection).** Under valid preparation, the byte filter rejects no pair that participates in a full quotient match.

**Proof.** A full match implies Equation (2). After the leaf equality has been accounted for, the next $b$ bits are Equation (4). The matching target's mid therefore has every checked byte of $m_*$. Preparation sets each corresponding occupancy bit, so every checked membership test succeeds. Restricting the filter to complete declared byte chunks can only weaken rejection. The restricted final byte of a full-width mid is equal on both sides because its unused bits are zero.

**Lemma 3 (vector coverage).** The row and overflow sweeps evaluate every source pair routed to a visited target leaf, without allowing padding to become a survivor.

**Proof.** Leaf partitioning is a stable permutation of source postings. Every $A$ posting occurs in exactly one carry range for a given $k$. Its unique partner leaf is traversed in blocks of sixteen entries: the main block covers the first sixteen and overflow blocks cover the disjoint remainder. The lane-valid mask removes positions beyond the actual final block. The first two byte predicates operate on modular sixteen-bit sums, exactly the low sixteen bits of Equation (4). The survivor path reconstructs the wider sum before checking later bytes. Together these operations implement the scalar byte predicate for every actual assigned pair.

**Theorem 1 (exact quotient reporting and authority preservation).** Assume valid aligned prepared input, recoverable original postings and supported indexes, supported keys with $35\leq Q\leq69$, a reset output sink, coverage of every nonempty target leaf (no probe prefix), successful allocation, and a completed ARWLF execution. Then, for every selected cell, ARWLF emits exactly the original identity triples satisfying Equation (1), with posting multiplicity preserved. When correctly populated residue and vector tables are attached, the sink identifies exactly the emitted subset satisfying original stored-residue equality and its attached closure and digit-count predicate.

**Proof.** A full quotient match has its unique compatible leaf by Lemma 1, and that leaf is nonempty. Lemmas 2 and 3 ensure that its pair survives to resolution. Resolution traverses every original target posting in the leaf, so the matching identity triple is considered. It compares all $W$ residual bits and, where present, the high quotient bits with the carry, hence emits the match. Conversely, emission requires those same equalities, which reconstruct Equation (1); no byte-only match is emitted. Posting indexes recover each original identity without coalescing equal values. The shared authority applies its exact predicates independently to every emitted triple, giving the final statement.

The theorem is conditional on successful full execution. A reduced prefix may return a completed prefix status yet omit undeclared leaves; an aborted or inapplicable call is not a negative result. When a guarded dispatcher retries another backend, it clears partial output before executing the fallback. Neither a completed empty selected-cell report nor exhaustion of a TER attempt proves global UNSAT.

## 6 Cost and storage

### 6.1 Row work and occupancy

Fix one family cell with source families $A_s,B_t$, target family $Y_u$ and family carry $\kappa$. Let $\alpha_i$, $\beta_j$ and $\gamma_k$ be the posting counts of leaf $i$ of $A_s$, leaf $j$ of $B_t$ and leaf $k$ of $Y_u$, and let $K^+=\{k:\gamma_k>0\}$ be the set of nonempty target leaves. The logical source-pair mass is $P=|A_s||B_t|$. The routed mass requiring byte tests is

$$
P_+=\sum_{k\in K^+}\sum_i \alpha_i\,\beta_{(k-i-\kappa)\bmod H}\leq P.
$$

Pairs assigned to empty target leaves are removed by routing. They are still included in the reported total logical pair mass $P$, so nanoseconds per logical pair is an amortized accounting statistic, not the latency of an individual lane.

The main vector sweep executes $|K^+||A_s|$ rows, including rows whose partner slot is empty. Let $\operatorname{blk}(x)$ be the number of sixteen-entry blocks needed for $x$ entries, the least integer $b$ with $16b\geq x$. The overflow contribution is

$$
R_{\rm over}=\sum_{k\in K^+}\sum_i \alpha_i\max\left(0,\operatorname{blk}(\beta_{(k-i-\kappa)\bmod H})-1\right).
$$

Thus $R=|K^+||A_s|+R_{\rm over}$ is the number of physical sixteen-lane tests. In densely populated target families, $P_+$ approaches $P$ and much of each row is useful. Sparse partner leaves waste lanes, while the main-row traversal remains. Fixed vector width changes the constant cost of a covered pair; it does not make the dense pair term subquadratic.

The census also counts conceptual sixteen-by-sixteen packets over nonempty compatible leaf triples:

$$
\Pi=\sum_{k\in K^+}\sum_{i:\beta_j>0}\operatorname{blk}(\alpha_i)\,\operatorname{blk}(\beta_j), \tag{6}
$$

where $j=(k-i-\kappa)\bmod H$, and define $L=P_+/\Pi$. For nonempty work, $1\leq L\leq256$. This occupancy measure predicts amortization, but is not itself a vector throughput measurement. The dispatcher uses a related estimate formed from per-family packet totals, with a conservative admission threshold. Observed $L$ in the plots comes from the exact census.

### 6.2 Survivor and output work

Let $S_k$ be the number of source pairs surviving all checked bytes for target leaf $k$, $V$ the number passing the first two bytes, and $E$ the number of full quotient triples emitted. On fixed-width hardware a useful implementation cost expression is

$$
T_{\rm work}=O\left(N+fH+R+B_{\rm over}+V+\sum_k S_k\gamma_k+E\,C_{\rm auth}\right). \tag{7}
$$

The overflow loop also incurs $B_{\rm over}=|K^+|\sum_j\max(\operatorname{blk}(\beta_j)-1,0)$ block traversals, including blocks whose compatible $A$ leaf is empty. Here $N$ is the total posting count in touched families, $f$ their number, and $C_{\rm auth}$ the cost of one original-residue and attached-predicate check. The $V$ term accounts for draining initial masks and checking later bytes. The resolver term is explicit because each surviving pair scans every target posting in its leaf. For the favorable measured dense regime it is small relative to the pair sweep. In an adversarial single-leaf distribution with saturated byte maps, the resolver can take $\Theta(|A_s||B_t||Y_u|)$ work. The method therefore has no unconditional quadratic total-time bound for arbitrary reporting inputs.

An output-sensitive cost cannot be removed by filtering: every emitted identity triple must be recovered, and vector validation scales with the dimension. Equal-valued postings can multiply $E$ even when the number of distinct parent vectors is small. No complexity claim in this paper silently replaces posting multiplicity with unique-parent count.

### 6.3 Memory

Target summaries require $4\cdot256$ bits, or 128 bytes, per leaf. Allocating all 8,192 summaries uses 1 MiB per touched target family and at most 64 MiB for all sixty-four. Source partitions, row addends, lane slots, full keys, and the pending mask array add storage linear in their prepared entries plus fixed per-family leaf directories.

ARWLF avoids a complete table of all source-pair sums. It nevertheless stores the pairs surviving one target leaf, and that vector retains its capacity across leaves. With $S_{\max}=\max_k S_k$, its auxiliary storage is $O(N+fH+S_{\max})$, in addition to shared authority data and any retained outputs. $S_{\max}$ can be quadratic in source-family sizes. A small measured cache-size counter would not establish an unconditional linear-memory bound or a process peak bound.

### 6.4 Adaptive selection

The frozen dispatcher considers QUEUE-DENSE, ARWLF, and K2-WIDE. It checks the queued reporter's capacity, admits ARWLF only when its width and packet-amortization rules hold, and probes the legal candidates on a common selected subset. K2-WIDE probes a prefix of source blocks, while ARWLF probes a prefix of nonempty target leaves. Predictions scale preparation and sweep work separately. The smallest finite predicted time determines the backend; ties retain a fixed order.

A selected ARWLF run can be guarded against the runner-up's predicted break-even time per logical pair. After a sufficient prefix, an unfavorable estimate aborts it and the runner-up starts from cleared output. This is a practical heuristic, not a universal regret theorem. Probe overhead matters most when the selected problem is tiny, and the admission threshold can exclude profitable cases. Those effects are part of the results rather than hidden by reporting only the backend ultimately selected.

## 7 Experimental design

### 7.1 Workloads and independent units

The main campaign captures ten authentic pre-root states: six at dimension 108 and four at dimension 72. Three dimension-108 seeds, 0x1001, 0x2003, and 0x3007, are calibration roots that influenced development. Seeds 0x4001, 0x5003, and 0x6007 are campaign holdout roots captured after the preregistration was frozen. At dimension 72 there is one calibration anchor and three holdout roots, seeds 0x7201, 0x7203, and 0x7207. Original instance targets are planted; the term authentic describes the complete native list construction, not an unplanted random-target solver trial.

Each root receives a census of all 4,096 cells. An anchor cell is selected by a fixed deterministic shuffle. Additional cells are selected by preregistered strata in pair mass and packet occupancy. After the census but before timing, an amendment restricts dimension-108 strata to the four corners of the original grid, because those roots are internally nearly uniform and the full timing sweep is expensive. Dimension-72 roots retain the complete stratified selection. A corner that coincides with the anchor is not timed twice. This gives fourteen authentic dimension-108 calibration cells and fifteen holdout cells.

Cells from the same root share the input instance and list construction. They are not independent instance replications. Aggregate speedups are consequently accompanied by a root-block bootstrap. The three holdout roots are the independent holdout units; fifteen cells do not establish generalization to fifteen independently generated instances.

### 7.2 Controlled geometry and target density

Source thinning retains entries of $A_s$ and $B_t$ by a deterministic hash of root, cell, list tag, original identity, and thinning seed. It leaves keys and attached authority unchanged. Three thinning seeds are used per calibration root. Retention is selected by an untimed census bisection to reach requested occupancies from $L=1$ through $96$, with unreachable targets recorded explicitly. These are controlled reductions of dimension-108 source families, not authentic TER instances at smaller dimensions.

Target thinning retains only entries of $Y_u$ at fractions one-half, one-quarter, and one-eighth, with source families untouched. It isolates the effect of target occupancy and projected candidate recovery. Comparisons use the same transformed input for all backends. Neither transformation is included in the authentic headline.

### 7.3 Machine, protocol, and timed boundary

Measurements use an Intel Core Ultra 9 275HX, pinned to P-core logical CPU 23, on AC power with EcoQoS disabled. The program is C++20, compiled in Release mode with MSVC 19.51 (recorded identifier 195136257). Optimization uses /O2 and /Ob2 with native AVX2 translation units. All backends execute single-threaded on identical prepared inputs.

Every case begins with an untimed exactness pass. The final protocol uses three warmup rounds and seven timed rounds; the discovery protocol uses one warmup and three timed rounds. Backend order rotates by round and reverses on odd rounds. Anchors, dimension-72 whole roots, and crossover confirmations use the final protocol. Strata and most thinning cases use discovery. The pooled authentic headline therefore combines these two declared protocols; the final-protocol anchors are also reported separately.

Timing includes the backend call, its lazy internal preparation, survivor resolution, shared sink processing, and dispatcher probes where applicable. Snapshot loading, common dataset conversion, full-key and common-index preparation, exactness passes, census, and post-call witness sorting are outside the prepared-backend comparison. A repeated pass rebuilds ARWLF's per-call cache; it does not reuse an uncharged persistent leaf index. Prepared-backend timings do not include complete TER list generation.

A CPU sentinel records machine conditions at process boundaries and during the campaign. Some resumed source-thinning points have slower sentinel readings; their matched ratios are retained. No accepted sample is recorded off the pinned core. This is one processor and one implementation toolchain, not a hardware-portability study.

### 7.4 Exactness gates and statistics

For each timed case, sorted residue-exact identity multisets agree with the K2 reference. Intermediate width-count and hash gates compare ARWLF with K2-WIDE and K2's narrower checks; a separately computed census agrees with counted geometry. Structural counts, identity hashes, directory misses, and residual mismatches are checked at their declared levels. Synthetic tests add whole-value modular brute-force comparisons and scalar/vector filter comparisons, including carry seams and overflow leaves.

All 221 completed campaign cases pass the recorded gates. Three source-thinning targets are unreachable, leaving 224 declared cases. The accepted timing set contains 3,220 samples; nine rows from incomplete cases are excluded from summaries and retained as partial evidence. All authentic and thinned campaign cases have zero full-quotient matches, residue-exact triples, and structural outputs. Agreement at those output gates is therefore supplemented by intermediate counts, independent bounded tests, and the constructed positive experiments below. Zero-output agreement alone would not validate recovery.

For case $i$, speedup is the median K2 time divided by the median candidate time. Multiplicative results aggregate as $G=\exp(\frac1m\sum_i\log S_i)$. Percentile bootstrap intervals use 10,000 resamples with fixed seed 0xB007. The root-block bootstrap resamples whole root clusters and retains their cells together. With only three roots per dimension-108 role, these intervals characterize the observed design and are not broad population guarantees. Dispatcher regret is its median time divided by the minimum median among applicable exact reporters, including K2.

### 7.5 Constructed outputs and full prepared roots

A separate experiment appends controlled target records to one authentic cell. For a selected authentic source pair, a constructed target has the full key and residue needed for arithmetic equality, and coordinate data chosen so the summed parent passes the attached finite test-parent predicate. A decoy condition appends the same number of target records without valid final outputs. The original source families and authentic target records are retained.

These appended records have ternary coordinate data but are not established members of the native level-one TER sets. Their stored residues and coordinate completions are assigned separately; equality with the coordinate dot product of the original weights is not established. The completion targets the attached finite test-parent predicate, not the binary production root. They test arithmetic filtering and identity recovery under prescribed output loads, and do not create authentic solver solutions. The completed repeated subset available for this paper comprises one calibration root and three samples per case. Other scheduled roots are not included as if their runs had finished.

The complete dimension-108 prepared-root experiment uses seed 0x1001, all 4,096 cells, and one constructed positive target. ARWLF and the dispatcher each run in one call, retaining their within-call family caches. The K2 run is checkpointed as 64 calls, one per target family, in the family-major order a single call would use; all 64 completed. ARWLF and the dispatcher run once before and once after K2 to bracket machine drift. Every run is a single measured pass, without the campaign's warmup and repeated rounds.

## 8 Results

### 8.1 Dense authentic microproblems

Table 1 reports the six final-protocol anchors in seconds, allowing the ratios to be checked directly. The source-pair mass is approximately $6.1\cdot10^9$ for seed 0x1001 and $1.2\cdot10^{10}$ for the other roots. ARWLF takes 0.842 to 1.679 seconds per anchor; K2 takes 44.558 to 131.159 seconds. The largest ratio occurs because K2 is slow on the 0x2003 anchor. It is retained, but it does not define the headline by itself.

[TABLE:anchors]

Across all declared authentic cells, the calibration geometric mean is 53.0875 and the holdout mean is 52.8933 (Table 2), with root-block 95 percent bootstrap intervals of 48.17 to 57.61 and 51.81 to 53.81. The similar holdout gain supports the dense operating regime on the three additional roots. It does not remove dependence among cells or establish results at other dimensions.

[TABLE:aggregates]

Table 3 reports each root separately. Calibration roots give 48.17, 57.61 and 52.88 and holdout roots 51.81, 53.81 and 53.07 (K2 over ARWLF). Every individual root exceeds a 48-fold gain, and no single root drives the pooled value. With only three roots per role, the 2.5th and 97.5th bootstrap percentiles are attained by resampling one root three times, so each interval above is exactly the range of its three root values and should be read as that range, not as a population estimate. The lowest root, 0x1001, has the smallest level-one lists and the lowest occupancy ($L\approx78$ against about 126), and the highest, 0x2003, contains the slow K2 anchor noted above.

[TABLE:per_root]

K2-WIDE improves on K2 by about 1.62 to 1.63 in the pooled sets. ARWLF remains faster than K2-WIDE by geometric means of 32.78 on calibration cells and 32.44 on holdout cells. Moving full-width comparison before the shared sink is beneficial, but it does not explain the full ARWLF result. ARWLF also changes the inexpensive pair test and suppresses target-posting visits before the exact resolver.

A straightforward general-purpose exact join is slower than both K2 baselines on the cell where it was measured. In a separate run on the 0x1001 anchor cell (one warmup and three timed rounds per backend; Table 1 notes), an untuned scalar hash join that keys a linear-probing table on the $W$-bit residual and checks the high bits of each match takes 72.67 seconds including its table build (11.83 ns per logical pair), against 47.93 seconds for K2, 27.47 for K2-WIDE and 0.890 for ARWLF in the same run. In every round each backend reports the same identity multiset as an untimed K2-WIDE reference; this cell has no full-quotient matches, so the check establishes only that no backend reports a false match. The hash join is thus about 82 times slower than ARWLF and 1.5 times slower than K2 on this cell. This is one cell of one root with three rounds, and the join uses no partitioning, prefetching or vectorization, so it shows what a straightforward exact join costs here, not what a tuned join could reach. The run's medians for K2, K2-WIDE and ARWLF are 7.6, 1.0 and 5.7 percent above the Table 1 values for the same cell, so only ratios within one run should be compared.

The authentic dimension-108 census has $L$ near 78 for the first calibration root and near 126 for the remaining roots. Within a root, cell pair mass and occupancy vary little. Figure 3 shows all twenty-nine individual speedups rather than only their means. The spread includes slow baseline cases and a holdout ARWLF timing outlier; no case is removed for being inconvenient.

[FIGURE:dense]

### 8.2 Occupancy crossover

Controlled source thinning changes the amortization without changing the quotient width or target family. At $L=1$, ARWLF loses: the rows and preparation are poorly amortized. At $L=2$, it wins on all three calibration roots, with discovery-protocol speedups over K2 ranging from 4.4 to 6.7 and gains over the fastest applicable rival of about 2.6 to 3.3. Final-protocol confirmations at the two bracket points reproduce a loss at one and a gain of approximately 2.7 to 3.4 over the fastest rival at two.

[FIGURE:crossover]

As source occupancy increases, ARWLF's advantage grows. This is a finite-regime crossover observed under this target geometry, rather than a universal threshold for arbitrary key distributions. Equation (7) explains the dependence: sparse blocks spend vector work on few real lanes, while dense blocks amortize the same row operation. Target selectivity, duplicated keys, and output load can move the crossover independently of $L$.

### 8.3 Target-density ablation

Figure 5 holds source pairs fixed and thins only the target family. Reducing target density accelerates K2 substantially, as fewer projected matches require posting expansion and downstream work. ARWLF also becomes faster, but its decrease is smaller. For seed 0x1001, the median K2 time falls from 44.56 seconds at full density to 8.31 seconds at one-eighth density, while ARWLF falls from 0.842 to 0.621 seconds. The corresponding speedup falls from 52.89 to 13.38.

[FIGURE:target]

The result supports a mechanism involving both cheap physical pair processing and early suppression of projected candidates. It does not isolate an exact causal percentage for either. Removing target entries changes byte occupancy, posting lists, and the set of nonempty leaves together. The experiment is consequently a controlled sensitivity study, not a decomposition into independently additive instruction costs.

### 8.4 Sparse complete roots and dispatcher behavior

The dimension-72 roots have $L$ near one. Table 4 reports complete prepared-root timings under the final repeated protocol. QUEUE-DENSE is faster than K2 by factors of 1.21 to 1.84; ARWLF is slower than K2 by factors of approximately 3.4 to 7.0. The dispatcher runs QUEUE-DENSE on all four roots, with regret between 1.039 and 1.140. On three dimension-72 solver attempts QUEUE-DENSE is also faster than the classical solver's own root (Section 8.7).

[TABLE:sparse]

On all twenty-nine authentic dimension-108 cells, the dispatcher runs ARWLF. Its median regret is 1.010, its 95th percentile is 1.020, and its maximum is 1.026. This is favorable measured selection behavior in the dense regime. It coexists with two clear failures elsewhere. On tiny dimension-72 cells the fixed probe cost dominates roughly 0.1 ms of useful work, producing regret as high as 172.9. On source-thinned dimension-108 cases with observed occupancy from two through sixteen, the packet admission rule excludes ARWLF even when it is several times faster than the alternatives. At occupancy sixteen, maximum regret is 17.6.

The frozen dispatcher is therefore useful at the evaluated large-root scopes, but cannot be described as universally regret-bounded. Backend capability, admission policy, prediction quality, and probing cost must be evaluated separately. This paper reports the policy as measured rather than retrospectively changing the threshold to make the curve look better.

### 8.5 Constructed positive outputs

Table 5 reports the completed one-root constructed-output experiment. Each point has one warmup and three matched timed samples. At each appended count, the planted case and its decoy twin have the same target-list growth. The comparison exposes output and target-density effects jointly; it is not an authentic TER scaling series.

[TABLE:positive]

All prescribed planted identities are recovered and satisfy the attached finite predicate, while decoys produce no residue-exact or structural outputs. As the target leaves grow, their byte maps become less selective and the survivor resolver has more target postings to scan. At one million appended entries, ARWLF takes 20.486 seconds in the positive condition and 19.458 seconds in the decoy condition. Much of the slowdown is therefore present without emitted outputs and is consistent with target/filter saturation; it cannot be charged entirely to per-output emission. These measurements demonstrate positive arithmetic recovery on the constructed contract. They neither establish the natural distribution of TER solutions nor validate a complete solver on the added targets.

### 8.6 A complete dimension-108 prepared root

Table 6 reports every backend over all 4,096 cells of root 0x1001, 25,189,929,333,072 logical pairs with one constructed positive target. K2 takes 197,243 seconds (54.79 hours). The two ARWLF passes take 3,575.5 and 3,469.8 seconds, and the two dispatcher passes, which select ARWLF, take 3,557.1 and 3,436.1 seconds. The measured whole-root ratios are therefore 55.2 and 56.8 for ARWLF and 55.5 and 57.4 for the dispatcher. All five runs report the same single residue-exact identity triple, and K2 reports it only in the target family that holds the anchor cell. Process peaks are about 3,279 MiB.

[TABLE:whole]

The whole-root ratio agrees with the 53-fold cell-level result, and the gain persists when all cells are covered: per logical pair, K2 costs 7.83 ns and ARWLF 0.138 to 0.142 ns. The K2 run spanned about two and a quarter days of ordinary machine use, and no interference log was recorded. Its 64 chunk times range from 2,694 to 4,959 seconds. As an explicitly labelled estimate, charging every chunk the median of the faster half of the chunks (2,791 seconds) gives about 49.6 hours, or a 50.0 to 52.0-fold ratio. The measured ratios are the reported result; the estimate is a heuristic sensitivity scenario, not an unloaded measurement or a bound on interference. The ARWLF passes after K2 are 3 to 4 percent faster than those before it, and the first ARWLF pass is known to have run beside background processes. These are single passes, so no dispersion estimate is available.

### 8.7 Staged integration and differential validation

The reporters above run on prepared roots. To test the method with the solver, each TER attempt is composed from existing components rather than by replacing the root inside the solver's in-process call: the classical solver's staged list construction streams the attempt's three level-one lists to a checksummed snapshot; the frozen dispatcher reports the root; residue-exact triples are accepted by the production binary-root rule; parents are rebuilt from the raw records independently of the backend's tables; and every parent is verified against the original instance by the solver's own witness verifier. A disagreement anywhere is an invariant violation, and exhausting the attempt budget is reported as such, never as unsatisfiability.

[TABLE:integration]

Table 7 summarizes the evidence. A differential test runs the classical solver, whose root uses the production design, and the integrated path with the dispatcher and with ARWLF forced, on 33 dimension-72 attempts: four seeds with eight attempts each, plus attempt 21 of seed 0x7201, the first attempt at which the classical solver finds a witness. Outcomes and accepted-triple counts agree on all 33 attempts, and on the found attempt the classical witness is among the verified parents. The complete attempt loop then solves the balanced modular instance of seed 0x7201 at dimension 72, whose random weights accompany a structured planted support: attempts 0 to 20 end without a witness and attempt 21 yields one verified parent, the planted solution. At dimension 72 the dispatcher selects QUEUE-DENSE, as Section 8.4 predicts. The same attempt loop was therefore repeated with ARWLF forced on every attempt: all 22 attempts produce the same outcome and accepted-triple count as the dispatcher run, and attempt 21 yields the identical verified parent. ARWLF's root takes 1.67 to 1.95 seconds per attempt here, against 0.23 to 0.46 seconds for QUEUE-DENSE, the sparse-regime loss of Section 8.4; that run was unpinned beside the dimension-108 solve described next, so its times are indicative only. These runs establish that a witness found through ARWLF survives independent verification, not a full-solver speedup.

The classical solver's own root, design B, was also timed against the staged backends on identical attempts (Table 7). Design B is an exact streaming three-list reporter that tiles the quotient residue space into cache-sized windows; these tiles are unrelated to K2's eight-by-eight source-pair tiles. The attempts are 0 and 10 of seed 0x7201 and 0 of seed 0x7203, which end without a witness, and the found attempt 21 of seed 0x7201. The run checks that both sides see the same level-one lists. On the three attempts without a witness, design B takes 583 to 1,008 ms per root, QUEUE-DENSE 290 to 317 ms, K2 285 to 487 ms and ARWLF 1,724 to 1,910 ms. Design B is therefore 1.89 to 3.48 times slower than QUEUE-DENSE, and ARWLF is 1.73 to 3.28 times slower than design B, in line with the sparse-regime result of Section 8.4. The two sides time different stage boundaries: design B's stage time includes its own setup (about 5 ms), whereas the staged times exclude snapshot load and key preparation (32 to 47 ms), so these ratios are approximate. On attempt 21 design B stops at the first witness (213 ms), while the staged backends report the whole root, so that attempt is not compared.

At dimension 108 the same attempt loop ran to its budget on the balanced modular instance of seed 0x8001, pinned to one performance core. The dispatcher selected ARWLF on all 32 attempts. Staged list construction takes 118 to 162 seconds per attempt with a peak working set of 1.52 GiB; the root process, which loads the three lists and the reporter's tables, peaks at 3.82 to 3.86 GiB. Both stay inside the 4 GiB limit under which the classical in-process root does not fit. The root takes 1.77 to 3.54 hours per attempt, median 1.82 hours, and 62.0 hours over the 32 attempts. The machine was not kept idle throughout, since the forced-ARWLF dimension-72 run above ran beside this solve, so these times are not controlled measurements. Seventeen attempts report one to three residue-exact triples; none has a binary digit sum of weight $n/2$, so no parent is accepted and no attempt yields a witness. The run therefore ends with the attempt limit reached. The instance has a planted solution, so this outcome reflects TER's finite per-attempt success probability at these parameters, not unsatisfiability. A single staged attempt at this dimension thus costs about two hours on one core, although the reporter is about 55 times faster than K2 on a complete root; the result is evidence about the root workload, not a practical dimension-108 solver.

The retained Release CTest log of the development build records 59 passing tests; the current source tree, rebuilt in Release on 2026-10-13, passes all 61 registered tests. The AddressSanitizer CTest run selected 58 tests, excluding the dimension-108 planted test: 57 passed and the differential test timed out after one hour. A separately recorded rerun of the differential test under AddressSanitizer (2026-10-11, 4.2 hours) completed all 341 checks with zero failures, exit code 0 and no sanitizer diagnostic; its command, executable SHA-256, timestamps and complete output are retained with the supplement. Over the successful dispatcher attempt loop at dimension 72, recorded capture and root components total 5.61 and 7.67 seconds respectively; their sums are not complete script wall time, and no matched end-to-end speedup was measured.

## 9 Scaling and limits

### 9.1 Measured dimensions and implementation limits

The finite parameterization evaluated here sets every TER correction term to zero. Level-three halves carry $n/36$ positive digits, level-two vectors $n/18$ and level-one vectors $n/6$, with the required equal-half splits. This parameterization requires $n$ to be a multiple of 36; other dimensions require a separately justified finite rounding, outside the present experiments. Among the admissible dimensions, $n=36$ has $Q=24$, below the evaluated reporters' supported quotient width, and dimensions 72 and 108 provide the sparse and dense regimes measured on the test machine. The restriction limits which dimensions can be instantiated, not which root regimes are measured: source thinning (Section 8.2) and target thinning (Section 8.3) vary leaf occupancy and target density within a dimension, without relabelling the reduced inputs as new authentic dimensions.

Dimension 144 requires $Q=91$. The frozen prepared-key implementation accepts at most 69 quotient bits and at most 128 coordinates, so it cannot execute that dimension. Appendix C therefore considers an abstract wider-key implementation retaining the measured leaf and resolver design. Its memory estimates also exceed the evaluated host's 4 GiB budget. These restrictions are limits of the evaluated implementation and resources, not impossibility results for TER or for larger machines.

### 9.2 Occupancy and saturation model

The exact level-three size is $|L_3|=\binom{n/2}{n/36}$; under uniform modular hashing the expected level-two size is $|L_3|^2/2^{r_2}$. At dimensions 72 and 108 these give level-three sizes 630 and 24,804, and expected level-two sizes 3,100.8 and 150,204.7. Captured level-two sizes fluctuate; the expectation does not assert identical lists. For level one, a representation-count independence approximation gives the uncalibrated size estimate

$$
M_1=\binom{n}{n/6}\,2^{-r_1}\,\rho_2\,2^{-2r_2}\,f_2^3,\qquad f_2=\binom{n/2}{n/36}^2\Big/\binom{n}{n/18}. \tag{8}
$$

Here $\rho_2$ is the exact level-two representation count. Equation (8) is an approximation for the deduplicated level-one set, not an exact expectation. The measured mean divided by $M_1$ is 0.6123 at dimension 72 and 0.8122 at dimension 108. The samples contain 22 captures of seed 0x7201 and seven captures of seed 0x8001 (attempts 0–6), respectively; the latter have six completed root timings (attempts 0–5). These inputs were frozen before the 32-attempt solve of Section 8.7 completed and were not refitted to it. Calibration is an observed scenario input; these two ratios are not a statistical interval for a new dimension.

Let source and target leaf sizes be independent Poisson variables with means given by list size divided by $64\cdot8192$. For source sizes $A$ and $B$, and with $\operatorname{blk}$ the block count of Section 6.1, conceptual census packet occupancy is

$$
L=\frac{\mathbb E[A]\mathbb E[B]}{\mathbb E[\operatorname{blk}(A)]\mathbb E[\operatorname{blk}(B)]}.
$$

This uses the census's sixteen-by-sixteen accounting packets and approaches 256. The physical AVX2 operation remains one row against sixteen partner lanes. For target-leaf size $Y$ and chunk widths $b_1,\ldots,b_h$, uniform independent chunk values give the conditional byte-pass approximation

$$
p(Y)=\prod_{j=1}^h\left(1-(1-2^{-b_j})^Y\right).
$$

The model averages $p(Y)$ over the Poisson distribution. Dimension 72 uses chunk widths $(8,8,8,4)$ for its 28-bit mid; dimension 108 uses four full bytes. K2's projected-pair survival is approximated by $1-e^{-|C_u|/2^{19}}$. These independence assumptions describe a model, not invariants guaranteed by TER.

[TABLE:scaling_validation]

Table 8 compares model quantities with retained measurements. At dimension 108 it gives $L=125.7$, byte pass $9.7\cdot10^{-6}$, K2 survival 0.188 and root time 6,733 seconds. Comparable measured roots give occupancy near 126, byte passes from $9.2\cdot10^{-6}$ to $1.1\cdot10^{-5}$, K2 survival 0.186 and six root times from 6,403 to 7,396 seconds. These checks draw on different roots and pooled size samples; they are not a matched prediction experiment. At dimension 72 the model gives $L=1.07$ and K2 survival 0.0010, consistent with the sparse anchor. Its tiny byte-pass estimate is $3.53\cdot10^{-10}$; zero observed survivors cannot distinguish such small probabilities. The dense row rate is not used to predict sparse root time, where fixed costs matter and the dispatcher runs QUEUE-DENSE.

### 9.3 Target saturation at larger dimension

At dimension 144 the model gives 4.5 to 7.3 billion entries per level-one list and about 8,500 to 13,900 entries per target leaf under the thirteen-bit geometry. Source occupancy approaches 256, but nearly every byte value then occurs in every target leaf, so the byte filter passes almost every pair and the resolver scans thousands of postings per pair; K2's projected filter saturates in the same way. Dense source occupancy alone therefore does not establish an advantage: target density decides whether candidate work is suppressed, and the fixed leaf width and 32-bit filter are matched to the dimension-108 regime. Restoring about thirteen entries per leaf would need millions of leaves per family, a wider geometry that has not been implemented. Cost scenarios under explicit assumptions are model extrapolations, not measurements, and appear in Appendix C.

## 10 Relation to prior work

### 10.1 Subset Sum representations and exact algorithms

Horowitz and Sahni split an instance into two parts and compare enumerated subset sums, establishing the classical meet-in-the-middle baseline [@hs1974]. Schroeppel and Shamir preserve the half-exponent time scale while reducing storage by generating ordered pair sums from four smaller lists [@ss1981]. These are general exact approaches. ARWLF neither replaces their worst-case algorithms nor implements a Schroeppel-Shamir recovery structure; it optimizes a three-list merge arising inside a specific representation construction.

Howgrave-Graham and Joux introduce multiple representations to exploit hard random knapsacks [@hgj2010]. Becker, Coron, and Joux extend the technique using signed entries and filtering, and analyze a heuristic $2^{0.291n}$ time regime [@bcj2011]. Their explicit treatment of pre-filter intermediate lists is particularly relevant here: the cost of rejected arithmetic candidates cannot be inferred from the final valid list. Bonnetain, Bricout, Schrottenloher, and Shen analyze improved classical and quantum representation algorithms [@bbss2020]. Li's later TER construction uses ternary merges and a depth-three topology, with a heuristic random density-one time bound $2^{0.2400n}$ [@li2025]. The finite-width reporter in this paper operates within such list geometry; it does not prove or improve those heuristic exponents.

Nederlof and Węgrzycki use representations and orthogonal vectors to improve Schroeppel and Shamir's space exponent in a Monte Carlo algorithm for worst-case instances, while retaining its half-exponent time scale [@nw2021]. Chen, Jin, Randolph, and Servedio obtain a randomized polynomial-factor improvement over the meet-in-the-middle time bound for worst-case Subset Sum using bit packing and representation methods [@cjrs2023]. Their word width grows with the instance encoding; a fixed sixteen-lane instruction alone would not establish that asymptotic improvement. These results demonstrate that word-level computation and representation ideas can yield theoretical improvements when supported by an asymptotic analysis. ARWLF's fixed sixteen-lane row sweep is instead a measured constant-factor engineering result, with explicit survivor costs and no claimed improvement of the general exact bound.

### 10.2 Packed reporting and machine-oriented joins

Baran, Demaine, and Pătraşcu give subquadratic 3SUM algorithms in computational models that exploit packing and word-level parallelism [@bdp2008]. Barba and coauthors study algebraic generalizations of 3SUM under real-RAM and algebraic decision-tree models, distinguishing decision from output-sensitive reporting [@barba2017]. A modular three-list equality resembles these settings arithmetically, but the measured TER workload also carries original-entry multiplicity and vector constraints. More fundamentally, ARWLF continues to cover dense source-pair mass at fixed vector width. A small amortized cost per pair is not a subquadratic pair count. The prior 3SUM result is a useful boundary for terminology, not a claimed consequence of this implementation.

Late materialization is established in column-oriented databases: predicates can be evaluated before assembling full tuples [@abadi2007]. Radix partitioning and hardware-conscious hash joins are also established techniques [@balkesen2013]. SIMD database work studies efficient predicate evaluation, gathers, scatters, and lane management [@polychroniou2015]. Earlier SIMD scans evaluate predicates directly on compressed values and expose the cost of expanding scan masks into output positions [@willhalm2009]. ARWLF shares the broader principle of keeping hot representations compact and postponing expensive identity work. Its specific formulation combines power-of-two carries, terminal residual leaves, independent byte occupancy, a target-leaf row schedule, and exact TER downstream authority. The contribution is that integration and its measured operating regime, not the invention of radix partitioning, bitmap membership, SIMD, or late materialization.

The mechanisms differ in where each pays per candidate pair. A hash join keyed by the full residual pays one hashed table access and one key comparison per pair; radix partitioning keeps the table cache-sized but leaves that per-pair access in place [@balkesen2013]. On the 0x1001 dimension-108 anchor cell an untuned scalar version takes about 82 times as long as ARWLF (Section 8.1). K2 (Section 3.1) behaves like a join on a short key: enumerating projected matches is cheap, but every projected candidate must then be recovered and compared at full width, and Section 8.3 shows that this cost falls sharply when the target family is thinned. Packed 3SUM algorithms place several elements in one word to reduce the number of pair operations asymptotically [@bdp2008]; ARWLF also packs sixteen pairs into one instruction, but still visits every source pair, so it changes the constant per pair, not the pair count. Vectorized query execution amortizes per-value overhead by processing a vector of values per primitive call [@boncz2005]; ARWLF applies the same amortization to a fixed set of target byte maps. Its byte maps are a membership summary without false negatives: a missing byte rejects a pair with certainty, and every false positive is removed by the full-width resolver, which is why the filter can be weak without affecting the reported multiset.

## 11 Discussion and limits

The evidence supports a simple design lesson: when a short projection creates a flood of candidates, moving high-information necessary conditions ahead of recovery can matter more than reducing the apparent count of valid outputs. K2-WIDE shows that early full-width rejection alone helps; ARWLF shows that a much cheaper filter and a different row organization can improve further. Target thinning then connects the gain to the baseline's projected-candidate sensitivity. Together these comparisons explain the architectural result without assigning the entire speedup to AVX2.

The dense advantage has a boundary. Sparse leaves waste lanes and expose preparation cost. Duplicated or highly correlated target mids can saturate independent byte maps, enlarge the survivor buffer, and increase leaf scans. Constructed output stress exercises that path, but remains a one-root controlled experiment. Memory and time should therefore be predicted from entries, row occupancy, survivor counts, target-leaf sizes, and output multiplicity together. A single list-size threshold is insufficient.

The holdout design is stronger than reporting a single favorable anchor, but modest in independent-instance count. Its fifteen holdout cells derive from three roots on one processor. The root-block intervals respect that clustering, yet cannot establish processor-independent performance or all-density behavior. Additional machines and complete repeated-root comparisons would answer distinct questions, rather than merely add more samples of the same cells.

The evaluated backend began as a prepared-root component, separate from production solving. The integration of Section 8.7 closes part of that gap: it uses the production binary-root rule, recovers and deduplicates parents from raw records, verifies every parent against the original instance, matches the classical solver differentially at dimension 72, and recovers the same verified witness with ARWLF forced. It does not establish a matched end-to-end comparison at dimension 108, where the classical in-process root does not fit the 4 GiB limit. The 32-attempt staged solve there exhausted its budget without a witness after 62 hours of root work, which bounds the practical value of the staged solver at that dimension independently of the reporter's relative gain. The attached finite test-parent predicate of the benchmark sink differs from the binary production root and is incompatible with authentic root digit sums; its structural counters measure that predicate only, while timings and residue-exact outputs are unaffected. The model of Section 9 explains the fixed geometry's target-saturation limit. Its dimension-144 scenarios exceed the evaluated host's memory budget and require a wider-key extension beyond the frozen implementation.

## 12 Conclusion

ARWLF routes TER root pairs through fixed terminal leaves and applies an inexpensive byte-occupancy test before exact identity recovery. Routing and filtering provably preserve every full quotient match, including carries beyond sixty-four bits, and the cost model charges survivors, resolver scans and outputs explicitly. In the dense dimension-108 regime the measured result is a 52.9-fold campaign-holdout gain over the K2 baseline on microproblems, a 32.4-fold gain over its full-width strengthening, and a 55.2 to 57.4-fold reduction on a complete root. Sparse roots favor another reporter; controlled thinning and an occupancy model checked against both measured dimensions expose the crossover, the dispatcher's limits, and the saturation of the fixed geometry at larger dimension. In a staged composition with the classical pipeline, the reporter reproduces the original solver differentially and, with ARWLF forced, recovers an independently verified dimension-72 witness; at dimension 108 a 32-attempt staged solve finds no witness, an attempt-limit outcome rather than unsatisfiability. The result is an exact, finite-size, constant-factor improvement in TER root reporting.

## Data and code availability

All experiments use the research repository subset-sum-ter-fft. Its frozen root backends (29 source files pinned by SHA-256), campaign harnesses, staged-solve scripts and reviewer check scripts form the source archive. The manuscript supplement contains the exact numerical inputs of every table and figure (raw case and sample CSV files with recorded SHA-256 values), the scripts that recompute them (`build_results.py`, `scaling_model.py`), and the frozen build records of Table 10, with full hashes in `data/provenance_hashes.json`. Running `python build_results.py` in the supplement regenerates every table and figure without native code.

The native builds were made from working trees with uncommitted changes, so a revision alone does not identify their code. Each of the three campaign builds behind Tables 1–6 recorded a manifest with the SHA-256 of every source file. `reviewer/check_build_sources.py` recovers every manifest entry byte-exactly from the archived tree and its committed history, except the build-system CMake files and, for the complete-root build, one result-summary script; it also confirms that each manifest contains the 29 pinned frozen backend files. The build configuration of those CMake files is recorded separately as compiler flags and generated project files. The staged-solve, hash-join and design-B executables were recorded only by SHA-256 and by revision 8e9fe7f with uncommitted changes.

The root snapshots are not distributed: those at dimension 108 are 0.84 to 1.03 GB each, and those at dimension 72 about 4 MB. Their SHA-256 values are recorded, and `ter_root_snapshot_capture` regenerates each deterministically from its seed. A snapshot header also stores capture metadata (elapsed time, peak memory, commit and a header checksum), so a regenerated file is compared by the SHA-256 of all other bytes, which cover every list entry; three of the ten roots, including the dimension-108 calibration root 0x1001, were regenerated in this way and matched. Reproducing native timings requires those snapshots, a Release build with AVX2, and the machine protocol of Section 7.3. The source archive, together with the repository's complete git history that the source recovery above requires, is deposited at Zenodo (https://doi.org/10.5281/zenodo.23353466).

## Acknowledgments

AI coding assistants were used during implementation and manuscript preparation. Codex assisted source interpretation, literature synthesis, statistical recomputation, analysis scripts, figure generation and drafting; Claude assisted campaign, solver-integration and verification work. Every reported number originates from retained native-run records and is computed from them by the deterministic scripts of the supplement. The correctness argument states its hypotheses explicitly and does not rely on any assistant's assertions.

## Appendix A Reproducibility details

The source basis is the archived repository tree: revision 8e9fe7fdda447088738b5e682ba6aea4cf83a121 with the uncommitted campaign additions for constructed targets, complete-root replay and staged solving, and the 29 SHA-256-pinned frozen root backends. The regime campaign was built earlier, from revision 53431a8 with uncommitted changes; its source manifest, not the revision, identifies that build (see Data and code availability). Supplementary hashes identify every numerical input used to construct the article. The whole-root comparison uses the completed run records (all 64 K2 calls and the four bracketing ARWLF and dispatcher passes); the earlier partial cutoff file is retained unchanged.

The principal result tables are computed from the accepted timed rows, grouped by root, case, and backend. Their medians agree with the retained case summaries. Regime status, protocol, source transformation, and calibration role remain part of every group key. Ratios use within-case matched inputs. Complete-root single-pass times are not pooled with repeated-cell medians, and constructed positives are not pooled with authentic output-free cases.

[TABLE:protocols]

[TABLE:provenance]

The supplemental analysis verifies all 221 completed case groups and retains nine incomplete timing rows outside the aggregate set. Figures use exact observed occupancy and the declared target retention, not a dimension inferred from thinning. Appendix B provides additional absolute timings for the controls, and the supplement's claim ledger (`claim_ledger.md`) maps each principal claim to its source and derivation.

## Appendix B Additional control timings

[TABLE:target_detail]

[TABLE:crossover_confirm]

The target-density table keeps the source pair mass fixed within each root. The crossover table uses only the final-protocol confirmations; discovery sweep curves are displayed separately in Figure 4. These controls address different mechanisms and are not combined into an additional authentic benchmark headline.

## Appendix C Hypothetical dimension-144 cost scenarios

These scenarios extrapolate the measured dimension-108 coefficients to a dimension that no implementation here can run (Section 9.1). They are model calculations, not measurements; the main text relies only on their qualitative conclusion, target saturation (Section 9.3).

At dimension 144 the finite parameters are $r_1=53$, $r_2=16$ and $Q=91$. Scaling Equation (8) by the two measured calibration ratios gives 4.46 and 5.92 billion entries per level-one list; the uncalibrated estimate is 7.29 billion. Under the thirteen-bit geometry these give 8,510 to 13,897 entries per target leaf (Table 13), so almost every byte value occurs in every leaf. The three sizes are scenarios, not confidence bounds.

[TABLE:scaling_n144]

The fixed-geometry scenario charges every logical source pair the observed dense coefficient, 0.1377 ns, plus a saturated resolver scan proportional to target-leaf occupancy. The scan coefficient, 0.7686 ns, is inferred from the time difference between the one-million-decoy and one-hundred-decoy controls, divided by survivors and mean target-leaf size; it is not an isolated instruction measurement. In general the scan term requires $\mathbb E[Yp(Y)]$; at these occupancies $p(Y)\approx1$ and it reduces to the mean leaf size. The scenario gives 4.13 to 17.97 million core-years for one root (Table 14). Dividing by the 630,784 hardware CPU cores of the Frontier system [@olcf_frontier] gives 6.54 to 28.49 years, assuming that every core sustains the observed Intel per-core rate and that work scales perfectly without communication or imbalance. No Frontier or AMD rate was measured, and no GPU execution is modeled.

[TABLE:scaling_cost]

An optimistic pair-sweep-only scenario drops the resolver term and gives 86.9 to 231.7 core-years, or 1.21 to 3.22 hours on the same hypothetical core count. It presumes a retuned geometry with about thirteen entries per leaf, that is $|L_1|/(64\cdot13)$, or 5.36 to 8.76 million leaves per family (rounded up to $2^{23}$, $2^{23}$ and $2^{24}$). That geometry has not been implemented, and its setup, larger directories, wider keys, residual survivor work and communication are not modeled. The result is a conditional floor within this model, not an achieved runtime or a lower bound for future algorithms.

With the measured 48-byte dimension-108 entry as a storage proxy, the three lists need 0.64 to 1.05 decimal TB; the current snapshot format, at 68 bytes per dimension-144 entry, needs 0.91 to 1.49 TB, before prepared keys, directories, temporaries and outputs. Every scenario covers the root of one attempt only: list construction adds to each attempt, and the number of attempts multiplies the total.
