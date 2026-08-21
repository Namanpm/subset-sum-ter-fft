# subset-sum-ter-fft
Experimental subset-sum solver exploring representation techniques (TER), FFT-based methods, exact pruning, and high-performance search.

## Classical depth-3 TER

The repository contains a single-threaded classical implementation of Yang
Li's depth-3 TER Algorithm 2 with the exact topology `18 -> 9 -> 3 -> 1`.
`FullTer` materializes complete level-3 sets or returns `ResourceLimit`;
`FixtureTer` is an explicitly reduced correctness mode and is never reported as
a complete execution. If the complete leaf cardinality exceeds the 64-bit
telemetry range, fixture mode exposes that overflow explicitly while full mode
returns `ResourceLimit`. The implementation currently supports the canonical
power-of-two modulus `N = 2^n`.

Build and test with MSVC:

```powershell
cmake -S . -B build-full -DTER_NATIVE_OPT=ON
cmake --build build-full --config Release
ctest --test-dir build-full -C Release --output-on-failure
```

Run the complete tiny-parameter end-to-end benchmark:

```powershell
.\build-full\Release\ter_full_bench.exe
```

The same bounded full-TER benchmark can exercise the M2 root path at `n=72`
(the genuine root quotient is 47 bits) without changing source:

```powershell
.\build-full\Release\ter_full_bench.exe --n=72
```

Its appended `l1_to_l0_*` CSV fields distinguish low-bit prefilter candidates
from full-quotient matches. A prefilter match is never accepted without the
full 47-bit check.

For explicitly synthetic adversarial L2-to-L1 merge fixtures, use the separate
stress benchmark. Its rows are never paper-faithful FullTer evidence:

```powershell
.\build-full\Release\ter_stress_bench.exe
```

See [TER_STRESS_GENERATOR.md](TER_STRESS_GENERATOR.md) for the stress API,
invariants, telemetry, and boundary semantics.

The `n=36` benchmark is a complete deterministic random-target performance
execution. Guided targets are reserved for differential pipeline-connectivity
tests and are not substituted for the paper's random-target benchmark.
The paper-rounded `n=360` complete level-3 sets are exponentially large and the
current machine-scale implementation correctly returns `ResourceLimit`; reduced
`n=360` fixtures must not be described as full Algorithm-2 runs.
