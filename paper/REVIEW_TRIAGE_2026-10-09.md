# External review triage (2026-10-09)

Cleanup update (2026-10-13): completed-run scripts now live in `paper/runs/`.
Earlier changelog entries preserve their original `paper/pending_runs/` commands.
The current `.gitignore` exposes manuscript sources and research records by
default; snapshots, local QA and scratch remain ignored. Historical PDF/TeX
regeneration suggestions below remain subject to the user's preservation rule.

Review of the TALG draft (`ARWLF_Naman_PM.docx`, 28 pages as rendered by Word).
Reviewer's scores: scientific quality 8/10, submission readiness 6.5/10.
Each claim was checked against the current DOCX and the repository.

## Verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| Equations around K+, P+, R, packets (pp. 9-10) are corrupted | **False alarm** | Word render of the current DOCX shows them correctly. The "malformed characters" come from PDF text extraction of Word math-italic glyphs. Minor nit: ceiling brackets in R_over render like tall bars. `ARWLF_Naman_PM.pdf` is the stale 2026-10-04 LaTeX build and must be regenerated anyway. |
| 28 pages | Correct | Word: 28 pages. |
| Whole root 55.2-56.8x | Correct, partial | ARWLF passes 55.2 / 56.9; dispatcher passes 55.5 / 57.4. |
| n=72 end-to-end solve used QUEUE-DENSE, not ARWLF | Correct | Stated in Section 8.7. |
| ASan record incomplete | Correct | CTest: 57 passed + 1 timeout. Direct differential run passed (341 checks, 0 failures), but `build-full-asan/asan_solve_tests.log` has no command, timestamp or exit-code wrapper. |
| Artifacts not reproducible from public GitHub | Correct | GitHub default branch is 8e9fe7f; integration code, fixes, whole-root results and `paper/` are local and uncommitted; snapshots were never in Git. |
| Section 8.7 title overstates (staged, not in-process) | Correct | Snapshot capture + frozen dispatcher + verifier; not inside `solve_full_ter`. Retitle "Staged integration and differential validation". |
| n=144 section too prominent | Mostly correct | Already retitled "Hypothetical dimension-144 cost scenarios". Move the Frontier/cost tables to an appendix; keep one saturation paragraph. |
| Only 3 independent holdout roots | Correct | Report per-root geometric means next to the bootstrap CIs. |
| Constructed positives are not native TER solutions | Correct | Already stated; keep it. |
| Title change | Taste | Suggested: "Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting". |
| One CPU only | Correct, optional | P-core/E-core or a second AVX2 CPU is nice to have. |
| Only 15 references | Correct, minor | Related work could compare mechanisms (packed Subset Sum, 3SUM) more. |

## Changelog

### 2026-10-12: design-B comparison (item A) done and in the DOCX
- First run (`results/arwlf/design-b-n72-compare`) was partly INVALID: the script passed `--seed=seed+attempt --attempt-limit=1` to `ter_full_bench`, which builds the instance from `--seed`, so the classic rows for attempts 10 and 21 timed other instances (level-one sizes differ). `NOTE.txt` added there; kept, not used.
- Script fixed: classic side uses the instance seed with `--attempt-limit=attempt+1`, and the run fails closed unless the terminal attempt index, attempt seed, status and level-one sizes equal the staged capture; raw bench CSV saved. Tested with 1 round into the scratchpad, then the author ran it (`-Out results/arwlf/design-b-n72-compare-v2`), all checks passed.
- Result, three attempts without a witness: design B 583 / 1,008 / 992 ms; QUEUE-DENSE (dispatcher) 308 / 290 / 317 ms; K2 285 / 473 / 487 ms; K2-WIDE 282 / 468 / 482 ms; ARWLF 1,910 / 1,748 / 1,724 ms. Design B is 1.89-3.48x slower than QUEUE-DENSE; ARWLF is 1.73-3.28x slower than design B. Attempt 21 (Found): design B stops at the first witness (213 ms, `l1_to_l0_early_stopped=1`), so it is not compared. This also explains the 0.18 s in the old heads-up below.
- Boundaries differ: design B stage includes its tiled setup (4.5-4.9 ms); staged root_ms excludes snapshot load and key preparation (32-47 ms). Classic: 3 warmups + 7 solves, medians only; staged: 1 warmup + 7 runs, pinned CPU 23.
- Frozen: `paper/data/design_b_n72_{classic.jsonl,staged.jsonl,summary.csv,provenance.json}` in `input_manifest.json`; Table 10 provenance entry. `build_results.py` recomputes and checks everything and writes a new last row of Table 7 plus a Table 9 row; Section 8.7 has a new paragraph and Section 8.4 one sentence. Table numbering unchanged.

### 2026-10-12: hash-join n=108 anchor-cell timing done and in the DOCX
- Run: `ter_hash_join_bench --snapshot=results/root-queue-generalization/snapshots/n108_s0x1001_a0.snap --microproblems=1 --warmups=1 --samples=3`, P-core CPU 23, AC. Output `results/arwlf/hash-join-n108-anchor.log` (stdout CSV only; no warmup rows). Exe SHA-256 e621c594...48d2 (built 2026-10-11 11:50, before the run); snapshot SHA-256 63fc31e8... (same as the recorded 0x1001 snapshot).
- Medians over 3 rounds, cell 2730, 6,143,192,070 pairs: ARWLF 890.2 ms; K2-WIDE 27,469 ms; K2 47,929 ms (rounds 60.3 / 47.9 / 45.8 s); **hash join 72,672 ms (11.83 ns/pair)**, i.e. 81.6x ARWLF and 1.52x K2. Same-run K2/K2-WIDE/ARWLF are 7.6 / 1.0 / 5.7 % above the Table 1 final-protocol medians.
- Every round of every backend matches an untimed K2-WIDE reference, but this cell has zero full-quotient matches, so it checks only for false matches (stated in the paper).
- Frozen to `paper/data/hash_join_n108_anchor.csv` (in `input_manifest.json`); new Table 10 provenance entry (marked as hashed from the executable, no build-freeze record). `build_results.py` checks the record against the 0x1001 anchor case (pairs, K2 hit counts, exact and host flags, 3 rounds x 4 backends) and refuses to build otherwise; tested by tampering four ways.
- Paper: Table 1 notes and a new Table 9 row (generated); new paragraph in Section 8.1; one sentence in Section 10.2. Abstract unchanged (238 words). The n=72 hash-join timing is NOT in the paper (never saved to a file).
- Built and checked: `build_results.py --tables-only` exit 0; `build_article.py` exit 0; `validate_docx_only.py` exit 0 (14 tables, 42 PDF/TeX files unchanged). New DOCX SHA-256 a5af3c6a...a8c5, copied to `Research papers/`.
- Gotcha: `build_article.py` also rewrites `paper/manuscript.tex`; it was restored byte-identical from `working/manuscript.tex.bak-20261009c` before validation, as in earlier passes.

### 2026-10-11, late evening: paper finished for the DOCX
- Recorded ASan run PASSED (see item B) and is in Section 8.7 / Table 7.
- n=108 solve frozen and written up (item C); Table 7 builder refuses to build if the frozen records change.
- Title changed (F); mechanism paragraph in 10.2 (G); intro preview trimmed and Figure 2 caption shortened (H); abstract trimmed to 238 words.
- Wording fixes: forced-ARWLF run now said to run beside the n=108 solve; "exact-negative" -> "without false negatives"; Section 8.3 reference no longer overstates what it measures; Section 9 notes the model inputs were frozen before the 32-attempt solve.
- `data/README.md`, `SUBMISSION_NOTES.md` updated (table numbers, ASan record, no claim of visual inspection).
- Built and checked: `build_results.py` exit 0; `build_article.py` exit 0; `validate_docx_only.py` exit 0 (14 tables, 238-word abstract). `manuscript.tex`, root `manuscript.tex` and the PDF are byte-identical to before. DOCX copied to `Research papers/` (same SHA-256).
- Not done: page-by-page visual check of the new DOCX in Word; PDF/LaTeX (left alone on purpose); optional runs A and hash-join n=108; commit.

### 2026-10-11, final edit pass (DOCX only; TeX/PDF byte-identical, verified by SHA-256)
- **ASan (item B) done.** Recorded run 2026-10-11 12:40:25Z-16:51:39Z UTC (15,072.8 s): exit 0, `ter_arwlf_solve_tests: 341 checks, 0 failures`, no sanitizer report, executable SHA-256 2de61373...7d55 (matches the binary on disk). Frozen to `paper/data/asan_differential_record.json` + `asan_differential_output.log`. Section 8.7 and Table 7 now cite this recorded rerun instead of "historical development logs". Applied by the new `paper/pending_runs/apply_asan_record.py` (fails closed unless exit 0, no sanitizer report and 341/0).
- **n=108 solve (item C) done in the paper.** Records frozen to `paper/data/full_solve_n108_attempts.jsonl` (byte copy) and `full_solve_n108_captures.jsonl`; Table 7 has an n=108 row; Section 8.7 has a new paragraph (32 attempts, ARWLF every attempt, lists 118-162 s and 1.52 GiB, root process 3.82-3.86 GiB, root 1.77-3.54 h, median 1.82 h, 62.0 h total, 17 attempts with 1-3 residue-exact triples, none binary weight n/2, AttemptLimitReached, planted instance so not UNSAT, not a practical solver); abstract, Section 11 and Section 12 updated; "in progress" wording removed; provenance label updated. `build_results.py` refuses to build if the frozen n=108 records change.
- **Title (F) changed** to "Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting" (manuscript.md, build_article.py header, package_release.py, README.md). Old title is in `working/*.bak-20261011` if you want it back.
- **Related work (G):** new mechanism paragraph in Section 10.2 (hash join, projected tiling, packed 3SUM, vectorized execution, exact-negative byte maps); adds the already-verified reference boncz2005 (16 references now).
- **Intro/caption (H):** question-by-question preview cut to one sentence; Figure 2 caption now points to Section 2.3 instead of repeating the FiniteSanity counts.
- `validate_docx_only.py`: table count 12 -> 14 (it was stale since the per-root and provenance tables were added). Validator passes: 14 tables match the data row by row, 198 equations, 5 figures, 42 PDF/TeX files unchanged, 32 Word-rendered pages, none empty. `build_results.py` passes all raw-data checks.
- `Research papers/ARWLF_Naman_PM.docx` refreshed (byte-identical to `paper/ARWLF_Naman_PM.docx`).

### 2026-10-11, status update (no paper text changed in this entry)
- **n=108 solve (seed 0x8001) finished:** 32 of 32 attempts, all NoWitnessThisAttempt, AttemptLimitReached (not UNSAT). Records in `results/arwlf/full-solve-n108-s0x8001/`. Paper not yet updated (item C).
- **Recorded ASan run started** 2026-10-11 at 18:10:25 by `paper/pending_runs/run_asan_differential_recorded.ps1`, after `cmake --build build-full-asan --config Debug --target ter_arwlf_solve_tests` relinked the test executable (build succeeded). Still running at the last check (18:10). No record.json yet. Item B is in progress, not done.
- **Correction to the earlier ASan wording:** the executable `build-full-asan/Debug/ter_arwlf_solve_tests.exe` was dated 2026-10-06 before the rebuild, not 2026-10-11 as stated in an earlier session. The earlier direct run (12:23 on 2026-10-11, PID 39284) left no `output.log` or `record.json`, which means it was not started through the recording script. It has no result and is not evidence. Its process is gone. Treat the "341 checks, 0 failures" result from earlier sessions as the only pass evidence until the recorded run completes.
- **UBSan dropped** by the author's decision (item E is withdrawn). No UBSan run exists; do not claim UBSan cleanliness.

### 2026-10-09, latest session (everything below was done in this session)
- Forced-ARWLF n=72 staged solve run: `-Backend arwlf -Pin none`, output `results/arwlf/full-solve-n72-s0x7201-arwlf/`. 22 attempts, Found at attempt 21, same verified parent as the dispatcher run. Records frozen to `paper/data/full_solve_n72_arwlf_attempts.jsonl`.
- `ter2_root/scripts/arwlf_full_solve_plan.ps1`: added `-Pin` parameter (`p-core` default, `none` allowed). The running n=108 solve is unaffected.
- `paper/build_results.py`: added forced-ARWLF table row and a consistency check (it refuses to build if the forced run disagrees with the dispatcher run); new Table 3 (per-root results); new Table 10 (frozen build provenance); executable names shortened; table list updated.
- `paper/data/`: `provenance_hashes.json` added and listed in `input_manifest.json`.
- `paper/manuscript.md` / `paper/ARWLF_Naman_PM.docx`: Section 8.7 retitled "Staged integration and differential validation" and rewritten; forced-ARWLF evidence added to Table 7, Section 8.1, 8.6-8.7 text, abstract, discussion and conclusion; Section 9.3 cut to one saturation paragraph; n=144 cost tables and Frontier calculation moved to new Appendix C; data-availability paragraph rewritten; Appendix A provenance table added.
- `paper/pending_runs/`: `run_design_b_comparison.ps1` and `run_asan_differential_recorded.ps1` written and syntax-checked. Neither has been run.
- `.gitignore`: added `!/paper/pending_runs/*.ps1`.
- `paper/SUBMISSION_NOTES.md`: scope line updated for the forced-ARWLF run and the staged integration.
- LaTeX sources (`manuscript.tex`) and `ARWLF_Naman_PM.pdf`: untouched; stale since 2026-10-04.
- Copies: `Research papers/ARWLF_Naman_PM.docx` synced to `paper/ARWLF_Naman_PM.docx` (identical hashes).

### Earlier, for reference
- 2026-10-08: scaling model (`paper/scaling_model.py`, `paper/data/scaling_*.json`), Table 5-9 build code, §9 scaling and limits, Gate 1 fixes (capture attempt budget; production root rule replaces the FiniteSanity structural check), Gate 2 end-to-end n=72 solve.

### 2026-10-11, hash-join baseline review (code written by Codex, reviewed here)
- New files: `ter2_root/campaign/ter_hash_join_root.{hpp,cpp}` (scalar exact hash join), `ter_hash_join_bench.cpp`, `ter2_root/tests/ter_hash_join_tests.cpp`, `ter_hash_join_cli_tests.cmake`; CMake block in `ter2_root/CMakeLists.txt` (+56 lines). No frozen file touched.
- Review result: no defect found; no code changed. Table build, backward-fill duplicate layout, probe, W-bit key compare, high-bit carry and sink call traced by hand against the ARWLF resolver.
- Verified by running: Release build 0 warnings; `ter_hash_join_tests` 218 checks 0 failures (Release and ASan Debug); CTest hash_join tests 2/2.
- Mutation check (file restored byte-identical afterwards): dropping the family carry, dropping the high-bit carry, and collapsing duplicate targets each make the tests fail (84, 23, 88 failures).
- Real data: n=72 and n=108 planted one-cell gates (`--plant=1 --microproblems=1 --validate-only`): K2, K2-WIDE, ARWLF and hash join emit the identical triple and residue sum. The whole-root n=72 gate has zero outputs, so it only checks agreement on emptiness.
- First timing, n=72 whole root, 3 warmups + 7 rounds, CPU 23, AC: K2 280.4 ms (0.45 ns/pair), K2-WIDE 275.8 ms, ARWLF 1,151.0 ms (1.85 ns/pair), **hash join 4,727.9 ms (7.61 ns/pair)**. All outputs exact after every shot. This is a sparse regime (L about 1); it says nothing yet about n=108.
- Not bugs, but worth knowing: (a) the baseline is deliberately untuned (branchy probe loop, two-multiply finalizer hash, no prefetch), so a tuned hash join could be faster; (b) its per-call input validation is charged inside the timed region (a few ms at n=108, negligible); (c) the triple-count preflight bound can return ResourceLimit on a root with about 4x more pairs x targets than n=108.
- n=108 anchor-cell timing: done 2026-10-12, see the changelog entry above.

## Current state (as of 2026-10-12)
- **n=108 solve (seed 0x8001): finished.** 32 of 32 attempts, all NoWitnessThisAttempt, ended AttemptLimitReached (not UNSAT). 62.0 h of root time, 106-212 min per attempt; 17 attempts had some residue-exact triple, none accepted. Records: `results/arwlf/full-solve-n108-s0x8001/`. In the paper (Section 8.7, Table 7).
- **ASan recorded run passed and is in the paper.** The machine is free; the hash-join n=108 timing is done and in the paper (2026-10-12); only optional run A remains.

## Status checklist (last updated 2026-10-12; items in the Done list are from 2026-10-09)

### Done
- [x] Forced-ARWLF n=72 staged solve: 22 attempts, Found at attempt 21, identical verified parent (results/arwlf/full-solve-n72-s0x7201-arwlf/; Table 7, Section 8.7). Run unpinned with the new `-Pin none` option beside the n=108 solve, so its root times are indicative only.
- [x] Section 8.7 retitled "Staged integration and differential validation"; staged vs in-process stated.
- [x] n=144 cost tables and Frontier calculation moved to Appendix C; Section 9.3 is one saturation paragraph.
- [x] Per-root results in Section 8.1 (Table 3); text notes the 3-root bootstrap interval equals the range of the per-root values.
- [x] Table 10 (Appendix A): frozen build records and hashes; data/provenance_hashes.json.
- [x] Data-availability paragraph rewritten with exact commands (archive location still missing, see below).
- [x] Whole-root comparison complete and in the paper (Table 6): K2 54.79 h vs ARWLF/dispatch 55.2-57.4x measured; load-adjusted 50.0-52.0x labelled as an estimate.
- [x] Release build clean (/W4), 59/59 tests, ASan on 57 CTest tests, differential 341 checks 0 failures (provenance gap closed by the recorded rerun, item B).
- [x] Pin option added to `arwlf_full_solve_plan.ps1` (`-Pin p-core|none`, default p-core); existing n=108 run unaffected (PowerShell parses the script at start).
- [x] Design B and recorded-ASan scripts written and syntax-checked in `paper/pending_runs/`; not run.
- [x] Table 3 holdout/calibration values re-verified against results_review.json.

### CPU runs
- [x] **A. Design B comparison.** DONE 2026-10-12 (second, corrected run; see changelog). Current reproduction: `pwsh -NoProfile -File paper/runs/run_design_b_comparison.ps1`
  Times the production root against K2, K2-WIDE, ARWLF and the dispatcher on the same n=72 attempts (minutes). Watch the first run: the parsing of ter_full_bench output was not exercised. Heads-up below. Then add a short table and sentence to Section 8.4 or 8.7.
- [x] **B. Recorded ASan rerun.** DONE: started 2026-10-11 18:10:25, ended 22:21 (4.19 h), exit 0, 341 checks 0 failures, no sanitizer report. Frozen to `paper/data/asan_differential_record.json` + `asan_differential_output.log`; Section 8.7 text and Table 7 row updated.
- [x] **C. n=108 solve result.** DONE: frozen to `paper/data/full_solve_n108_attempts.jsonl` + `full_solve_n108_captures.jsonl`; in Section 8.7 (new paragraph), Table 7 (new row), abstract, Sections 11-12; Table 10 label and "in progress" wording removed.
- [x] P. Hash-join n=108 anchor-cell timing. DONE 2026-10-12: 72.67 s vs ARWLF 0.890 s on the 0x1001 cell; Section 8.1, Section 10.2, Table 1 notes, Table 9, Table 10.
- [ ] D. (optional) A few K2 chunks rerun on a quiet machine; split vs continuous K2 on a small n=72 root.
- [x] E. ~~UBSan~~: withdrawn by the author on 2026-10-11. Not run; do not claim it.

### Can be done now (no CPU needed)
- [x] F. DONE (title changed). Title: consider "Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting" (current: "Wide Leaf Filtering for Exact Ternary Subset Sum Root Reporting").
- [x] G. DONE (mechanism paragraph in 10.2). Related work: compare packed Subset Sum / 3SUM techniques by mechanism, not only by citation (current reference list: 15 cited).
- [x] H. DONE. Trim the Introduction's question-by-question results preview; move the FiniteSanity digit-count detail out of the Figure 2 caption into Section 2.3.
- [x] I. Already covered: Section 7.3 names the CPU (Core Ultra 9 275HX, P-core CPU 23, MSVC) and says it is not a hardware-portability study.

### Final steps (after A-C)
- [ ] J. Regenerate the PDF and LaTeX from the final sources. The .tex and ARWLF_Naman_PM.pdf are untouched since 2026-10-04 and stale; build_article.py regenerates manuscript.tex, then `compile.ps1 -SkipBuild` for the PDF. Read the rendered pages.
- [x] K. DONE for the DOCX (build_results.py + validate_docx_only.py pass). Final editorial and number-check pass; run `python build_results.py` once more.
- [ ] L. Artifact package and commit (only when requested): nothing is committed. Current ignore rules make sources, manuscript assets and research records visible without forced staging. Select the intended source, organization and paper/evidence changes separately; snapshots, local QA, scratch and the historical release ZIP remain ignored.

### Author-only (cannot be done by the assistant)
- [ ] M. Affiliation and correspondence email; authorship, acknowledgments, assistant-use disclosure.
- [x] N. Archive location and persistent identifier for the source, supplement and snapshot regeneration (the Data availability paragraph says it "must be supplied at submission"). Resolved 2026-10-14: the article cites the reserved Zenodo DOI 10.5281/zenodo.23353466 for the source archive and full git history; the record is published before submission.
- [ ] O. Current TALG author instructions, portal metadata and declarations; confirm the venue's fit (TALG favours provable guarantees; this is a constant-factor engineering result, so lead with the exactness proof and cost model).

### Not worth doing
Another 55 h K2 run; an n=144 implementation; more source-thinning cases.

## Heads-up for item A
One unrepeated `ter_full_bench` run (2026-10-07, before the n=108 solve started) reported a classic-solver root stage of about 0.18 s
for attempt 21 of seed 0x7201 at n=72. The staged QUEUE-DENSE root for the same attempt is 0.23-0.46 s. Stage boundaries differ,
so this is not a comparison, but design B may be as fast as or faster than QUEUE-DENSE at n=72. Section 8.4 says QUEUE-DENSE wins among
K2, K2-WIDE, QUEUE-DENSE and ARWLF, which stays true; do not claim it beats the production root until item A has run.
