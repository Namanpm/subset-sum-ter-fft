# RETIRED TER² experiments

**Status: RETIRED. Not built, not tested, not maintained.**

These are closed TER² root experiments. Each was exact, and each lost to K2 or was superseded, for a measured reason recorded below. They were moved here on 2026-10-02 so that `ter2_root/` reads as the live ARWLF line.

The code is kept verbatim for provenance. It is **not** part of the CMake build; its CMake blocks were removed. The last commit where every one of them built and passed its tests is tag **`pre-cleanup-3eaad92`** (commit 3eaad92). To run one, check out that tag. To revive one in place, re-add a CMake block in `ter2_root/CMakeLists.txt`; the headers it needs are in `ter2_root/frozen/`.

Code these experiments contributed to the live backends is **not** here. The MicroReport8 kernel and dataset, the translated-tag dataset and locator, the radix-13 locator and packed route, and `ter_a64_chqr.cpp` (which hosts K2_WIDE) are frozen dependencies in `ter2_root/frozen/`. Their tests stay live in `ter2_root/tests/`.

| Folder | Experiment | Verdict (from the result artifacts) | Results |
|---|---|---|---|
| `convsig/` | Exact short convolution with source-signature decoding (GPU NTT); dataset exporter, CUDA kernels, Python tests | **TRANSFORM_NO_GO**: 145.0 ms against a frozen 145 ms gate (expected 105 ms). Exact GPU brute force did the same job in 26.7 ms | `results/convolution-signature-inference/` |
| `windowing/` | Residual windowing: window-local targets and value-based retiling; probe, geometry header, spike script, tests | **WINDOWING_GEOMETRY_UNPROMISING**: residual family gaps make 8x8 tiles about 19.5x too sparse | `results/ter2-windowing-2026090{3,5}/` |
| `microreport8_probe/` | MicroReport8 packed 8x8 leaf reporters (the probe; its kernel is frozen) | **TOO_SLOW**: best exact-or-superset front end projects 2.96x | `results/microreport8-20260902/` |
| `translated_tag_bench/` | Translated-tag locator benchmark (its dataset and locator are frozen) | **NO_GO_SLOWER_THAN_K2** | `results/translated-tag-locator/` |
| `a64_chqr_bench/` | A64-CHQR digit-trie carry-aware quotient refinement: bench and campaign script (the algorithm and K2_WIDE stay in `frozen/ter_a64_chqr.cpp`) | **PAIR_BOUND**: refine work / pairs = 1.00-1.02 at n=72 and n=108; slower than K2_WIDE | `results/a64-chqr/` |
| `root_queue_generalization/` | QUEUE_DENSE generalization campaign: bench, runner, analysis | **Completed.** QUEUE_DENSE wins on authentic n=72 (8/8 roots, geometric mean 1.77x) inside a density band; not applicable at n=108 (capacity). QUEUE_DENSE itself is frozen and still measured by the ARWLF campaign | `results/root-queue-generalization/` |
| `stage_d_bitset_join/` | Standalone C reference kernel for the Stage-D pair-listing floor: every (a, b) with (a + b) mod 2^K in a 2 MB target bitset (never part of the CMake build) | **Reference only.** Per its own header: not output-sensitive; about 1.7 ns per probe single-thread, measured on a different machine and never under the campaign protocol | none |
| `root_pairspace_probe/` | Stage-D region-pruning pair-space probe | **Analysis complete.** Pruning power is real (up to 250x less work if survivors could be enumerated output-optimally at n=72), but enumeration cost stays near P0. Open problem | `docs/root_pair_space_analysis.md` |

See also `docs/history/SUPERSEDED.md`.
