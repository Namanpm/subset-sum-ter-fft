"""Correctness tests for the TER^2 convolution/source-signature spike.

Run before any performance measurement.  CPU tests always run.  GPU tests run
when --nvrtc-dir is given; a GPU that cannot be opened is reported as a failure,
never silently skipped when GPU testing was requested.

Oracles are independent of the kernels under test:
  * O(M^2) np.convolve cyclic convolution (no transform)
  * direct pair enumeration with np.bincount (no transform)
  * exhaustive enumeration of all 814,384 decoder multisets
"""
from __future__ import annotations

import argparse
import ctypes as C
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bench"))

import convsig_reference as ref  # noqa: E402

M, P, G, D = ref.M, ref.P, ref.G, ref.D
CHECKS = 0
FAILURES = 0


def check(condition: bool, what: str) -> None:
    global CHECKS, FAILURES
    CHECKS += 1
    if not condition:
        FAILURES += 1
        print(f"FAIL: {what}")


# ---------------------------------------------------------------------------
# CPU: field and transform
# ---------------------------------------------------------------------------
def test_field_constants() -> None:
    check(ref.OMEGA == 81, "omega != 81")
    check(pow(81, M, P) == 1 and pow(81, M // 2, P) == P - 1, "81 does not have order exactly M")
    check(ref.M_INVERSE == 65533 and M * ref.M_INVERSE % P == 1, "M^-1 != 65533")
    check((P - 1) % M == 0, "M does not divide p-1")


def test_twiddles() -> None:
    table = ref.forward_twiddles()
    for span in (2, 4, 64, 1024, M):
        half = span // 2
        root = pow(3, (P - 1) // span, P)
        expect = [pow(root, offset, P) for offset in range(half)]
        check(table[half - 1:half - 1 + half].tolist() == expect, f"twiddle table wrong at span {span}")


def _vectors(rng: np.random.Generator) -> list[np.ndarray]:
    zeros = np.zeros(M, dtype=np.int64)
    impulse0 = zeros.copy()
    impulse0[0] = 1
    impulse_top = zeros.copy()
    impulse_top[M - 1] = 7
    maximum = np.full(M, P - 1, dtype=np.int64)
    random = rng.integers(0, P, M, dtype=np.int64)
    sparse = zeros.copy()
    sparse[rng.integers(0, M, 40)] = rng.integers(1, 64, 40)
    top = zeros.copy()
    top[M - 50:] = rng.integers(0, P, 50)  # forces cyclic wraparound in products
    return [zeros, impulse0, impulse_top, maximum, random, sparse, top]


def test_ntt_roundtrip_cpu(rng: np.random.Generator) -> None:
    batch = np.stack(_vectors(rng))
    back = ref.ntt_inverse(ref.ntt_forward(batch))
    check(np.array_equal(back, batch), "CPU forward+inverse round trip failed")


def test_ntt_convolution_cpu(rng: np.random.Generator) -> None:
    vectors = _vectors(rng)
    pairs = [(vectors[4], vectors[4]), (vectors[3], vectors[3]), (vectors[6], vectors[6]),
             (vectors[1], vectors[4]), (vectors[2], vectors[5]), (vectors[5], vectors[6]),
             (vectors[0], vectors[4])]
    for index, (left, right) in enumerate(pairs):
        spectra = ref.ntt_forward(np.stack([left, right]))
        via_ntt = ref.ntt_inverse((spectra[0] * spectra[1] % P)[None, :])[0]
        check(np.array_equal(via_ntt, ref.cyclic_convolution(left, right)),
              f"CPU NTT convolution != scalar cyclic convolution (pair {index})")
    for trial in range(5):
        left = np.zeros(M, dtype=np.int64)
        right = np.zeros(M, dtype=np.int64)
        left[rng.integers(0, M, 64)] += 1
        right[rng.integers(0, M, 300)] += 1
        spectra = ref.ntt_forward(np.stack([left, right]))
        via_ntt = ref.ntt_inverse((spectra[0] * spectra[1] % P)[None, :])[0]
        check(np.array_equal(via_ntt, ref.cyclic_convolution(left, right)),
              f"CPU NTT count convolution property trial {trial}")


# ---------------------------------------------------------------------------
# CPU: signatures and decoder
# ---------------------------------------------------------------------------
def test_decoder_no_false_negative_exhaustive(table: np.ndarray, labels: np.ndarray) -> None:
    from itertools import chain, combinations_with_replacement
    total = 0
    for count in range(1, D + 1):
        combos = np.fromiter(chain.from_iterable(combinations_with_replacement(range(G), count)),
                             dtype=np.int64).reshape(-1, count)
        signature = labels[combos].sum(axis=1) % P
        mask = np.zeros(len(combos), dtype=np.uint64)
        for column in range(count):
            mask |= np.left_shift(np.uint64(1), combos[:, column].astype(np.uint64))
        decoded = table[count][signature]
        missing = np.count_nonzero(mask & ~decoded)
        check(missing == 0, f"decoder drops a true position for count {count} ({missing} multisets)")
        total += len(combos)
    check(total == 814384, f"multiset enumeration total {total} != 814,384")


def _synthetic_shard() -> tuple[np.ndarray, np.ndarray]:
    sizes = [1] * 22 + [2] * 8 + [3] * 4 + [4] * 2 + [6]
    values = []
    for group, size in enumerate(sizes):
        base = 101 + 257 * group
        for copy in range(size):
            values.append(base + copy * M)  # identical projection mod M
    x = np.array(values, dtype=np.int64)
    # q and q + M share a projection, so one source position contributes twice.
    q = np.array([0, 5000, M, 2 * M + 5000, 9999], dtype=np.int64)
    assert len(x) == G and x.max() < (1 << 19)
    return x, q


def test_signature_decoding_synthetic(table: np.ndarray, labels: np.ndarray) -> None:
    x, q = _synthetic_shard()
    weights = labels[:len(x)]
    count, signature = ref.count_signature_exact(x, q, weights)
    residues = (x[:, None] + q[None, :]) % M
    truth_mask = np.zeros(M, dtype=np.uint64)
    positions = np.broadcast_to(np.arange(len(x))[:, None], residues.shape).ravel()
    np.bitwise_or.at(truth_mask, residues.ravel(),
                     np.left_shift(np.uint64(1), positions.astype(np.uint64)))
    live = (1 << len(x)) - 1
    buckets = {"0": 0, "1": 0, "2": 0, "3": 0, "4": 0, ">4": 0}
    dropped = 0
    repeated = 0
    for r in range(M):
        c = int(count[r])
        buckets[str(c) if c <= 4 else ">4"] += 1
        decoded = ref.decode_mask(table, c, int(signature[r]), live)
        if int(truth_mask[r]) & ~decoded:
            dropped += 1
        if 0 < c <= 4 and bin(int(truth_mask[r])).count("1") < c:
            repeated += 1
        if c > 4:
            check(decoded == live, f"count>4 at residue {r} did not return the live mask")
    check(dropped == 0, f"synthetic decode dropped true positions at {dropped} residues")
    for name, seen in buckets.items():
        check(seen > 0, f"synthetic geometry never produced count {name}")
    check(repeated > 0, "synthetic geometry never produced a repeated source position")
    for r in (int(np.argmax(count)), int((x[0] + q[1]) % M), int((x[30] + q[0]) % M)):
        slow = ref.contributors(x, q, r)
        check(len(slow) == int(count[r]), f"definitional contributor count mismatch at {r}")
        check(sum(int(labels[i]) for i in slow) % P == int(signature[r]),
              f"definitional signature mismatch at {r}")


# ---------------------------------------------------------------------------
# GPU
# ---------------------------------------------------------------------------
def gpu_tests(nvrtc_dir: str, rng: np.random.Generator) -> None:
    import convsig_cuda as cu
    import convsig_kernels as kernels

    gpu = cu.Gpu(nvrtc_dir)
    props = gpu.properties()
    module, _ = gpu.compile(kernels.SOURCE, b"convsig.cu", props["arch_flag"])
    fn = {name: gpu.function(module, name) for name in kernels.FUNCTIONS}
    u32 = C.c_uint

    def device(array: np.ndarray) -> int:
        pointer = gpu.alloc(array.nbytes)
        gpu.upload(pointer, np.ascontiguousarray(array))
        return pointer

    # Field multiplication: every edge value against every edge value, plus random.
    edges = np.array([0, 1, 2, 255, 256, 32767, 32768, 65535, 65536], dtype=np.uint32)
    left = np.concatenate([np.repeat(edges, len(edges)), rng.integers(0, P, 1 << 20, dtype=np.uint32)])
    right = np.concatenate([np.tile(edges, len(edges)), rng.integers(0, P, 1 << 20, dtype=np.uint32)])
    pl, pr = device(left), device(right)
    po = gpu.alloc(left.nbytes)
    gpu.launch(fn[b"field_mul_batch"], len(left), [cu.DevicePointer(pl), cu.DevicePointer(pr),
                                                   cu.DevicePointer(po), u32(len(left))])
    gpu.synchronize()
    out = np.empty_like(left)
    gpu.download(out, po)
    check(np.array_equal(out.astype(np.int64), left.astype(np.int64) * right.astype(np.int64) % P),
          "GPU field multiply is not exact")

    twiddles = device(ref.forward_twiddles().astype(np.uint32))
    batch = np.stack(_vectors(rng)).astype(np.uint32)
    arrays = batch.shape[0]
    expect_forward = ref.ntt_forward(batch)

    def run_forward(pointer: int, variant: str) -> None:
        if variant == "stage":
            for log_half in range(ref.LOG_M - 1, -1, -1):
                gpu.launch(fn[b"gs_forward_stage"], arrays * (M // 2),
                           [cu.DevicePointer(pointer), cu.DevicePointer(twiddles), u32(log_half),
                            u32(arrays * (M // 2))])
        else:
            for log_h in range(ref.LOG_M - 2, -1, -2):
                gpu.launch(fn[b"gs_forward_pair"], arrays * (M // 4),
                           [cu.DevicePointer(pointer), cu.DevicePointer(twiddles), u32(log_h),
                            u32(arrays * (M // 4))])

    def run_inverse(pointer: int, variant: str) -> None:
        if variant == "stage":
            for log_half in range(0, ref.LOG_M):
                gpu.launch(fn[b"ct_inverse_stage"], arrays * (M // 2),
                           [cu.DevicePointer(pointer), cu.DevicePointer(twiddles), u32(log_half),
                            u32(arrays * (M // 2))])
        else:
            for log_h in range(0, ref.LOG_M, 2):
                gpu.launch(fn[b"ct_inverse_pair"], arrays * (M // 4),
                           [cu.DevicePointer(pointer), cu.DevicePointer(twiddles), u32(log_h),
                            u32(arrays * (M // 4))])

    for variant in ("stage", "pair"):
        pointer = device(batch)
        run_forward(pointer, variant)
        gpu.synchronize()
        spectra = np.empty_like(batch)
        gpu.download(spectra, pointer)
        check(np.array_equal(spectra.astype(np.int64), expect_forward),
              f"GPU {variant} forward differs from CPU reference")
        run_inverse(pointer, variant)
        gpu.launch(fn[b"field_scale"], batch.size, [cu.DevicePointer(pointer), u32(ref.M_INVERSE),
                                                    u32(batch.size)])
        gpu.synchronize()
        back = np.empty_like(batch)
        gpu.download(back, pointer)
        check(np.array_equal(back, batch), f"GPU {variant} forward+inverse round trip failed")

    # The production-shaped pipeline: count and signature convolutions for
    # synthetic shards against synthetic Q classes, compared with direct pair
    # enumeration (no transform in the oracle).
    labels = ref.labels()
    shards = 3
    classes = 4
    a_host = np.zeros((shards, 2, M), dtype=np.uint32)
    x_sets = []
    for shard in range(shards):
        x = np.sort(rng.choice(1 << 19, G, replace=False)).astype(np.int64)
        x_sets.append(x)
        np.add.at(a_host[shard, 0], x % M, 1)
        acc = np.zeros(M, dtype=np.int64)
        np.add.at(acc, x % M, labels[:G])
        a_host[shard, 1] = (acc % P).astype(np.uint32)
    b_host = np.zeros((classes, M), dtype=np.uint32)
    q_sets = []
    for cls in range(classes):
        q = np.sort(rng.choice(1 << 19, 300 + 7 * cls, replace=False)).astype(np.int64)
        q_sets.append(q)
        np.add.at(b_host[cls], q % M, 1)
    jobs = [(shard, cls) for shard in range(shards) for cls in range(classes)]
    job_shard = np.array([j[0] for j in jobs], dtype=np.uint32)
    job_class = np.array([j[1] for j in jobs], dtype=np.uint32)
    for variant in ("stage", "pair"):
        pa, pb = device(a_host), device(b_host)
        arrays = shards * 2
        run_forward(pa, variant)
        arrays = classes
        run_forward(pb, variant)
        gpu.launch(fn[b"field_scale"], b_host.size, [cu.DevicePointer(pb), u32(ref.M_INVERSE),
                                                     u32(b_host.size)])
        ps, pc = device(job_shard), device(job_class)
        out = gpu.alloc(len(jobs) * 2 * M * 4)
        gpu.launch(fn[b"spectral_multiply"], len(jobs) * M,
                   [cu.DevicePointer(pa), cu.DevicePointer(pb), cu.DevicePointer(out),
                    cu.DevicePointer(ps), cu.DevicePointer(pc), u32(0), u32(len(jobs) * M)])
        arrays = len(jobs) * 2
        run_inverse(out, variant)
        gpu.synchronize()
        result = np.empty((len(jobs), 2, M), dtype=np.uint32)
        gpu.download(result, out)
        for index, (shard, cls) in enumerate(jobs):
            count, signature = ref.count_signature_exact(x_sets[shard], q_sets[cls], labels[:G])
            check(np.array_equal(result[index, 0].astype(np.int64), count),
                  f"GPU {variant} count convolution wrong for job {index}")
            check(np.array_equal(result[index, 1].astype(np.int64), signature),
                  f"GPU {variant} signature convolution wrong for job {index}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nvrtc-dir", default=None)
    args = parser.parse_args()
    rng = np.random.default_rng(0x5EED0C0)
    test_field_constants()
    test_twiddles()
    test_ntt_roundtrip_cpu(rng)
    test_ntt_convolution_cpu(rng)
    print("cpu transform tests done")
    labels = ref.labels()
    table, stats = ref.build_decoder(labels)
    print("decoder built:", stats)
    test_decoder_no_false_negative_exhaustive(table, labels)
    test_signature_decoding_synthetic(table, labels)
    print("cpu signature tests done")
    if args.nvrtc_dir:
        try:
            gpu_tests(args.nvrtc_dir, rng)
            print("gpu tests done")
        except Exception as error:  # a requested GPU test that cannot run is a failure
            check(False, f"GPU tests could not run: {error}")
    else:
        print("gpu tests NOT RUN (no --nvrtc-dir)")
    print(f"checks={CHECKS} failures={FAILURES}")
    return 0 if FAILURES == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
