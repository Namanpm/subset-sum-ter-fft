# `src/`

**What it is.** Implementation of the classical TER library (`ter_t0`).

**Purpose in the repository.** Holds the correctness-critical code: exact arithmetic, instance and shape construction, the scalar reference and packed ternary representations, the validators that serve as independent authorities, the frozen T0 merge, the complete depth-3 FullTer solver, the closed Q3REPORT count-only NTT experiment, the tiled reporter and the Stage-C root partition. It contains no FFT or NTT in the solver itself.

**Files directly in this folder:** 18 (besides this README).  
Repository-wide status of every file: [`LEGACY_MAP.md`](../LEGACY_MAP.md).
