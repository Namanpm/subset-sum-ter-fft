# `bench/`

**What it is.** Source of the benchmark executables and analysis scripts for the classical TER code and the closed Q3REPORT/tiled/partition experiments.

**Purpose in the repository.** Provides the measurement harnesses for `src/`: `ter_full_bench` (FullTer solves and attempt cost, used by the paper's design-B comparison), `ter_baseline_bench`, `ter_stress_bench`, the Q3REPORT NTT benches, the tiled merge benches and the root-partition bake-off, with the Python analysers and PowerShell campaign scripts that produced `results/tiled-q3report-final-20260829/` and `results/partition/`. The TER² root harnesses live in `ter2_root/campaign/`, not here.

**Files directly in this folder:** 13 (besides this README).  
Repository-wide status of every file: [`LEGACY_MAP.md`](../LEGACY_MAP.md).
