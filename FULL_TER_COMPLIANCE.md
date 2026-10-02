# Classical Depth-3 TER Compliance Map

Authoritative paper: Yang Li, *From Subset-Sum to Decoding: Improved
Classical and Quantum Algorithms via Ternary Representation Technique*,
Information 16(10), 887 (2025), DOI `10.3390/info16100887`.

| Paper/contract requirement | Implementation | Verification |
| --- | --- | --- |
| Definition 1 exact ternary distributions | `FullTerParameters`, `count_digits` | `test_parameters`, stage audits |
| Tables 3 and 4 finite counts | `derive_full_ter_parameters` | exact finite and paper-rounded parameter tests |
| Exact `R1`, `R2`; `r=floor(log2(R))/2` | prime-factor multinomial calculation in `full_ter.cpp` | exact `R1=17153136`, `R2=90`, `r1=12`, `r2=3` test |
| Paper target relationships | `random_targets`, `guided_targets`, `targets_are_consistent` | every solver attempt fails closed on inconsistency |
| 18 logical half-supported L3 lists | two shared immutable physical bases plus logical count 18 | complete cardinality/support/sharing tests |
| Packed ternary representation | disjoint positive/negative word masks in optimized full-TER entries | optimized packed stages vs independent scalar stage oracles |
| 18 to 9 binary MITM | `binary_mitm` | nested-loop `binary_oracle` in audit mode |
| 9 to 3 ternary merge and distribution filter | `ternary_mitm` | cubic `ternary_oracle` in audit mode |
| 3 to 1 full-target root merge | M2 `ternary_tiled_l1_to_l0` for every well-formed shape (`bounded_root_supported(n, r1)` is `r1 != 0 && r1 < n`); `ternary_mitm` remains only as the malformed-shape branch | root scalar oracle, n=72 47-bit quotient fixture, and guided end-to-end test |
| 9 to 3 ternary merge dispatch | bounded projected tiling whenever `bounded_full_ter_l2_l1_supported(n, r1, r2)` holds, i.e. `min(r1, r2+32) < 64` and `< n`; `ternary_mitm` otherwise | preflight regressions at n=72/108/144 |
| Parent deduplication and provenance multiplicity | packed canonicalization plus audit-only child-ID records | exact optimized-vs-scalar provenance comparison at all three stages |
| Independent exact root verification | `verify_full_ter_witness` | valid, non-binary, wrong-weight and target tests |
| Deterministic retries; no false UNSAT | `solve_full_ter` | random replay and attempt-limit tests |
| Full versus reduced semantics | `TerExecutionMode` | complete tiny L3 and paper-rounded resource-limit tests |
| Resource and cancellation failure states | checked preflights and cancellation flag | resource-limit and pre-cancel tests |
| Frozen T0 compatibility | unchanged T0 public lane | all five original suites remain green |
| Single-threaded random-target benchmark with stage telemetry | `ter_full_bench` | native Release benchmark execution |

The implementation supports exact arithmetic modulo powers of two, including
multiword residues. It does not claim arbitrary-modulus support. `FixtureTer`
may retain a guided representation in reduced authentic lists; it is a pipeline
fixture, not a full performance result.

## M2 root merge

For a supported root, M2 derives the complete quotient
`((residue - child_target) mod 2^n) >> r1` from the multiword residue, keeps it
in `uint64_t`, and sends only its low `w` bits to the cache-tiled reporter. A
low-bit match is merely a prefilter candidate: the full `n-r1` bit equation is
checked before packed ternary validation and parent construction. This keeps
the solution set exact and avoids an `|A|*|B|` root pair-record array.

There is no `n - r1 <= 64` width gate. Where the root quotient no longer fits a
machine word the quotient congruence is demoted to a no-false-negative
prefilter and the arbitrary-width residue words remain the deciding authority,
so the bounded streaming root covers every constructible finite shape. Removing
the `|A|*|B|` pair *memory* is not the same as removing the `|A|*|B|` pair
*work*: the root still performs the streaming probes.

The deterministic defaults (`w=24`, preferred tile width `15`) are M2 policy,
not optimality claims. M2 does not change TER's asymptotic exponent; M3, if
authorized later, is the only place for parameter tuning.

`FullTerConfig::root_capture` is a deliberately narrow replay seam: it copies
the genuine L1 lists immediately before the root stage, retaining every residue
as little-endian 32-bit words. Its copied storage is charged against the same
declared memory ceiling, and its explicit execution mode prevents FixtureTer
data from being labelled FullTer.

Out of scope and absent from the FullTer solver: NTT, FFT, sparse convolution,
Schroeppel-Shamir integration, GPU execution, parallelism, quantum TER, and
decoding.

This statement is about solver integration. The repository does contain an
isolated count-only Q3REPORT NTT comparator (`q3report_audit`,
`q3report_crossover`, `q3report_paper_crossover`), authorized separately by
`TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md`. It is an experimental
arithmetic comparator only: no FullTer stage dispatches to it, and it grants no
authorization to integrate NTT into the solver.
