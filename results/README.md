# `results/`

**What it is.** All experimental output of the repository: raw measurements, logs, provenance records and per-experiment verdicts.

**Purpose in the repository.** The evidence layer. Each subfolder is one experiment, named for what it tested and (mostly) the date it was run. Only a few feed the article (see `LEGACY_MAP.md` section 1: `arwlf/` items and the snapshots); the rest are the recorded evidence for closed experiments. `.gitattributes` marks the folder `-text` so line endings never change the recorded SHA-256 values. The two `*.runner.log` files at the top belong to the QUEUE_DENSE campaign.

**Subfolders:** `a64-chqr/`, `arwlf/`, `convolution-signature-inference/`, `microreport8-20260902/`, `ntt-crossover-bench-20260817/`, `partition/`, `radix13-locator/`, `radix13-packed-tag/`, `root-queue-generalization/`, `ter2-windowing-20260903/`, `ter2-windowing-20260905/`, `tiled-q3report-final-20260829/`, `translated-tag-locator/`.

**Files directly in this folder:** 2 (besides this README).  
Repository-wide status of every file: [`LEGACY_MAP.md`](../LEGACY_MAP.md).
