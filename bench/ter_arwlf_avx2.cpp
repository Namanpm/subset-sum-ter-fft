// ARWLF row sweep, AVX2 translation unit (compiled with /arch:AVX2 or -mavx2
// only; callers gate on translation_unit_is_avx2() and the runtime ISA check).

#include "ter_arwlf.hpp"

#include <bit>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#define TER_ARWLF_NOINLINE __declspec(noinline)
#define TER_ARWLF_FORCEINLINE __forceinline
#else
#define TER_ARWLF_NOINLINE __attribute__((noinline))
#define TER_ARWLF_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace ter::arwlf::avx2 {
namespace {

// The rare path: lanes whose bytes 0 and 1 passed.  Kept out of line so the
// row loop keeps all of its state in registers.
struct Slow {
  const std::uint32_t* q_mid;
  const std::uint32_t* x_mid;
  const LeafTables* tables;
  std::uint32_t chunks;
  std::uint32_t mid_mask;
  std::vector<Survivor>* queue;
};

// `pass`: bit 2i set for each lane i of the block at xbase.  Bytes 2 ..
// chunks-1 are tested on the full mid sum; lanes that pass them are queued.
TER_ARWLF_NOINLINE void take(const Slow& c, const std::uint32_t qa, const std::uint32_t xbase,
                             std::uint32_t pass, const std::uint32_t carry) {
  const std::uint32_t q = c.q_mid[qa] + carry;
  do {
    const std::uint32_t xa = xbase + (static_cast<std::uint32_t>(std::countr_zero(pass)) >> 1U);
    pass &= pass - 1U;
    const std::uint32_t s = (q + c.x_mid[xa]) & c.mid_mask;
    bool keep = true;
    for (std::uint32_t chunk = 2U; chunk < c.chunks; ++chunk) {
      const std::uint32_t byte = (s >> (8U * chunk)) & 0xFFU;
      keep = keep && ((c.tables->word[chunk][byte >> 5U] >> (byte & 31U)) & 1U) != 0U;
    }
    if (keep) {
      c.queue->push_back(Survivor{qa, xa, carry});
    }
  } while (pass != 0U);
}

// Live-lane mask of a 16-lane block holding `lanes` (1 .. 16+) entries.
[[nodiscard]] std::uint32_t lane_mask(const std::uint32_t lanes) noexcept {
  return lanes >= kPacket ? 0x55555555U : ((1U << (2U * lanes)) - 1U) & 0x55555555U;
}

#if defined(__AVX2__)

// Byte tables of chunks 0 and 1 for pshufb: a = values [0, 128), b = values
// [128, 256), 16 bytes each, broadcast to both 128-bit halves.
struct Lut {
  __m256i a0, b0, a1, b1;
  __m256i bits;  // byte i -> 1 << i, i < 8
  __m256i low5, low3, lo_bias, hi_bias, odd;
};

TER_ARWLF_FORCEINLINE Lut make_lut(const LeafTables& t) noexcept {
  const auto half = [](const std::uint32_t* p) noexcept {
    return _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(p)));
  };
  Lut l;
  l.a0 = half(t.word[0]);
  l.b0 = half(t.word[0] + 4);
  l.a1 = half(t.word[1]);
  l.b1 = half(t.word[1] + 4);
  l.bits = _mm256_setr_epi8(1, 2, 4, 8, 16, 32, 64, -128, 0, 0, 0, 0, 0, 0, 0, 0,  //
                            1, 2, 4, 8, 16, 32, 64, -128, 0, 0, 0, 0, 0, 0, 0, 0);
  l.low5 = _mm256_set1_epi8(0x1F);
  l.low3 = _mm256_set1_epi8(0x07);
  l.lo_bias = _mm256_set1_epi8(0x70);                      // v + 0x70: bit 7 clear iff v < 16
  l.hi_bias = _mm256_set1_epi8(static_cast<char>(0xF0));  // v + 0xF0: bit 7 clear iff v >= 16
  l.odd = _mm256_set1_epi16(static_cast<short>(0xFF00));  // byte 1 of every 16-bit lane
  return l;
}

// Movemask of the bytes of s absent from their table: byte 0 of each 16-bit
// lane against chunk 0, byte 1 against chunk 1.  Table byte b >> 3 holds the
// values [8 (b >> 3), +8); pshufb yields 0 for an index with bit 7 set, which
// selects the a (b < 128) or b (b >= 128) half without a branch.
TER_ARWLF_FORCEINLINE std::uint32_t absent(const __m256i s, const Lut& l) noexcept {
  const __m256i v = _mm256_and_si256(_mm256_srli_epi16(s, 3), l.low5);  // per byte: byte >> 3
  const __m256i lo = _mm256_add_epi8(v, l.lo_bias);
  const __m256i hi = _mm256_add_epi8(v, l.hi_bias);
  const __m256i row0 = _mm256_or_si256(_mm256_shuffle_epi8(l.a0, lo), _mm256_shuffle_epi8(l.b0, hi));
  const __m256i row1 = _mm256_or_si256(_mm256_shuffle_epi8(l.a1, lo), _mm256_shuffle_epi8(l.b1, hi));
  const __m256i row = _mm256_blendv_epi8(row0, row1, l.odd);
  const __m256i bit = _mm256_shuffle_epi8(l.bits, _mm256_and_si256(s, l.low3));
  return static_cast<std::uint32_t>(
      _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(row, bit), _mm256_setzero_si256())));
}

// Passing lanes of one 16-lane block against one Q row (bit 2i per lane).
TER_ARWLF_FORCEINLINE std::uint32_t test(const Lut& l, const __m256i x, const std::uint32_t dup,
                                         const std::uint32_t valid) noexcept {
  const std::uint32_t fail = absent(_mm256_add_epi16(x, _mm256_set1_epi32(static_cast<int>(dup))), l);
  return ~(fail | (fail >> 1U)) & valid;
}

// Rows [begin, end) with constant carry: row q meets X leaf base - leaf(q).
// Branch-free and call-free: every row writes (pass << 32 | q) to `pending`
// and advances the count only when a lane passed.  Returns the count.
TER_ARWLF_FORCEINLINE std::uint32_t rows(const RowSweep& a, const Lut& l,
                                         const std::uint32_t* const dup, const std::uint32_t begin,
                                         const std::uint32_t end, const std::uint32_t base) noexcept {
  // Locals: the pending stores would otherwise force a reload of every
  // pointer member of `a` per row (MSVC assumes they may alias).
  std::uint64_t* const pending = a.pending;
  const std::uint16_t* const q_leaf = a.q_leaf;
  const LaneSlot* const x_slot = a.x_slot;
  const std::uint16_t* const x_lo = a.x_lo;
  std::uint32_t n = 0U;
  for (std::uint32_t q = begin; q < end; ++q) {
    const LaneSlot slot = x_slot[base - q_leaf[q]];
    const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x_lo + slot.offset));
    const std::uint32_t pass = test(l, x, dup[q], slot.valid);
    pending[n] = (static_cast<std::uint64_t>(pass) << 32U) | q;
    n += pass != 0U ? 1U : 0U;
  }
  return n;
}

#else

// Portable twin (same survivors, same order) for a build whose AVX2 unit is
// not compiled for AVX2; run_arwlf never selects it at runtime.
struct Lut {
  const LeafTables* t;
};

Lut make_lut(const LeafTables& t) noexcept { return Lut{&t}; }

[[nodiscard]] bool in(const LeafTables& t, const std::uint32_t chunk, const std::uint32_t b) noexcept {
  return ((t.word[chunk][b >> 5U] >> (b & 31U)) & 1U) != 0U;
}

std::uint32_t test(const Lut& l, const std::uint16_t* x, const std::uint32_t dup,
                   const std::uint32_t valid) noexcept {
  std::uint32_t pass = 0U;
  for (std::uint32_t lane = 0U; lane < kPacket; ++lane) {
    const std::uint32_t s = (x[lane] + dup) & 0xFFFFU;
    pass |= (in(*l.t, 0U, s & 0xFFU) && in(*l.t, 1U, s >> 8U) ? 1U : 0U) << (2U * lane);
  }
  return pass & valid;
}

std::uint32_t rows(const RowSweep& a, const Lut& l, const std::uint32_t* const dup,
                   const std::uint32_t begin, const std::uint32_t end, const std::uint32_t base) noexcept {
  std::uint32_t n = 0U;
  for (std::uint32_t q = begin; q < end; ++q) {
    const LaneSlot slot = a.x_slot[base - a.q_leaf[q]];
    const std::uint32_t pass = test(l, a.x_lo + slot.offset, dup[q], slot.valid);
    a.pending[n] = (static_cast<std::uint64_t>(pass) << 32U) | q;
    n += pass != 0U ? 1U : 0U;
  }
  return n;
}

#endif

// Drains pending rows of a main pass: row q's block starts at X leaf
// base - leaf(q).
void drain_rows(const RowSweep& a, const Slow& slow, const std::uint32_t n, const std::uint32_t base,
                const std::uint32_t carry) {
  for (std::uint32_t i = 0U; i < n; ++i) {
    const auto q = static_cast<std::uint32_t>(a.pending[i]);
    const auto pass = static_cast<std::uint32_t>(a.pending[i] >> 32U);
    take(slow, q, a.x_slot[base - a.q_leaf[q]].offset, pass, carry);
  }
}

}  // namespace

std::uint64_t sweep_rows(const RowSweep& a, std::vector<Survivor>& queue) {
  const Lut l = make_lut(*a.tables);
  const Slow slow{a.q_mid, a.x_mid, a.tables, a.chunks, a.mid_mask, &queue};
  // Q leaves <= k - kappa meet their X leaf without a carry, the rest with one.
  const auto split = static_cast<std::int32_t>(a.k) - static_cast<std::int32_t>(a.kappa);
  const std::uint32_t q_split = split >= 0 ? a.q_offset[split + 1] : 0U;
  if (split >= 0) {
    const auto base = static_cast<std::uint32_t>(split);
    drain_rows(a, slow, rows(a, l, a.q_dup[0], 0U, q_split, base), base, 0U);
  }
  const auto base = static_cast<std::uint32_t>(split + static_cast<std::int32_t>(kLeaves));
  drain_rows(a, slow, rows(a, l, a.q_dup[1], q_split, a.q_count, base), base, 1U);
  std::uint64_t tested = a.q_count;
  // Overflow: the 16-lane blocks past the first of X leaves with > 16 entries.
  for (std::uint32_t o = 0U; o < a.x_overflow_count; ++o) {
    const std::uint32_t j = a.x_overflow[o];
    const std::uint32_t xb = a.x_offset[j];
    const std::uint32_t nx = a.x_offset[j + 1U] - xb;
    const std::uint32_t i = (a.k - a.kappa - j) & (kLeaves - 1U);
    const std::uint32_t carry = (i + j + a.kappa) >> kLeafBits;
    const std::uint32_t* const dup = a.q_dup[carry];
    const std::uint32_t qb = a.q_offset[i];
    const std::uint32_t qe = a.q_offset[i + 1U];
    for (std::uint32_t b = kPacket; b < nx; b += kPacket) {
      const std::uint32_t valid = lane_mask(nx - b);
#if defined(__AVX2__)
      const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a.x_lo + xb + b));
#else
      const std::uint16_t* const x = a.x_lo + xb + b;
#endif
      std::uint32_t n = 0U;
      for (std::uint32_t q = qb; q < qe; ++q) {
        const std::uint32_t pass = test(l, x, dup[q], valid);
        a.pending[n] = (static_cast<std::uint64_t>(pass) << 32U) | q;
        n += pass != 0U ? 1U : 0U;
      }
      tested += qe - qb;
      for (std::uint32_t p = 0U; p < n; ++p) {
        take(slow, static_cast<std::uint32_t>(a.pending[p]), xb + b,
             static_cast<std::uint32_t>(a.pending[p] >> 32U), carry);
      }
    }
  }
  return tested;
}

bool translation_unit_is_avx2() noexcept {
#if defined(__AVX2__)
  return true;
#else
  return false;
#endif
}

}  // namespace ter::arwlf::avx2
