# End-to-end Full TER solve with the frozen root dispatcher — preregistration

Written 2026-10-04, before any of this code was compiled or run (a whole-root benchmark was running; nothing
was built beside it). Phase I3 of TER_L1_to_L0_ARWLF_Architecture_v1.0.md, section 9. The frozen backends
(FROZEN_ROOT_BACKENDS.md) and the core solver (`src/`, `include/ter/`) are unchanged.

## Question

Can the complete classical Full TER pipeline, with its L1 -> L0 root merge done by the frozen dispatcher
(ARWLF at n=108), solve a random density-one instance at n=108, with the answer verified independently against
the original instance? And how is one attempt's time split between building the L1 lists and the root merge?

## Design (ruling)

The spec's I3 wording is "ARWLF inside the FullTer root behind a flag". That is not done literally:

1. **Dependency direction.** The frozen backends (`ter2_frozen`) link the core solver library (`ter_t0`): their
   workload loaders call the solver. A call from `solve_full_ter` into them would be a cycle, or would move
   frozen files into the core.
2. **Memory at n=108.** The solver's in-process root capture holds every L1 entry unpacked; at n=108
   (17.4 million entries) it does not fit 4 GiB, which is why every n=108 snapshot was made by the staged
   path below.
3. **Nothing protected changes** this way: no core edit, no frozen edit.

Instead each attempt k is composed from existing, tested machinery:

| Step | Component | Status before this work |
|---|---|---|
| L3 -> L2 -> L1 of attempt k | `ter::stream_full_ter_root_lists` via `ter_root_snapshot_capture --attempt=k` | the solver's own merges and targets (seed + k); identity-gated entry for entry against `solve_full_ter`'s capture at n = 36, 72 |
| L1 -> L0 | frozen `dispatch_and_run` over all 4096 microproblems (`ter_arwlf_whole_root` library) | frozen; whole-root n=108 run in progress |
| accepted triples | frozen authority (residue sum mod 2^n), then solve_full_ter's root rule: binary parent of weight n/2 | rule new (see Gate 1 record) |
| parents | digit sum of the three raw, checksummed snapshot records | new, independent of the backends' tables |
| verdict | `ter::verify_full_ter_witness` against the attempt's original instance | the solver's own verifier |

New code: `ter2_root/campaign/ter_arwlf_solve.{hpp,cpp}` (one attempt), `ter_arwlf_solve_attempt_main.cpp`
(executable), `ter2_root/tests/ter_arwlf_solve_tests.cpp`, `ter2_root/scripts/arwlf_full_solve_plan.ps1` (the
attempt loop). Per-attempt outcomes mirror `solve_full_ter`: Found only with every root parent verified;
NoWitnessThisAttempt otherwise; any internal disagreement is InvariantViolation and stops the run; an
exhausted budget is AttemptLimitReached, never UNSAT.

**Cost if this ruling is wrong:** a reader may object that the attempt loop is a script around the solver's
staged path rather than a flag inside `solve_full_ter`. The answer is the identity gate (same lists as the
solver) plus the differential test below (same per-attempt outcome as the solver).

## The instance (stated exactly)

`root_generalization::authentic_instance(n, seed)`, the instance of every ARWLF result so far: the n weights
are `ter::build_instance`'s seeded SplitMix64 draws (uniform n-bit integers); the planted solution is then
**fixed** to the first n/4 coordinates of each half (coordinates 0..26 and 54..80 at n=108) and the target is
their sum mod 2^n. Random weights, structured planted support: exactly TER's balanced case (n/4 ones per
half). It is not a uniformly random planted subset; reports must say so. The solver may also find a different
valid subset; `planted_found` records whether the planted one was among the verified parents.

## Gates (in order; each must pass before the next starts)

1. **Build and tests** (only on an otherwise idle machine): Release build; full CTest including
   `ter_arwlf_solve_tests` and `ter_frozen_root_backends`; `ter_arwlf_solve_tests` under ASan.
   `ter_arwlf_solve_tests` checks, at n=72 over 4 seeds x up to 8 attempts, with DISPATCH and with ARWLF
   forced: the same outcome as the classic attempt, accepted triples equal to the classic
   `report_full_ter_root` count, the classic witness among the verified parents, and at least one Found
   attempt; n=36 NotApplicable (root Q = 24); a missing or damaged snapshot refused.
2. **n=72 end-to-end dry run** of the plan script (`-N 72 -Seed 0x7201 -MaxAttempts 16`, own output folder).
3. **n=108 solve**, below.

## The n=108 solve (preregistered)

- n = 108, seed **0x8001** (not used by any earlier experiment), backend `dispatch`, at most **32 attempts**,
  P-core pinned root stage, `bench_guard.ps1` running, no other benchmark on the machine.
- Expected cost per attempt (from measured parts): capture about 2.5 min, workload load about 0.3 min, root
  about 60 min. 32 attempts would be about 33 h.
- **Attempt count is not predicted.** The parameters' representation counts (`--preflight`: 2^79 at level 1
  against 2^78 of constraint; 2^24 against 2^24 at level 2) only suggest a moderate per-attempt success
  probability; attempt 0 of the n=72 anchor and of n=108 root 0x1001 both had no root output. Gate 1's scan
  measures the n=72 rate on the way.
- Reported: the attempt that succeeded (or AttemptLimitReached), per attempt the capture / load / root /
  verification times, the verified witness, whether it is the planted solution, peak memory, and the guard's
  interference audit.

## Not in scope

The classic design-B root at n=108 per attempt (its in-process capture does not fit; the n=108 cross-check is
the whole-root run, where ARWLF, DISPATCH and frozen K2 report identical outputs on one attempt). The I2
baseline timings. Any change to the solver, the frozen backends, the TER parameters or the attempt policy.

## Gate 1 record (2026-10-06)

Two defects in the new wiring were found by its own differential test and fixed before any result:

1. **Attempts k >= 1 could not be captured.** The staged stream admits attempt k only inside a configuration
   budget of more than k attempts, and `authentic_config` has a budget of 1. `ter_root_snapshot_capture` and the
   test now set `attempt_limit = attempt + 1`; the budget gates nothing else in the stream.
2. **The frozen structural check is the wrong root rule.** `tt::structural_parent_valid` compares a root
   parent with `build_shape(FiniteSanity, n).parent`, the level-1 distribution (at n = 72: 2 negative,
   56 zero, 14 positive). A genuine root parent is binary with weight n/2 (`src/full_ter.cpp`,
   `root_counts{0, n/2, n/2}`), so no real solution passes it; the campaign's planted outputs pass only
   because they were constructed to that distribution. The solve path now accepts a residue-exact triple
   by the solver's rule and no longer uses the sink's structural count. The frozen file is unchanged.
   Consequence for earlier reports: their "structural" counts measure the FiniteSanity rule, not the
   root rule; timings and residue-exact counts are unaffected.

The positive path is pinned: seed 0x7201 attempt 21 is the first Found attempt of that instance
(`ter_full_bench --n=72 --mode=time-to-found --seed=29185`); the 32-attempt scan has none.

## Gate 2 record (2026-10-07)

`arwlf_full_solve_plan.ps1 -N 72 -Seed 0x7201 -MaxAttempts 32`; results in
`results/arwlf/full-solve-n72-s0x7201/` (capture sha256 2f7aab60..., solve sha256 1355f45f...).

- **Budget changed from the preregistered 16 to 32**, before the run, because Gate 1 had already measured
  this instance's first Found attempt at 21; 16 could only end AttemptLimitReached.
- **Script fix:** the CPU-name lookup read the whole registry key, which fails under PowerShell 7 on a
  binary value; it now reads `ProcessorNameString` alone. No computation changed.
- **Result:** attempts 0-20 NoWitnessThisAttempt, attempt 21 **Found** (DISPATCH ran QUEUE_DENSE): one
  residue-exact triple, one parent, verified against the original instance, equal to the planted solution;
  root 0.38 s, peak working set 44 MiB, pinned CPU 23. The loop stopped at the first Found as specified.
  Same attempt and outcome as the classic solver (Gate 1 differential).
