# Which root architecture is production? (repair items P16 / P19 / §14)

Two root designs existed in this repository at once. This document resolves
which one ships, on measured grounds, so the specification and the code stop
telling different stories.

## The two candidates

**A. Specified v2.1 refined funnel**

```text
RootAux -> Stage C partition -> Stage D region pruning
        -> Stage E carry refinement -> narrowed recovery
```

**B. Generalized bounded streaming root** (what repair items P12/P14 built)

```text
low-w tiled prefilter (width-independent)
  -> low-64 quotient congruence (exact, decisive for Q <= 64)
  -> arbitrary-width residue equality over the original words (Q > 64)
  -> exact TER structural validation -> independently verified witness
```

## Decision

**B is the production root.** A is reclassified: Stage C becomes an optional,
bypassable layout substrate rather than a mandatory production stage, and
Stages D/E remain unimplemented research with a precisely stated open problem
(see `root_pair_space_analysis.md`).

This is a decision about which code path ships. It is **not** a claim that A is
impossible.

## Evidence

### B satisfies every required exact-authority property

| Required property | How B provides it | Evidence |
|---|---|---|
| No false negatives | low-w and low-64 filters are necessary conditions on an exact congruence | `test_wide_quotient_root_uses_arbitrary_width_authority` |
| Arbitrary width | residue equality over original BigInt words; no truncation | Q=69 fixture; mutation-tested |
| Bounded memory | streams A x B, never materializes a pair index | `l1_to_l0_pair_records = 0` at n=72; n=108 no longer refuses |
| Exact witness recovery | retains the parent digits of the accepted triple | n=36 `--attempt-limit=100` → `L0=1`, verified |
| Exact residue equality | `residue != root_target` decides | `src/full_ter.cpp` root visitor |
| TER structural validation | `add_three` against expected digit counts | existing full-TER tests |
| Cancellation correctness | cooperative flag polled in the scan | `ter_bench_cli_timeout_is_cancelled` |
| Resource-limit correctness | fail-closed admission bounds | `test_memory_accounting_is_conservative` |

### Stage C's measured cost and benefit

Genuine captured n=72 root, CurrentScatter, 1 thread, 2 warmup + 10 repeats:

| list | N | median partition ns | scratch bytes |
|---|---:|---:|---:|
| A | 19,109 | 28,450 | 156,968 |
| B | 32,496 | 53,450 | 264,064 |
| C | 33,505 | 54,200 | 272,136 |

- Stage-C front end for A+B+C: **0.136 ms**
- Stage-C scratch: **0.66 MB**
- Production root wall time: **530.89 ms**
- Stage C as a fraction of root time: **0.03 %**

So Stage C is *not* rejected for being expensive. It is essentially free.

It is deferred because it currently **enables nothing**: the resulting
`rho_open` is unchanged. The production tiled reporter already realizes the
same low-`k` filtering by windowing, without physically rearranging records —
it reports `low_prefilter_candidates = 1,238,960`, which is the k=24 opened
mass the pair-space probe independently measures as 1,240,683.

Physically partitioning the lists would therefore add 0.03 % cost for zero
measured pruning gain **today**. It remains the natural substrate for the
output-optimal enumeration that the open Stage-D question would need, and at
0.03 % its cost is not the obstacle.

## What this does NOT claim

- It does not claim B is faster than A. A was never implemented end to end.
- It does not claim a solver speedup of any kind. B removes a width cliff and a
  memory cliff; pair work is unchanged at 620,966,064 probes, `rho_open = 1.0`.
- It does not claim Stage D/E are impossible. See
  `root_pair_space_analysis.md`: `rho_open` reaches 0.002 (500x) and an
  output-optimal enumeration would give 250x total-work reduction. The gating
  question — sparse 3SUM enumeration over `Z_{2^k}` — is open.

## Consequent status

- **P16**: DEFERRED WITH MEASURED JUSTIFICATION. Stage C reclassified from
  mandatory production stage to optional bypassable substrate.
- **P19**: NOT IMPLEMENTED. The narrowed-recovery pipeline does not exist. B
  supplies the exact-authority properties the pipeline was required to
  guarantee (table above), but B is a different design, not that pipeline.
- **P17 / P18 / P28**: NOT IMPLEMENTED. See `root_pair_space_analysis.md`.
