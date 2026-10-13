# Reviewer scripts

PowerShell 7 entry points for checking the article's claims. Each one wraps the repository's own build, test, capture and campaign tools and changes no recorded result. Run them from any directory, as `pwsh -NoProfile -File reviewer/<script>.ps1`. Every script states its cost and options in its header.

The measurements were taken with Windows, MSVC and an AVX2 CPU. `check_integrity.ps1` (which also needs CMake) and `check_paper.ps1` run on any platform with PowerShell 7. The other three need a Windows build in `build-full/`.

| Question | Script | Cost on the author's machine |
|---|---|---|
| Are the frozen backend sources, the paper's input files and the archived runs the recorded bytes? | `check_integrity.ps1` (add `-Snapshots` to also hash the local `.snap` files) | Seconds; `-Snapshots` reads about 6 GB |
| Can the exact sources of the three campaign builds be recovered? | `python reviewer/check_build_sources.py` (`--extract DIR` writes the recovered trees) | Seconds |
| Does the code build, and do the 61 tests pass? | `build_and_test.ps1` (`-Regex` selects a subset) | About 7 minutes of tests |
| Do the tables, model numbers and DOCX follow from the frozen inputs? | `check_paper.ps1` (needs Python 3 with NumPy, Matplotlib, lxml) | Seconds |
| Is a measured root snapshot what FullTer produces from its seed? | `recapture_snapshot.ps1 -Root <root>` (`-List` shows the roots; `-Install` keeps the file for timing) | About a second at n=72; about 2-3 minutes and 1.6 GB of memory at n=108 |
| Does the K2/ARWLF ratio of an anchor cell reproduce? | `rerun_anchor.ps1 -Root <root>` (`-Smoke` runs one pass) | Seconds at n=72; 15-35 minutes per n=108 root |

The campaign builds were made from working trees with uncommitted changes, so each recorded a `source_manifest.txt` (SHA-256 of every source file) whose own hash is in its `build_freeze.json`. `check_build_sources.py` verifies those hashes, then finds every manifest entry byte-exactly in the current tree or in a blob reachable from a git commit. On 2026-10-13 it recovered 130 of 131, 139 of 140 and 118 of 120 entries for the regime, constructed-target and complete-root builds; the misses are the edited-in-place CMake files and one summary script, and each manifest contains all 29 frozen backend files. The staged-solve, hash-join and design-B builds recorded no source manifest.

`snapshot_digest.ps1` is a helper that both snapshot scripts load. `snapshot_payload_digests.json` holds the ten recorded snapshot digests that the recapture is compared against.

A suggested order is: `check_integrity`, then `check_build_sources`, then `check_paper`, then `build_and_test`, then `recapture_snapshot -Root n108_s0x1001_a0 -Install`, then `rerun_anchor -Root n108_s0x1001_a0`. Root `n108_s0x1001_a0` is the calibration root behind the paper's whole-root and hash-join measurements. Its recorded anchor-cell K2/ARWLF ratio is 52.89.

## What a recapture can and cannot match

The root snapshots are 1 GB files and are not in git. Header bytes 352-423 of a snapshot hold run metadata: the capture's peak working set, its elapsed time, the git commit and a header checksum. So a recapture never reproduces the whole-file SHA-256 recorded in `results/arwlf/regime-20261001/snapshots.json`. `recapture_snapshot.ps1` instead compares the SHA-256 of every other byte, which covers all three root lists and every deterministic header field.

On 2026-10-13, `n72_anchor`, `n72_s0x7203_auto` and `n108_s0x1001_a0` were recaptured at commit 8e9fe7f. Each one matched the snapshot captured at 53431a8. The other seven roots were not recaptured that day.

## Scripts that produced the evidence (left in place)

The scripts below produced the recorded results. They were not moved here. Their recorded commands, `$PSScriptRoot`-relative paths and provenance records name their current locations, and moving them would break those records.

Do not run them in place to check a result. They resume into the recorded result folders, skip finished cases and rewrite summaries such as `summary.md`. Use the reviewer scripts above, or give a script a new output folder where it accepts one.

| Evidence | Script | Notes |
|---|---|---|
| Regime campaign, `results/arwlf/regime-20261001/` | `ter2_root/scripts/arwlf_regime_campaign_plan.ps1` calling `run_arwlf_regime_campaign.ps1` | The plan fixes its output folder. `ARWLF_REGIME_CAMPAIGN.md` is the preregistration. |
| Positive-output experiment, `results/arwlf/positive-20261003/` | `ter2_root/scripts/arwlf_positive_output_plan.ps1` | Amendment 2 of the campaign document. |
| Whole-root run, `results/arwlf/whole-root-20261003/` | `ter2_root/scripts/arwlf_whole_root_plan.ps1` | `ARWLF_WHOLE_ROOT.md`. About 55 hours, 51 of them for K2. |
| Staged full solves, `results/arwlf/full-solve-*` | `ter2_root/scripts/arwlf_full_solve_plan.ps1 -N <n> -Seed <seed> [-Backend arwlf] -Out <dir>` | `ARWLF_FULL_SOLVE.md`. |
| Hash-join baseline, `results/arwlf/hash-join-n108-anchor.log` | `paper/runs/run_hash_join_baseline.ps1` | `paper/runs/HASH_JOIN_BASELINE.md`. |
| Design-B comparison, `results/arwlf/design-b-n72-compare-v2/` | `paper/runs/run_design_b_comparison.ps1` | Writes a new dated folder by default. |
| ASan differential record, `results/arwlf/asan-differential-20261011/` | `paper/runs/run_asan_differential_recorded.ps1` | About 4 hours. |
| Benchmark protection | `ter2_root/scripts/bench_guard.ps1` | Guards a running benchmark; measures nothing. |

The remaining `.ps1` files belong to closed experiments that the article does not use (`bench/`, `ter2_root/retired/`), or to the historical 4-October PDF pipeline (`paper/compile.ps1`, `paper/render_word.ps1`). `paper/render_docx_pages.ps1` renders the DOCX pages for the author's local validation. `../LEGACY_MAP.md` gives the status of every file.
