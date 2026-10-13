# Why finite TER requires `n % 36 == 0`

Repair item **P11**. Derived from `derive_full_ter_parameters_impl()` in
`src/full_ter.cpp` and verified with `ter_full_bench --preflight`.

## Result

`n = 100` **cannot** be supported exactly under the current finite TER shape
construction. The obstruction is arithmetic, not a defensive guard that could
simply be deleted. `36 | n` is not a conservative over-approximation; it is
exactly the set of dimensions for which the FiniteSanity digit-class counts are
integral with the required parities.

## Derivation

FiniteSanity leaves every epsilon at zero, so with
`ε1_level1 = ε0_level1 = ε1_level2 = ε0_level2 = ε₋1_level2 = 0`:

```text
w1        = ε1_level1 + 2*ε0_level1              = 0
positive1 = n/6 + w1                             = n/6
zero1     = n - w1 - positive1                   = n - n/6
w2        = w1/3 + ε₋1_level2 + ε1_level2 + 2*ε0_level2 = 0
positive2 = n/18 + w2                            = n/18
level3_active_half.positive = positive2 / 2      = n/36
```

The derivation then enforces these checks, each of which is a real TER
constraint rather than an implementation convenience:

| Check in source | Requires | Meaning |
|---|---|---|
| `positive1 = n/6` integral | `6 \| n` | the L1 positive digit class must have an integral count |
| `(n/6 - ε1_level1) % 2 == 0` | `12 \| n` | that class must split evenly across the two halves |
| `positive2 = n/18` integral | `18 \| n` | the L2 positive digit class must have an integral count |
| `positive2 % 2 == 0` | `36 \| n` | that class must split evenly across the two halves |
| `level3_active_half.positive = positive2/2` integral | `36 \| n` | the active L3 half-support must be integral |

`lcm(12, 36) = 36`, so the binding condition is exactly `36 | n`.

## Why `n = 100` fails

`100 / 6 = 16.667`. The L1 positive class count is not an integer, so
`positive1` is undefined before any parity constraint is even considered. Two
further constraints fail independently:

- `100 / 18 = 5.556` — the L2 positive class is not integral;
- even taking C++ truncation, `n/18 = 5` is odd, so `positive2 % 2 == 0` fails.

No choice of epsilon repairs this: `n/6` and `n/18` appear as additive terms in
`positive1` and `positive2`, so the epsilons shift those counts but cannot make
a non-multiple of 6 divide evenly. Note that `n / 6U` is C++ integer division
and silently truncates, which is precisely why `finite_dimension_supported()`
must reject these dimensions up front — without the gate the construction would
quietly build a shape whose digit counts do not sum to `n`.

## Measured domain

`ter_full_bench --n=<N> --preflight`:

| n | shape status | r1 | r2 | root Q = n-r1 | L2->L1 Q = r1-r2 |
|---:|---|---:|---:|---:|---:|
| 36 | Completed | 12 | 3 | 24 | 9 |
| 72 | Completed | 25 | 7 | 47 | 18 |
| 100 | InvalidConfiguration | — | — | — | — |
| 101 | InvalidConfiguration | — | — | — | — |
| 102 | InvalidConfiguration | — | — | — | — |
| 107 | InvalidConfiguration | — | — | — | — |
| 108 | Completed | 39 | 12 | 69 | 27 |
| 144 | Completed | 53 | 16 | 91 | 37 |
| 180 | Completed | 67 | 21 | 113 | 46 |

## What supporting `n = 100` would require

A new finite-rounding policy analogous to `PaperRounded`, which already searches
a bounded neighbourhood for integral, correctly-parited counts. That machinery
exists but is currently gated behind the same `36 | n` test, so extending the
domain is not a matter of reusing it as-is.

Such a policy is **new TER mathematics** and carries a proof obligation before
any code is written:

1. show that the chosen integral counts still satisfy the exact Table-3 and
   Table-4 class capacities;
2. show the half-support decomposition remains exact (every class splits
   evenly across the two physical halves);
3. show the resulting representation counts still yield `0 < r2 < r1 < n`;
4. show the exact TER target relationships are preserved, so a recovered
   witness still verifies modulo `2^n`;
5. state what the rounded shape means — it is a different finite shape, not the
   same problem at a nearby `n`, and benchmark rows must say so.

Until that proof exists, the dimension ladder is `36, 72, 108, 144, 180, ...`
and the coarseness is a genuine property of the construction rather than a bug.
