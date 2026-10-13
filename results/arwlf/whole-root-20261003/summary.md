# ARWLF whole-root run

Root n108_s0x1001_a0 (n=108, Q=69, seed 0x1001), all 4096 microproblems, one planted final output [1753507, 754371, 6896564] on the anchor microproblem 2730 (plant seed 0xb1a57001). Build Release MSVC 195136257, commit 8e9fe7fdda44 (dirty at configure: 1), pin p-core (cpu 23).

## Runs

| run | backend | chunks | microproblems | whole-root ms | ns per pair | final outputs | planted found | chunk ms min / median / max | off-core chunks | AC throughout | peak WS MiB | DISPATCH ran |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| k2-1 | k2 | 64 | 4,096 | 197,243,163.5 | 7.830 | 1 | yes | 2,693,646.8 / 2,885,428.4 / 4,959,494.5 | 0 | yes | 3,279.5 |  |
| arwlf-1 | arwlf | 1 | 4,096 | 3,575,458.1 | 0.142 | 1 | yes | 3,575,458.1 / 3,575,458.1 / 3,575,458.1 | 0 | yes | 3,278.5 |  |
| arwlf-2 | arwlf | 1 | 4,096 | 3,469,783.8 | 0.138 | 1 | yes | 3,469,783.8 / 3,469,783.8 / 3,469,783.8 | 0 | yes | 3,279.3 |  |
| dispatch-1 | dispatch | 1 | 4,096 | 3,557,074.4 | 0.141 | 1 | yes | 3,557,074.4 / 3,557,074.4 / 3,557,074.4 | 0 | yes | 3,278.6 | ARWLF |
| dispatch-2 | dispatch | 1 | 4,096 | 3,436,054.7 | 0.136 | 1 | yes | 3,436,054.7 / 3,436,054.7 / 3,436,054.7 | 0 | yes | 3,279.3 | ARWLF |

## Exactness

- Complete runs: 5; identical final outputs and structural counts: **yes**; every run reports the planted output: **yes**.
- Final outputs of the whole root: 1 = the planted one + 0 authentic .

## Whole-root time and speedup vs frozen K2

| run | backend | whole-root time | speedup vs K2 |
|---|---|---|---|
| k2-1 | k2 | 197,243.2 s (54.79 h) | 1.00x |
| arwlf-1 | arwlf | 3,575.5 s (0.99 h) | 55.17x |
| arwlf-2 | arwlf | 3,469.8 s (0.96 h) | 56.85x |
| dispatch-1 | dispatch | 3,557.1 s (0.99 h) | 55.45x |
| dispatch-2 | dispatch | 3,436.1 s (0.95 h) | 57.40x |

## Setup (untimed, once per process, ms)

| run | load snapshot | plant | family keys | index + tables | census |
|---|---|---|---|---|---|
| arwlf-1 | 7,577.8 | 270.7 | 258.6 | 135.4 | 27.2 |
| arwlf-2 | 7,905.7 | 328.5 | 262.5 | 150.0 | 28.3 |
| dispatch-1 | 7,681.9 | 310.0 | 277.0 | 156.5 | 29.5 |
| dispatch-2 | 7,333.1 | 282.0 | 247.8 | 131.8 | 19.5 |
| k2-1 | 7,741.4 | 306.7 | 274.6 | 144.7 | 28.6 |

The timed region of every run is the regime campaign's (sink reset + the frozen backend call), so ARWLF's leaf-cache construction is inside its time. Family keys are used by K2_WIDE, ARWLF and DISPATCH, not by K2; the index by all four. A resumed run reports the setup of its last process.

## Interference audit (bench_guard.csv)

- No guard log: interference was not recorded for any run.

