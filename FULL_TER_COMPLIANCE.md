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
| 3 to 1 full-target root merge | `ternary_mitm` with `D^n[0,1/2]` | root scalar oracle and guided end-to-end test |
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

Out of scope and absent from the implementation: NTT, FFT, sparse convolution,
Schroeppel-Shamir integration, GPU execution, parallelism, quantum TER, and
decoding.
