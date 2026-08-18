# CLAUDE.md

## Claude repository instructions

Before doing any repository work, read `AGENTS.md` completely.

`AGENTS.md` is the canonical repository-wide instruction file and takes priority over this file for:

- TER mathematics and terminology;
- FullTer versus FixtureTer semantics;
- correctness authorities;
- failure semantics;
- testing requirements;
- benchmark methodology;
- scientific-integrity rules;
- performance rules;
- Git/commit behavior;
- scope restrictions;
- the TER-Q3REPORT NTT experiment.

Do not duplicate or reinterpret those rules here.

If this file and `AGENTS.md` disagree, follow `AGENTS.md` unless the user's current task explicitly overrides it.

---

## Working style

Use the repository itself as the primary implementation context.

Before editing:

1. read `AGENTS.md`;
2. inspect the relevant source/header files;
3. inspect relevant tests;
4. inspect the applicable design or frozen experiment document;
5. search for existing helpers/types before creating new ones.

Do not rely only on conversational summaries. Do not hallucinate APIs. Do not assume a planned API was actually implemented. Do not redesign an existing subsystem unless the task explicitly asks for redesign.

---

## Token efficiency

Do not repeatedly summarize what you are about to do. Do not paste large unchanged files. Do not produce long ceremonial progress narration.

For a well-specified implementation task, proceed through:

```text
inspect
-> implement
-> build
-> test
-> debug
-> verify
-> benchmark if required
-> inspect diff
-> report
```

without asking for confirmation after routine steps.

Spend context on code, failures, diagnosis, correctness, and verification.

---

## Planning

For substantial work, form a concise internal plan identifying:

```text
required behavior
current authority
files likely affected
tests proving correctness
verification commands
```

Do not create architecture documents unless requested or genuinely necessary. Do not spend a large fraction of the task rewriting the user's specification.

---

## Mathematical changes

For TER arithmetic, merge semantics, generators, packed representations, or FullTer stages:

- inspect the existing correctness authority first;
- preserve exact semantics;
- use independent differential/reference tests where available;
- do not share a transformed formula with its supposedly independent oracle;
- treat mismatches as blockers;
- explicitly test wraparound and boundary arithmetic.

Never simplify TER mathematics merely because another implementation is easier.

---

## Experimental work

Treat experiments as experiments and do not optimize toward a predetermined conclusion.

For Q3REPORT specifically:

- it is count-only;
- it is not solver-integrated NTT;
- it currently does not authorize witness recovery;
- authentic, distribution-matched, and fixed-Q snapshot families remain distinct;
- existing finite negative results remain preserved;
- new scaling studies are added rather than rewriting old measurements.

A negative result is acceptable.

---

## Coding style

Match the existing C++20 repository.

Prefer:

```text
flat contiguous storage
explicit ownership
checked integer arithmetic
small concrete helpers
deterministic algorithms
clear correctness invariants
```

Avoid unnecessary:

```text
inheritance
virtual dispatch
factories
generic plugin systems
std::function in hot code
per-element dynamic allocation
framework-building
```

Do not mass-format unrelated files.

---

## Testing

Never report a test as passing unless it was actually executed.

During iteration, run the narrow relevant target. Before completion, run every suite required by `AGENTS.md` for the changed semantics.

If a requested sanitizer cannot run on the active toolchain, state that instead of implying success.

Do not delete, skip, weaken, or rewrite a failing correctness test merely to make the task green.

---

## Performance

Do not benchmark Debug builds. Do not claim an optimization is faster without before/after Release measurements.

Do not hide required algorithm costs. Do not include optional audit/report costs in core timings unless the benchmark is explicitly end-to-end.

Profile before substantial micro-optimization. Keep an optimization only if:

```text
correctness still matches
AND
measured performance actually improves
```

---

## Git

Do not commit, stage, push, reset, or rewrite history unless explicitly requested.

Preserve unrelated user modifications.

Before a requested commit:

```text
inspect diff
run required verification
exclude generated artifacts
commit only intended files
```

---

## Completion

Before claiming completion:

1. re-read the relevant requirements;
2. inspect the final diff;
3. run required tests;
4. inspect actual outputs;
5. run required benchmarks for performance claims;
6. verify no unintended scope expansion occurred.

Final response should be short and contain only useful evidence:

```text
files changed
implementation summary
important correctness/design decisions
tests/builds actually run
benchmark results if relevant
blockers/deviations
Git/commit status when relevant
```

Do not produce a victory speech because the compiler returned zero.
