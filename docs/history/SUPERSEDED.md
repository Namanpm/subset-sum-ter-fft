# SUPERSEDED: TER L1 -> L0 Radix-Refined Root Architecture v2.1 (CODEX)

**File:** `TER_L1_to_L0_Radix_Refined_Root_Architecture_v2.1_CODEX.pdf` (18 pages, dated 24 August 2026)
**SHA-256:** `31172874ccfbf3b4e6fa1ac57bf8fd1cecfb51deb57f67d72465427bbab594a6`. Byte-identical to the original; moved here unchanged on 2026-10-02 from the repository root.
**Status:** SUPERSEDED as the working root architecture. Kept immutable as a historical design record.
**Successor:** `TER_L1_to_L0_ARWLF_Architecture_v1.0.md` (repository root).

```text
Status: superseded by ARWLF (TER_L1_to_L0_ARWLF_Architecture_v1.0.md)

Original hypothesis:
    a fast, scalable exact root requires E << |A||B|
    (opened pair mass reduced by region pruning before recovery)

Later finding (finite n):
    ARWLF retains Theta(|A||B|) logical pair work, but makes that work
    cheap and suppresses materialized survivors: ~53x frozen K2 on
    authentic n=108 roots, holdout-confirmed.

Still open:
    E << |A||B| remains the requirement for any scaling claim
    (the Stage-D enumeration problem).
```

## What it proposed

A 13-stage exact root funnel, Stages A-M:
- **Partition (Stage C):** RootAux metadata, then exact low-k projection, then radix/CSR partitioning.
- **Narrow (Stages D-E):** exact region/bucket pruning, then carry-aware exact refinement.
- **Recover (Stages F-G):** small surviving groups, then direct or Schroeppel-Shamir-style witness recovery.
- **Check (Stages H-M):** cheap exact rejection gates, then original-residue proof and TER structural validation.

Its decision record (section 31) states the root objective as *reduce opened pair mass before pair recovery*. Its complexity contract (section 21) sets the intended regime `S, V << Z << E << P0` and states: "If E remains near P0, the root scaling objective has failed even if locality improves wall time."

## What happened to it

| Stage / phase | Outcome |
|---|---|
| P0, P1 (replay; exact wide tail) | Built. The low64 gate and arbitrary-width residue authority are in the production root (commit 53431a8) |
| Stage C / P2 (radix partition) | Built (commit 6e52c2b). CurrentScatter stayed the default and BlockInPlace was slower. Cost was 0.03 % of root time with **no pruning gain**, so it was demoted to an optional substrate (`docs/root_architecture_resolution.md`, 2026-08-26) |
| Stage D (region pruning) | Never built. `docs/root_pair_space_analysis.md` shows the pruning power is real (up to 250x less work if survivors could be enumerated output-optimally at n=72). The blocker is the enumeration cost Phi, which naive traversal keeps near P0. **This is still an open problem** |
| Stage E / P4 (carry refinement) | Tested as A64-CHQR: exact, but refine work / pairs = 1.00-1.02 at n=72 and n=108 (pair-bound). Closed |
| Stages F-M, P5-P9 | Partly present as the exact authority chain (low64, residue proof, structural validation); the rest was never built |

The production FullTer root became design B (bounded streaming root), not this funnel.

## What superseded it, and what that does and does not show

ARWLF (adaptive radix wide-leaf filter) took the opposite route.
- **Pair mass is not reduced.** Within a microproblem it still makes every logical pair test: lanes = |Q_s| |X_t|, so E ≈ P0.
- **Each pair test is made very cheap.** Exact 13-bit leaf routing plus a 32-bit byte-table filter evaluates 16 pairs per AVX2 row.
- **Almost no candidate survivors reach the expensive stages.**

On authentic n=108 roots it is a geometric-mean **52.9x** faster than frozen K2 on three unseen holdout roots (95 % case-level bootstrap 51.2-55.0; the root-block interval reported in the paper is 51.8-53.8), and 53.1x on the calibration roots, with 221/221 campaign cases exact (`results/arwlf/regime-20261001/summary.md`).

What this **does** show: at the finite dimensions that can be run here, a fast exact root does not require reducing opened pair mass. Making each pair test cheap and suppressing survivors was enough for a 53x wall-time gain.

What it **does not** show: by this document's own Falsification A (section 27) and its rejected shortcut "faster quadratic work remains quadratic" (section 30), ARWLF does **not** meet the v2.1 *scaling* objective. ARWLF is a constant-factor result. Any asymptotic improvement of the TER root still requires E << P0, which is the open Stage-D enumeration problem above.

## Still valid, and carried into the successor

- The fail-closed exactness contract (section 6): only original-residue arithmetic plus the canonical TER structural validator authorize an L0 result.
- Exact filters reject only with a proved no-false-negative implication. Incomplete, cancelled or refused work is never a negative proof.
- Pairless memory: no pair arrays, pair sums or 2^Q tables.
- EntryId-preserving data paths.
- The differential testing matrix (section 26) and the benchmark/falsification protocol (section 27).
- Adaptive backends only after paired measurements exist (section 31). They now exist: the ARWLF dispatcher.

Lineage, in design terms only:
- Stage C's radix/CSR digits became ARWLF's 13-bit terminal leaves.
- Stage E's carry-aware refinement became ARWLF's carry split, in which each target leaf's Q rows form two contiguous carry ranges.

## Retained for

Architectural provenance; negative-results documentation; benchmark methodology; exactness and fail-closed requirements.

Other documents that cite this PDF by name (`docs/history/implementation_plan.md`, `docs/history/CLAUDE_HANDOFF_2026-08-25.md`) are themselves historical. It now lives here.
