# ARWLF regime campaign — preregistration

Frozen 2026-10-01, before any campaign timing was taken. Benchmark surface only:
no backend, the dispatcher, or any production path is changed by this campaign.
Executable: `ter_arwlf_campaign` (`bench/ter_arwlf_campaign_main.cpp`), library
`bench/ter_arwlf_campaign.{hpp,cpp}`, tests `ter_arwlf_campaign_tests`.

## Question

Which geometry makes ARWLF fast, where does it stop winning, and does the
existing dispatcher (frozen as of the v3 report) recognise the regime?

Primary variable: **L = logical lanes per physical 16 x 16 packet** of the
ARWLF leaf-pair geometry (`lanes / packets`, at most 256), measured by an exact
census and gated against the counted ARWLF run on every timed case.

## Data and labels

| Label | Meaning |
|---|---|
| `AUTHENTIC_FULLTER` | an unmodified microproblem (or whole root) of a captured Full TER root |
| `AUTHENTIC_SOURCE_THINNED` | an authentic microproblem with Q_s and X_t thinned; EntryIds, keys and authority untouched. A controlled occupancy perturbation of an authentic n=108 root, **not** a TER instance at any other n |
| `AUTHENTIC_TARGET_THINNED` | the same with only Y_u thinned (ablation) |

Calibration roots (they influenced development): n=108 seeds 0x1001, 0x2003,
0x3007 (attempt 0) and the n=72 anchor (seed 19797034556212562).

Holdout roots (captured after this file was frozen, never used for tuning):
n=108 seeds **0x4001, 0x5003, 0x6007** (attempt 0, staged capture); n=72 seeds
**0x7201, 0x7203, 0x7207** (`--verify-against-solve --attempt=auto`).

## Per-root phases

| Phase | Cases | Protocol |
|---|---|---|
| census | all 4096 microproblems: sizes, leaf occupancy, packets, lanes, rows, L, group counts, root QUEUE_DENSE legality | none (no backend runs) |
| anchor | the declared microproblem: first of the fixed 0x5EED5EED shuffle (the cell every earlier n=108 result used) | final |
| stratified | 4 x 4 grid on log2(pairs) x L, cut at each axis' 25/50/75 % quantiles; per nonempty cell the member nearest the cell median (axes scaled by population IQR; ties to the lower cell id). A pick that is the anchor is left to the anchor phase | discovery |
| thin-qx (calibration n=108 only) | anchor, target L in {1, 2, 4, 8, 12, 16, 20, 24, 32, 48, 64, 96}, thinning seeds 0xA11CE001/2/3; targets above the authentic L are recorded `UNREACHABLE` | discovery |
| thin-y (calibration n=108 only) | anchor, p_Y in {1/2, 1/4, 1/8}, seed 0xA11CE001 | discovery |
| whole (n=72 only) | all 4096 microproblems as one case | final |
| confirm (calibration n=108 only) | crossover points, rule below | final |

Protocols: **discovery** = 1 warmup round + 3 timed rounds; **final** = 3 warmup
rounds + 7 timed rounds (the v3 protocol). Before either, an untimed exactness
pass. Backends raced: frozen K2, K2_WIDE, QUEUE_DENSE (only where its existing
capacity guard admits it), ARWLF, DISPATCH (the frozen v3 dispatcher). Round r
runs the backends in canonical order rotated left by r, reversed on odd rounds.
P-core pinned (`--pin=p-core`), AC power required, Release build required.

## Thinning

An entry survives with probability p iff
`H(root seed, cell, list tag, EntryId, thinning seed) < floor(p 2^64)`, H a
fixed SplitMix64 chain (deterministic, order-independent, nested in p). Q_s and
X_t share p. The retention for a target L starts from `sqrt(L_t / L_0)` and is
then bisected (48 steps, census only, untimed) to the largest p whose census L
does not exceed the target, because L falls slower than p^2 once leaves thin
out (measured: the sqrt rule aimed at L=4 gave L=6.19 on 0x1001). L >= 1 always,
so target 1 means the largest thinning whose occupied leaf pairs are all
singletons: a tiny microproblem, by definition. Both p values and
the observed L are recorded; plots use the observed L.

## Exactness (any failure aborts the campaign and writes an artifact)

Per case, untimed: every backend's final-authority output (the sorted list of
residue-exact (a, b, c) EntryId triples) equals frozen K2's; the v3 gates
(ARWLF == K2_WIDE on all full-key triple hashes; ARWLF k=32 hits == K2's low-32
survivors; ARWLF hits at k = 19 + mid == K2_WIDE's width histogram;
residue/structural counts and multiset hash equal; no directory misses); and the
census lanes / packets / leaf triples / rows equal the counted ARWLF run. Every
timed run is re-checked (digest and witness list) against the reference.

## Resources

One root resident per process; rows stream to CSV + JSONL as produced; a rerun
skips cases already written (status included). A case whose working set exceeds
4096 MiB after its exactness pass is recorded `RESOURCE_LIMIT` and not timed. A
fixed CPU sentinel runs at process start, every 20 cases and at the end.

## Statistics

Per case and backend: median, p25, p75, min, max over the timed rounds;
speedup S_b = median(K2) / median(b). Across independent authentic cases:
geometric-mean speedup with a percentile bootstrap 95 % CI (10 000 resamples,
seed 0xB007, resampling cases; and separately resampling roots). Dispatcher
regret = median(DISPATCH) / min over the legal exact backends' medians
(K2, K2_WIDE, QUEUE_DENSE, ARWLF); reported as median / p95 / max, plus
winner-selection accuracy (the backend DISPATCH ran in most samples == the
fastest legal backend).

Crossover: per calibration root and target L, the geometric mean over the three
thinning seeds of median(ARWLF) / min(median(K2_WIDE), median(QUEUE_DENSE) if
legal). **Confirm rule:** for each calibration root, the two smallest-L reachable
targets whose ratio brackets 1 (seed 0xA11CE001) are rerun under the final
protocol; if no target brackets 1, the smallest reachable target is rerun and
"no crossover in the reachable range" is reported.

## Amendment 1 (2026-10-01, after the census, before any campaign timing)

The census showed every n=108 root internally uniform (L within +/-1.6 %, pairs
within +/-2 % across all 4096 microproblems), while the 4 x 4 stratified sweep
on six n=108 roots would cost about 19 h of pinned time (frozen K2 is 50-100 s
per run). By the user's decision the n=108 roots run phase
`stratified-corners`: the four corner grid cells (pair bin 0 or 3 x lane bin 0
or 3) of the same preregistered selection, discovery protocol. n=72 roots keep
the full 4 x 4 `stratified` phase. Nothing else changes. Because the executable
changed (the new phase only), the build was re-frozen and the census rerun
(deterministic: same rows).

## Not in scope

No change to ARWLF, the dispatcher (including its preregistered
packets-per-pair <= 1/16 applicability rule), K2, K2_WIDE or QUEUE_DENSE. No
n=144. Distribution-matched widths are not part of this run.

## Addendum (2026-10-02, after the campaign; no change to the protocol)

The campaign sources were relocated with `git mv`, contents unchanged: `bench/ter_arwlf_campaign*` is now
`ter2_root/campaign/`, the backends are in `ter2_root/frozen/`, the scripts in `ter2_root/scripts/`. See
`ter2_root/README.md`.

## Amendment 2 (2026-10-03, after the campaign, before any positive-output timing): positive outputs

**Why.** All 221 exact campaign cases emitted zero final triples (every backend's residue-exact and
structural counts are 0; checked from `results/arwlf/regime-20261001/cases.csv`). The campaign
therefore measures the backends on authentic microproblems *without final outputs* and says nothing
about the cost of emitting one. This amendment adds an experiment in which the outputs are known in
advance. The frozen backends and every gate above are unchanged.

**Construction** (`PlantedTargets`, `ter2_root/campaign/ter_arwlf_campaign.cpp`; tested independently
in `ter_arwlf_planted_tests`). On the anchor microproblem (s, t) of a root, with u = (s + t) & 63:

| Label | What is appended to the root's C list (family u, then Y_u re-sorted and the TargetSet rebuilt) |
|---|---|
| `AUTHENTIC_PLANTED_TARGETS` | M records c, one per distinct authentic pair (q in Q_s, x in X_t) drawn by `H(root seed, cell, 0x504C414E54, draw, seed)`, with full key d = q_a + q_b mod 2^Q, low-32 quotient tau_low32 - d_lo, residue root - res_a - res_b mod 2^n, and digits completing the pair's parent to the expected distribution: every (q, x, c) is a final output the frozen authority accepts |
| `AUTHENTIC_DECOY_TARGETS` | M records with a random key in family u, a quotient consistent with that key, a random residue and zero digits: the same Y growth, no final output |

The planted records are constructed, **not** TER vectors and not outputs of the L1 merge; the A, B
lists and every authentic C record are untouched. The experiment measures what a backend pays per
emitted output on authentic sources; it does not say how many outputs a real TER root has.

**Cases.** Phase `plant`, calibration roots only, M in {100, 1 000, 10 000, 100 000, 1 000 000}, a
positive and a decoy case per M, seed 0xB1A57001, discovery protocol, roots in the order
**0x1001, 0x3007, 0x2003** (attempt 0). Order reason: 0x1001 has the most memory headroom (authentic
anchor peak 3280 MiB against about 4000 MiB for the other two); M planted records add about 0.3 GiB at
M = 10^6, so the M = 10^6 cases of 0x3007 and 0x2003 are expected to hit the existing 4096 MiB guard and
be recorded `RESOURCE_LIMIT` (never silently skipped). A single smoke-protocol run of the M = 10^6
positive case on 0x1001 (one timed round, all gates passed, peak 3589 MiB) was made to size this
amendment; it is not part of the results.

**Gate** (added to the exactness pass; a failure aborts as before): the planted part of frozen K2's
witness list (C EntryId >= the first planted id) equals the planted set exactly, and the structural
count is >= M; for a decoy the planted part must be empty. Every other backend must equal K2's full
witness list, as before.

**Analysis.** Per root and M: the medians of each backend for the positive and the decoy case,
S_b = median(K2) / median(b) for both, and the **marginal cost per output**
(median_positive - median_decoy) / M for K2, K2_WIDE and ARWLF, reported as measured (it can be
negative within noise at small M). No cross-root pooling, no outlier removal.
