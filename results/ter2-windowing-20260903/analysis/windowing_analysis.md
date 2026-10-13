# TER² residual windowing spike — analysis

Companion to `../raw/window_geometry.json`, `../raw/window_geometry.csv`,
`../raw/value_tiling_sweep.csv` and `../raw/probe_stdout.txt`.

Verdict: **WINDOWING_GEOMETRY_UNPROMISING**. Neither route survives contact with
the authentic n=72 geometry. This document records *why*, because the reason is
a property of the data rather than of any kernel, and therefore rules out a
whole family of future designs rather than one implementation.

## 0. What was tested

Two independent hypotheses, both measured exhaustively over all 9,890,364
authentic rank tiles (620,966,064 source lanes, zero sampling):

- **Route A** — the existing rank-ordered 8×8 tiles already have enough numeric
  locality that a tiny window-local exact target beats K1's scattered gathers.
- **Route B** — they do not, but re-tiling by numeric *value* range creates that
  locality and lets whole X-bin × Q-bin rectangles be rejected exactly, before
  any leaf reporter runs.

## 1. The controlling densities

Every number below follows from three measured family densities over the
residual universe M = 2¹⁹ = 524,288:

| family | mean size | mean gap between consecutive values |
|---|---|---|
| X_t (from B) | 507.75 | M / 507.75 = **1032.6** |
| Q_s (from A) | 298.58 | M / 298.58 = **1755.9** |
| Y_u (targets) | 523.52 | M / 523.52 = **1001.5** |

Two consequences fix the entire result.

**(a) The window needed to FILL an 8×8 tile.** Eight consecutive X values span
about 7 × 1032.6 = 7,228 slots; eight consecutive Q values span about
7 × 1755.9 = 12,292. Their sum set therefore spans about **19,520 slots**.

**(b) The window needed to REJECT a rectangle.** A window of W slots contains on
average W / 1001.5 target values, so emptiness requires roughly **W < 1,000
slots**.

These differ by a factor of **19.5**, and the factor is set by the data, not by
the reporter. Any scheme that fills an 8-wide tile dimension has already made
its window ~19× too wide to reject; any scheme narrow enough to reject holds
about one source value per bin.

## 2. Route A: rank tiles are already span-optimal

Measured exact minimal circular windows (max-cyclic-gap definition, so a tile
clustered across the 2¹⁹−1 → 0 wrap is correctly scored as local):

```
all tiles      min=1     mean=19225.0  median=18745  p90=26480  p99=34076  max=60626
full 8x8       min=3110  mean=19463.7  median=18935  p90=26611  p99=34189  max=60626
partial tiles  min=1     mean=13220.9  median=12693  p90=19909  p99=26609  max=45760
```

The measured mean over full tiles, 19,463.7 slots, is **99.7 % of the 19,520
slots predicted by the densities alone**. That is not a coincidence and it is
the heart of the matter: for a sorted family, partitioning into *consecutive*
blocks minimises the total within-block span over all partitions into equal-size
groups (standard exchange argument). Rank tiling on a value-sorted family is
therefore already the span-optimal 8×8 grouping. There is no regrouping left to
find — the tiles are as numerically tight as 8×8 tiles over these families can be.

Consequently the lane mass available to a small local bitmap is negligible:

| window ≤ | tiles | fraction of tiles | fraction of lane mass |
|---|---|---|---|
| 64 | 20 | 2.02e-06 | 3.22e-08 |
| 256 | 30 | 3.03e-06 | 6.44e-08 |
| 1024 | 114 | 1.15e-05 | **4.65e-07** |
| 4096 | 4,184 | 4.23e-04 | 7.89e-05 |
| 16384 | 3,190,545 | 0.3226 | 0.3132 |
| 32768 | 9,741,462 | 0.9849 | 0.9847 |

A 1024-slot local bitmap would serve **0.0000465 %** of the pair mass. The median
tile needs 18,745 slots — 2.3 KiB — which is larger than the 8 KiB-per-family
coarse bitmap K2 already uses divided across families, and far past anything
register- or L1-resident. Route A is closed.

### Why K1's gathers were expensive — measured, not inferred

Per tile, over the existing exact 2¹⁹-bit Y bitmap:

```
distinct bits        mean 62.66   (the 64 sums are essentially all distinct)
distinct 32b words   mean 59.01
distinct cache lines mean 28.91   <=16: 2.4%   <=32: 72.9%   >32: 27.1%
distinct 4KiB pages  mean  1.59   <=1: 41.5%   <=2: 99.86%
```

So K1 paid for ~29 distinct cache lines per tile spread across only 1–2 pages.
It is a **cache-line scatter cost, not a TLB cost** — the accesses are confined
to a small address range but touch nearly half the lines in it. A local bitmap
would have to cover that same ~19,000-slot range to help, which is precisely
what it cannot do cheaply.

### One clean positive result

`W_box = span(X) + span(Q)` equals the exact sum window for **100 % of the
9,890,364 tiles** (mean, median, p90, p95, p99 and max of `W_box / W_exact` are
all exactly 1.000). This is structural: within a rank tile X and Q are
contiguous ascending slices, so the extreme sums x_lo+q_lo and x_hi+q_hi both
occur and the largest cyclic gap is always the wrap gap. A router can therefore
know a tile's exact window in O(1) from four values, with no sum materialization
and no sorting. The bound is perfect — it simply has nothing useful to report,
because the window it names is 18,745 slots wide.

Window reuse is likewise absent: over 9,886,268 consecutive tile pairs within a
packet, the same window start occurred **0 times**, mean Jaccard overlap was
0.164, and the same-aligned-region rate was 0 for every L from 64 to 2048.

## 3. Route B: rejection and fill are the same knob, pulled opposite ways

Exhaustive sweep over 13 symmetric and 6 asymmetric bin geometries, with pair
mass conserved exactly (rejected + surviving = 620,966,064) in all 19.

| B | rectangles | mass rejected | tiles | tiles vs rank | fragmentation |
|---|---|---|---|---|---|
| 16 | 613,614,944 | **99.888 %** | 649,189 | **0.066×** | 59.83 |
| 256 | 513,885,456 | 97.006 % | 8,324,635 | 0.842× | 28.66 |
| 1024 | 299,898,310 | 68.981 % | 58,147,829 | 5.879× | 19.32 |
| 2048 | 159,183,376 | 31.302 % | 83,547,156 | 8.447× | 12.53 |
| 8192 | 16,609,358 | 0.023 % | 24,143,545 | 2.441× | 2.49 |
| 65536 | 262,144 | **0 %** | 11,221,696 | 1.135× | 1.16 |

Read down the table: the two columns that matter move in opposite directions and
never overlap. Small bins reject almost everything but shatter the tiles
(fragmentation 59.8, i.e. 1.07 useful lanes per 64-lane tile); large bins fill
tiles well but reject nothing. **No configuration achieves fragmentation below
1.157, and the rank tiling already achieves 1.019** — every fixed-width value
binning is worse at filling tiles than the rank tiling it would replace.

The rejection column also has a ceiling that is not obvious from the table: at
B = 16 the surviving mass is 694,475 pairs against 619,303 true lanes. The
"filter" is within 12 % of exact. That is the tell.

## 4. The cost that closes it: rejection is not free

At B = 16 the scheme issues **613,614,944 rectangle tests to examine 620,966,064
source pairs — 1.012 pairs per test.** The mean surviving-rectangle window is
1.50 slots. The rejection stage is the original pairwise scan, renamed.

This is not a modelling assumption. The range-emptiness query was implemented
against the existing 64 KiB exact Y bitmap and timed in this executable on a
uniform reservoir (Algorithm R, 65,536 rectangles per configuration) of the real
rectangle population, 51 samples, rotated order, ≥20 ms regions,
`timing_stability = acceptable` (worst relative IQR 0.0366).

```
t_pair (this executable)  0.66909 ns   vs prior MicroReport8 0.6605 ns, agreement 1.013
t_rect  B=16              2.505 ns     B=256  4.218 ns     B=1024 16.55 ns
        B=8192           48.63 ns      B=65536 48.79 ns
```

Charging it:

| B | routing ms | leaf ms @ K3 cost | total ms | ceiling vs 410.175 ms |
|---|---|---|---|---|
| 16 | 1536.9 | 1.9 | 1538.8 | **0.267×** (slower than the scalar scan) |
| 1024 | 4962.1 | 168.6 | 5130.8 | 0.080× |
| 8192 | 807.8 | 70.0 | 877.8 | 0.467× |
| 65536 | 12.8 | 32.5 | 45.3 | **9.048×** |
| *unchanged rank tiling* | *0* | *28.7* | *28.7* | ***14.301×*** |

Even charging each rectangle the absolute floor of a single pair probe — a bound
no range-emptiness implementation can beat, since such a query cannot cost less
than the one masked word read a pair probe already performs — the best of the 19
configurations reaches **12.537×**, against **14.301×** for doing nothing at all
to the tiling. **All 19 configurations lose to the unchanged rank tiling on the
identical optimistic bound.** Route B is closed.

## 5. Selectivity does not improve inside small windows

A natural hope is that narrow windows are where the true hits concentrate. They
are not. True-lane density per valid lane, by window class:

```
<=1024  0.005291     <=4096  0.001147     <=16384 0.0009995
<=2048  0.002443     <=8192  0.001022     <=32768 0.0009962   <=65536 0.0009916
```

Against a global density of 9.973e-04. The small classes look richer only
because they contain a few hundred tiles out of 9.89 million; the mass-weighted
picture is flat. Window width carries essentially no information about where the
answers are.

## 6. Answer to the original question

> Was MicroReport8 slow because the CPU is too slow, or because we gave the CPU
> numerically terrible 8×8 groups?

**Neither.** The groups are not terrible — they are provably span-optimal and
land within 0.3 % of the analytic minimum. The CPU is not the problem either:
K3 already reaches 2.900 ns for 64 packed lanes.

The problem is that the residual universe is 2¹⁹ while a tile carries 64 sums,
and the target family is ~1000× sparser than a tile's window is narrow. A tile's
sums are unavoidably ~19,000 slots apart because the sources are ~1,000 and
~1,700 slots apart. No regrouping of 8 × 8 values out of these families changes
that, because rank grouping already minimises it.

## 7. What this rules out, and what it does not

Ruled out, on measured authentic geometry:

- window-local exact target representations for the current 8×8 leaf, at every
  capacity from 64 to 1024 slots (Route A);
- fixed-width value binning as a pre-leaf exact rejection stage, at every tested
  symmetric width 16…65536 and six asymmetric pairings (Route B);
- more generally, any pre-leaf rejection whose granularity is a rectangle of
  source values, since the rejection rate is governed by the same window width
  that governs tile fill.

Not ruled out, and not tested here:

- changing the *leaf shape* (a wider tile dimension, or a non-square leaf, would
  move the fill/reject tension to a different point on the same curve — the
  density arithmetic in §1 predicts where, and that prediction is checkable);
- reducing the residual universe itself, which is the only quantity in §1 that
  is a free parameter rather than a measured property of the fixture;
- anything about FullTER wall-clock behaviour. Every speedup here is an
  isolated-front-end projection.

## 8. Evidence classes

- **MEASURED** — all geometry (exhaustive, 9,890,364 tiles), all counts, `t_pair`
  and `t_rect` (this executable, this machine, pinned to cpu0 at high priority).
- **PROJECTED** — routing-inclusive totals: a measured per-unit cost multiplied
  by a measured count. No achieved reporter.
- **PRIOR_MEASURED** — 410.175 ms, 138.524 ms / 2.961×, 2.900 ns, 15.471 ns,
  9,890,364 tiles, from the MicroReport8 run on the same machine and fixture.
- **MODEL_ONLY** — the 57.4625 M blocked-bitmap work figure. Not calibrated to
  nanoseconds, not used in any conclusion here.
