# `include/ter/`

**What it is.** Public headers of the classical TER library (`ter_t0`).

**Purpose in the repository.** Declares the contracts that `src/`, `tests/`, `bench/` and the frozen root code build on: exact power-of-two arithmetic, instances, shapes, ternary vectors (reference and packed), validators, FullTer, the Q3REPORT and tiled reporters, the stress generator and root partition. The frozen root backends depend only on a few of these (for example `TerPartitionEntry` from `root_partition.hpp`).

**Files directly in this folder:** 19 (besides this README).  
Repository-wide status of every file: [`LEGACY_MAP.md`](../../LEGACY_MAP.md).
