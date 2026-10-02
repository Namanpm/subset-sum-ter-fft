"""CUDA C kernels for the TER^2 convolution/source-signature spike (compiled by NVRTC).

Arithmetic is exact over F_65537.  Field elements are canonical in [0, 65536].
Multiplication uses the Fermat-prime identity 2^16 == -1 (mod p): for
x = hi * 2^16 + lo, x == lo - hi (mod p).  (p-1)^2 = 2^32 needs 33 bits, so the
product is formed in 64 bits; a 32-bit product would silently wrap.

Two transform kernel families are PRE-DECLARED before any authentic
measurement, and the verdict uses the faster of the two:

    STAGE  one launch per NTT stage (14 memory passes per transform)
    PAIR   two stages fused per launch (7 memory passes per transform)

Both perform exactly the donor Gentleman-Sande forward / Cooley-Tukey inverse
arithmetic of src/q3report_paper_crossover.cpp, so they must agree bit for bit.

Inverse scaling by M^-1 is folded into the B spectra once (the transform is
linear), so no extra full pass over the job outputs is executed.
"""

SOURCE = rb"""
typedef unsigned int u32;
typedef unsigned long long u64;
static const u32 FIELD_P = 65537u;
static const u32 LENGTH = 16384u;

static inline __device__ u32 field_mul(u32 a, u32 b) {
    u64 x = (u64)a * (u64)b;
    long long r = (long long)(x & 0xFFFFull) - (long long)(x >> 16);
    if (r < 0) r += (long long)FIELD_P;
    return (u32)r;
}
static inline __device__ u32 field_add(u32 a, u32 b) {
    u32 s = a + b;
    return s >= FIELD_P ? s - FIELD_P : s;
}
static inline __device__ u32 field_sub(u32 a, u32 b) {
    return a >= b ? a - b : a + FIELD_P - b;
}

/* ---- STAGE family: 8192 butterflies per array per stage ---------------- */
extern "C" __global__ void gs_forward_stage(u32* data, const u32* tw, u32 log_half, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    u32 arr = t >> 13;
    u32 j = t & 8191u;
    u32 half = 1u << log_half;
    u32 group = j >> log_half;
    u32 off = j & (half - 1u);
    u64 a = (u64)arr * (u64)LENGTH + (u64)group * (u64)(half << 1) + (u64)off;
    u64 b = a + (u64)half;
    u32 upper = data[a];
    u32 lower = data[b];
    u32 diff = field_sub(upper, lower);
    data[a] = field_add(upper, lower);
    data[b] = off == 0u ? diff : field_mul(diff, tw[half - 1u + off]);
}

extern "C" __global__ void ct_inverse_stage(u32* data, const u32* tw, u32 log_half, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    u32 arr = t >> 13;
    u32 j = t & 8191u;
    u32 half = 1u << log_half;
    u32 group = j >> log_half;
    u32 off = j & (half - 1u);
    u64 a = (u64)arr * (u64)LENGTH + (u64)group * (u64)(half << 1) + (u64)off;
    u64 b = a + (u64)half;
    u32 even = data[a];
    u32 odd = off == 0u ? data[b] : field_mul(data[b], FIELD_P - tw[half - 1u + half - off]);
    data[a] = field_add(even, odd);
    data[b] = field_sub(even, odd);
}

/* ---- PAIR family: 4096 four-element networks per array per launch ------ */
extern "C" __global__ void gs_forward_pair(u32* data, const u32* tw, u32 log_h, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    u32 arr = t >> 12;
    u32 j = t & 4095u;
    u32 h = 1u << log_h;
    u32 group = j >> log_h;
    u32 o = j & (h - 1u);
    u64 p0 = (u64)arr * (u64)LENGTH + (u64)group * (u64)(h << 2) + (u64)o;
    u64 p1 = p0 + h, p2 = p0 + 2u * h, p3 = p0 + 3u * h;
    u32 d0 = data[p0], d1 = data[p1], d2 = data[p2], d3 = data[p3];
    /* stage span 4h, half 2h: (p0,p2) at offset o, (p1,p3) at offset o+h */
    u32 s02 = field_add(d0, d2), x02 = field_sub(d0, d2);
    u32 s13 = field_add(d1, d3), x13 = field_sub(d1, d3);
    u32 two_h = h << 1;
    x02 = o == 0u ? x02 : field_mul(x02, tw[two_h - 1u + o]);
    x13 = field_mul(x13, tw[two_h - 1u + o + h]);
    /* stage span 2h, half h: (p0,p1) and (p2,p3) both at offset o */
    u32 s01 = field_add(s02, s13), x01 = field_sub(s02, s13);
    u32 s23 = field_add(x02, x13), x23 = field_sub(x02, x13);
    if (o != 0u) {
        u32 w = tw[h - 1u + o];
        x01 = field_mul(x01, w);
        x23 = field_mul(x23, w);
    }
    data[p0] = s01; data[p1] = x01; data[p2] = s23; data[p3] = x23;
}

extern "C" __global__ void ct_inverse_pair(u32* data, const u32* tw, u32 log_h, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    u32 arr = t >> 12;
    u32 j = t & 4095u;
    u32 h = 1u << log_h;
    u32 group = j >> log_h;
    u32 o = j & (h - 1u);
    u64 p0 = (u64)arr * (u64)LENGTH + (u64)group * (u64)(h << 2) + (u64)o;
    u64 p1 = p0 + h, p2 = p0 + 2u * h, p3 = p0 + 3u * h;
    u32 d0 = data[p0], d1 = data[p1], d2 = data[p2], d3 = data[p3];
    /* stage span 2h, half h: (p0,p1) and (p2,p3) at offset o */
    if (o != 0u) {
        u32 w = FIELD_P - tw[h - 1u + h - o];
        d1 = field_mul(d1, w);
        d3 = field_mul(d3, w);
    }
    u32 e0 = field_add(d0, d1), e1 = field_sub(d0, d1);
    u32 e2 = field_add(d2, d3), e3 = field_sub(d2, d3);
    /* stage span 4h, half 2h: (p0,p2) at offset o, (p1,p3) at offset o+h */
    u32 two_h = h << 1;
    u32 f2 = o == 0u ? e2 : field_mul(e2, FIELD_P - tw[two_h - 1u + two_h - o]);
    u32 f3 = field_mul(e3, FIELD_P - tw[two_h - 1u + two_h - (o + h)]);
    data[p0] = field_add(e0, f2); data[p2] = field_sub(e0, f2);
    data[p1] = field_add(e1, f3); data[p3] = field_sub(e1, f3);
}

/* ---- spectral products and scaling ------------------------------------ */
extern "C" __global__ void spectral_multiply(const u32* a_hat, const u32* b_hat, u32* out,
                                             const u32* job_shard, const u32* job_class,
                                             u32 job_offset, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    u32 local = t >> 14;
    u32 k = t & 16383u;
    u32 job = job_offset + local;
    u64 sa = (u64)job_shard[job] * 2ull * (u64)LENGTH + (u64)k;
    u64 sb = (u64)job_class[job] * (u64)LENGTH + (u64)k;
    u32 bv = b_hat[sb];
    u64 o = (u64)local * 2ull * (u64)LENGTH + (u64)k;
    out[o] = field_mul(a_hat[sa], bv);
    out[o + (u64)LENGTH] = field_mul(a_hat[sa + (u64)LENGTH], bv);
}

extern "C" __global__ void field_scale(u32* data, u32 factor, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    data[t] = field_mul(data[t], factor);
}

/* test-only: exhaustive-style multiply check */
extern "C" __global__ void field_mul_batch(const u32* a, const u32* b, u32* out, u32 total) {
    u32 t = blockDim.x * blockIdx.x + threadIdx.x;
    if (t >= total) return;
    out[t] = field_mul(a[t], b[t]);
}
"""

FUNCTIONS = (
    b"gs_forward_stage", b"ct_inverse_stage", b"gs_forward_pair", b"ct_inverse_pair",
    b"spectral_multiply", b"field_scale", b"field_mul_batch",
)
