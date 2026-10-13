# Root-queue generalization: QUEUE_DENSE vs frozen K2

cells read: 8; usable (valid, AC power, timed): 8; listed but excluded: 0

## Per-cell results (backend-total medians, ms)

| cell | family | n | scale | P0 | rho_q | bits/word | K2 | QUEUE | S | S steady | lane-skip |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| authentic_n72_s0x46554c4c544552_anchor | AUTHENTIC_FULLTER | 72 | 1.000 | 620,966,064 | 0.2129 | 1.122 | 310.03 | 179.57 | 1.727 | 1.755 | 434.29 |
| authentic_n72_s0x1001 | AUTHENTIC_FULLTER | 72 | 1.000 | 626,628,516 | 0.1346 | 1.072 | 317.43 | 211.23 | 1.503 | 1.522 | 562.74 |
| authentic_n72_s0x2003 | AUTHENTIC_FULLTER | 72 | 1.000 | 1,158,653,232 | 0.2187 | 1.126 | 579.06 | 301.20 | 1.923 | 1.941 | - |
| authentic_n72_s0x3007 | AUTHENTIC_FULLTER | 72 | 1.000 | 1,317,980,892 | 0.2446 | 1.144 | 694.98 | 396.16 | 1.754 | 1.768 | - |
| authentic_n72_s0x4009 | AUTHENTIC_FULLTER | 72 | 1.000 | 1,192,101,559 | 0.2229 | 1.129 | 613.12 | 324.69 | 1.888 | 1.906 | - |
| authentic_n72_s0x500b | AUTHENTIC_FULLTER | 72 | 1.000 | 660,452,800 | 0.2240 | 1.130 | 353.54 | 206.66 | 1.711 | 1.736 | - |
| authentic_n72_s0x600d | AUTHENTIC_FULLTER | 72 | 1.000 | 1,155,867,297 | 0.2288 | 1.134 | 623.11 | 357.96 | 1.741 | 1.756 | - |
| authentic_n72_s0x700f | AUTHENTIC_FULLTER | 72 | 1.000 | 1,155,861,798 | 0.2235 | 1.129 | 606.51 | 313.94 | 1.932 | 1.950 | - |

## Aggregate speedup S = median(K2) / median(QUEUE_DENSE)

| group | cells | geo-mean | median | p25 | p75 | min | max | wins | steady geo-mean |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ALL | 8 | 1.767 | 1.748 | 1.723 | 1.897 | 1.503 | 1.932 | 8/8 | 1.787 |
| AUTHENTIC_FULLTER | 8 | 1.767 | 1.748 | 1.723 | 1.897 | 1.503 | 1.932 | 8/8 | 1.787 |
| AUTHENTIC_FULLTER (declared seeds only, anchor excluded) | 7 | 1.773 | 1.754 | 1.726 | 1.905 | 1.503 | 1.932 | 7/7 | 1.791 |
| CONTROLLED (both synthetic families) | 0 | - | - | - | - | - | - | - | - |
| SYNTHETIC_UNIFORM | 0 | - | - | - | - | - | - | - | - |
| RESIDUAL_PROFILE_EMULATION | 0 | - | - | - | - | - | - | - | - |

## Does workload telemetry predict the speedup?

| predictor of ln S | Pearson r | Spearman rho |
|---|---:|---:|
| ln rho_q | 0.748 | 0.238 |
| bits per queued word | 0.717 | 0.238 |
| mean popcount | 0.717 | 0.238 |
| ln P0 | 0.699 | 0.595 |
| ln candidate bits | 0.643 | 0.476 |
| ln (candidate bits / P0) | -0.469 | -0.167 |

OLS fit over all usable cells: ln S = 1.0859 + 0.3319 ln rho_q, R^2 = 0.559; predicted break-even rho_q (S = 1) = 0.0379.

## Pre-registered interpretation: EXCELLENT

queue wins across most authentic and controlled cells with S_G >= 1.4

The automated rule above tests a MONOTONIC association (Spearman rank correlation
and a linear fit in ln rho_q).  It is reported unchanged.  The section below was
added after the data were seen and is labelled post-hoc accordingly.

## POST-HOC (not pre-registered): non-monotonic dependence on density

Quadratic fit over all usable cells: ln S = -1.1965 (ln rho)^2 + -3.8183 ln rho + -2.4390, R^2 = 0.656, peak at rho_q = 0.2028

