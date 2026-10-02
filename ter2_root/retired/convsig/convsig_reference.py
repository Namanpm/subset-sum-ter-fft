"""Exact CPU reference for the TER^2 convolution/source-signature spike.

SCOPE.  Research tooling.  These routines are the independent correctness
authority for the GPU kernels.  They use plain int64 NumPy arithmetic with `% P`,
never the GPU kernels' Fermat reduction, so a defect in one path cannot hide in
both.

Frozen proposal parameters (never tuned here):
    g = 64 source positions per X shard
    M = 2^14 cyclic transform length
    p = 65537 = 2^16 + 1
    d = 4 maximum decoded count
"""
from __future__ import annotations

from itertools import chain, combinations_with_replacement

import numpy as np

G = 64
M = 1 << 14
LOG_M = 14
P = 65537
D = 4
PRIMITIVE_ROOT = 3
OMEGA = pow(PRIMITIVE_ROOT, (P - 1) // M, P)  # 81
M_INVERSE = pow(M, P - 2, P)  # 65533

# Deterministic source labels, frozen before any authentic measurement.  One
# scheme, one seed; never re-drawn after observing results.
LABEL_SEED = 0x7E2C516EA70C0001


def _splitmix64(value: int) -> int:
    value = (value + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return value ^ (value >> 31)


def labels() -> np.ndarray:
    """w_i in [1, p-1] for source position i in [0, 64)."""
    return np.array([1 + _splitmix64(LABEL_SEED + index) % (P - 1) for index in range(G)],
                    dtype=np.int64)


def forward_twiddles() -> np.ndarray:
    """Donor layout (src/q3report_paper_crossover.cpp): tw[half-1+offset] = root_span^offset."""
    table = np.zeros(M - 1, dtype=np.int64)
    span = 2
    while span <= M:
        half = span // 2
        stage_root = pow(PRIMITIVE_ROOT, (P - 1) // span, P)
        power = 1
        for offset in range(half):
            table[half - 1 + offset] = power
            power = power * stage_root % P
        span *= 2
    return table


_TW = forward_twiddles()


def ntt_forward(batch: np.ndarray) -> np.ndarray:
    """Gentleman-Sande forward, natural -> bit-reversed.  batch shape (K, M), int64."""
    data = np.array(batch, dtype=np.int64, copy=True).reshape(-1, M)
    span = M
    while span >= 2:
        half = span // 2
        view = data.reshape(data.shape[0], M // span, span)
        upper = view[:, :, :half].copy()
        lower = view[:, :, half:].copy()
        twiddle = _TW[half - 1:half - 1 + half]
        view[:, :, :half] = (upper + lower) % P
        view[:, :, half:] = ((upper - lower) % P) * twiddle % P
        span //= 2
    return data


def ntt_inverse(batch: np.ndarray, scale: bool = True) -> np.ndarray:
    """Cooley-Tukey inverse, bit-reversed -> natural, then multiply by M^-1."""
    data = np.array(batch, dtype=np.int64, copy=True).reshape(-1, M)
    span = 2
    while span <= M:
        half = span // 2
        view = data.reshape(data.shape[0], M // span, span)
        twiddle = np.empty(half, dtype=np.int64)
        twiddle[0] = 1
        if half > 1:
            offsets = np.arange(1, half)
            twiddle[1:] = P - _TW[half - 1 + half - offsets]
        even = view[:, :, :half].copy()
        odd = view[:, :, half:] * twiddle % P
        view[:, :, :half] = (even + odd) % P
        view[:, :, half:] = (even - odd) % P
        span *= 2
    if scale:
        data = data * M_INVERSE % P
    return data


def cyclic_convolution(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    """Definitional O(M^2) cyclic convolution mod P; values < P so int64 cannot overflow."""
    full = np.convolve(np.asarray(left, dtype=np.int64), np.asarray(right, dtype=np.int64))
    folded = full[:M].copy()
    folded[:len(full) - M] += full[M:]
    return folded % P


def count_signature_exact(x_values: np.ndarray, q_values: np.ndarray,
                          weights: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Exact C[r] and F[r] by enumerating every (x, q) pair: no transform at all."""
    residues = (x_values.astype(np.int64)[:, None] + q_values.astype(np.int64)[None, :]) % M
    count = np.bincount(residues.ravel(), minlength=M).astype(np.int64)
    weighted = np.broadcast_to(weights.astype(np.float64)[:, None], residues.shape).ravel()
    # Sums stay below 64 * 1024 * 65536 < 2^53, so float64 accumulation is exact.
    signature = np.rint(np.bincount(residues.ravel(), weights=weighted, minlength=M))
    return count, signature.astype(np.int64) % P


def build_decoder(label_values: np.ndarray) -> tuple[np.ndarray, dict]:
    """decode[c][sig] = OR of the masks of every c-multiset of positions with that signature."""
    table = np.zeros((D + 1, P), dtype=np.uint64)
    stats = {}
    for count in range(1, D + 1):
        combos = np.fromiter(chain.from_iterable(combinations_with_replacement(range(G), count)),
                             dtype=np.int64).reshape(-1, count)
        signature = label_values[combos].sum(axis=1) % P
        mask = np.zeros(len(combos), dtype=np.uint64)
        for column in range(count):
            mask |= np.left_shift(np.uint64(1), combos[:, column].astype(np.uint64))
        np.bitwise_or.at(table[count], signature, mask)
        occupied = np.unique(signature).size
        stats[count] = {"multisets": int(len(combos)), "occupied_signatures": int(occupied)}
    return table, stats


def popcount64(values: np.ndarray) -> np.ndarray:
    values = values.astype(np.uint64)
    result = np.zeros(values.shape, dtype=np.int64)
    for shift in range(0, 64, 16):
        chunk = ((values >> np.uint64(shift)) & np.uint64(0xFFFF)).astype(np.int64)
        result += _POPCOUNT16[chunk]
    return result


_POPCOUNT16 = np.array([bin(index).count("1") for index in range(1 << 16)], dtype=np.int64)


def decode_mask(table: np.ndarray, count: int, signature: int, live_mask: int) -> int:
    """No false negatives: count 0 is certainly empty, count > D returns every live position."""
    if count == 0:
        return 0
    if count > D:
        return live_mask
    return int(table[count][signature])


def contributors(x_values: np.ndarray, q_values: np.ndarray, residue: int) -> list[int]:
    """Definitional multiset of source positions contributing to C[residue]."""
    positions = []
    for index, x in enumerate(x_values.tolist()):
        for q in q_values.tolist():
            if (x + q) % M == residue:
                positions.append(index)
    return positions
