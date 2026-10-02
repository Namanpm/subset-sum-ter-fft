"""DIAGNOSTIC CONTROL: exact GPU source-pair enumeration on the authentic n=72 root.

Never a candidate architecture and never verdict-eligible.  It answers one
question the proposal's verdict depends on: does the GPU win simply because it is
a GPU?  It enumerates every authentic unique residual source pair (q, x) --
620,490,266 of them -- on the same device, forms y = (q + x + kappa) mod 2^19 and
tests exact membership in the 64 KiB Y_u bitmap, writing one exact hit flag per
pair.  No transform, no signature, no decoding.

SCOPE COMPARISON.  Its output (an exact hit flag for every pair) is strictly
further along the pipeline than the convolution transform phase's output (count
and signature arrays that still require target extraction, decoding and roughly
47 M projected candidate probes).

TIMED.  upload of flattened Q, X, class and bitmap arrays; 64 launches (one per Q
class); final synchronization.  The flag buffer holds one class at a time, so the
largest class sets its size; it is reused exactly as the transform path reuses its
batch buffer.

CORRECTNESS (outside timing).  Every class's flags are downloaded and compared
element for element with a NumPy evaluation of the same predicate, and the total
hit count is compared with the authenticated 618,857 distinct residual pair hits.
"""
from __future__ import annotations

import argparse
import csv
import ctypes as C
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convsig_cuda as cu  # noqa: E402
import convsig_transform_bench as tb  # noqa: E402

SOURCE = rb"""
typedef unsigned int u32;
typedef unsigned long long u64;
extern "C" __global__ void brute_pairs(const u32* q_values, const u32* x_values,
                                       const u32* x_class, const u32* bitmaps,
                                       unsigned char* flags, u32 q_begin, u32 x_count,
                                       u32 s, u32 total) {
    u32 i = blockDim.x * blockIdx.x + threadIdx.x;
    if (i >= total) return;
    u32 qi = i / x_count;
    u32 xi = i - qi * x_count;
    u32 sum = s + x_class[xi];
    u32 u = sum & 63u;
    u32 kappa = sum >> 6;
    u32 v = (q_values[q_begin + qi] + x_values[xi] + kappa) & 524287u;
    u32 word = bitmaps[(u64)u * 16384ull + (u64)(v >> 5)];
    flags[i] = (unsigned char)((word >> (v & 31u)) & 1u);
}
"""
U32 = C.c_uint


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--families", required=True)
    parser.add_argument("--nvrtc-dir", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--warmups", type=int, default=5)
    parser.add_argument("--samples", type=int, default=30)
    args = parser.parse_args()
    for name in ("bruteforce_control.json", "bruteforce_control_samples.csv"):
        if os.path.exists(os.path.join(args.result_dir, name)):
            print(f"FATAL: {name} exists; results are never overwritten")
            return 2

    families = tb.load_families(args.families)
    q_unique = [np.unique(f[:, 0]).astype(np.uint32) for f in families["q"]]
    x_unique = [np.unique(f[:, 0]).astype(np.uint32) for f in families["x"]]
    y_unique = [np.unique(f[:, 0]).astype(np.uint32) for f in families["y"]]
    q_values = np.concatenate(q_unique)
    q_offsets = np.concatenate([[0], np.cumsum([len(v) for v in q_unique])]).astype(np.int64)
    x_values = np.concatenate(x_unique)
    x_class = np.concatenate([np.full(len(v), t, dtype=np.uint32) for t, v in enumerate(x_unique)])
    bitmaps = np.zeros((64, 16384), dtype=np.uint32)
    for u, values in enumerate(y_unique):
        np.bitwise_or.at(bitmaps[u], values >> 5, np.left_shift(np.uint32(1), values & 31))
    x_count = len(x_values)
    widest = max(len(v) for v in q_unique) * x_count

    gpu = cu.Gpu(args.nvrtc_dir)
    props = gpu.properties()
    module, compile_info = gpu.compile(SOURCE, b"brute.cu", props["arch_flag"])
    kernel = gpu.function(module, b"brute_pairs")
    q_dev = gpu.alloc(q_values.nbytes)
    x_dev = gpu.alloc(x_values.nbytes)
    c_dev = gpu.alloc(x_class.nbytes)
    b_dev = gpu.alloc(bitmaps.nbytes)
    f_dev = gpu.alloc(widest)
    device_bytes = q_values.nbytes + x_values.nbytes + x_class.nbytes + bitmaps.nbytes + widest

    def upload() -> None:
        gpu.upload(q_dev, q_values)
        gpu.upload(x_dev, x_values)
        gpu.upload(c_dev, x_class)
        gpu.upload(b_dev, bitmaps)

    def launch(s: int) -> int:
        total = len(q_unique[s]) * x_count
        gpu.launch(kernel, total, [cu.DevicePointer(q_dev), cu.DevicePointer(x_dev),
                                   cu.DevicePointer(c_dev), cu.DevicePointer(b_dev),
                                   cu.DevicePointer(f_dev), U32(int(q_offsets[s])),
                                   U32(x_count), U32(s), U32(total)])
        return total

    # Exhaustive correctness outside timing.
    upload()
    hits = 0
    mismatched_classes = 0
    pairs = 0
    started = time.perf_counter()
    for s in range(64):
        total = launch(s)
        gpu.synchronize()
        flags = np.empty(total, dtype=np.uint8)
        gpu.download(flags, f_dev)
        q = q_unique[s].astype(np.int64)
        summed = s + x_class.astype(np.int64)
        u = summed & 63
        kappa = summed >> 6
        v = (q[:, None] + (x_values.astype(np.int64) + kappa)[None, :]) & 524287
        words = bitmaps[u[None, :], v >> 5]
        expect = ((words >> (v & 31)) & 1).astype(np.uint8).ravel()
        if not np.array_equal(flags, expect):
            mismatched_classes += 1
        hits += int(flags.sum())
        pairs += total
    verify_seconds = time.perf_counter() - started

    def run() -> dict:
        gpu.synchronize()
        t0 = time.perf_counter_ns()
        upload()
        t1 = time.perf_counter_ns()
        for s in range(64):
            launch(s)
        gpu.synchronize()
        t2 = time.perf_counter_ns()
        return {"upload_ms": (t1 - t0) / 1e6, "device_ms": (t2 - t1) / 1e6, "total_ms": (t2 - t0) / 1e6}

    for _ in range(args.warmups):
        run()
    rows = [{"sample": i, **run()} for i in range(args.samples)]
    result = {
        "kind": "DIAGNOSTIC CONTROL -- exact GPU source-pair enumeration; never verdict-eligible",
        "gpu": props["name"], "compile": {k: v for k, v in compile_info.items() if k != "log"},
        "pairs_enumerated": pairs, "hit_flags": hits, "expected_distinct_residual_pair_hits": 618857,
        "hits_match_authenticated_truth": hits == 618857,
        "mismatched_classes_vs_numpy": mismatched_classes, "verify_seconds": verify_seconds,
        "device_bytes": device_bytes, "launches_per_run": 64,
        "device_ms": tb.stats([r["device_ms"] for r in rows]),
        "upload_ms": tb.stats([r["upload_ms"] for r in rows]),
        "total_ms": tb.stats([r["total_ms"] for r in rows]),
    }
    json.dump(result, open(os.path.join(args.result_dir, "bruteforce_control.json"), "w"), indent=1)
    with open(os.path.join(args.result_dir, "bruteforce_control_samples.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps(result, indent=1))
    return 0 if mismatched_classes == 0 and hits == 618857 else 1


if __name__ == "__main__":
    raise SystemExit(main())
