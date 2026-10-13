# Hash-join baseline implementation ledger

Local checkout: Desktop/subset-sum-ter-fft, HEAD 8e9fe7f, existing dirty work preserved.
Claude's outline had no implemented hash-join files at inspection: resume at implementation, not a rewrite.

Protected core: all 29 files in ter2_root/frozen, linked unchanged.
Allowed surface: new campaign reporter/runner, new tests/run scripts, additive CMake registration.
No staging, commits, solver changes or automatic long benchmark runs.
An n108 solve process was active at preflight; all verification runs must use one build/test worker.
The canonical work_state tool is unavailable in this session; this ledger records continuation state.

## Contract and plan

1. Independent cubic original-whole-Q oracle with exact identity multiset; RED before implementation.
2. Scalar flat open-addressed Y table, <=0.5 load factor, contiguous duplicate posting ranges.
3. Full W equality then high-bit carry; exact shared sink; no allocations during pair probing.
4. Preflight widths/layouts/size arithmetic/budget before any output. No cap/truncation or partial-success state.
5. Add matched snapshot harness and provenance script; all builds charged inside reset+call timing.
6. Build/targeted tests/sanitizers and independent four-objective review; verify frozen identity.
7. Leave authentic n108 timing/cross-checks for user execution while the solver is active.

Ruling: K2's early triple count is low-25, not full-Q. Compare terminal exact witnesses/counts to K2,
and full-key multiset digests/counts to K2-WIDE and ARWLF. Comparing all early counters is invalid.
Ruling: Reuse this dirty local checkout directly as requested; do not create a worktree, branch or commit.
Ruling: Timings/paper row cannot be marked measured until the user runs the supplied benchmark.

Pre-change: frozen manifest 29/29 intact; existing ter_a64_chqr_tests passed 1/1 (15.83 s).
Task 1: RED observed: 194 checks, 181 failures against the temporary missing-reporter stub.
An initial test compile caught an ADL name collision; renamed the independent identity oracle.
MSVC rejected /Qvec-; removed that unsupported flag. The reporter has no intrinsics or vector kernel.
Task 2: implementation written; flat 16-byte slots, contiguous duplicate IDs/high fields,
checked preflight, per-call scratch reused only across table rebuilds; GREEN: 218 checks, zero failures.

## Final implementation and validation status

Code complete: independent reporter, matched four-backend runner, safe PowerShell
campaign script, oracle tests, CLI safety regression, additive CMake registration.
No FullTer production dispatch changes. No frozen backend source changes.

Actually verified on 2026-10-10:
- Native MSVC Release build of reporter/tests/runner passed.
- Release CTest: both new tests passed; core test reports 218 checks, zero failures.
- MSVC Debug AddressSanitizer: new core test passed (1.43 s CTest wall time).
- GCC 15.2 strict standalone compile of new reporter with -Werror passed.
- Authentic n72 whole-root validation, all 4096 cells for 0x7201, 0x7203,
  0x7207: K2, K2-WIDE, ARWLF and hash join agree on exact terminal witness
  multisets and counts; the three full-Q reporters also agree on raw digests.
  All three unmodified roots have zero full-key hits; this is not global UNSAT.
- Authentic n72 anchor-cell validation passed unmodified and with one explicit
  planted authority target; planted output has exactly one matching witness.
- Independent four-objective reviewer found one output/input-alias safety defect;
  fixed before loading/opening output. Disposable-fixture regression passes for
  identical, ./, Windows case and hard-link aliases, with input SHA256 unchanged.
  Reviewer rechecked fix: no remaining concrete finding.
- PowerShell parser: zero errors. Frozen manifest: all 29 files match.
- No controlled performance samples were run; validation times are not benchmarks.

Remaining gates, not silently counted as passes:
- Authentic n108 anchor validation and timed discovery are deferred while the
  existing n108 solve is running. Supplied script refuses concurrent ter_* work.
- UBSan execution blocked: MSVC has no UBSan; local WSL Ubuntu cannot start
  (Wsl/Service/0x8007274c). An isolated GCC UBSan-trap build reached the frozen
  translated-tag locator, which calls unavailable posix_memalign under MinGW.
  No frozen portability fix was made. This is a build refusal, not a UBSan finding.
- No measured paper performance row or cost extrapolation can be asserted yet.

All new files remain unstaged/uncommitted. Existing dirty work is preserved.
Existing '*.md' ignore rule hides this ledger and HASH_JOIN_BASELINE.md from
ordinary git status; ignore rules were not changed. The files exist locally.
