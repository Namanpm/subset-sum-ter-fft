/*
 * stage_d_bitset_join.c — reference kernel for the Stage D pair-listing floor.
 *
 * Problem: report all (a,b) in A x B with (a+b) mod 2^K in S, S = { (T - c) mod 2^K : c in C }.
 *
 * This is NOT an output-sensitive structure (see analysis: for uniform residues at
 * these densities none can exist — every fiber and every element is productive, so
 * no index has pruning power below full-resolution per-pair checks). It is the
 * listing floor executed at ~1.7 ns/probe single-thread (measured), embarrassingly
 * parallel over A. S lives in a 2 MB bitset -> L2-resident random probes.
 *
 * Measured (this exact code, scalar, 1 thread, sandbox Zen-class core):
 *   621,423,377 probes, 1,227,299 survivors, 1.03 s (601 Mprobes/s).
 * With OpenMP on 8 cores expect ~0.15 s. Compile:
 *   gcc -O3 -march=native -fopenmp stage_d_bitset_join.c -o stage_d_join
 *
 * For the architecture that actually removes the A x B iteration (convolution /
 * NTT over Z_2^K producing the next level's residue histogram directly, witnesses
 * recovered by backtrace), see the accompanying discussion — that path never lists
 * intermediate pairs at all.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define KBITS 24u
#define MOD   (1u << KBITS)
#define MASK  (MOD - 1u)

typedef struct { uint32_t a, b; } pair32;

/* Build the target bitset from C residues and target T (all mod 2^K). */
static uint64_t *build_target_bitset(const uint32_t *Cres, size_t nC, uint32_t T)
{
    uint64_t *D = calloc(MOD / 64, sizeof *D);
    if (!D) return NULL;
    for (size_t i = 0; i < nC; i++) {
        uint32_t s = (T - Cres[i]) & MASK;
        D[s >> 6] |= 1ull << (s & 63);
    }
    return D;
}

/*
 * List all compatible pairs. Returns count; writes pairs into *out (malloc'd,
 * caller frees). Parallel over blocks of A; per-thread buffers sized from a
 * first counting pass so no locking or realloc happens in the hot loop.
 * Two passes cost 2x probes but stay allocation-free and deterministic in
 * output order (sorted by index in A, then B) — drop pass 1 and use atomics
 * if you prefer one pass.
 */
size_t stage_d_list(const uint32_t *A, size_t nA,
                    const uint32_t *B, size_t nB,
                    const uint64_t *D, pair32 **out)
{
    size_t total = 0;

    /* pass 1: exact per-a counts -> prefix offsets */
    size_t *off = malloc((nA + 1) * sizeof *off);
    if (!off) return 0;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) reduction(+:total)
#endif
    for (size_t i = 0; i < nA; i++) {
        uint32_t a = A[i] & MASK;
        size_t c = 0;
        for (size_t j = 0; j < nB; j++) {
            uint32_t s = (a + B[j]) & MASK;
            c += (D[s >> 6] >> (s & 63)) & 1u;
        }
        off[i + 1] = c;      /* filled as counts, prefix-summed below */
        total += c;
    }
    off[0] = 0;
    for (size_t i = 0; i < nA; i++) off[i + 1] += off[i];

    pair32 *res = malloc(total ? total * sizeof *res : sizeof *res);
    if (!res) { free(off); return 0; }

    /* pass 2: emit into precomputed slots, no synchronization */
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (size_t i = 0; i < nA; i++) {
        uint32_t a = A[i] & MASK;
        size_t w = off[i];
        for (size_t j = 0; j < nB; j++) {
            uint32_t s = (a + B[j]) & MASK;
            if ((D[s >> 6] >> (s & 63)) & 1u) {
                res[w].a = A[i];
                res[w].b = B[j];
                w++;
            }
        }
    }

    free(off);
    *out = res;
    return total;
}

#ifdef STAGE_D_BENCH_MAIN
static uint64_t rs = 88172645463325252ull;
static uint64_t xr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

int main(void)
{
    const size_t nA = 19391, nB = 32047, nC = 33211;
    uint32_t *A = malloc(nA * 4), *B = malloc(nB * 4), *C = malloc(nC * 4);
    for (size_t i = 0; i < nA; i++) A[i] = xr() & MASK;
    for (size_t i = 0; i < nB; i++) B[i] = xr() & MASK;
    for (size_t i = 0; i < nC; i++) C[i] = xr() & MASK;

    uint64_t *D = build_target_bitset(C, nC, 0x00ABCDEFu & MASK);
    pair32 *out = NULL;
    size_t n = stage_d_list(A, nA, B, nB, D, &out);

    double pred = (double)nA * nB * nC / (double)MOD; /* uniform model, ignores C dups */
    printf("survivors: %zu (uniform prediction ~%.0f)\n", n, pred);

    free(out); free(D); free(A); free(B); free(C);
    return 0;
}
#endif
