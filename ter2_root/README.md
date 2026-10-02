# ter2_root: TER² L1 -> L0 root backends (ARWLF line)

Everything the TER² root research line needs, in one place: the four exact root backends, the ARWLF dispatcher, the regime campaign that measured them, and their tests.

**Architecture:** `../TER_L1_to_L0_ARWLF_Architecture_v1.0.md`
**Freeze contract:** `../FROZEN_ROOT_BACKENDS.md`
**Results:** `../results/arwlf/regime-20261001/RESULTS.md`

This is the benchmark surface: captured authentic roots replayed from snapshots. The production `solve_full_ter` root is unchanged (design B, `../docs/root_architecture_resolution.md`).

## Result in one line

On authentic n=108 microproblems ARWLF is a geometric-mean **53.1x** faster than frozen K2 on the calibration roots and **52.9x** on unseen holdout roots, with 221/221 campaign cases exact. This is a finite-n, constant-factor result.

## Layout

| Path | Contents |
|---|---|
| `frozen/` | The **29 frozen sources**, byte-identical to what the campaign measured, pinned by `frozen_root_backends.sha256`; CTest `ter_frozen_root_backends` fails on any change. ARWLF + dispatcher (`ter_arwlf*`); K2 driver, RootSink and QUEUE_DENSE route (`ter_root_queue_kernels*`); K2_WIDE (`ter_a64_chqr*`); K2 kernel and dataset (`ter_microreport8_*`); QUEUE_DENSE internals (`ter_radix13_*`); authority tables (`ter_translated_tag_*`); snapshot loading and full keys (`ter_root_snapshot*`, `ter_root_generalization_workload*`, `ter_root_capture_adapter*`) |
| `campaign/` | `ter_arwlf_campaign` (regime campaign: census, thinning, matched timing; `ter_arwlf_campaign.{hpp,cpp}` + `_main.cpp`), `ter_arwlf_bench` (v3 matched harness), `ter_root_snapshot_capture` (captures an authentic root to a `.snap`), `ter_bench_host.hpp` (pinning, power, memory) |
| `tests/` | `ter_arwlf_tests`, `ter_arwlf_campaign_tests`, `ter_a64_chqr_tests`, `ter_root_queue_generalization_tests`, `ter_root_snapshot_tests`, `ter_microreport8_tests`, `ter_translated_tag_tests`, `ter_radix13_tests`, `ter_radix13_packed_tests`; `ter_synthetic_root.hpp` is the shared whole-value brute-force oracle |
| `scripts/` | `run_arwlf_regime_campaign.ps1` (provenance-recording step driver), `arwlf_regime_campaign_plan.ps1` (the whole campaign, resumable), `summarize_arwlf_regime_campaign.py` (tables, bootstrap CIs, crossover rule), `check_frozen_root_backends.cmake` |
| `retired/` | Closed experiments, **not built**; see `retired/README.md` |

## Build and test

```powershell
cmake -S . -B build-full -DTER_NATIVE_OPT=ON
cmake --build build-full --config Release --parallel
ctest --test-dir build-full -C Release --output-on-failure
```

Executables land in `build-full/Release/` like every other target.

## Run

```powershell
# Capture an authentic root (n must be a multiple of 36):
build-full/Release/ter_root_snapshot_capture.exe --n=108 --seed=0x4001 --out=root.snap --attempt=0

# One campaign step (pinned to a P-core, AC power, Release only):
build-full/Release/ter_arwlf_campaign.exe --snapshot=root.snap --out-dir=out --phase=anchor

# The whole preregistered campaign (resumable; skips finished cases):
pwsh -NoProfile -File ter2_root/scripts/arwlf_regime_campaign_plan.ps1
```

The authentic snapshots are not in git. `results/arwlf/regime-20261001/snapshots.json` lists each by SHA-256 together with its capture command.

## Rules

Files in `frozen/` are never edited; a new idea becomes a new backend in new files, measured against these baselines (`FROZEN_ROOT_BACKENDS.md`).
