# TER L1 -> L0 root: ARWLF Architecture v1.0

**Status:** CURRENT, normative for the TER² root-backend line (dated 2026-10-02).
**Supersedes:** `docs/history/TER_L1_to_L0_Radix_Refined_Root_Architecture_v2.1_CODEX.pdf`. See `docs/history/SUPERSEDED.md`.
**Implementation:** frozen; see `FROZEN_ROOT_BACKENDS.md` (29 files, SHA-256 pinned, CTest `ter_frozen_root_backends`).
**Evidence:** `ARWLF_REGIME_CAMPAIGN.md` (preregistration) and `results/arwlf/regime-20261001/summary.md`.

**Scope boundary.** This architecture runs on the benchmark surface: captured authentic roots replayed from snapshots. The production `solve_full_ter` root is still design B (`docs/root_architecture_resolution.md`) until the integration gates in section 9 pass. Nothing here is an asymptotic claim. ARWLF is a finite-n, constant-factor result.

## 0. Pipeline (frozen)

```text
authentic root snapshot (SHA-256 identified)
    -> workload: L1 lists, full Q-bit keys, authority tables
    -> geometry: families, 13-bit leaves, packets / logical pair
    -> dispatcher (probe, predict, guard)
        -> QUEUE_DENSE   [sparse: every family <= 65,536 distinct residues]
        -> ARWLF         [dense: packets / pair <= 1/16]
        -> K2_WIDE       [fallback: always applicable]
    -> full-width exact authority (all W residual bits, then the high Q - 64 bits with carry)
    -> low-32 quotient prefilter -> original-residue equality mod 2^n
    -> TER structural validation -> verified root triple (EntryIds)
```

**Cost on fixed hardware:**
`T_ARWLF = Theta(|Q_s| |X_t|)` per microproblem, plus Theta(N) builds and the exact work on surviving pairs.
Every logical pair is still tested, about 16 per AVX2 row. The speedup over K2 is a constant factor at the measured n. **It is not a sub-quadratic 3SUM method and does not change the TER exponent.**

## 1. Lineage: what changed and why

| | v2.1 radix-refined root | ARWLF v1.0 |
|---|---|---|
| Objective | Reduce opened pair mass before recovery (E << P0) | Make each logical pair test cheap; keep candidate survivors near zero |
| Pair work | Hierarchical region pruning, then refinement | Every logical pair of a microproblem is tested (lanes = \|Q_s\| \|X_t\|) |
| Where the time goes | Region enumeration (Phi), then recovery | One AVX2 row of 16 pairs at a time; the exact resolver sees about 1e-5 of pairs |
| Outcome | Stage D never built (Phi stays near P0 with naive enumeration); Stage E as A64-CHQR is pair-bound | About 53x frozen K2 on authentic n=108 microproblems, holdout-confirmed |

The v2.1 scaling objective E << P0 is **not** met and remains open (the Stage-D enumeration problem). ARWLF changes the finite-n engineering objective, not the asymptotics.

## 2. Exact root mathematics

For depth-3 TER at dimension n (n % 36 == 0; `docs/ter_dimension_domain.md`), the root merges the L1 lists A, B, C:

- Quotient width `Q = n - r1`; for example Q = 47 at n=72 and Q = 69 at n=108.
- Root equation: `q_A + q_B == d (mod 2^Q)`, with `d = tau - q_C`, where tau comes from the donor's verified child-target summation.
- Fast key `F = min(Q, 64)` low bits. The low 6 bits split the problem into 4096 microproblems `(s, t)`, with `u = (s + t) mod 64` and `kappa = (s + t) >> 6`.
- Residual width `W = F - 6`, and per microproblem: `r_q + r_x + kappa == r_y (mod 2^W)`.
- When Q > 64, the high `Q - 64` bits are checked using the carry out of the low-F addition.

Only original-residue arithmetic mod 2^n plus TER structural validation may authorize a root result (section 6).

## 3. Backends (all exact, all frozen)

| Backend | Mechanism | Applicable |
|---|---|---|
| **K2** (baseline) | 8x8 tile kernel on the 19-bit residual plus exact Y bitmap (25 quotient bits), then the authority chain | always |
| **K2_WIDE** | K2's kernel, plus a full-key check (W bits and the high bits) before the authority chain | always (needs full keys) |
| **QUEUE_DENSE** | Per-query queue of nonzero guard words, then the exact radix-13 dense leaf | every family has <= 65,536 distinct residues (true at n=72, false at n=108) |
| **ARWLF** | Section 4 | W >= 29 (13 leaf bits + 16 filter bits) |
| **DISPATCH** | Section 5 | always |

## 4. ARWLF

### 4.1 Terminal leaves and exact routing
- Each family is partitioned once by residual bits [0, 13) into 8,192 leaves, keeping donor order inside each leaf.
- Q leaf i and X leaf j can reach only one target leaf, `k = (i + j + kappa) mod 8192`, with low-13 carry `c = (i + j + kappa) >> 13`.
- For a fixed k, c = 0 for every i <= k - kappa and c = 1 above that. So a target leaf's Q rows form **two contiguous ranges with constant carry**.
- Thirteen quotient bits are therefore matched exactly by routing, with no comparison.

### 4.2 Wide leaf filter
- `mid` = residual bits [13, 13 + m), with m = min(32, W - 13).
- For each pair, `s = (mid_q + mid_x + c) mod 2^m` is exactly those bits of the residual sum.
- Each Y leaf has four 256-bit byte-occupancy tables, one per byte of mid.
- A pair is rejected only if some byte of s occurs in no entry of the Y leaf, so **no true solution is ever rejected**.
- The tables summarize Y alone.

### 4.3 AVX2 row sweep (the hot loop)
- **One row:** one Q entry against its partner X leaf's first 16 entries, as 16 x 16-bit lanes in one ymm register. `dup = ((mid_q + c) & 0xFFFF) * 0x10001`.
- **Bytes 0 and 1:** tested with `pshufb` nibble lookups into the leaf's tables (a/b halves, `blendv` for odd bytes), then `cmpeq` and `movemask`. `pass = ~(fail | fail >> 1) & valid`, where the `valid` lane mask is precomputed per X leaf.
- **Branch-free and call-free:** each row writes `(pass << 32) | q` to a pending buffer, which is drained after the loop. Survivors are tested on bytes 2..chunks-1 there.
- **Overflow pass:** X leaves with more than 16 entries get their extra 16-lane blocks in a separate pass.
- **Scalar reference:** `wide_leaf_filter_scalar` plus the per-leaf-pair packet sweep is the test oracle and must produce identical survivors.

### 4.4 Exact resolver and authority
Each filter survivor (q, x) is compared with every y of its target leaf in four stages:
1. on mid bits;
2. on all W residual bits;
3. on the high Q - 64 bits, using the carry;
4. through the RootSink chain: low-32 quotient prefilter, original-residue equality mod 2^n, then TER structural validation.

EntryId multiplicity is preserved; nothing is deduplicated.

### 4.5 Data layout and memory
- **Per-family structures** (built lazily and charged to the call): leaf offsets, 32-bit mids and posting indices; Q-side leaf ids and the two `dup` arrays; X-side 16-bit lows, lane slots and overflow list; and 4 x 256-bit tables per Y leaf.
- **Size:** about 4-5 MiB per n=108 microproblem; build time about 2.6-2.9 ms.
- **Memory is pairless:** O(N) plus per-family structures. No pair arrays, no pair sums, no 2^Q tables.

### 4.6 Cost model (measured, finite n)
- **Rows:** `rows = |K_nonempty| * |Q_s| + sum over target leaves of the overflow rows`, with lanes per row about 0.70 x 16 at n=108.
- **Per pair:** about 0.135 ns per logical pair at n=108 (0x2003 anchor: 1,679 ms for 1.24e10 pairs).
- **Survivors:** the filter passes about 1e-5 of pairs.

ARWLF loses when leaves are sparse. At L (logical lanes per 16 x 16 packet) close to 1, as at n=72, most rows carry one useful lane and fixed build costs dominate. ARWLF overtakes every other exact backend between L = 1 and L = 2 (final-protocol confirmation on all three calibration roots).

## 5. Dispatcher (frozen v3)

1. **Applicability:** QUEUE_DENSE only inside its capacity; ARWLF only if packets / logical pair <= 1/16 (that is, L >= 16) and W >= 29; K2_WIDE always.
2. **Probe:** a square block of up to 8 x 8 selected microproblems with work, cut to 2^21 logical pairs (K2_WIDE) and 2^26 (ARWLF). QUEUE_DENSE gets the minimum of two build + route runs. The selection is swapped in place and restored afterwards.
3. **Prediction:** each probe is extrapolated to the full selection (builds scale with entries, sweeps with rows or pairs), and the smallest prediction runs. Ties keep the order QUEUE_DENSE, ARWLF, K2_WIDE; if no candidate has a finite prediction, K2_WIDE runs.
4. **Guard:** a chosen ARWLF aborts if, after the first 1/64 of target leaves (at least 65,536 pairs), its ns per pair exceeds the runner-up's break-even; the runner-up then runs from scratch.

**Measured regret** (DISPATCH / fastest backend):
- 1.002-1.026 on authentic n=108 microproblems (29/29 ran ARWLF);
- 1.04-1.14 on whole n=72 roots (4/4 ran QUEUE_DENSE).

**Known defects (recorded, not fixed in v1.0):**
- The L >= 16 applicability rule forfeits up to 17x between L = 2 and 16.
- About 10 ms of probing dominates microproblems that take about 0.1 ms.

A corrected dispatcher must be a new dispatcher in new files (section 8).

## 6. Exactness contract (inherited from v2.1 section 6; binding)

1. Only original-residue arithmetic mod 2^n plus the canonical TER structural validator authorize an L0 result.
2. Every filter (leaf routing, byte tables, low-32 prefilter, K2's 25-bit filter) is a necessary condition. It may reject only with a proved no-false-negative implication.
3. Aborted, cancelled, incomplete or refused work is never a negative proof. `NotApplicable`, `ResourceLimit` and `Aborted` stay distinct.
4. No pair materialization; no persistent wide quotient beyond the existing keys.
5. EntryId identity and multiplicity are preserved end to end.

## 7. Verification and measurement requirements

- **Differential:**
  - ARWLF, K2_WIDE and the dispatcher agree with the whole-value brute force on synthetic roots (Q = 47 and 69, sparse, dense, single-leaf and overflow geometries);
  - AVX2 survivors equal the scalar reference;
  - on authentic roots: ARWLF k=32 hits == frozen K2 low-32 survivors, ARWLF hits at k = 19 + m == K2_WIDE's width histogram, and every backend's sorted residue-exact witness list equals frozen K2's.
- **Geometry gate:** the independent census (leaf triples, packets, lanes, rows) equals the counted ARWLF run on every timed case.
- **Mutation:** deliberate kernel and campaign mutations must be caught (8/8 kernel, 10/10 campaign at freeze time).
- **Timing protocol:**
  - P-core pinned; AC power; Release build;
  - exactness pass first; matched rounds with order rotated each round and reversed on odd rounds;
  - final protocol 3 warmup + 7 timed rounds; geometric means with bootstrap CIs over cases and over roots;
  - calibration and holdout roots kept separate.
- **Sanitizers:** ASan clean on `ter_arwlf_tests`, `ter_a64_chqr_tests` and `ter_arwlf_campaign_tests`.

## 8. Change control

- The section 3 backends and the section 5 dispatcher are frozen.
- New ideas, including a fixed dispatcher, become new backends in new files, measured against these baselines under section 7.
- A frozen file changes only for a documented bug fix, per `FROZEN_ROOT_BACKENDS.md`.
- A v1.1 of this document may add backends; it may not silently change v1.0's.

## 9. Next phases and their gates

| Phase | Work | Gate |
|---|---|---|
| I1 Whole root | Run ARWLF over all 4,096 microproblems of one n=108 root; report the time breakdown of one FullTer attempt (capture vs root) | Exact (differential vs K2_WIDE on sampled microproblems). Report K2 whole-root time as a census-based extrapolation, labelled modeled |
| I2 Baselines | Compare ARWLF with the production root (design B) and with a textbook exact join (hash or sort-merge on the 3SUM equation) on identical inputs | Matched protocol, same inputs |
| I3 Integration | ARWLF (or a dispatcher) inside the FullTer root behind a flag, fail-closed, differential against design B | Identical root outcomes and witnesses on n = 36, 72, 108 fixtures; full suite plus ASan |
| I4 Dispatcher v2 | A new dispatcher with ARWLF admitted from about L >= 2 and a size floor for probing | Holdout regret <= 1.05 across the thinned and authentic sets |

Out of scope for v1.0: Bloom gates, fingerprints, Schroeppel-Shamir recovery, planners, parallel groups, finite-n reoptimization, and any claim about the TER exponent.
