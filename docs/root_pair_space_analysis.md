# Root pair-space analysis: what Stage-D region pruning can and cannot do

Repair items **P17 / P28**. Produced by `ter_root_pairspace_probe` on genuine
captured `FullTer` roots plus synthetic geometries.

> **Correction.** An earlier revision of this document concluded that exact
> region pruning "cannot reduce the root work" because `total/P0 >= 1.000` at
> every radix width. That statement conflated two different quantities. It is
> withdrawn. The corrected analysis is below.

## What the probe measures, precisely

For radix width `k`, every pair `(a,b)` lands in exactly one rectangle
`(low_k(q_a), low_k(q_b))`, and the low-`k` congruence fixes a **single**
required C digit `d(i,j) = (T0 - i - j) mod 2^k`. The decomposition is
therefore disjoint and free of target multiplicity; the probe asserts pair-mass
conservation (`opened + rejected == P0`) at every width and aborts otherwise.

| symbol | meaning | probe column |
|---|---|---|
| `P0` | `\|A\|*\|B\|` logical pairs | header |
| `rho_open` | opened pair mass / `P0` | `rho_open` |
| `rho_region` | surviving rectangles / rectangles considered | `rho_region` |
| `Phi` | rectangle-enumeration cost | `phi_region_visits` |
| `total` | `Phi + opened_pair_mass` | `total_work` |

The earlier headline used `total/P0`, which is `Phi + opened`, **not**
`rho_open`. The two are now reported separately.

## Result 1 — the pruning power is real and large

Genuine captured n=72 root (`|A|=19,109`, `|B|=32,496`, `|C|=33,505`,
`P0 = 620,966,064`):

| bits | rho_region | rho_open | opened pairs | pair-work reduction |
|---:|---:|---:|---:|---:|
| 16 | 0.399779 | 0.399783 | 248,251,705 | 2.50x |
| 20 | 0.031462 | 0.031463 | 19,537,381 | 31.78x |
| 24 | 0.001998 | 0.001998 | 1,240,683 | **500.50x** |

`rho_open` follows the closed form `1 - exp(-|C| / 2^k)` (at k=24:
`33,505 / 2^24 = 0.001997` predicted vs `0.001998` measured). This is a
property of the *width*, not of the summary data structure: for a `k`-bit
digest, the occupancy test is already the strongest exact predicate available.
A hierarchical or sparse bitset changes the cost of the lookup, never
`rho_open`.

For reference, the production root's existing low-24 prefilter already reports
`low_prefilter_candidates = 1,238,960` — the same quantity the probe measures
as 1,240,683 opened pairs at k=24.

## Result 2 — the blocker is `Phi`, the enumeration cost

The probe's `Phi` is a naive sweep of every occupied-A x occupied-B rectangle,
which for these list sizes is itself ~`P0`:

| bits | surviving rectangles | Phi (naive) | naive total/P0 |
|---:|---:|---:|---:|
| 20 | 19,084,454 | 606,588,558 | 1.0083 |
| 24 | 1,238,841 | 620,049,035 | 1.0005 |

But `Phi` is a property of the enumeration strategy, not of Stage D. A lower
bound for any strategy that lists survivors is the survivor count itself:

| bits | output-optimal total (`surviving + opened`) | total/P0 | reduction |
|---:|---:|---:|---:|
| 20 | 38,621,835 | 0.0622 | 16.1x |
| 22 | 9,858,500 | 0.0159 | 63.0x |
| 24 | **2,479,524** | **0.0040** | **250.4x** |

**So Stage D is not structurally dead on this root.** If the surviving
rectangles could be enumerated in time proportional to their number rather
than by scanning all `|A_occ| x |B_occ|` combinations, total root work would
drop by up to 250x.

## Result 3 — the balanced-root null point

Minimizing `total(x) = x^2 + |C|*P0/x` over `x = 2^k` (valid while
`2^k <= min(|A|,|B|)`) gives `x* = (|C|*P0/2)^(1/3)` and
`total(x*) = 3*(|C|*P0/2)^(2/3)`. For a balanced root
(`|A|=|B|=|C|=N`, `P0=N^2`) that is `3 / 2^(2/3) = 1.890 * P0` — worse than
scanning. The governing ratio is `|C| / sqrt(|A|*|B|)`.

Five genuine captured roots, different seeds:

| root seed | \|A\| | \|B\| | \|C\| | C/sqrt(AB) | best k | min naive total/P0 |
|---|---:|---:|---:|---:|---:|---:|
| 19797034556212562 | 19,109 | 32,496 | 33,505 | 1.3445 | 8 | 1.0001 |
| 1 | 34,083 | 35,200 | 33,815 | 0.9763 | 20 | 0.9993 |
| 424242 | 33,830 | 34,775 | 33,816 | 0.9859 | 18 | 0.9982 |
| 99999999 | 34,581 | 34,753 | 33,462 | 0.9652 | 18 | 0.9974 |
| 7777777777 | 36,078 | 35,634 | 36,090 | 1.0065 | 22 | 0.9999 |

`rho_open` is stable across all five (0.0314-0.0338 at k=20; 0.00199-0.00215 at
k=24), so this is a regime property, not a seed artifact.

Synthetic uniform quotients confirm the predicted crossover:

| \|A\| | \|B\| | \|C\| | C/sqrt(AB) | best k | min naive total/P0 |
|---:|---:|---:|---:|---:|---:|
| 30,000 | 30,000 | 30,000 | 1.0000 | 24 | 1.0000 |
| 30,000 | 30,000 | 3,000 | 0.1000 | 14 | 0.3779 |
| 30,000 | 30,000 | 300 | 0.0100 | 12 | 0.0892 |
| 30,000 | 30,000 | 30 | 0.0010 | 12 | 0.0257 |
| 100,000 | 100,000 | 1,000 | 0.0100 | 14 | 0.0859 |

Even with the naive `Phi`, Stage D pays 2.65x-38.9x once `|C| << sqrt(|A|*|B|)`.
TER's three L1 lists are produced by the same L2->L1 merge under the same shape
parameters, so they are comparable by construction and the ratio sits at ~1.0.
**Stage D is falsified for the balanced regime TER actually produces, not in
general.**

## The precise open question

> Can the surviving rectangles — those `(i,j)` with `C[(T0-i-j) mod 2^k]`
> occupied — be enumerated in time proportional to their count, instead of by
> scanning all `|A_occ| x |B_occ|` combinations?

This is sparse 3SUM enumeration over `Z_{2^k}`. Every pairwise-product
enumeration tried here costs ~`P0`:

- occupied-A x occupied-B: 620,049,035 at k=24
- occupied-A x occupied-C: 19,099 * 33,479 = 639,417,821
- occupied-B x occupied-C: 32,465 * 33,479 = 1,086,955,735

Counting (not listing) is cheap by convolution over `Z_{2^k}`, but a count does
not yield the pairs, and a low-`k` count is essentially never zero here
(`|A||B||C| / 2^20 ~ 2.0e7`), so it cannot cheaply reject a failing attempt
either. Note also that this repository previously measured an NTT convolution
path and rejected it on a performance gate, so an FFT/NTT-based enumeration
would have to clear that bar as well as being exact.

Practical bar: the current streaming root does 620,966,064 probes in ~530 ms,
i.e. ~0.85 ns per probe. A sparse enumeration producing ~1.24M rectangles would
have to stay well under that budget to win.

**This question is unresolved. It was not implemented and it is not proven
impossible.**

## Status of the repair items

- **P17 (Stage D): NOT IMPLEMENTED.** The coarse-occupancy predicate with naive
  rectangle enumeration was prototyped and measured; it does not pay on
  balanced roots. The pruning power itself (`rho_open`) is confirmed real.
- **P28 (pair-space reduction): NOT FIXED.** `rho_open` remains 1.0 in the
  shipped solver; pair work remains 620,966,064.
- **P18 (Stage E): NOT IMPLEMENTED.** Carry-aware refinement was not built or
  measured. The correct next-chunk relation is
  `a1 + b1 + carry0 == d1 (mod 2^h)` with `carry0 = floor((a0+b0)/2^k)`, never
  `a1 + b1 == d1`. Nothing here falsifies it; Stage D's `rho_open` already
  reaches 0.002 at k=24, so the room Stage E would refine is small on this
  regime, but that is an observation, not a measurement.
- **P19 (narrowed recovery): NOT IMPLEMENTED.**

## Reproducing

```powershell
cmake --build build-final-clean --config Release --target ter_root_pairspace_probe
.\build-final-clean\Release\ter_root_pairspace_probe.exe
.\build-final-clean\Release\ter_root_pairspace_probe.exe --root-seed=424242
.\build-final-clean\Release\ter_root_pairspace_probe.exe --synthetic --a=30000 --b=30000 --c=300
```

The probe reports work counts and its own sweep timings only. Pair-work
reduction is never a wall-clock speedup claim.
