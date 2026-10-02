# Frozen TER² root backends: K2, K2_WIDE, QUEUE_DENSE, ARWLF

Frozen 2026-10-02. These four exact L1 -> L0 root backends (and the ARWLF
dispatcher) are research baselines: every number in
`results/arwlf/regime-20261001/` was measured on exactly this code. Benchmark
surface only; production `solve_full_ter` is unaffected.

## What is frozen

The complete source closure of the four backends: 29 files, listed with their
SHA-256 in `ter2_root/frozen_root_backends.sha256`; the files live in `ter2_root/frozen/`. Each digest is
taken over the file's content with every CR byte removed, which equals the SHA-256 of the committed
(LF) git blob, so the check does not depend on `core.autocrlf` or the checkout platform. They are byte-identical to the
build the regime campaign froze (`results/arwlf/regime-20261001/source_manifest.txt`).
CTest `ter_frozen_root_backends` fails if any of them changes or disappears.

| Backend | Entry point | What it is |
|---|---|---|
| **K2** | `root_generalization::run_k2` (`ter2_root/frozen/ter_root_queue_kernels.cpp`), kernel `microreport8::avx2::k2_coarse_gather` | 8 x 8 tile kernel on the 19-bit residual plus exact Y bitmap (25 quotient bits), then the RootSink authority chain (low-32 prefilter, residue, structural). |
| **K2_WIDE** | `a64::run_k2_wide` (`ter2_root/frozen/ter_a64_chqr.cpp`) | The same frozen K2 kernel, plus a full-key check (all W residual bits and the high Q-64 bits) before the sink. |
| **QUEUE_DENSE** | `root_generalization::run_radix(..., RadixRoute::QueueDense, RadixStop::Full, ...)` -> `radix13_packed::bmi2::route_query_queued_dense_control` | Branch-free per-query queue of nonzero guard words, then the plain radix-13 exact dense leaf. Applicable only when every family has <= 65,536 distinct residues (true at n=72, false at n=108). |
| **ARWLF** (v3) | `arwlf::run_arwlf` (`ter2_root/frozen/ter_arwlf.cpp`, `ter2_root/frozen/ter_arwlf_avx2.cpp`) | 13-bit terminal leaves (exact target-leaf routing), 32-bit byte-table filter on 16 x 16-bit AVX2 lanes (pshufb), exact resolver on full keys, then the authority chain. |
| **DISPATCH** | `arwlf::dispatch_and_run` (`ter2_root/frozen/ter_arwlf.cpp`) | Probes K2_WIDE (2^21 pairs), ARWLF (2^26 pairs) and QUEUE_DENSE, then runs the smallest prediction. ARWLF is a candidate only if packets/pair <= 1/16, QUEUE_DENSE only inside its capacity. Runtime guard at the runner-up's break-even. |

Shared frozen machinery: snapshot loading (`ter_root_snapshot`), workload and
full keys (`ter_root_generalization_workload`), authority tables
(`ter_translated_tag_dataset`), group index and RootSink
(`ter_root_queue_kernels`), radix-13 classes (`ter_radix13_locator`), and the
MicroReport8 dataset and tiles (`ter_microreport8_dataset`).

## Reference measurements (frozen code)

Pinned P-core, AC power, final protocol (3 warmup + 7 matched rounds), all exact:

| Workload | K2 | K2_WIDE | QUEUE_DENSE | ARWLF | DISPATCH (ran) |
|---|---|---|---|---|---|
| n=72 anchor, whole root | 301.5 ms | 298.4 | 167.6 | 1,137.4 | 178.2 (QUEUE_DENSE) |
| n=108 0x1001, anchor microproblem | 44,558 ms | 27,193 | N/A | 842.4 | 864.2 (ARWLF) |
| n=108 0x2003, anchor | 131,159 ms | 54,570 | N/A | 1,679.4 | 1,701.7 (ARWLF) |
| n=108 0x3007, anchor | 86,390 ms | 54,579 | N/A | 1,637.6 | 1,653.7 (ARWLF) |

Over 29 authentic n=108 microproblems: ARWLF/K2 geometric mean 53.1x on the
calibration roots and 52.9x on the unseen holdout roots. Full tables:
`results/arwlf/regime-20261001/summary.md`.

## Rules

1. Do not edit a frozen file. A new idea becomes a new backend in new files,
   compared against these baselines; it does not modify them.
2. Fixing a bug in a frozen file is the only exception. Record the reason and
   date below, rerun `ter_arwlf_tests`, `ter_a64_chqr_tests`,
   `ter_root_queue_generalization_tests` and `ter_arwlf_campaign_tests`,
   re-measure any number that depends on the change, then regenerate the manifest:
   `for f in $(awk '{print $2}' ter2_root/frozen_root_backends.sha256); do printf '%s  %s\n' "$(tr -d '\r' < $f | sha256sum | cut -c1-64)" "$f"; done > m.new && mv m.new ter2_root/frozen_root_backends.sha256`
3. The dispatcher's known weaknesses are recorded, not fixed here: it ignores
   ARWLF below L=16 (regret up to 17x at L=2..16), and its ~10 ms of probes
   dominate microproblems that take ~0.1 ms. A tuned dispatcher is a new
   dispatcher.

## Change log

- 2026-10-02: frozen (29 files).
- 2026-10-02: relocated from `bench/` to `ter2_root/frozen/` (`git mv`; contents byte-identical: each
  file's CR-stripped content equals its blob at commit 3eaad92, the campaign's code). The digest definition
  changed at the same time, from the raw working-copy bytes to the CR-stripped content. The first manifest
  had hashed this machine's mixed CRLF/LF checkout, so a fresh clone on any platform would have failed the
  check. As a result, 12 digests changed value while their code did not. The campaign's own
  `results/arwlf/regime-20261001/source_manifest.txt` keeps the original paths as history.
