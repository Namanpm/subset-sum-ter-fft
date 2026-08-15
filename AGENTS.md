# Repository Scope

This repository implements Yang Li's classical depth-3 ternary representation
algorithm for random density-one subset sum.

In scope:

- the frozen T0 level-2 to level-1 merge baseline;
- complete classical TER with topology `18 -> 9 -> 3 -> 1`;
- exact finite parameter derivation and power-of-two modular arithmetic;
- scalar correctness authorities, packed representations, deterministic retry
  semantics, telemetry, tests, and single-threaded benchmarks.

Out of scope until separately authorized:

- NTT or FFT filtering;
- sparse convolution;
- Schroeppel-Shamir integration;
- GPU or multithreaded execution;
- quantum TER or decoding algorithms.

`FullTer` always means complete level-3 sets with no sampling or truncation.
`FixtureTer` is an explicitly reduced correctness/benchmark mode and must never
be reported as a complete Algorithm-2 execution. Attempt exhaustion is UNKNOWN,
not UNSAT. A solution is reported only after independent full-modulus witness
verification.
