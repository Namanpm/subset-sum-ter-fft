"""Pre-registered confirmation measurement for the Phase B transform kill test.

WHY.  In run 20260912-0112 the verdict-eligible end-to-end block measured the PAIR
kernel at 186.9 ms median (IQR 0.06 ms), while later blocks in the same process and
in a fresh process measured 144.8-150.5 ms.  Upload time moved 14.2 ms -> 6.5-7.8 ms
at the same time.  nvidia-smi during the run showed pstates P2-P5, PCIe gen 2-5 and
graphics clocks 247-2692 MHz.  A sync-cadence experiment (sync every 1/2/8 batches
vs end-only) moved device time by < 3 ms, so queued launches are not the cause; the
evidence points to device power/performance state.

PROTOCOL (frozen before running).
  * 60 s sustained warm-up executing the PAIR pipeline.
  * 3 blocks of 30 samples, both kernels alternating, Pipeline.run_end_to_end, the
    verdict-eligible 128 MiB capped configuration, unchanged code path.
  * last-batch output check against direct pair enumeration after every sample.
  * nvidia-smi sampled every 100 ms; per-sample host timestamps recorded to join.

DECISION RULE (frozen).  The transform gate is applied to the pooled median of ALL
verdict-eligible capped end-to-end samples of the better kernel: the original 30
from run 20260912-0112 plus the 90 recorded here.  No sample is discarded.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import statistics
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convsig_cuda as cu  # noqa: E402
import convsig_kernels as kernels  # noqa: E402
import convsig_reference as ref  # noqa: E402
import convsig_transform_bench as tb  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--families", required=True)
    parser.add_argument("--nvrtc-dir", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--warmup-seconds", type=float, default=60.0)
    parser.add_argument("--blocks", type=int, default=3)
    parser.add_argument("--samples", type=int, default=30)
    args = parser.parse_args()
    for name in ("confirmation.json", "confirmation_samples.csv", "confirmation_nvidia_smi.csv"):
        if os.path.exists(os.path.join(args.result_dir, name)):
            print(f"FATAL: {name} exists; results are never overwritten")
            return 2

    families = tb.load_families(args.families)
    labels = ref.labels()
    problem = tb.build_problem(families, labels)
    gpu = cu.Gpu(args.nvrtc_dir)
    props = gpu.properties()
    module, _ = gpu.compile(kernels.SOURCE, b"convsig.cu", props["arch_flag"])
    functions = {name: gpu.function(module, name) for name in kernels.FUNCTIONS}
    twiddle_host = ref.forward_twiddles().astype(np.uint32)
    twiddles = gpu.alloc(twiddle_host.nbytes)
    gpu.upload(twiddles, twiddle_host)
    fixed = (problem["a_host"].nbytes + problem["b_host"].nbytes + problem["job_shard"].nbytes +
             problem["job_class"].nbytes + twiddle_host.nbytes)
    batch = (tb.DEVICE_CAP_BYTES - fixed) // (2 * tb.M * 4)
    pipeline = tb.Pipeline(gpu, functions, problem, batch)
    jobs = problem["jobs"]
    last_offset = ((jobs - 1) // batch) * batch
    last_count = jobs - last_offset
    last_exact = [tb.exact_job(problem, labels, last_offset + i) for i in range(last_count)]

    def output_ok() -> bool:
        out = pipeline.download_batch(last_count)
        return all(np.array_equal(out[i, 0].astype(np.int64), last_exact[i][0]) and
                   np.array_equal(out[i, 1].astype(np.int64), last_exact[i][1])
                   for i in range(last_count))

    smi_path = os.path.join(args.result_dir, "confirmation_nvidia_smi.csv")
    smi = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,utilization.gpu,power.draw,"
                            "pstate,clocks.current.graphics,pcie.link.gen.current,"
                            "clocks_event_reasons.active", "--format=csv", "-lms", "100",
                            "-f", smi_path])
    rows = []
    failures = 0
    try:
        end = time.perf_counter() + args.warmup_seconds
        warm_runs = 0
        while time.perf_counter() < end:
            pipeline.run_end_to_end(twiddles, "pair")
            warm_runs += 1
        variants = ("stage", "pair")
        for block in range(args.blocks):
            for sample in range(args.samples):
                order = variants if sample % 2 == 0 else variants[::-1]
                for variant in order:
                    started = time.time()
                    run = pipeline.run_end_to_end(twiddles, variant)
                    ok = output_ok()
                    failures += 0 if ok else 1
                    rows.append({"block": block, "sample": sample, "variant": variant,
                                 "wall_start_epoch": started, **run, "output_check": ok})
            print(f"block {block} done")
    finally:
        smi.terminate()
        smi.wait(timeout=10)

    original = []
    with open(os.path.join(args.result_dir, "transform_samples.csv")) as handle:
        for row in csv.DictReader(handle):
            if row["mode"] == "capped_end_to_end":
                original.append((row["variant"], float(row["device_ms"]), float(row["upload_ms"])))

    summary = {"protocol": __doc__, "warmup_runs": warm_runs, "batch_jobs": batch,
               "output_check_failures": failures, "per_block": {}, "pooled": {}}
    for variant in ("stage", "pair"):
        for block in range(args.blocks):
            device = [r["device_ms"] for r in rows if r["variant"] == variant and r["block"] == block]
            upload = [r["upload_ms"] for r in rows if r["variant"] == variant and r["block"] == block]
            summary["per_block"][f"{variant}_block{block}"] = {
                "device": tb.stats(device), "upload_median": statistics.median(upload)}
        new = [r["device_ms"] for r in rows if r["variant"] == variant]
        old = [device for v, device, _ in original if v == variant]
        summary["pooled"][variant] = {
            "original_block_median": statistics.median(old) if old else None,
            "confirmation_median": statistics.median(new),
            "pooled_all_verdict_eligible": tb.stats(old + new),
        }
    best = min(("stage", "pair"),
               key=lambda v: summary["pooled"][v]["pooled_all_verdict_eligible"]["median"])
    pooled = summary["pooled"][best]["pooled_all_verdict_eligible"]["median"]
    summary["best_variant"] = best
    summary["pooled_transform_ms"] = pooled
    summary["classification"] = ("STRONG_PASS" if pooled <= tb.STRONG_PASS_MS else
                                 "MARGINAL" if pooled <= tb.NO_GO_MS else "TRANSFORM_NO_GO")
    json.dump(summary, open(os.path.join(args.result_dir, "confirmation.json"), "w"), indent=1)
    with open(os.path.join(args.result_dir, "confirmation_samples.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({k: v for k, v in summary.items() if k != "protocol"}, indent=1))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
