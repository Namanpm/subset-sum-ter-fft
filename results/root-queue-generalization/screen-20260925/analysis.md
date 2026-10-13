# Root-queue generalization: QUEUE_DENSE vs frozen K2

cells read: 68; usable (valid, AC power, timed): 58; listed but excluded: 10

## Per-cell results (backend-total medians, ms)

| cell | family | n | scale | P0 | rho_q | bits/word | K2 | QUEUE | S | S steady | lane-skip |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| authentic_n72_s0x46554c4c544552_anchor | AUTHENTIC_FULLTER | 72 | 1.000 | 620,966,064 | 0.2129 | 1.122 | 305.79 | 181.06 | 1.689 | 1.716 | 430.58 |
| authentic_n72_s0x1001 | AUTHENTIC_FULLTER | 72 | 1.000 | 626,628,516 | 0.1346 | 1.072 | 313.20 | 219.86 | 1.425 | 1.443 | 579.11 |
| authentic_n72_s0x2003 | AUTHENTIC_FULLTER | 72 | 1.000 | 1,158,653,232 | 0.2187 | 1.126 | 567.89 | 315.87 | 1.798 | 1.815 | - |
| authentic_n72_s0x3007 | AUTHENTIC_FULLTER | 72 | 1.000 | 1,317,980,892 | 0.2446 | 1.144 | 665.06 | 374.95 | 1.774 | 1.788 | - |
| authentic_n72_s0x4009 | AUTHENTIC_FULLTER | 72 | 1.000 | 1,192,101,559 | 0.2229 | 1.129 | 583.67 | 298.36 | 1.956 | 1.976 | - |
| authentic_n72_s0x500b | AUTHENTIC_FULLTER | 72 | 1.000 | 660,452,800 | 0.2240 | 1.130 | 323.58 | 169.06 | 1.914 | 1.945 | - |
| authentic_n72_s0x600d | AUTHENTIC_FULLTER | 72 | 1.000 | 1,155,867,297 | 0.2288 | 1.134 | 563.33 | 294.65 | 1.912 | 1.929 | - |
| authentic_n72_s0x700f | AUTHENTIC_FULLTER | 72 | 1.000 | 1,155,861,798 | 0.2235 | 1.129 | 561.26 | 288.55 | 1.945 | 1.963 | - |
| uniform_x0.25_s0x1001 | SYNTHETIC_UNIFORM | 0 | 0.250 | 38,808,348 | 0.0155 | 1.008 | 17.62 | 22.88 | 0.770 | 0.850 | 20.99 |
| profile_x0.25_s0x1001 | RESIDUAL_PROFILE_EMULATION | 0 | 0.250 | 38,921,190 | 0.0155 | 1.008 | 17.29 | 22.53 | 0.767 | 0.840 | 21.03 |
| uniform_x0.25_s0x2003 | SYNTHETIC_UNIFORM | 0 | 0.250 | 38,808,348 | 0.0155 | 1.008 | 17.47 | 23.64 | 0.739 | 0.808 | - |
| profile_x0.25_s0x2003 | RESIDUAL_PROFILE_EMULATION | 0 | 0.250 | 38,921,190 | 0.0155 | 1.007 | 17.45 | 23.48 | 0.743 | 0.815 | - |
| uniform_x0.25_s0x3007 | SYNTHETIC_UNIFORM | 0 | 0.250 | 38,808,348 | 0.0155 | 1.007 | 17.77 | 24.21 | 0.734 | 0.806 | - |
| profile_x0.25_s0x3007 | RESIDUAL_PROFILE_EMULATION | 0 | 0.250 | 38,921,190 | 0.0154 | 1.008 | 17.75 | 25.12 | 0.707 | 0.775 | - |
| uniform_x0.25_s0x4009 | SYNTHETIC_UNIFORM | 0 | 0.250 | 38,808,348 | 0.0155 | 1.007 | 17.72 | 23.30 | 0.761 | 0.837 | - |
| profile_x0.25_s0x4009 | RESIDUAL_PROFILE_EMULATION | 0 | 0.250 | 38,921,190 | 0.0155 | 1.008 | 17.50 | 23.52 | 0.744 | 0.814 | - |
| uniform_x0.25_s0x500b | SYNTHETIC_UNIFORM | 0 | 0.250 | 38,808,348 | 0.0155 | 1.008 | 17.82 | 23.11 | 0.771 | 0.844 | - |
| profile_x0.25_s0x500b | RESIDUAL_PROFILE_EMULATION | 0 | 0.250 | 38,921,190 | 0.0155 | 1.008 | 17.56 | 23.98 | 0.732 | 0.801 | - |
| uniform_x0.50_s0x1001 | SYNTHETIC_UNIFORM | 0 | 0.500 | 155,249,640 | 0.0595 | 1.031 | 68.70 | 54.09 | 1.270 | 1.330 | 90.14 |
| profile_x0.50_s0x1001 | RESIDUAL_PROFILE_EMULATION | 0 | 0.500 | 155,672,315 | 0.0597 | 1.030 | 69.84 | 53.38 | 1.308 | 1.370 | 88.41 |
| uniform_x0.50_s0x2003 | SYNTHETIC_UNIFORM | 0 | 0.500 | 155,249,640 | 0.0595 | 1.030 | 69.06 | 53.14 | 1.300 | 1.362 | - |
| profile_x0.50_s0x2003 | RESIDUAL_PROFILE_EMULATION | 0 | 0.500 | 155,672,315 | 0.0596 | 1.031 | 69.73 | 53.27 | 1.309 | 1.371 | - |
| uniform_x0.50_s0x3007 | SYNTHETIC_UNIFORM | 0 | 0.500 | 155,249,640 | 0.0594 | 1.031 | 68.63 | 54.79 | 1.253 | 1.310 | - |
| profile_x0.50_s0x3007 | RESIDUAL_PROFILE_EMULATION | 0 | 0.500 | 155,672,315 | 0.0595 | 1.030 | 69.53 | 52.59 | 1.322 | 1.386 | - |
| uniform_x0.50_s0x4009 | SYNTHETIC_UNIFORM | 0 | 0.500 | 155,249,640 | 0.0597 | 1.031 | 68.86 | 53.34 | 1.291 | 1.351 | - |
| profile_x0.50_s0x4009 | RESIDUAL_PROFILE_EMULATION | 0 | 0.500 | 155,672,315 | 0.0597 | 1.030 | 69.93 | 53.21 | 1.314 | 1.377 | - |
| uniform_x0.50_s0x500b | SYNTHETIC_UNIFORM | 0 | 0.500 | 155,249,640 | 0.0597 | 1.031 | 69.08 | 53.13 | 1.300 | 1.362 | - |
| profile_x0.50_s0x500b | RESIDUAL_PROFILE_EMULATION | 0 | 0.500 | 155,672,315 | 0.0596 | 1.031 | 70.11 | 53.79 | 1.304 | 1.365 | - |
| uniform_x1.00_s0x1001 | SYNTHETIC_UNIFORM | 0 | 1.000 | 620,966,064 | 0.2126 | 1.121 | 313.77 | 173.46 | 1.809 | 1.840 | 427.08 |
| profile_x1.00_s0x1001 | RESIDUAL_PROFILE_EMULATION | 0 | 1.000 | 620,966,064 | 0.2122 | 1.123 | 310.62 | 173.40 | 1.791 | 1.822 | 428.60 |
| uniform_x1.00_s0x2003 | SYNTHETIC_UNIFORM | 0 | 1.000 | 620,966,064 | 0.2122 | 1.122 | 312.66 | 173.60 | 1.801 | 1.832 | - |
| profile_x1.00_s0x2003 | RESIDUAL_PROFILE_EMULATION | 0 | 1.000 | 620,966,064 | 0.2117 | 1.122 | 310.43 | 174.98 | 1.774 | 1.804 | - |
| uniform_x1.00_s0x3007 | SYNTHETIC_UNIFORM | 0 | 1.000 | 620,966,064 | 0.2120 | 1.122 | 310.28 | 172.95 | 1.794 | 1.825 | - |
| profile_x1.00_s0x3007 | RESIDUAL_PROFILE_EMULATION | 0 | 1.000 | 620,966,064 | 0.2117 | 1.123 | 309.61 | 170.88 | 1.812 | 1.843 | - |
| uniform_x1.00_s0x4009 | SYNTHETIC_UNIFORM | 0 | 1.000 | 620,966,064 | 0.2123 | 1.122 | 312.24 | 172.47 | 1.810 | 1.841 | - |
| profile_x1.00_s0x4009 | RESIDUAL_PROFILE_EMULATION | 0 | 1.000 | 620,966,064 | 0.2117 | 1.121 | 309.65 | 173.53 | 1.784 | 1.815 | - |
| uniform_x1.00_s0x500b | SYNTHETIC_UNIFORM | 0 | 1.000 | 620,966,064 | 0.2118 | 1.122 | 314.59 | 174.26 | 1.805 | 1.836 | - |
| profile_x1.00_s0x500b | RESIDUAL_PROFILE_EMULATION | 0 | 1.000 | 620,966,064 | 0.2129 | 1.122 | 309.69 | 172.86 | 1.792 | 1.822 | - |
| uniform_x2.00_s0x1001 | SYNTHETIC_UNIFORM | 0 | 2.000 | 2,483,864,256 | 0.5942 | 1.510 | 1437.13 | 1255.89 | 1.144 | 1.148 | 1954.35 |
| profile_x2.00_s0x1001 | RESIDUAL_PROFILE_EMULATION | 0 | 2.000 | 2,483,864,256 | 0.5936 | 1.506 | 1400.02 | 1136.34 | 1.232 | 1.236 | 1801.88 |
| uniform_x2.00_s0x2003 | SYNTHETIC_UNIFORM | 0 | 2.000 | 2,483,864,256 | 0.5938 | 1.508 | 1365.03 | 1117.66 | 1.221 | 1.225 | - |
| profile_x2.00_s0x2003 | RESIDUAL_PROFILE_EMULATION | 0 | 2.000 | 2,483,864,256 | 0.5935 | 1.509 | 1359.96 | 1106.04 | 1.230 | 1.234 | - |
| uniform_x2.00_s0x3007 | SYNTHETIC_UNIFORM | 0 | 2.000 | 2,483,864,256 | 0.5946 | 1.506 | 1346.91 | 1095.68 | 1.229 | 1.233 | - |
| profile_x2.00_s0x3007 | RESIDUAL_PROFILE_EMULATION | 0 | 2.000 | 2,483,864,256 | 0.5934 | 1.506 | 1346.92 | 1097.77 | 1.227 | 1.231 | - |
| uniform_x2.00_s0x4009 | SYNTHETIC_UNIFORM | 0 | 2.000 | 2,483,864,256 | 0.5936 | 1.505 | 1341.27 | 1090.79 | 1.230 | 1.234 | - |
| profile_x2.00_s0x4009 | RESIDUAL_PROFILE_EMULATION | 0 | 2.000 | 2,483,864,256 | 0.5940 | 1.507 | 1330.72 | 1083.34 | 1.228 | 1.233 | - |
| uniform_x2.00_s0x500b | SYNTHETIC_UNIFORM | 0 | 2.000 | 2,483,864,256 | 0.5945 | 1.506 | 1322.29 | 1075.90 | 1.229 | 1.233 | - |
| profile_x2.00_s0x500b | RESIDUAL_PROFILE_EMULATION | 0 | 2.000 | 2,483,864,256 | 0.5937 | 1.509 | 1318.72 | 1074.80 | 1.227 | 1.231 | - |
| uniform_x4.00_s0x1001 | SYNTHETIC_UNIFORM | 0 | 4.000 | 9,935,457,024 | 0.9613 | 3.294 | 6000.93 | 6275.14 | 0.956 | 0.957 | 8190.24 |
| profile_x4.00_s0x1001 | RESIDUAL_PROFILE_EMULATION | 0 | 4.000 | 9,935,457,024 | 0.9602 | 3.296 | 5957.94 | 6209.36 | 0.960 | 0.960 | 8121.09 |
| uniform_x4.00_s0x2003 | SYNTHETIC_UNIFORM | 0 | 4.000 | 9,935,457,024 | 0.9615 | 3.294 | 5927.47 | 6228.17 | 0.952 | 0.952 | - |
| profile_x4.00_s0x2003 | RESIDUAL_PROFILE_EMULATION | 0 | 4.000 | 9,935,457,024 | 0.9607 | 3.296 | 5929.39 | 6174.62 | 0.960 | 0.961 | - |
| uniform_x4.00_s0x3007 | SYNTHETIC_UNIFORM | 0 | 4.000 | 9,935,457,024 | 0.9619 | 3.296 | 6102.72 | 6421.02 | 0.950 | 0.951 | - |
| profile_x4.00_s0x3007 | RESIDUAL_PROFILE_EMULATION | 0 | 4.000 | 9,935,457,024 | 0.9602 | 3.296 | 6156.91 | 6502.79 | 0.947 | 0.948 | - |
| uniform_x4.00_s0x4009 | SYNTHETIC_UNIFORM | 0 | 4.000 | 9,935,457,024 | 0.9612 | 3.301 | 6329.67 | 6643.28 | 0.953 | 0.954 | - |
| profile_x4.00_s0x4009 | RESIDUAL_PROFILE_EMULATION | 0 | 4.000 | 9,935,457,024 | 0.9599 | 3.294 | 6266.26 | 6593.01 | 0.950 | 0.951 | - |
| uniform_x4.00_s0x500b | SYNTHETIC_UNIFORM | 0 | 4.000 | 9,935,457,024 | 0.9619 | 3.299 | 6216.25 | 6533.97 | 0.951 | 0.952 | - |
| profile_x4.00_s0x500b | RESIDUAL_PROFILE_EMULATION | 0 | 4.000 | 9,935,457,024 | 0.9606 | 3.299 | 6349.66 | 6716.21 | 0.945 | 0.946 | - |

## Cells not entering aggregates

- authentic_n36_s0x1001: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n36_s0x2003: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n36_s0x3007: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n36_s0x4009: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n36_s0x500b: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n36_s0x600d: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n36_s0x700f: status=NotApplicable valid=0 power_ok=0 quotient width Q=24 is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit residual microproblem does not exist at this n
- authentic_n108_s0x1001: status=ResourceLimit valid=0 power_ok=0 root capture did not complete; solver status=ResourceLimit (declared memory limit 4096 MiB; solver peak working bytes 3033378032: root capture exceeds the declared memory limit)
- authentic_n108_s0x2003: status=ResourceLimit valid=0 power_ok=0 root capture did not complete; solver status=ResourceLimit (declared memory limit 4096 MiB; solver peak working bytes 3222511560: root capture exceeds the declared memory limit)
- authentic_n108_s0x3007: status=ResourceLimit valid=0 power_ok=0 root capture did not complete; solver status=ResourceLimit (declared memory limit 4096 MiB; solver peak working bytes 3219730768: root capture exceeds the declared memory limit)

## Aggregate speedup S = median(K2) / median(QUEUE_DENSE)

| group | cells | geo-mean | median | p25 | p75 | min | max | wins | steady geo-mean |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ALL | 58 | 1.224 | 1.231 | 0.951 | 1.774 | 0.707 | 1.956 | 38/58 | 1.260 |
| AUTHENTIC_FULLTER | 8 | 1.793 | 1.855 | 1.752 | 1.922 | 1.425 | 1.956 | 8/8 | 1.813 |
| AUTHENTIC_FULLTER (declared seeds only, anchor excluded) | 7 | 1.808 | 1.912 | 1.786 | 1.930 | 1.425 | 1.956 | 7/7 | 1.828 |
| CONTROLLED (both synthetic families) | 50 | 1.151 | 1.229 | 0.950 | 1.309 | 0.707 | 1.812 | 30/50 | 1.188 |
| SYNTHETIC_UNIFORM | 25 | 1.150 | 1.229 | 0.951 | 1.300 | 0.734 | 1.810 | 15/25 | 1.188 |
| RESIDUAL_PROFILE_EMULATION | 25 | 1.152 | 1.228 | 0.947 | 1.314 | 0.707 | 1.812 | 15/25 | 1.189 |

## Controlled density sweep (geometric mean over seeds)

| family | scale | cells | mean rho_q | mean bits/word | geo-mean S | min S | max S | K2 median ms (geo) | QUEUE median ms (geo) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SYNTHETIC_UNIFORM | 0.25 | 5 | 0.0155 | 1.008 | 0.755 | 0.734 | 0.771 | 17.68 | 23.42 |
| SYNTHETIC_UNIFORM | 0.50 | 5 | 0.0596 | 1.031 | 1.283 | 1.253 | 1.300 | 68.87 | 53.69 |
| SYNTHETIC_UNIFORM | 1.00 | 5 | 0.2121 | 1.122 | 1.804 | 1.794 | 1.810 | 312.70 | 173.35 |
| SYNTHETIC_UNIFORM | 2.00 | 5 | 0.5942 | 1.507 | 1.210 | 1.144 | 1.230 | 1361.96 | 1125.36 |
| SYNTHETIC_UNIFORM | 4.00 | 5 | 0.9616 | 3.297 | 0.953 | 0.950 | 0.956 | 6113.70 | 6418.44 |
| RESIDUAL_PROFILE_EMULATION | 0.25 | 5 | 0.0155 | 1.008 | 0.738 | 0.707 | 0.767 | 17.51 | 23.71 |
| RESIDUAL_PROFILE_EMULATION | 0.50 | 5 | 0.0596 | 1.030 | 1.311 | 1.304 | 1.322 | 69.83 | 53.25 |
| RESIDUAL_PROFILE_EMULATION | 1.00 | 5 | 0.2121 | 1.122 | 1.791 | 1.774 | 1.812 | 310.00 | 173.12 |
| RESIDUAL_PROFILE_EMULATION | 2.00 | 5 | 0.5936 | 1.508 | 1.229 | 1.227 | 1.232 | 1350.98 | 1099.45 |
| RESIDUAL_PROFILE_EMULATION | 4.00 | 5 | 0.9603 | 3.296 | 0.952 | 0.945 | 0.960 | 6129.79 | 6435.66 |

## Does workload telemetry predict the speedup?

| predictor of ln S | Pearson r | Spearman rho |
|---|---:|---:|
| ln rho_q | 0.327 | 0.077 |
| bits per queued word | -0.325 | 0.073 |
| mean popcount | -0.325 | 0.073 |
| ln P0 | 0.217 | 0.071 |
| ln candidate bits | 0.216 | 0.069 |
| ln (candidate bits / P0) | 0.212 | 0.059 |

OLS fit over all usable cells: ln S = 0.3332 + 0.0736 ln rho_q, R^2 = 0.107; predicted break-even rho_q (S = 1) = 0.0108.

## Pre-registered interpretation: BAD_SPECIALIZATION

speedup is not explained by the measured workload geometry

The automated rule above tests a MONOTONIC association (Spearman rank correlation
and a linear fit in ln rho_q).  It is reported unchanged.  The section below was
added after the data were seen and is labelled post-hoc accordingly.

## POST-HOC (not pre-registered): non-monotonic dependence on density

Share of controlled-cell variance in ln S explained by (family, scale): 0.9974
Quadratic fit over all usable cells: ln S = -0.1650 (ln rho)^2 + -0.6198 ln rho + -0.0513, R^2 = 0.934, peak at rho_q = 0.1528

| family | between scales | scale at S = 1 | rho_q at S = 1 | direction |
|---|---|---:|---:|---|
| SYNTHETIC_UNIFORM | 0.25-0.50 | 0.361 | 0.0317 | queue starts winning |
| SYNTHETIC_UNIFORM | 2.00-4.00 | 3.475 | 0.8720 | K2 starts winning |
| RESIDUAL_PROFILE_EMULATION | 0.25-0.50 | 0.360 | 0.0315 | queue starts winning |
| RESIDUAL_PROFILE_EMULATION | 2.00-4.00 | 3.504 | 0.8760 | K2 starts winning |

