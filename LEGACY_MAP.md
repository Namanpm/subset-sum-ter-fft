# Legacy map and reviewer guide

Written 2026-10-12 from the working tree (HEAD 8e9fe7f plus uncommitted work). Every statement
below was checked against the files named; nothing was re-run. It supersedes the dated survey
`docs/idea_inventory_2026-10-02.md`, whose paths predate the 2026-10-02 move of the root line into
`ter2_root/`.

This repository holds three generations of work: the classical TER solver (Aug 2026), a series of
closed L2->L1 and L1->L0 experiments (Aug-Sep 2026), and the ARWLF root line that the paper
reports (Sep-Oct 2026). Most of the older material is kept deliberately as negative results and
baselines. This file says, for each item, what it is, whether anything live depends on it, and
whether a reviewer of the paper should read it.

## Status labels

| Label | Meaning |
|---|---|
| **PAPER** | Code or data the manuscript's numbers come from. |
| **LIVE** | Built, tested, used by current code, but not itself a paper result. |
| **FROZEN** | Pinned by SHA-256 (`ter2_root/frozen_root_backends.sha256`, CTest `ter_frozen_root_backends`). Never edited, even to fix comments. |
| **CLOSED (built)** | Experiment finished with a recorded verdict; code still compiles and its tests still run. |
| **RETIRED** | Experiment finished; code kept verbatim, **not built** (`ter2_root/retired/`). |
| **HISTORICAL** | Plan, spec or note superseded by later work. Kept as a record; not a description of current code. |
| **STALE ARTIFACT** | A generated file that no longer matches its current source. |
| **SCRATCH** | Local, git-ignored, no research value (`.attic/`). |

---

## 1. Reviewer path: what supports the paper

The article is `paper/ARWLF_Naman_PM.docx`, built from `paper/manuscript.md`. Everything a claim in
it rests on is in this chain:

| Claim area | Code | Evidence (frozen copy in `paper/data/`) | Raw source |
|---|---|---|---|
| ARWLF, K2, K2-WIDE, QUEUE-DENSE, dispatcher | `ter2_root/frozen/` (29 files) | — | `FROZEN_ROOT_BACKENDS.md` |
| Exactness proofs | `paper/manuscript.md` §4-§6 | `ter2_root/tests/` (exactness, planted, differential) | `ter2_root/tests/ter_synthetic_root.hpp` (brute-force oracle) |
| 53.1x / 52.9x microproblem gain (Tables 1-3) | `ter2_root/campaign/ter_arwlf_campaign*` | `regime_cases.csv`, `regime_samples.csv` | `results/arwlf/regime-20261001/` (preregistered in `ARWLF_REGIME_CAMPAIGN.md`) |
| Constructed positive outputs (§8.5) | same, phase `plant` | `positive_cases.csv`, `positive_samples.csv` | `results/arwlf/positive-20261003/` |
| Complete n=108 prepared root, 55.2-57.4x (§8.6) | `ter2_root/campaign/ter_arwlf_whole_root*` | `whole_root_complete.jsonl`, `whole_root_cutoff.jsonl` | `results/arwlf/whole-root-20261003/` (`ARWLF_WHOLE_ROOT.md`) |
| Staged solver integration, n=72 and n=108 (§8.7) | `ter2_root/campaign/ter_arwlf_solve*` | `full_solve_*.jsonl` | `results/arwlf/full-solve-*/` (`ARWLF_FULL_SOLVE.md`) |
| Design B vs staged backends (Table 7, last row) | `bench/ter_full_bench.cpp` + `ter_arwlf_solve_attempt` | `design_b_n72_*` | `results/arwlf/design-b-n72-compare-v2/` |
| Hash-join baseline (§8.1, §10.2) | `ter2_root/campaign/ter_hash_join_*` | `hash_join_n108_anchor.csv` | `results/arwlf/hash-join-n108-anchor.log` |
| ASan differential record | — | `asan_differential_*` | `results/arwlf/asan-differential-20261011/` |
| Occupancy / scaling model (§9, App. C) | `paper/scaling_model.py` | `scaling_inputs.json`, `scaling_model.*` | — |
| Classical solver the root sits in | `src/full_ter.cpp` (+ `tiled_q3report`, `validator`, `arithmetic`, `packed`, `shape`) | — | `FULL_TER_COMPLIANCE.md` |
| Authentic root snapshots | `ter2_root/campaign/ter_root_snapshot_capture.cpp` | `snapshot_manifest.json` (SHA-256 per file) | `results/root-queue-generalization/snapshots/` (calibration) and `results/arwlf/holdout-snapshots/` (holdout + n=72) |

`paper/data/input_manifest.json` and `paper/data/provenance_hashes.json` list the exact files and
hashes; `paper/build_results.py` refuses to build if any frozen input disagrees with them.
The scripts in `reviewer/` (see `reviewer/README.md`) check this chain: hashes, build and tests,
tables and DOCX, snapshot recapture from the seed, and a rerun of one anchor cell.

**Nothing else in `results/` feeds the paper.**

## 2. Traps: things a reviewer could misread

| # | What they might see | What it actually is |
|---|---|---|
| T1 | `paper/ARWLF_Naman_PM.pdf`, `paper/manuscript.tex`, root `manuscript.tex`, `paper/ARWLF_submission_sources.zip` | STALE ARTIFACTS of an earlier article (3-4 Oct). They do not match the current DOCX. Kept unchanged on purpose. |
| T2 | `paper/SHA256SUMS.txt`, `paper/release_manifest.json` | Hashes and manifest of that 4-Oct package. The DOCX hash listed there is not the current DOCX. |
| T3 | `results/arwlf/regime-20261001/RESULTS.md`: "ARWLF / K2 ratio is 79.8x" | A separate v3-harness run on the 0x3007 anchor (`results/arwlf/campaign-20261001-v3-0x3007/`) with a noisy K2 side. RESULTS.md itself says to report the campaign figure (52.8x for that microproblem). Not used by the paper. |
| T4 | "95 % CI 51.2-55.0" (holdout) in older docs vs "51.81-53.81" in the paper | Two different intervals from the same data: case-level bootstrap vs root-block bootstrap (`summary.md`, line 141). The paper reports the root-block interval because cells within a root are dependent. |
| T5 | `results/root-queue-generalization/*/analysis.json`: verdicts `EXCELLENT` and `BAD_SPECIALIZATION`; QUEUE-DENSE "1.77x" | An earlier QUEUE-DENSE generalization campaign (Sep 25-26) with its own preregistered verdict rule (`ter2_root/retired/root_queue_generalization/analyze_root_queue_generalization.py`, line 280). `EXCELLENT` is the authentic-only run; `BAD_SPECIALIZATION` is the screen that mixed authentic, synthetic and emulated families. The paper's QUEUE-DENSE figures (1.21-1.84x over K2 at n=72) come from the regime campaign instead. |
| T6 | `results/root-queue-generalization/snapshots/` | Despite the folder name, these are the three **calibration** n=108 snapshots and the n=72 anchor used by the ARWLF paper. They were captured during the QUEUE-DENSE campaign and reused. |
| T7 | `results/arwlf/history/design-b-n72-compare/` | The **invalid** first design-B run (wrong instance for attempts 10 and 21; see its `NOTE.txt`). The valid run is `design-b-n72-compare-v2/`. |
| T8 | `results/arwlf/history/campaign-20260929*/` | Single-shot development runs of 29-30 Sep, before the campaign was preregistered on 2026-10-01; the folder suffix names the ARWLF variant (original, pinned/inline, v3). Their K2 and ARWLF times differ from the campaign's. Not campaign data. |
| T9 | "modeled 49.9x" (CERT-QUAD), "projected 176 ms" (radix-13 packed tag), "up to 15.9x" (tiling prototype) | Model or prototype numbers in historical design documents. None is a measurement of the code the paper reports. CERT-QUAD was never built. |
| T10 | Repository name `subset-sum-ter-fft`; README tagline "FFT-based methods" | The solver contains no FFT or NTT. Exact NTT appears only in the closed, count-only Q3REPORT experiments (§4.2 below), which failed their gates. |
| T11 | "tile-sweep baseline" (paper, = K2) vs design B (paper §8.7) | Two different tilings. K2 tiles **source pairs** 8 x 8. Design B (`src/tiled_q3report.cpp`) tiles the **quotient residue space** into cache-sized windows. Since the 13 October revision the paper says "tile-sweep" only for K2 and describes design B by its windows; older drafts called both "tiled". |
| T12 | "TER²" | Not a squared or second-level TER. It names the root decomposition by the low 6 quotient bits into 64 families and 4,096 microproblems `(s, t)` ("TER inside TER", `docs/history/the holy markdown files/The_holy_TER^2.md`). |
| T13 | "K2" | A name from the MicroReport8 spike (`microreport8::avx2::k2_coarse_gather`), not a published algorithm. It is this repository's tiled baseline. |
| T14 | "Stage A-M", "Phase A/B", "P0-P9", "repair item P11" | Four unrelated label systems. Stages A-M and phases P0-P9 come from the superseded v2.1 root architecture PDF. "Repair item Pn" (P1-P28) refers to `docs/history/FULL_TER_REPAIR_AUDIT.md`. "Phase A/B" means different things in the tiling spec, the n=108 capture (`ter_root_snapshot_capture.cpp`: capture = phase A) and the convsig experiment. |
| T15 | n=108 staged solve (seed 0x8001): 32 attempts, no witness | `AttemptLimitReached`, not UNSAT. The instance has a planted solution; the run shows TER's finite per-attempt success probability at these parameters (paper §8.7). Seventeen attempts found residue-exact triples, none of binary weight n/2. |
| T16 | Hash-join "exact" on the 0x1001 anchor | That cell has zero full-quotient matches, so "exact" only means no false matches were reported. The paper says so. |
| T17 | `TER_n108_A64_CHQR_Compiler_God_Spec.pdf`, `TER_L1_to_L0_Inference_Problem.pdf` cited in code comments | These specs are not in the repository. The A64-CHQR algorithm is defined by `ter2_root/frozen/ter_a64_chqr.cpp` itself. |
| T18 | Frozen-file comments naming `bench/ter_microreport8_dataset.cpp`, `bench/ter_translated_tag_dataset.cpp`, `bench/ter_root_pairspace_probe.cpp` | Pre-2026-10-02 paths. The first two are now `ter2_root/frozen/`, the third `ter2_root/retired/root_pairspace_probe/`. Frozen files cannot be edited to fix this. `ARWLF_REGIME_CAMPAIGN.md` has the same old `bench/` paths for the campaign sources (now `ter2_root/campaign/`). |
| T19 | `paper/runs/` | The runs in it are finished; their outputs are in `results/arwlf/` and `paper/data/`. The folder holds the scripts that produced them. Since 2026-10-13 `run_design_b_comparison.ps1` defaults to a new dated output folder and refuses to write into an existing one; its measurement logic is unchanged. |
| T20 | Snapshots: "regenerated deterministically" | True for the list contents (`ter_root_snapshot_capture --n=108 --seed=<seed> --attempt=0` with the recorded executable); the recorded SHA-256 values in `snapshot_manifest.json` are the check. They are not in git (0.8-1.0 GB each). |

## 3. Legacy ideas at a glance

Chronological. "Survives as" names what, if anything, the live code took from the idea.

| Idea | When | Verdict (from the artifacts) | Code now | Survives as |
|---|---|---|---|---|
| T0 single-merge prototype | Aug 14 | Implemented and frozen | `src/merge.cpp` (CLOSED, built) | The frozen T0 baseline; `merge` also hosts the public tiled merges |
| Count-only NTT at L2->L1 (Q3REPORT) | Aug 17 | Fails the cheap-kill gate in every family | `src/q3report_audit.*` (CLOSED, built) | Nothing in the solver |
| NTT crossover at larger L | Aug 17-18 | No regime where exact NTT beats direct counting | `src/q3report_crossover.*`, `src/q3report_paper_crossover.*` (CLOSED, built) | The paper-scaling snapshot builder is reused by the tiled parameter sweep |
| Cache-tiled exact L2->L1 merge | Aug 19-29 | Exact; slower than the Q-wide direct comparator below L = 512, 12.93x faster at L = 16,384 | `src/tiled_q3report.*` (LIVE) | The FullTer L2->L1 stage and the production root (design B) |
| Adversarial stress generator | Aug 21 | Tooling, no verdict | `src/stress_generator.*` (LIVE) | `ADVERSARIAL_STRESS` fixtures only |
| v2.1 refined root funnel, Stages A-M | Aug 24 | Superseded; Stage C built with no pruning gain; Stage D never built (open problem); Stage E pair-bound | `src/root_partition*` (Stage C, CLOSED, built) | `TerPartitionEntry` type used by the frozen capture adapter |
| Stage C BlockInPlace (IPS2Ra-derived) | Aug 25 | Slower than CurrentScatter | `src/root_partition_block_inplace.cpp` | Nothing |
| Production root decision (design B) | Aug 26 | Design B ships | `src/full_ter.cpp` | The classical solver's root today |
| Stage D region pruning probe | Aug 26 | Pruning power real (up to 250x less work at n=72) but enumeration stays near P0 | `ter2_root/retired/root_pairspace_probe/` | `docs/root_pair_space_analysis.md` |
| CERT-QUAD, TER² recursion | Aug 29-30 | CERT-QUAD never built (modeled only) | none | TER² family decomposition (64 x 64 microproblems) |
| MicroReport8 packed 8x8 reporters | Sep 2 | TOO_SLOW | probe RETIRED; kernel and dataset FROZEN | K2's kernel |
| Residual windowing | Sep 3-5 | WINDOWING_GEOMETRY_UNPROMISING | RETIRED | Nothing |
| Translated-tag locator | Sep 9 | NO_GO_SLOWER_THAN_K2 | bench RETIRED; dataset and locator FROZEN | Authority tables used by all backends |
| Convolution-signature inference (GPU NTT) | Sep 12 | TRANSFORM_NO_GO | RETIRED | Nothing |
| Radix-13 occupancy locator | Sep 13-14 | CORRECT_BUT_SLOWER_THAN_K2 | FROZEN (`ter_radix13_locator*`) | Radix-13 classes inside QUEUE-DENSE |
| Radix-13 packed tag | Sep 23 | IMPROVEMENT_BELOW_1_5X | FROZEN (`ter_radix13_packed*`) | Its queued-dense route **is** QUEUE-DENSE |
| QUEUE-DENSE generalization | Sep 25-26 | Wins on authentic n=72; not applicable at n=108 (capacity) | bench RETIRED; backend FROZEN | QUEUE-DENSE |
| A64-CHQR carry refinement (Stage E) | Sep 26 | PAIR_BOUND | bench RETIRED; `ter_a64_chqr.cpp` FROZEN | The file also hosts K2-WIDE |
| ARWLF v1/v2 packet sweep, orientation estimator, two-row packing | before Oct 1 | Replaced by the v3 flat row sweep / dropped (no gain) | git history only | The scalar packet sweep survives as a test oracle |
| ARWLF v3 + dispatcher | Oct 1 | 53.1x / 52.9x over K2 (n=108) | FROZEN | **The paper** |

---

## 4. Per-file inventory

### 4.1 Repository root

| File | Status | What it is |
|---|---|---|
| `README.md` | LIVE | Overview and build commands. |
| `AGENTS.md` | LIVE | Repository rules (correctness, failure semantics, frozen backends, git discipline). Test and bench lists updated 2026-10-12. |
| `CLAUDE.md` | LIVE | How Claude Code works in this repo; imports `AGENTS.md`. |
| `LEGACY_MAP.md` | LIVE | This file. |
| `FULL_TER_COMPLIANCE.md` | LIVE | Contract for `FullTer` against Li (2025). |
| `TER_L1_to_L0_ARWLF_Architecture_v1.0.md` | LIVE | Current root architecture. |
| `FROZEN_ROOT_BACKENDS.md` | LIVE | Freeze contract and reference timings for the 29 frozen files. |
| `ARWLF_REGIME_CAMPAIGN.md` | PAPER | Preregistration of the regime campaign, with Amendments 1-2. Its source paths are pre-move (T18). |
| `ARWLF_WHOLE_ROOT.md`, `ARWLF_FULL_SOLVE.md` | PAPER | Preregistrations of the whole-root run and the staged solve. |
| `TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md` | HISTORICAL (frozen plan) | Scope contract of the closed NTT experiment. Checked as a frozen artifact by `bench/analyse_tiled_q3report.py`. |
| `TER_STRESS_GENERATOR.md` | LIVE | Contract of the stress generator. |
| `ter_minimal_prototype_implementation_plan.md` | HISTORICAL | T0 plan (Aug 14). Names about 30 planned files (`ntt.cpp`, `orientation.cpp`, `residue_index.cpp`, ...) that were never created under those names. |
| `ter_ss_modular_convolution_master_spec.md` | HISTORICAL | Original research spec (TER + Schroeppel-Shamir + modular convolution). Mentions `SS.cpp`, `ssp_oracle(9).cpp` and notes that are not in this repository. |
| `tiling_implementation_idea.pdf` | HISTORICAL (protected path) | Older 16-section PDF of the tiled-merge write-up; the current text is `docs/history/tiling_implementation_idea.md`. |
| `paper_crossover.csv`, `paper_crossover_stderr.log` | CLOSED (data) | Output and run log of the Aug 18 paper-scaling NTT crossover. The CSV is checked as a frozen artifact at this path by `bench/analyse_tiled_q3report.py`; do not move it. |
| `manuscript.tex` | STALE ARTIFACT | Oct 3 TeX copy of an earlier article (T1). Untracked; kept unchanged. |
| `CMakeLists.txt` | LIVE | Builds `ter_t0`, 13 core test suites, 9 benches; includes `ter2_root/`. |

### 4.2 Library: `include/ter/` + `src/` (target `ter_t0`)

All files are compiled into one static library. "Used by" lists the non-test consumers.

| Module | Status | What it does | Used by |
|---|---|---|---|
| `types.hpp` | LIVE | `Status` and the failure states (`ResourceLimit`, `AttemptLimitReached`, `NotApplicable`, ...), ids, residues. | everything |
| `arithmetic` | LIVE | Exact power-of-two modular arithmetic. | everything |
| `limitations.hpp` | LIVE | Registry of runtime limits, applicability gates and checked size arithmetic. | everything |
| `instance` | LIVE | Density-one instance generation and storage. | solver, benches |
| `shape` | LIVE | Finite TER shape; enforces `36 | n` (`docs/ter_dimension_domain.md`). | solver |
| `model.hpp` | LIVE | Instance + arithmetic model types. | solver, merge |
| `reference_vector` | LIVE | Scalar `{-1,0,+1}^n` vectors; correctness authority. | solver, validator |
| `packed` | LIVE | Packed ternary masks. | solver, merge |
| `validator` | LIVE | Independent structure, residue and witness checks. | solver, merge, generator |
| `telemetry.hpp` | LIVE | Counters for the T0 merge. | `merge` |
| `full_ter` (4,225 lines) | LIVE, PAPER | Complete depth-3 solver `solve_full_ter`, parameter derivation, attempts, root capture/streaming, design-B root. | snapshot capture, `ter_full_bench`, frozen adapter |
| `tiled_q3report` | LIVE | Exact cache-tiled three-list locator (no Q-wide table, no pair array). The only tiling implementation. | `full_ter`, `merge`, frozen MicroReport8 dataset (cross-check) |
| `merge` (1,349) | CLOSED (built) | Frozen T0 single merge, plus the public tiled merges `run_pair_tiled*`. **Not** used by `full_ter`. | baseline, stress and tiled benches |
| `generator` (1,013) | CLOSED (built) | T0 snapshot generator (reference, rejection, MITM, planted modes). Not used by `full_ter`. | Q3REPORT benches, stress generator |
| `stress_generator` | LIVE (tooling) | `ADVERSARIAL_STRESS` merge fixtures; never FullTer evidence. | `ter_stress_bench` |
| `q3report_audit` | CLOSED (built) | Count-only NTT experiment at L2->L1: snapshot audit, cubic oracle, exact single-prime NTT. Verdict: fails the cheap-kill gate. | `ter_q3report_bench` |
| `q3report_crossover` | CLOSED (built) | Larger TER-shaped NTT crossover. | `ter_q3report_crossover_bench` |
| `q3report_paper_crossover` (2,180) | CLOSED (built) | Paper-scaling dense-NTT crossover (V2). | `ter_q3report_paper_crossover_bench`; its snapshot builder is also used by `ter_tiled_parameter_sweep` |
| `root_partition` (+ `_current_scatter`, `_block_inplace`, `_internal.hpp`) | CLOSED (built) | Stage C one-digit radix partition `partition_chunk`. CurrentScatter adapts the donor `docs/history/donors/ssp_oracle.cpp`; BlockInPlace adapts IPS2Ra (`third_party/licenses/`). | Frozen code uses only the `TerPartitionEntry` type from the header; `partition_chunk` is used only by its bench and tests |

### 4.3 Core tests: `tests/`

| File | Covers | Status |
|---|---|---|
| `shape_tests`, `core_tests`, `generator_tests`, `packed_tests`, `merge_tests`, `full_ter_tests`, `limitations_tests`, `stress_generator_tests` | the modules of the same name | LIVE |
| `tiled_q3report_tests` | tiling arithmetic against an independent count | LIVE |
| `q3report_audit_tests`, `q3report_crossover_tests`, `q3report_paper_crossover_tests` | the three NTT experiments | CLOSED (built) |
| `root_partition_tests` | Stage C partition (compiled with `TER_ROOT_PARTITION_TESTING`) | CLOSED (built) |

CLI regressions `ter_bench_cli_*` and `ter_tiled_sweep_cli_*` are defined in `CMakeLists.txt`.

### 4.4 Benches: `bench/`

| File | Status | What it measures |
|---|---|---|
| `ter_full_bench.cpp` | LIVE, PAPER | FullTer solves, attempt cost, preflight. Classic side of the design-B comparison. |
| `ter_baseline_bench.cpp` | CLOSED | T0 merge baseline. |
| `ter_stress_bench.cpp` | LIVE (tooling) | Stress fixtures. |
| `ter_q3report_bench.cpp`, `ter_q3report_crossover_bench.cpp`, `ter_q3report_paper_crossover_bench.cpp` | CLOSED | The three NTT experiments. |
| `ter_tiled_complete_merge_bench.cpp`, `ter_tiled_parameter_sweep.cpp` | CLOSED | Tiled vs pair-materializing merge; tile-width sweep. |
| `ter_root_partition_bench.cpp` | CLOSED | Stage C bake-off. |
| `analyse_tiled_q3report.py`, `run_tiled_publication_campaign.ps1` | CLOSED | Tiled publication campaign (`results/tiled-q3report-final-20260829/`). |
| `analyse_root_partition.py`, `run_task11_partition_bakeoff.ps1` | CLOSED | Stage C bake-off (`results/partition/`). The script still points at the deleted `build-root-v2/` folder. |

### 4.5 `ter2_root/frozen/` (FROZEN; all PAPER)

The names are those of the experiments that produced each file; all 29 files are used by the four
frozen backends.

| Files | Role in the frozen backends | Origin experiment |
|---|---|---|
| `ter_arwlf.{hpp,cpp}`, `ter_arwlf_avx2.cpp` | ARWLF and the dispatcher | ARWLF v3 |
| `ter_root_queue_kernels.{hpp,cpp}` | K2 driver, `RootSink` authority chain, QUEUE-DENSE route | QUEUE-DENSE generalization |
| `ter_a64_chqr.{hpp,cpp}` | K2-WIDE; also the A64-CHQR refinement (closed) | A64-CHQR (T17) |
| `ter_microreport8_kernels.hpp`, `ter_microreport8_kernels_avx2.cpp`, `ter_microreport8_dataset.{hpp,cpp}` | K2 kernel and the authentic dataset | MicroReport8 |
| `ter_radix13_locator*`, `ter_radix13_packed*` (incl. `_kernel.inl`, `_bmi2.cpp`) | QUEUE-DENSE internals | Radix-13 locator and packed tag |
| `ter_translated_tag_dataset.*`, `ter_translated_tag_locator*` | Authority tables shared by all backends | Translated-tag locator |
| `ter_root_snapshot.*` | Packed `.snap` format, load and checksum | n=108 root experiment |
| `ter_root_generalization_workload.*` | Snapshot to keyed workload (full Q-bit keys) | QUEUE-DENSE generalization |
| `ter_root_capture_adapter.*` | FullTer root capture to records (namespace `root_partition_bench` is a leftover name) | Stage C bench |

### 4.6 `ter2_root/campaign/`, `tests/`, `scripts/`

| File | Status | What it is |
|---|---|---|
| `campaign/ter_arwlf_campaign.{hpp,cpp}`, `_main.cpp` | PAPER | Regime campaign: census, stratification, thinning, timed phases. |
| `campaign/ter_arwlf_bench.cpp` | CLOSED (built) | v3 matched harness used before the campaign (T3, T8). |
| `campaign/ter_root_snapshot_capture.cpp` | PAPER | Captures one attempt's three L1 lists to a `.snap`. |
| `campaign/ter_arwlf_whole_root*` | PAPER | Whole-root runner. |
| `campaign/ter_arwlf_solve*` | PAPER | Staged solver attempt ending in the solver's own witness verification. |
| `campaign/ter_hash_join_*` | PAPER | Untuned full-key hash-join baseline. |
| `campaign/ter_bench_host.hpp` | PAPER | P-core pinning, AC power, memory (Windows). |
| `tests/ter_arwlf_tests.cpp`, `ter_arwlf_planted_tests.cpp` (CTest also runs it as `_n72`), `ter_arwlf_campaign_tests.cpp`, `ter_arwlf_whole_root_tests.cpp`, `ter_arwlf_solve_tests.cpp`, `ter_hash_join_tests.cpp`, `ter_hash_join_cli_tests.cmake` | PAPER | Exactness, planted outputs, campaign logic, whole-root, solver differential, hash join. |
| `tests/ter_a64_chqr_tests.cpp`, `ter_root_queue_generalization_tests.cpp`, `ter_root_snapshot_tests.cpp`, `microreport8_tests.cpp`, `translated_tag_locator_tests.cpp`, `radix13_locator_tests.cpp`, `radix13_packed_tests.cpp` | LIVE | Tests of the frozen components, named after their origin experiments. |
| `tests/ter_synthetic_root.hpp` | PAPER | Shared brute-force oracle. |
| `scripts/run_arwlf_regime_campaign.ps1`, `arwlf_regime_campaign_plan.ps1`, `summarize_arwlf_regime_campaign.py` | PAPER | Campaign driver, plan and summary. |
| `scripts/arwlf_positive_output_plan.ps1`, `arwlf_whole_root_plan.ps1`, `summarize_arwlf_whole_root.py`, `arwlf_full_solve_plan.ps1` | PAPER | Run plans for the later experiments. |
| `scripts/bench_guard.ps1` | LIVE | Benchmark interference guard (`bench.lock`). |
| `scripts/check_frozen_root_backends.cmake` | LIVE | The frozen-hash CTest. |

### 4.7 `ter2_root/retired/` (RETIRED, not built)

Verdicts and result folders are in `ter2_root/retired/README.md`. Last commit where all of it built:
tag `pre-cleanup-3eaad92`.

| Folder | Files | Experiment |
|---|---|---|
| `convsig/` | 7 Python files + `ter_convsig_dataset_export.cpp` | GPU NTT convolution with source signatures. `convsig_transform_bench.py` reads `bench/crossover_full.csv`, now at `results/ntt-crossover-bench-20260817/crossover_full.csv`. |
| `windowing/` | probe, geometry header, tests, spike script | Residual windowing. |
| `microreport8_probe/` | `ter_microreport8_probe.cpp` | MicroReport8 front ends. |
| `translated_tag_bench/` | `ter_translated_tag_bench.cpp` | Translated-tag benchmark. |
| `a64_chqr_bench/` | bench + campaign script | A64-CHQR timing. |
| `root_queue_generalization/` | bench, runner, analysis | QUEUE-DENSE generalization (T5). |
| `root_pairspace_probe/` | `ter_root_pairspace_probe.cpp` | Stage D pair-space analysis. |
| `stage_d_bitset_join/` | `stage_d_bitset_join.c` | Reference C kernel for the Stage D pair-listing floor; never built by CMake, measured on another machine. |

### 4.8 `docs/`

| File | Status | What it is |
|---|---|---|
| `root_architecture_resolution.md` | LIVE (decision record) | Why design B is the production root. |
| `root_partition_decision.md` | LIVE (decision record) | Stage C: CurrentScatter stays default, BlockInPlace experimental. |
| `root_pair_space_analysis.md` | LIVE (decision record) | Stage D: pruning power vs enumeration cost; the open problem. |
| `root_v2_baseline.md` | HISTORICAL | Stage C baseline and donor mapping (`ssp_oracle.cpp`, IPS2Ra). |
| `ter_dimension_domain.md` | LIVE | Derivation of `36 | n`. |
| `idea_inventory_2026-10-02.{md,pdf}` | HISTORICAL | Dated survey, superseded by this file. |
| `superpowers/plans/2026-08-19-tiled-l2-l1.md` | HISTORICAL | Executed plan for the tiled merge integration. |
| `history/SUPERSEDED.md` | LIVE | Fate of the v2.1 Stage A-M architecture. |
| `history/TER_L1_to_L0_Radix_Refined_Root_Architecture_v2.1_CODEX.pdf` | HISTORICAL | The Stage A-M root architecture. |
| `history/TER_Hybrid_Partition_Backend_v2.0_CODEX.pdf` | HISTORICAL | Stage C partition backend spec. |
| `history/TER_Tiled_L2_L1_End_to_End_Implementation_Spec.pdf` | HISTORICAL | Tiled merge spec ("up to 15.9x" is a prototype figure, T9). |
| `history/tiling_implementation_idea.md` | HISTORICAL | Tiled merge write-up; sections 16-17 hold the final numbers cited by `README.md`. Its older 16-section PDF stays at the repository root (`tiling_implementation_idea.pdf`) because PDF paths are protected (see 4.11). |
| `history/TER_2pow47_Root_Research_Map.pdf` | HISTORICAL | Aug research map of the n=72 root as modular 3SUM. |
| `history/full_architecture.md`, `brute_froce_fix_plan.md`, `entire_project_direction_from_now_to_a_research_paper.md`, `Remaining_ideas.md` | HISTORICAL | Aug design conversations: v2.1 funnel complexity; 64-bit quotient boundary plan (superseded by full keys in A64/K2-WIDE/ARWLF); research pipeline plan; the 56-idea "graveyard" (see `docs/idea_inventory_2026-10-02.md` §3 for what each became). |
| `history/FULL_TER_REPAIR_AUDIT.md` | HISTORICAL | Repair items P1-P28 (source of "repair item Pn" labels, T14). |
| `history/implementation_plan.md`, `CLAUDE_HANDOFF_2026-08-25.md`, `CLAUDE_instructions_2026-08-18.md` | HISTORICAL | Stage C plan (executed in 6e52c2b), handoff, old instructions file. |
| `history/PROJECT_DOSSIER.md` | HISTORICAL | Oct 3 source dossier of the four backends; predates the paper evidence updates. |
| `history/the holy markdown files/` | HISTORICAL | CERT-QUAD and TER² notes and PDFs (`final_architecture.pdf`, `final_candidate.pdf`); the radix-13 packed-tag spec PDF (implemented, see §3). Model numbers only (T9). |
| `history/donors/ssp_oracle.cpp` | HISTORICAL | 17,672-line DLL source from another project; radix mechanics adapted into `src/root_partition_current_scatter.cpp`. Not built. |

### 4.9 `results/`

| Folder | Status | What it holds |
|---|---|---|
| `arwlf/regime-20261001/` | PAPER | The preregistered campaign (census, anchors, thinning, confirmations, `summary.md`, `RESULTS.md`, build freeze, `pre-amendment/`). |
| `arwlf/positive-20261003/`, `whole-root-20261003/`, `full-solve-*`, `design-b-n72-compare-v2/`, `hash-join-n108-anchor.log`, `asan-differential-20261011/` | PAPER | See §1. |
| `arwlf/holdout-snapshots/` | PAPER | Holdout n=108 and n=72 snapshots (`.snap` not in git) with capture logs and metadata. |
| `arwlf/history/design-b-n72-compare/` | CLOSED (invalid) | T7. |
| `arwlf/history/campaign-20260929/`, `-pinned-inline/`, `-v3/` | CLOSED | September development runs (T8). Moved here unchanged on 2026-10-13; `history/RELOCATION_MANIFEST.json` maps every file to its original path and SHA-256. |
| `arwlf/campaign-20261001-v3-0x3007/` | CLOSED | The v3-harness run on the 0x3007 anchor behind the 79.8x outlier (T3). Left in place because `regime-20261001/RESULTS.md` cites this path. |
| `root-queue-generalization/` | CLOSED + PAPER snapshots | QUEUE-DENSE campaign (T5); `snapshots/` is PAPER (T6). The two `*.runner.log` files at the top of `results/` belong to it. |
| `a64-chqr/` | CLOSED | PAIR_BOUND. |
| `radix13-locator/`, `radix13-packed-tag/`, `translated-tag-locator/`, `microreport8-20260902/`, `ter2-windowing-2026090{3,5}/`, `convolution-signature-inference/` | CLOSED | Evidence for each retired verdict (`ter2_root/retired/README.md`). |
| `partition/` | CLOSED | Stage C bake-off (task 6 and task 11 runs); checked as a frozen artifact by `bench/analyse_tiled_q3report.py`. |
| `tiled-q3report-final-20260829/` | CLOSED | Tiled merge publication campaign. |
| `ntt-crossover-bench-20260817/` | CLOSED | `crossover_full.csv`, rescued from a removed worktree (see its `NOTE.md`). |

### 4.10 `paper/`

| File | Status | What it is |
|---|---|---|
| `manuscript.md` | PAPER | The article source. |
| `ARWLF_Naman_PM.docx` | PAPER | The article, built on ACM's Word submission template. |
| `archive/2026-10-13-theory-template/` | HISTORICAL | The 13 October build on the earlier theory-paper template (same content, SHA-256 `bcfea551…`). Do not cite or submit. |
| `build_results.py` | PAPER | Validates frozen inputs, regenerates tables and figures. |
| `build_article.py`, `math_native.py` | PAPER | DOCX builder (TeX math to native Word math). Without `--docx-only` it also rewrites `manuscript.tex`. |
| `validate_docx_only.py`, `render_docx_pages.ps1`, `rasterize_word_emf.py` | PAPER | DOCX checks and page rendering. |
| `scaling_model.py` | PAPER | Occupancy and cost model. |
| `data/` | PAPER | Frozen evidence and manifests (`data/README.md`). |
| `figures/`, `tables/tables.json`, `template/acm_submission_template.docx`, `references.{bib,json}` | PAPER | Generated figures and tables, ACM Word template, bibliography. |
| `template/reference.docx` | HISTORICAL | Theory-paper template of the archived 13 October build; the builder no longer reads it. |
| `claim_ledger.md` | PAPER | Claim-to-evidence map. |
| `README.md`, `SUBMISSION_NOTES.md`, `REVIEW_TRIAGE_2026-10-09.md` | LIVE | Build instructions, author items, review triage. |
| `runs/` | PAPER | Scripts that produced the hash-join, ASan and design-B records (T19). |
| `ARWLF_Naman_PM.pdf`, `.pdf.txt`, `manuscript.tex`, `ARWLF_submission_sources.zip`, `SHA256SUMS.txt`, `release_manifest.json`, `compile.ps1`, `render_word.ps1`, `validate_article.py`, `package_release.py` | STALE ARTIFACT / HISTORICAL pipeline | The 4-Oct PDF/TeX package and the scripts that built it (T1, T2). `package_release.py` reads about 15 files under `qa/` and `working/`. |
| `qa/`, `working/` | LOCAL (git-ignored) | Render QA and scratch. The one tracked exception is the preservation inventory `qa/continuation-20261009/protected_files_before.json`. Default `validate_docx_only.py` requires local preservation and page metadata; `--portable` validates distributable article inputs. `working/results_review.json` is needed only to re-freeze historical inputs; normal reproduction reads `data/`. |

### 4.11 Everything else

| Path | Status | What it is |
|---|---|---|
| `reviewer/` | LIVE | Reviewer entry-point scripts (`check_integrity`, `build_and_test`, `check_paper`, `recapture_snapshot`, `rerun_anchor`) and the recorded snapshot payload digests. They wrap the experiment scripts, which stay where their records name them. |
| `third_party/licenses/` | LIVE | Licence for IPS2Ra-derived code. |
| `Research papers/` | git-ignored | Cited literature, old drafts (`manuscript.docx`, `research-paper.*`, `arwlf-journal-paper.pdf`), two unrelated PDFs, and a copy of the current DOCX. Its PDFs are protected (below), so they stay where they are. |
| `.github/copilot-instructions.md`, `.vscode/` | LIVE | Editor and assistant configuration (`.vscode` uses `build/`). |
| `build/`, `build-full/`, `build-full-asan/` | git-ignored | VS Code build; Release build used by all scripts; the ASan record's build. |
| `graphify-out/`, `.gstack/`, `.cmem-projects/`, `.superpowers/`, `.claude/` | git-ignored | Tool state. |
| `.attic/` | SCRATCH | Old root notes, the Stage C agent ledger, a chat transcript, the previous `.gitignore`. Safe to delete. |

**Protected files.** `paper/validate_docx_only.py` hashes 42 PDF and TeX files at fixed paths (list: `paper/qa/continuation-20261009/protected_files_before.json`), including the root `tiling_implementation_idea.pdf` and the PDFs in `Research papers/`, and fails if any is changed, moved or added. Do not move or rename any `.pdf` or `.tex` file.
