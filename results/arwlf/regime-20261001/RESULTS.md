# ARWLF regime campaign: frozen result record

Campaign `regime-20261001`, run 2026-10-01/02. Preregistration and Amendment 1: `ARWLF_REGIME_CAMPAIGN.md`.
Generated tables: `summary.md` (from `scripts/summarize_arwlf_regime_campaign.py`).
Figure data: `speedup_vs_L.csv`, `regret_vs_L.csv`.

**Build:** `build_freeze.json` (executable SHA-256 `932d2803…e7ef`), `source_manifest.txt`, `build_flags.txt`.
Commit 3e6d164 reproduces the campaign sources: of the 137 manifest files, only `CMakeLists.txt` differs, by the
later `ter_frozen_root_backends` test registration.

**Inputs:** `snapshots.json` lists the 10 authentic root snapshots by SHA-256. They are not versioned.

**Machine:** Intel Core Ultra 9 275HX; MSVC 195136257; pinned to P-core 23 with EcoQoS off; AC power.

**Labels:** finite-n, constant-factor results only. No asymptotic or TER-exponent claim.

## Integrity

- 224 cases: 221 timed and exact, plus 3 recorded `UNREACHABLE` targets (L=96 on 0x1001).
- Every timed case passed every gate: each backend's final witness list equals frozen K2's; the v3 count and hash gates hold; the census geometry equals the counted ARWLF run.
- 0 timed samples off the pinned core; 0 `RESOURCE_LIMIT` cases.

## Headline (authentic n=108 microproblems; ARWLF / frozen K2 speedup)

| Set | Cases (roots) | Geometric mean | 95 % CI (cases) | 95 % CI (roots) | Range |
|---|---|---|---|---|---|
| Calibration (0x1001, 0x2003, 0x3007) | 14 (3) | **53.1x** | 50.3-57.1 | 48.2-57.6 | 46.5-78.1x |
| Unseen holdout (0x4001, 0x5003, 0x6007) | 15 (3) | **52.9x** | 51.2-55.0 | 51.8-53.8 | 46.9-63.2x |

- ARWLF vs K2_WIDE on the same 29 cases: 29.6-33.3x (median 32.7x).
- Values above about 55x come from slow K2 samples. K2 ranged 82-131 s on comparable microproblems, while ARWLF stayed at 1.58-1.76 s.
- Within-root geometry is near-uniform: L within +/-1.6 % and pairs within +/-2 % across all 4,096 microproblems of every n=108 root.

## Dispatcher (frozen v3)

| Workload | Ran | Regret (DISPATCH / fastest backend) |
|---|---|---|
| Authentic n=108 microproblems | ARWLF 29/29 | 1.002-1.026 (median 1.010) |
| Authentic n=72 whole roots | QUEUE_DENSE 4/4 | 1.04-1.14 |
| Thinned n=108, L = 20-96 | ARWLF | 1.02-1.08 |
| Thinned n=108, L = 2-16 | K2_WIDE (ARWLF admitted only at L >= 16) | 2.7-17x |
| Single n=72 microproblems (about 0.1 ms of work) | K2_WIDE | 64-173x (probe overhead) |

## Crossover (source thinning of the anchor microproblem)

| Target L | ARWLF / K2 (3 calibration roots, geometric mean over 3 seeds) |
|---|---|
| 1 | 0.04-0.09x: ARWLF loses |
| 2 | 4.4-6.7x |
| 4 | 9.3-11.8x |
| 8 | 16.9-17.9x |
| 16 | 23.3-24.5x |
| 32 | 29.8-41.5x |
| 64 | 41.1-53.1x |
| 96 | 49.4-64.2x |

- ARWLF loses near L = 1 and wins by L = 2 on all three tested roots.
- This was confirmed under the final protocol at L = 1 and L = 2 on each root (`qx:*:confirm` cases). At L = 2, ARWLF is 2.7-3.4x faster than K2_WIDE, the fastest alternative.

## Target-density ablation (Y_u thinned; Q and X untouched)

| Y kept | ARWLF / K2 | ARWLF / K2_WIDE |
|---|---|---|
| all | about 53x | about 33x |
| 1/2 | 28.9-31.3x | 20.3-20.9x |
| 1/4 | 18.2-19.7x | 13.1-13.6x |
| 1/8 | 13.4-14.5x | 10.3-10.7x |

Halving Y roughly halves K2's work (its 25-bit survivor flood scales with |Y|) but barely changes ARWLF. A large part of the 53x is therefore survivor-flood suppression, not SIMD throughput alone.

## n=72 (sparse, L about 1)

| Root | K2 ms | QUEUE_DENSE ms (vs K2) | ARWLF vs K2 |
|---|---|---|---|
| anchor (calibration) | 301.5 | 167.6 (1.80x) | 0.27x |
| 0x7201 (holdout) | 327.7 | 269.9 (1.21x) | 0.14x |
| 0x7203 (holdout) | 545.3 | 298.1 (1.83x) | 0.30x |
| 0x7207 (holdout) | 551.5 | 299.2 (1.84x) | 0.28x |

## Separate matched run (Phase A; v3 harness, not the campaign harness)

`results/arwlf/campaign-20261001-v3-0x3007/`, 0x3007 anchor, 3 + 7 matched rounds, all 35 samples on P-core 23, EXACT:

| K2 | K2_WIDE | ARWLF | DISPATCH | K2 sweep floor |
|---|---|---|---|---|
| 159,346 ms | 60,808 ms | 1,997 ms | 2,010 ms (ran ARWLF) | 4,643 ms |

- The ARWLF / K2 ratio is 79.8x here, against 52.8x for the same microproblem in the campaign.
- K2 is the noisy side (p25-p75: 146-171 s here, versus 86 s in the campaign); ARWLF ran 1.6-2.0 s in both.
- Report the campaign figure.

## Caveats

- Strata and thinning cases use the discovery protocol (1 warmup + 3 timed rounds). Anchors, whole roots and confirmations use the final protocol (3 + 7).
- By Amendment 1, the n=108 strata were reduced to the four corner cells per root.
- The fixed-CPU sentinel stayed at 65.4-66.7 ms for every final, anchor, strata, thin-y and confirmation run. It read 74-92 ms only at process starts during resumes and around some thin-qx runs. Absolute times of those points may be inflated; their within-case ratios are matched.
- All n=108 timings are per microproblem (1 of 4,096). No whole-root n=108 time has been measured.
