"""Phase B transform-only kill test for the TER^2 convolution/source-signature proposal.

SCOPE.  Standalone research spike.  No production source is touched.  This file
measures only whether the dense transform workload the proposal requires is
viable on the real device, on the authentic n=72 job set, before any of the
source-signature inference machinery is built.

FROZEN PROPOSAL PARAMETERS (never tuned): g = 64, M = 16384, p = 65537, d = 4.

WHAT IS TIMED (per sample, sync-bracketed, authoritative end-to-end):
    upload      host -> device: A (count + label polynomials per X shard),
                B (count polynomials per Q class), job index arrays
    device      forward NTT of every A and B array, B scaled by M^-1,
                spectral multiplication of every (shard, class) job,
                inverse NTT of every count and signature array,
                final device synchronization
The twiddle table is root-independent and reusable for the process lifetime; it
is uploaded once at setup and reported as a one-time cost.

PRE-DECLARED KERNEL FAMILIES (verdict uses the faster, both exact):
    STAGE  one launch per NTT stage (14 memory passes per transform)
    PAIR   two stages fused per launch (7 memory passes per transform)

BATCHING.  The frozen proposal caps incremental device memory at 128 MiB.
Verdict-eligible runs size the job batch so every device allocation fits under
that cap.  An all-jobs-resident run is recorded only as a labelled diagnostic.

OBSERVABILITY.  Before timing, every job's count and signature arrays from both
kernel families are downloaded and compared with direct pair enumeration
(np.bincount, no transform).  After every timed sample the last batch's outputs
are downloaded outside the timed region and compared again, so no timed run can
be a run that computed nothing.

GATES (frozen): transform <= 105 ms STRONG PASS; <= 145 ms MARGINAL; else
TRANSFORM NO-GO.
"""
from __future__ import annotations

import argparse
import csv
import ctypes as C
import datetime
import hashlib
import json
import math
import os
import platform
import statistics
import struct
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convsig_cuda as cu  # noqa: E402
import convsig_kernels as kernels  # noqa: E402
import convsig_reference as ref  # noqa: E402

PROCESS_START_NS = time.perf_counter_ns()

M, P, G, D = ref.M, ref.P, ref.G, ref.D
LOG_M = ref.LOG_M
BUTTERFLIES = (M // 2) * LOG_M
DEVICE_CAP_BYTES = 128 * 2**20
STRONG_PASS_MS = 105.0
NO_GO_MS = 145.0
HIST_K2_MS = 338.337
HIST_SCALAR_MS = 558.751
MEANINGFUL_MS = HIST_K2_MS / 1.5
PROPOSAL = {
    "H_bound": 570,
    "inverse_butterflies": 8_367_636_480,
    "transform_expected_ms": 105.0,
    "butterflies_per_second_required": 79.69e9,
    "nonzero_masks": 13.149e6,
    "candidate_probes": 46.935e6,
    "count_gt4_probes": 8.438e6,
    "candidate_stage_ms": 10.0,
    "cpu_authority_ms": 22.0,
    "total_ms": 163.0,
    "device_memory_mib": 110.0,
}
U32 = C.c_uint


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stats(values: list[float]) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {}

    def quantile(fraction: float) -> float:
        position = fraction * (len(ordered) - 1)
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        weight = position - lower
        return ordered[lower] * (1 - weight) + ordered[upper] * weight

    return {"median": quantile(0.5), "p25": quantile(0.25), "p75": quantile(0.75),
            "p95": quantile(0.95), "iqr": quantile(0.75) - quantile(0.25),
            "min": ordered[0], "max": ordered[-1], "samples": len(ordered)}


# ---------------------------------------------------------------------------
# Authentic data
# ---------------------------------------------------------------------------
def load_families(path: str) -> dict:
    raw = open(path, "rb").read()
    if raw[:8] != b"TERCSIG1":
        raise ValueError("families file has the wrong magic")
    n, r1, q_bits, tau = struct.unpack_from("<4I", raw, 8)
    (seed,) = struct.unpack_from("<Q", raw, 24)
    offset = 32
    blocks = []
    for _ in range(3):
        families = []
        for _ in range(64):
            (count,) = struct.unpack_from("<I", raw, offset)
            offset += 4
            pairs = np.frombuffer(raw, dtype="<u4", count=2 * count, offset=offset).reshape(count, 2)
            offset += 8 * count
            families.append(pairs.copy())
        blocks.append(families)
    if offset != len(raw):
        raise ValueError("families file has trailing or missing bytes")
    return {"n": n, "r1": r1, "quotient_bits": q_bits, "tau_low25": tau, "seed": seed,
            "q": blocks[0], "x": blocks[1], "y": blocks[2]}


def build_problem(families: dict, label_values: np.ndarray) -> dict:
    start = time.perf_counter()
    q_unique = [np.unique(family[:, 0]).astype(np.int64) for family in families["q"]]
    x_unique = [np.unique(family[:, 0]).astype(np.int64) for family in families["x"]]
    shards = []
    for t, values in enumerate(x_unique):
        for begin in range(0, len(values), G):
            shards.append((t, values[begin:begin + G]))
    H = len(shards)
    a_host = np.zeros((H, 2, M), dtype=np.uint32)
    for index, (_, values) in enumerate(shards):
        residues = values % M
        count = np.bincount(residues, minlength=M)
        weighted = np.bincount(residues, weights=label_values[:len(values)].astype(np.float64),
                               minlength=M)
        a_host[index, 0] = count.astype(np.uint32)
        a_host[index, 1] = (np.rint(weighted).astype(np.int64) % P).astype(np.uint32)
    b_host = np.zeros((64, M), dtype=np.uint32)
    for s, values in enumerate(q_unique):
        b_host[s] = np.bincount(values % M, minlength=M).astype(np.uint32)
    job_shard = np.repeat(np.arange(H, dtype=np.uint32), 64)
    job_class = np.tile(np.arange(64, dtype=np.uint32), H)
    build_ms = (time.perf_counter() - start) * 1e3
    return {"q_unique": q_unique, "x_unique": x_unique, "shards": shards, "H": H,
            "a_host": a_host, "b_host": b_host, "job_shard": job_shard, "job_class": job_class,
            "jobs": H * 64, "host_build_ms": build_ms,
            "a_max_bin": int(a_host[:, 0].max()), "b_max_bin": int(b_host.max())}


# ---------------------------------------------------------------------------
# Device pipeline
# ---------------------------------------------------------------------------
class Pipeline:
    def __init__(self, gpu: cu.Gpu, functions: dict, problem: dict, batch_jobs: int):
        self.gpu = gpu
        self.fn = functions
        self.problem = problem
        self.H = problem["H"]
        self.jobs = problem["jobs"]
        self.batch_jobs = batch_jobs
        self.launches = 0
        self.a_dev = gpu.alloc(problem["a_host"].nbytes)
        self.b_dev = gpu.alloc(problem["b_host"].nbytes)
        self.shard_dev = gpu.alloc(problem["job_shard"].nbytes)
        self.class_dev = gpu.alloc(problem["job_class"].nbytes)
        self.out_dev = gpu.alloc(batch_jobs * 2 * M * 4)
        self.allocated = (problem["a_host"].nbytes + problem["b_host"].nbytes +
                          problem["job_shard"].nbytes + problem["job_class"].nbytes +
                          batch_jobs * 2 * M * 4)

    def release(self) -> None:
        for pointer in (self.a_dev, self.b_dev, self.shard_dev, self.class_dev, self.out_dev):
            self.gpu.free(pointer)

    def _launch(self, name: bytes, total: int, args: list) -> None:
        self.gpu.launch(self.fn[name], total, args)
        self.launches += 1

    def forward(self, pointer: int, arrays: int, twiddles: int, variant: str) -> None:
        if variant == "stage":
            for log_half in range(LOG_M - 1, -1, -1):
                total = arrays * (M // 2)
                self._launch(b"gs_forward_stage", total,
                             [cu.DevicePointer(pointer), cu.DevicePointer(twiddles),
                              U32(log_half), U32(total)])
        else:
            for log_h in range(LOG_M - 2, -1, -2):
                total = arrays * (M // 4)
                self._launch(b"gs_forward_pair", total,
                             [cu.DevicePointer(pointer), cu.DevicePointer(twiddles),
                              U32(log_h), U32(total)])

    def inverse(self, pointer: int, arrays: int, twiddles: int, variant: str) -> None:
        if variant == "stage":
            for log_half in range(0, LOG_M):
                total = arrays * (M // 2)
                self._launch(b"ct_inverse_stage", total,
                             [cu.DevicePointer(pointer), cu.DevicePointer(twiddles),
                              U32(log_half), U32(total)])
        else:
            for log_h in range(0, LOG_M, 2):
                total = arrays * (M // 4)
                self._launch(b"ct_inverse_pair", total,
                             [cu.DevicePointer(pointer), cu.DevicePointer(twiddles),
                              U32(log_h), U32(total)])

    def batches(self):
        for offset in range(0, self.jobs, self.batch_jobs):
            yield offset, min(self.batch_jobs, self.jobs - offset)

    def upload(self) -> None:
        self.gpu.upload(self.a_dev, self.problem["a_host"])
        self.gpu.upload(self.b_dev, self.problem["b_host"])
        self.gpu.upload(self.shard_dev, self.problem["job_shard"])
        self.gpu.upload(self.class_dev, self.problem["job_class"])

    def spectra(self, twiddles: int, variant: str) -> None:
        self.forward(self.a_dev, 2 * self.H, twiddles, variant)
        self.forward(self.b_dev, 64, twiddles, variant)
        self._launch(b"field_scale", 64 * M,
                     [cu.DevicePointer(self.b_dev), U32(ref.M_INVERSE), U32(64 * M)])

    def job_batch(self, offset: int, count: int, twiddles: int, variant: str) -> None:
        self._launch(b"spectral_multiply", count * M,
                     [cu.DevicePointer(self.a_dev), cu.DevicePointer(self.b_dev),
                      cu.DevicePointer(self.out_dev), cu.DevicePointer(self.shard_dev),
                      cu.DevicePointer(self.class_dev), U32(offset), U32(count * M)])
        self.inverse(self.out_dev, 2 * count, twiddles, variant)

    def run_end_to_end(self, twiddles: int, variant: str) -> dict:
        self.launches = 0
        self.gpu.synchronize()
        t0 = time.perf_counter_ns()
        self.upload()  # blocking host -> device copies
        t1 = time.perf_counter_ns()
        self.spectra(twiddles, variant)
        for offset, count in self.batches():
            self.job_batch(offset, count, twiddles, variant)
        self.gpu.synchronize()
        t2 = time.perf_counter_ns()
        return {"upload_ms": (t1 - t0) / 1e6, "device_ms": (t2 - t1) / 1e6,
                "total_ms": (t2 - t0) / 1e6, "launches": self.launches}

    def run_instrumented(self, twiddles: int, variant: str) -> dict:
        self.launches = 0
        self.gpu.synchronize()
        t0 = time.perf_counter_ns()
        self.upload()
        t1 = time.perf_counter_ns()
        self.spectra(twiddles, variant)
        self.gpu.synchronize()
        t2 = time.perf_counter_ns()
        multiply_ns = 0
        inverse_ns = 0
        for offset, count in self.batches():
            a = time.perf_counter_ns()
            self._launch(b"spectral_multiply", count * M,
                         [cu.DevicePointer(self.a_dev), cu.DevicePointer(self.b_dev),
                          cu.DevicePointer(self.out_dev), cu.DevicePointer(self.shard_dev),
                          cu.DevicePointer(self.class_dev), U32(offset), U32(count * M)])
            self.gpu.synchronize()
            b = time.perf_counter_ns()
            self.inverse(self.out_dev, 2 * count, twiddles, variant)
            self.gpu.synchronize()
            c = time.perf_counter_ns()
            multiply_ns += b - a
            inverse_ns += c - b
        t3 = time.perf_counter_ns()
        return {"upload_ms": (t1 - t0) / 1e6, "forward_ms": (t2 - t1) / 1e6,
                "multiply_ms": multiply_ns / 1e6, "inverse_ms": inverse_ns / 1e6,
                "instrumented_total_ms": (t3 - t0) / 1e6, "launches": self.launches}

    def download_batch(self, count: int) -> np.ndarray:
        out = np.empty((count, 2, M), dtype=np.uint32)
        self.gpu.download(out, self.out_dev)
        return out


def exact_job(problem: dict, labels: np.ndarray, job: int) -> tuple[np.ndarray, np.ndarray, int]:
    shard = int(problem["job_shard"][job])
    cls = int(problem["job_class"][job])
    x = problem["shards"][shard][1]
    q = problem["q_unique"][cls]
    count, signature = ref.count_signature_exact(x, q, labels[:len(x)])
    return count, signature, len(x) * len(q)


def verify_exhaustively(pipeline: Pipeline, problem: dict, labels: np.ndarray, twiddles: int,
                        variant: str) -> dict:
    start = time.perf_counter()
    pipeline.upload()
    pipeline.spectra(twiddles, variant)
    mismatched_jobs = 0
    conservation_failures = 0
    pairs = 0
    for offset, count in pipeline.batches():
        pipeline.job_batch(offset, count, twiddles, variant)
        pipeline.gpu.synchronize()
        result = pipeline.download_batch(count)
        for local in range(count):
            job = offset + local
            exact_count, exact_signature, job_pairs = exact_job(problem, labels, job)
            pairs += job_pairs
            if (not np.array_equal(result[local, 0].astype(np.int64), exact_count) or
                    not np.array_equal(result[local, 1].astype(np.int64), exact_signature)):
                mismatched_jobs += 1
            if int(result[local, 0].astype(np.int64).sum()) != job_pairs:
                conservation_failures += 1
    return {"variant": variant, "jobs_verified": pipeline.jobs, "mismatched_jobs": mismatched_jobs,
            "count_conservation_failures": conservation_failures,
            "unique_pairs_enumerated": pairs, "seconds": time.perf_counter() - start}


# ---------------------------------------------------------------------------
# Telemetry
# ---------------------------------------------------------------------------
def nvidia_smi_fields() -> list[str]:
    candidates = ["timestamp", "utilization.gpu", "utilization.memory", "memory.used",
                  "power.draw", "temperature.gpu", "pstate", "clocks.current.graphics",
                  "pcie.link.gen.current", "pcie.link.width.current", "clocks_event_reasons.active"]
    valid = []
    for field in candidates:
        result = subprocess.run(["nvidia-smi", f"--query-gpu={field}", "--format=csv,noheader"],
                                capture_output=True, text=True)
        if result.returncode == 0 and "not a valid field" not in (result.stdout + result.stderr):
            valid.append(field)
    return valid


def git_state(repo: str) -> dict:
    def run(*args):
        return subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True).stdout.strip()
    return {"commit": run("rev-parse", "HEAD"), "status_porcelain": run("status", "--porcelain")}


HISTORICAL = """PREVIOUS FFT / NTT EXPERIMENTS IN THIS REPOSITORY (recovered from evidence, not memory)

1. Exact count-only Q3REPORT NTT experiment
   commits: 788046a (2026-08-17) and 8ee990a (TER-faithful crossover), plan
   TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md
   raw data: .claude/worktrees/ntt-crossover-bench/crossover_full.csv (182 rows)
   what it did: one dense exact cyclic count convolution over the whole quotient
     modulus Q (single prime 998244353), counts only, no witness recovery
   hardware: CPU, single thread; no GPU, no transfers
   transform sizes: Q = 256 ... 4,194,304 (L = 64 ... 8192)
   measured (median per Q/L group): NTT amortized total was slower than direct
     arithmetic at EVERY tested point, by 2.42x (Q=4,194,304, L=8192) up to
     313x (Q=65,536, L=128); at Q=16,384 the transform alone took 1.09-1.16 ms
     against 0.03-0.12 ms for direct arithmetic (9.6x-39.9x slower)
   verdict recorded: fails the cheap kill gate in every family; witness recovery
     never authorized, so candidate-recovery cost was never measured

2. Paper-scaling dense NTT crossover (commit 7e25779)
   raw data: results/tiled-q3report-final-20260829/raw/paper-crossover-tiled.csv
   hardware: CPU, single thread; primes 998244353 / 2013265921
   transform sizes: Q = 2^23 ... 2^26 (L = 4096 ... 16384)
   measured at L = 16384, Q = 2^26 (5 seeds): NTT transform 5.83-15.63 s;
     warm total 5.94-15.87 s; DIRECT_PRODUCTION 3.96-5.48 s (NTT 1.42x-3.16x
     slower); tiled direct 0.255-0.561 s (NTT 19.3x-49.8x slower);
     NTT run bytes 512 MiB, peak 768 MiB
   recorded verdict: DENSE NTT NO-GO

MEASURED ROOT CAUSE
   Both experiments lost on TRANSFORM COST ITSELF: a dense transform over the
   whole modular universe (Theta(Q log Q) field operations and full-array memory
   passes) cost more than direct arithmetic that visits the same universe in
   Theta(L^2) = Theta(Q).  Candidate recovery and host/device transfer were never
   implemented or measured (CPU-only, count-only), so no conclusion about those
   costs exists: RECOVERY_COST = UNKNOWN, TRANSFER_COST = NOT APPLICABLE.
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--families", required=True)
    parser.add_argument("--summary", required=True)
    parser.add_argument("--nvrtc-dir", required=True)
    parser.add_argument("--authority-pdf", required=True)
    parser.add_argument("--out-root", default=os.path.join(HERE, "..", "results",
                                                           "convolution-signature-inference"))
    parser.add_argument("--warmups", type=int, default=5)
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--instrumented-samples", type=int, default=5)
    parser.add_argument("--diagnostic-samples", type=int, default=10)
    args = parser.parse_args()
    repo = os.path.abspath(os.path.join(HERE, ".."))
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M")
    out_dir = os.path.abspath(os.path.join(args.out_root, stamp))
    if os.path.exists(out_dir):
        print(f"FATAL: {out_dir} exists; results are never overwritten")
        return 2
    os.makedirs(out_dir)
    print(f"# results -> {out_dir}")
    git_before = git_state(repo)

    # -- fixture identity --------------------------------------------------
    families = load_families(args.families)
    summary = json.load(open(args.summary))
    capture_hash = sha256_file(args.families)
    expected = {"n": 72, "seed": 19797034556212562, "size_A": 19109, "size_B": 32496,
                "size_C": 33505, "P0": 620966064, "decomposition_crosscheck": 619579,
                "production_low25": 619579, "tau_low25": 5005954}
    discrepancies = {k: (summary.get(k), v) for k, v in expected.items() if summary.get(k) != v}
    counts = [sum(len(f) for f in families[key]) for key in ("q", "x", "y")]
    if counts != [19109, 32496, 33505]:
        discrepancies["family_postings"] = (counts, [19109, 32496, 33505])
    if families["seed"] != 19797034556212562 or families["n"] != 72:
        discrepancies["header"] = (families["seed"], families["n"])
    if discrepancies:
        print("FATAL fixture discrepancy:", discrepancies)
        json.dump({"verdict": "FIXTURE_DISCREPANCY", "discrepancies": str(discrepancies)},
                  open(os.path.join(out_dir, "transform_benchmark.json"), "w"), indent=1)
        return 3
    print(f"# fixture identity verified, capture sha256 {capture_hash}")

    labels = ref.labels()
    problem = build_problem(families, labels)
    H = problem["H"]
    jobs = problem["jobs"]
    forward_transforms = 2 * H + 64
    inverse_transforms = 2 * jobs
    print(f"# H_actual={H} jobs={jobs} forward_transforms={forward_transforms} "
          f"inverse_transforms={inverse_transforms} host_build_ms={problem['host_build_ms']:.1f}")
    if problem["a_max_bin"] >= P or problem["b_max_bin"] * G >= P:
        print("FATAL: count polynomial bound violated")
        return 4

    # -- cold start ----------------------------------------------------------
    cold = {}
    t = time.perf_counter()
    gpu = cu.Gpu(args.nvrtc_dir)
    cold["context_open_ms"] = (time.perf_counter() - t) * 1e3
    props = gpu.properties()
    module, compile_info = gpu.compile(kernels.SOURCE, b"convsig.cu", props["arch_flag"])
    cold["nvrtc_compile_ms"] = compile_info["compile_ms"]
    cold["module_load_ms"] = compile_info["module_load_ms"]
    functions = {name: gpu.function(module, name) for name in kernels.FUNCTIONS}
    t = time.perf_counter()
    twiddle_host = ref.forward_twiddles().astype(np.uint32)
    twiddles = gpu.alloc(twiddle_host.nbytes)
    gpu.upload(twiddles, twiddle_host)
    cold["twiddle_build_upload_ms"] = (time.perf_counter() - t) * 1e3

    fixed_bytes = (problem["a_host"].nbytes + problem["b_host"].nbytes + problem["job_shard"].nbytes +
                   problem["job_class"].nbytes + twiddle_host.nbytes)
    per_job_bytes = 2 * M * 4
    capped_batch = (DEVICE_CAP_BYTES - fixed_bytes) // per_job_bytes
    if capped_batch < 1:
        print("FATAL: frozen 128 MiB device cap cannot hold one job")
        return 5
    print(f"# device cap batch_jobs={capped_batch} batches={math.ceil(jobs / capped_batch)} "
          f"fixed_bytes={fixed_bytes}")

    limits = gpu.probe_limits(functions[b"field_scale"],
                              [cu.DevicePointer(twiddles), U32(1), U32(16)])
    gpu.upload(twiddles, twiddle_host)  # the probe scaled the first entries by 1; restore anyway

    t = time.perf_counter()
    capped = Pipeline(gpu, functions, problem, capped_batch)
    cold["pipeline_alloc_ms"] = (time.perf_counter() - t) * 1e3
    first = capped.run_end_to_end(twiddles, "pair")
    cold["first_run_total_ms"] = first["total_ms"]
    cold["cold_total_ms"] = (time.perf_counter_ns() - PROCESS_START_NS) / 1e6
    print(f"# cold: {cold}")

    # -- host launch overhead ---------------------------------------------
    probe_buffer = gpu.alloc(64)
    reps = 3000
    gpu.synchronize()
    t = time.perf_counter()
    for _ in range(reps):
        gpu.launch(functions[b"field_scale"], 16, [cu.DevicePointer(probe_buffer), U32(1), U32(16)])
    launch_us = (time.perf_counter() - t) / reps * 1e6
    gpu.synchronize()
    gpu.free(probe_buffer)

    # -- exhaustive correctness before any timing ---------------------------
    verification = []
    for variant in ("stage", "pair"):
        print(f"# verifying {variant} kernels exhaustively against direct pair enumeration...")
        report = verify_exhaustively(capped, problem, labels, twiddles, variant)
        print(f"#   {report}")
        verification.append(report)
    correct = all(r["mismatched_jobs"] == 0 and r["count_conservation_failures"] == 0
                  for r in verification)

    last_offset = ((jobs - 1) // capped_batch) * capped_batch
    last_count = jobs - last_offset
    last_exact = [exact_job(problem, labels, last_offset + i) for i in range(last_count)]

    def sample_check() -> bool:
        result = capped.download_batch(last_count)
        return all(np.array_equal(result[i, 0].astype(np.int64), last_exact[i][0]) and
                   np.array_equal(result[i, 1].astype(np.int64), last_exact[i][1])
                   for i in range(last_count))

    rows = []
    sample_failures = 0
    smi_fields = nvidia_smi_fields()
    smi_path = os.path.join(out_dir, "nvidia_smi_samples.csv")
    smi = subprocess.Popen(["nvidia-smi", f"--query-gpu={','.join(smi_fields)}", "--format=csv",
                            "-lms", "50", "-f", smi_path])
    try:
        if correct:
            variants = ("stage", "pair")
            print(f"# warmups={args.warmups} per variant")
            for _ in range(args.warmups):
                for variant in variants:
                    capped.run_end_to_end(twiddles, variant)
            print(f"# timing {args.samples} samples per variant, alternating order")
            for sample in range(args.samples):
                order = variants if sample % 2 == 0 else variants[::-1]
                for variant in order:
                    run = capped.run_end_to_end(twiddles, variant)
                    ok = sample_check()
                    sample_failures += 0 if ok else 1
                    rows.append({"mode": "capped_end_to_end", "variant": variant, "sample": sample,
                                 **run, "output_check": ok})
            for sample in range(args.instrumented_samples):
                for variant in variants:
                    run = capped.run_instrumented(twiddles, variant)
                    rows.append({"mode": "capped_instrumented", "variant": variant, "sample": sample,
                                 **run})
            capped.release()
            free_bytes, _ = gpu.memory_info()
            need = fixed_bytes + jobs * per_job_bytes
            if need < free_bytes - (512 << 20):
                print(f"# DIAGNOSTIC (not verdict-eligible): all {jobs} jobs resident, "
                      f"{need / 2**30:.2f} GiB")
                diagnostic = Pipeline(gpu, functions, problem, jobs)
                for variant in variants:
                    diagnostic.run_end_to_end(twiddles, variant)
                for sample in range(args.diagnostic_samples):
                    for variant in variants:
                        run = diagnostic.run_end_to_end(twiddles, variant)
                        rows.append({"mode": "uncapped_diagnostic", "variant": variant,
                                     "sample": sample, **run})
                diagnostic.release()
            else:
                print("# uncapped diagnostic skipped: insufficient free device memory")
    finally:
        smi.terminate()
        smi.wait(timeout=10)

    # -- analysis ------------------------------------------------------------
    def series(mode: str, variant: str, key: str) -> list[float]:
        return [row[key] for row in rows if row["mode"] == mode and row["variant"] == variant]

    results = {}
    for variant in ("stage", "pair"):
        results[variant] = {
            "device_ms": stats(series("capped_end_to_end", variant, "device_ms")),
            "upload_ms": stats(series("capped_end_to_end", variant, "upload_ms")),
            "total_ms": stats(series("capped_end_to_end", variant, "total_ms")),
            "launches": (series("capped_end_to_end", variant, "launches") or [0])[0],
            "instrumented": {key: stats(series("capped_instrumented", variant, key))
                             for key in ("forward_ms", "multiply_ms", "inverse_ms",
                                         "instrumented_total_ms")},
            "uncapped_diagnostic_device_ms": stats(series("uncapped_diagnostic", variant, "device_ms")),
        }
    best = min(("stage", "pair"), key=lambda v: results[v]["device_ms"].get("median", math.inf))
    transform_ms = results[best]["device_ms"].get("median", math.inf)
    launches_best = results[best]["launches"]
    host_overhead_ms = launches_best * launch_us / 1e3
    total_butterflies = (forward_transforms + inverse_transforms) * BUTTERFLIES
    inverse_butterflies = inverse_transforms * BUTTERFLIES
    inverse_ms = results[best]["instrumented"]["inverse_ms"].get("median", math.inf)

    def classify(value: float) -> str:
        if value <= STRONG_PASS_MS:
            return "STRONG_PASS"
        if value <= NO_GO_MS:
            return "MARGINAL"
        return "TRANSFORM_NO_GO"

    transform_class = classify(transform_ms)
    adjusted_class = classify(transform_ms - host_overhead_ms)
    if not correct or sample_failures:
        verdict = "INCORRECT"
    elif transform_class == "TRANSFORM_NO_GO":
        verdict = "TRANSFORM_NO_GO"
    elif transform_class == "MARGINAL":
        verdict = "MARGINAL_REQUIRES_HEADROOM_ANALYSIS"
    else:
        verdict = "TRANSFORM_STRONG_PASS"

    result = {
        "schema": "ter_convsig_transform_benchmark", "schema_version": 1,
        "phase": "B (transform-only kill test)",
        "git_commit": git_before["commit"], "git_status_at_run": git_before["status_porcelain"],
        "capture_hash_sha256": capture_hash,
        "baseline_hash_sha256": sha256_file(args.authority_pdf),
        "baseline_source": "TER_L1_to_L0_Inference_Problem.pdf (historical matched medians)",
        "implementation_hash_sha256": hashlib.sha256(b"".join(
            open(os.path.join(HERE, name), "rb").read()
            for name in ("convsig_transform_bench.py", "convsig_kernels.py",
                         "convsig_reference.py", "convsig_cuda.py"))).hexdigest(),
        "cpu": platform.processor(), "python": sys.version.split()[0], "numpy": np.__version__,
        "gpu": props, "compile": {k: v for k, v in compile_info.items() if k != "log"},
        "device_limits_probe": limits,
        "frozen": {"g": G, "M": M, "p": P, "d": D, "omega": ref.OMEGA, "M_inverse": ref.M_INVERSE,
                   "label_seed": hex(ref.LABEL_SEED)},
        "fixture": {k: summary[k] for k in expected},
        "H_actual": H, "H_upper_bound_proposal": 570,
        "H_upper_64_ceil_max": 64 * math.ceil(max(len(v) for v in problem["x_unique"]) / G),
        "jobs": jobs, "forward_transforms": forward_transforms,
        "inverse_transforms": inverse_transforms,
        "forward_butterflies": forward_transforms * BUTTERFLIES,
        "inverse_butterflies": inverse_butterflies,
        "butterflies_per_transform": BUTTERFLIES,
        "max_count_bound_64_times_max_Q": 64 * max(len(v) for v in problem["q_unique"]),
        "host_build_ms": problem["host_build_ms"],
        "device_cap_bytes": DEVICE_CAP_BYTES, "capped_batch_jobs": capped_batch,
        "capped_batches": math.ceil(jobs / capped_batch),
        "device_allocated_bytes_capped": capped.allocated + twiddle_host.nbytes,
        "device_peak_bytes_tracked": gpu.peak_device_bytes,
        "host_polynomial_bytes": problem["a_host"].nbytes + problem["b_host"].nbytes,
        "cold_start": cold,
        "host_launch_overhead_us_per_launch": launch_us,
        "verification": verification, "timed_sample_output_check_failures": sample_failures,
        "results": results, "best_variant": best,
        "transform_ms": transform_ms,
        "transform_with_upload_ms": results[best]["total_ms"].get("median"),
        "transforms_per_second": (forward_transforms + inverse_transforms) / (transform_ms / 1e3),
        "butterflies_per_second": total_butterflies / (transform_ms / 1e3),
        "inverse_butterflies_per_second_instrumented": inverse_butterflies / (inverse_ms / 1e3),
        "required_butterflies_per_second_105ms": inverse_butterflies / 0.105,
        "required_butterflies_per_second_145ms": inverse_butterflies / 0.145,
        "logical_bytes": {
            "LOGICAL_NOT_MEASURED": True,
            "stage_inverse_per_transform": 2 * LOG_M * M * 4,
            "pair_inverse_per_transform": 2 * (LOG_M // 2) * M * 4,
            "multiply_per_job": 5 * M * 4,
            "run_total_stage": (forward_transforms + inverse_transforms) * 2 * LOG_M * M * 4 + jobs * 5 * M * 4,
            "run_total_pair": (forward_transforms + inverse_transforms) * 2 * (LOG_M // 2) * M * 4 + jobs * 5 * M * 4,
        },
        "not_measured": ["occupancy", "registers per thread", "shared memory per block usage "
                         "(kernels use none)", "DRAM bytes (no CUPTI/Nsight)", "L2 hit rate",
                         "thread divergence stalls", "local-memory spills", "device clock"],
        "gates": {"strong_pass_ms": STRONG_PASS_MS, "no_go_ms": NO_GO_MS,
                  "meaningful_total_ms": MEANINGFUL_MS, "beat_k2_ms": HIST_K2_MS,
                  "ten_x_scalar_ms": HIST_SCALAR_MS / 10},
        "transform_classification": transform_class,
        "host_overhead_ms_estimate": host_overhead_ms,
        "classification_if_host_launch_overhead_removed": adjusted_class,
        "max_plausible_speedup_vs_k2": HIST_K2_MS / transform_ms,
        "max_plausible_speedup_vs_scalar": HIST_SCALAR_MS / transform_ms,
        "proposal": PROPOSAL,
        "verdict": verdict,
    }
    json.dump(result, open(os.path.join(out_dir, "transform_benchmark.json"), "w"), indent=1, default=str)
    with open(os.path.join(out_dir, "transform_samples.csv"), "w", newline="") as handle:
        keys = sorted({key for row in rows for key in row})
        writer = csv.DictWriter(handle, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)
    with open(os.path.join(out_dir, "historical_fft_comparison.txt"), "w") as handle:
        handle.write(HISTORICAL)
        handle.write("\nTHIS PROPOSAL, MEASURED\n")
        handle.write(f"   {forward_transforms + inverse_transforms} length-{M} transforms "
                     f"({total_butterflies:,} butterflies) replace a {620966064:,}-pair universe: "
                     f"{total_butterflies / 620966064:.1f} field butterflies per source pair\n")
        handle.write(f"   best kernel {best}: {transform_ms:.1f} ms median device time "
                     f"(gate {NO_GO_MS} ms) -> {transform_class}\n")
    smi_text = open(smi_path).read() if os.path.exists(smi_path) else "(no samples)"
    with open(os.path.join(out_dir, "gpu_profile.txt"), "w") as handle:
        handle.write(json.dumps({"gpu": props, "limits": limits, "compile": compile_info,
                                 "host_launch_overhead_us": launch_us,
                                 "launches_per_run": {v: results[v]["launches"] for v in results},
                                 "nvidia_smi_fields": smi_fields,
                                 "not_measured": result["not_measured"]}, indent=1, default=str))
        handle.write("\n\nnvidia-smi samples (50 ms) during timing:\n")
        handle.write(smi_text)
    with open(os.path.join(out_dir, "summary.txt"), "w") as handle:
        handle.write(f"PHASE B TRANSFORM KILL TEST -- verdict {verdict}\n")
        handle.write(f"device {props['name']} cc {props['compute_capability']} driver {props['driver_api_version']} "
                     f"nvrtc {props['nvrtc_version']}\n")
        handle.write(f"H_actual {H} (bound 570); jobs {jobs}; forward {forward_transforms}; "
                     f"inverse {inverse_transforms}; butterflies {total_butterflies:,}\n")
        handle.write(f"exhaustive verification: {verification}\n")
        for variant in ("stage", "pair"):
            r = results[variant]
            handle.write(f"{variant}: device median {r['device_ms'].get('median')} ms "
                         f"p25 {r['device_ms'].get('p25')} p75 {r['device_ms'].get('p75')} "
                         f"min {r['device_ms'].get('min')} p95 {r['device_ms'].get('p95')}; "
                         f"upload median {r['upload_ms'].get('median')} ms; launches {r['launches']}; "
                         f"instrumented inverse median {r['instrumented']['inverse_ms'].get('median')} ms; "
                         f"uncapped diagnostic median {r['uncapped_diagnostic_device_ms'].get('median')} ms\n")
        handle.write(f"transform_ms (best={best}) {transform_ms}; gate {NO_GO_MS}; classification "
                     f"{transform_class}; if host launch overhead removed {adjusted_class}\n")
        handle.write(f"butterflies/s {result['butterflies_per_second']:.4g}; required at 105 ms "
                     f"{result['required_butterflies_per_second_105ms']:.4g}\n")
        handle.write(f"max plausible speedup vs historical K2 {result['max_plausible_speedup_vs_k2']:.3f}x\n")
    print(json.dumps({k: result[k] for k in ("H_actual", "jobs", "best_variant", "transform_ms",
                                             "transform_with_upload_ms", "butterflies_per_second",
                                             "transform_classification",
                                             "classification_if_host_launch_overhead_removed",
                                             "verdict")}, indent=1))
    for variant in ("stage", "pair"):
        print(variant, json.dumps(results[variant], default=str))
    return 0 if verdict != "INCORRECT" else 1


if __name__ == "__main__":
    raise SystemExit(main())
