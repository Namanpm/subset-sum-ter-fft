# CLAUDE.md

@AGENTS.md

`AGENTS.md` (imported above) holds the repository rules and wins over anything
here. This file adds how Claude runs work in this repo: an
operator loop that plans, executes, verifies and records lessons. It is adapted
from a generic "software factory" workflow; the parts that don't fit a
correctness-critical research codebase are changed or dropped below.

---

## What "done" means here

The outcome of a task is a **verified research artifact**, not a deployed app:

- a code change with differential tests, built in Release and passing CTest;
- an experiment result in `results/`, reproducible from its recorded command,
  seed, commit and build configuration;
- a doc or addendum that matches the implemented semantics.

There is no deployment. "Shipping" means: Release build, the full relevant CTest
set green, the diff inspected, and a commit **only when the user asks for one**.
A negative or inconclusive result is a finished deliverable if it is reported
honestly.

---

## Operator loop

1. **Plan.** Write a small task graph: the outcome, the correctness authority,
   the files involved, the tests that prove it, dependencies, and risks or open
   questions. Find the governing document first (source priority in
   `AGENTS.md`). Keep the plan minimal. Spend effort on code and evidence.
2. **Execute.** Inspect, then edit, then build the narrowest target, then test
   and debug. Search before adding any API (`include/ter/`, `src/`,
   `ter2_root/`).
3. **Verify.** Use the gates below before moving to the next task.
4. **Feed back.** Handle failures as described below, and record anything
   reusable in **Lessons** at the end of this file.
5. **Complete.** Run the completion checklist and write the final report in the
   format `AGENTS.md` requires.

### Roles → what they mean in this repo

| Generic role | Here |
|---|---|
| Product | The frozen spec or experiment plan for the task. Never invent requirements; ask if the task leaves a mathematical choice open. |
| Engineer | C++20 under the strict warning set; exact arithmetic; no speculative abstraction. |
| QA | Independent oracles (scalar/reference, cubic ternary oracle, root scalar oracle, `ter2_root/tests/ter_synthetic_root.hpp`), differential tests, ASan/UBSan. |
| DevOps | The build and benchmark host: Release builds, pinned cores, `ter2_root/scripts/bench_guard.ps1`, provenance recording. |
| Research | Repository documents first (`Research papers/`, `docs/`, the `*.md` specs at the root), then external literature. |
| Design | Not applicable, except figures and tables for `paper/` and `manuscript.tex`. |

### Subagents and parallelism

- Spawn subagents only when the user asks. Give each one only the context it
  needs and require it to return evidence (commands, outputs, file paths).
- Parallel agents that edit code use separate git worktrees
  (`.claude/worktrees/`) **and separate build directories**. Two agents must
  never build into the same `build-*` tree.
- Never run builds, tests or other heavy work while a benchmark is running. If
  `bench.lock` exists at the repo root, `bench_guard.ps1` is protecting a
  running benchmark: wait for it to finish, or ask the user.
- A subagent's claim isn't evidence until its output has been checked. Re-run the
  key test or read the produced file yourself.

---

## Verification gates

```powershell
# Release build + full suite (61 CTest tests at the time of writing)
cmake -S . -B build-full -DTER_NATIVE_OPT=ON
cmake --build build-full --config Release --parallel
ctest --test-dir build-full -C Release --output-on-failure

# Narrow iteration
cmake --build build-full --config Release --target <test_target>
ctest --test-dir build-full -C Release -R <regex> --output-on-failure

# List what is registered
ctest --test-dir build-full -N
```

`AGENTS.md` groups the registered tests (updated 2026-10-12); `ctest -N` stays
the authoritative inventory. `ter_frozen_root_backends` fails if any pinned
frozen file changes.

Gate per change type:

| Change touches | Minimum verification |
|---|---|
| Shared arithmetic, types, packed or generator code | Full suite (Release), plus ASan where supported |
| FullTer | `ter_full_tests`, `ter_bench_cli_*`, plus the lower-level suites |
| Q3REPORT / tiled | The Q3REPORT and tiled suites, plus arithmetic and FullTer regressions |
| `ter2_root/` | The `ter_arwlf*`, `ter_hash_join*` and `ter_root_*` suites, plus `ter_frozen_root_backends` |
| Performance claim | Before/after Release measurements on identical inputs, with repeats and medians. Never from Debug. |
| Docs only | Check every stated command, number and file path against the repo |

Also review the diff for edge cases, overflow, shifts, narrowing, invalidated
references and failure-state conflation before marking a task complete.

---

## When something fails

1. Read the actual error, log or test output. Don't guess.
2. Find the root cause, then write a focused fix for it.
3. Retry up to **3 times**. If it still fails, stop and escalate to the user
   with the evidence and concrete options.
4. **Exception:** if an optimized path disagrees with an independent oracle,
   that is a blocker, not something to retry around. The optimized code is
   wrong until proven otherwise. Never weaken, skip or rewrite the test or
   oracle to get green.
5. If the failure taught something reusable (a toolchain quirk, a build
   pitfall, a semantic trap), add one line to **Lessons** below.

---

## Completion checklist

- [ ] Every task in the plan is done, or explicitly reported as not done.
- [ ] The relevant tests were actually run in Release, and the outputs were read.
- [ ] The full suite was run if shared/core code changed. ASan was run if required and supported (otherwise say it was unsupported).
- [ ] Performance claims are backed by measured Release before/after numbers.
- [ ] No frozen file changed (`ter_frozen_root_backends` passes).
- [ ] Failure semantics are intact (ResourceLimit, AttemptLimitReached and NotApplicable are not reported as UNSAT or zero).
- [ ] Living docs are updated. Frozen plans are left untouched; results go in addenda.
- [ ] The diff is inspected: no scope creep, no build outputs, no unrelated user changes touched.
- [ ] The final report covers: files changed, what changed, decisions, tests and benchmarks actually run, limitations, git status, and next steps.

---

## Lessons

Append one dated line per lesson. Keep only what would prevent a repeat.

- 2026-10-12: `bench.lock` at the repo root means a guarded benchmark is
  running. Heavy work started then contaminates timings
  (`ter2_root/scripts/bench_guard.ps1`).
- 2026-10-12: `paper/validate_docx_only.py` hashes 42 PDF/TeX files at fixed paths
  (including root `tiling_implementation_idea.pdf` and `Research papers/*.pdf`); moving one
  fails validation. Never move or rename a `.pdf`/`.tex` file.
