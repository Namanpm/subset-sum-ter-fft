// TER^2 MicroReport8 research spike: AVX2 packed leaf reporters.
//
// This translation unit, and only this translation unit, is compiled with the
// AVX2 option.  The probe, the dataset builder, the truth oracle, the portable
// fallbacks and the runtime dispatch are compiled portably, so the rest of the
// executable cannot silently acquire AVX2 instruction selection.
//
// Nothing here is reachable unless detect_isa().usable is true, which requires
// OSXSAVE, AVX, XCR0[2:1] == 11b and AVX2 -- not merely the AVX2 feature bit.
//
// Instruction set used: AVX (vbroadcastss, vmovmskps, vblendvb), AVX2
// (vpaddd/vpaddb, vpand, vpsrld/vpsrlvd/vpslld, vpshufb, vpcmpeqb/vpcmpgtd,
// vpgatherdd, vpbroadcastq, vbroadcasti128).  No AVX-512.  No BMI2.
//
// Every kernel here is a bulk lane reporter: the 64 lanes are produced eight at
// a time in vector registers, the membership decision is taken eight (K0, K1,
// K2) or thirty-two (K3) lanes per instruction, and the 64-bit mask is
// assembled from movemask results.  There is no per-lane branch and no per-lane
// scalar table load anywhere in this file.

#include "ter_microreport8_kernels.hpp"

#include <immintrin.h>

namespace ter::microreport8::avx2 {
namespace {

// The eight residual sums of row i, exact in 32-bit lanes:
//   sums[j] = (x_i + q_j + kappa) mod 2^19.
[[nodiscard]] inline __m256i row_sums(const __m256i q_vector,
                                      const std::uint32_t x_plus_kappa,
                                      const __m256i residual_mask) noexcept {
  const __m256i broadcast = _mm256_set1_epi32(static_cast<int>(x_plus_kappa));
  return _mm256_and_si256(_mm256_add_epi32(broadcast, q_vector), residual_mask);
}

// Eight membership bits -> eight mask bits, via the sign lane of movemask.
[[nodiscard]] inline std::uint64_t row_bits(const __m256i membership_bit) noexcept {
  const __m256i top = _mm256_slli_epi32(membership_bit, 31);
  return static_cast<std::uint64_t>(
      static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(top))));
}

// K0 body.  SUM-MATERIALIZATION FLOOR CONTROL, not a reporter: it materializes
// the 64 exact sums and emits a mask from a predicate true for every
// representable residual.  It measures the floor cost of this particular
// sum-generation scheme, and nothing more.
[[nodiscard]] inline std::uint64_t k0_body(const Tile& tile) noexcept {
  const __m256i q_vector = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tile.q));
  const __m256i residual_mask = _mm256_set1_epi32(static_cast<int>(kResidualMask));
  const __m256i universe = _mm256_set1_epi32(static_cast<int>(kResidualUniverse));
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    const __m256i sums = row_sums(q_vector, tile.x[i] + tile.kappa, residual_mask);
    // Always true for a 19-bit residual; the comparison keeps the sums live.
    const __m256i in_range = _mm256_cmpgt_epi32(universe, sums);
    const std::uint64_t bits = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(in_range))));
    mask |= bits << (8U * i);
  }
  return mask & tile_valid_mask(tile);
}

// K1 body.  Exact: one vpgatherdd pulls the eight 32-bit bitmap words holding
// the eight lanes' membership bits, vpsrlvd aligns each bit to lane bit 0.
[[nodiscard]] inline std::uint64_t k1_body(const Tile& tile,
                                           const std::uint32_t* const exact) noexcept {
  const __m256i q_vector = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tile.q));
  const __m256i residual_mask = _mm256_set1_epi32(static_cast<int>(kResidualMask));
  const __m256i word_mask = _mm256_set1_epi32(31);
  const auto* const base = reinterpret_cast<const int*>(exact);
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    const __m256i sums = row_sums(q_vector, tile.x[i] + tile.kappa, residual_mask);
    const __m256i words = _mm256_i32gather_epi32(base, _mm256_srli_epi32(sums, 5), 4);
    const __m256i bit = _mm256_srlv_epi32(words, _mm256_and_si256(sums, word_mask));
    mask |= row_bits(bit) << (8U * i);
  }
  return mask & tile_valid_mask(tile);
}

// K2 body.  Superset: identical shape to K1 but against the block-of-8 coarse
// bitmap, which is one eighth the size and therefore far more cache resident.
[[nodiscard]] inline std::uint64_t k2_body(const Tile& tile,
                                           const std::uint32_t* const coarse) noexcept {
  const __m256i q_vector = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tile.q));
  const __m256i residual_mask = _mm256_set1_epi32(static_cast<int>(kResidualMask));
  const __m256i word_mask = _mm256_set1_epi32(31);
  const auto* const base = reinterpret_cast<const int*>(coarse);
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    const __m256i sums = row_sums(q_vector, tile.x[i] + tile.kappa, residual_mask);
    const __m256i block = _mm256_srli_epi32(sums, static_cast<int>(kCoarseShift));
    const __m256i words = _mm256_i32gather_epi32(base, _mm256_srli_epi32(block, 5), 4);
    const __m256i bit = _mm256_srlv_epi32(words, _mm256_and_si256(block, word_mask));
    mask |= row_bits(bit) << (8U * i);
  }
  return mask & tile_valid_mask(tile);
}

// ---------------------------------------------------------------------------
// K3 support: 32 lanes per membership instruction group, zero memory traffic.
//
// Only the low byte of each residual sum is needed, and byte arithmetic carries
// nowhere, so (x_i + q_j + kappa) mod 256 is computed with vpaddb directly on
// packed bytes.  Membership in a 256-element byte set is then the standard
// two-vpshufb nibble decomposition: sig07/sig815 give, for each low nibble, the
// eight high nibbles present; a one-hot table selects the tested high nibble.
// ---------------------------------------------------------------------------

// Low bytes of eight 32-bit values, packed into the low eight bytes of an xmm.
[[nodiscard]] inline __m128i pack_low_bytes(const std::uint32_t* const values) noexcept {
  const __m256i wide = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values));
  const __m256i selector =
      _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                       0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m256i selected = _mm256_shuffle_epi8(wide, selector);
  return _mm_unpacklo_epi32(_mm256_castsi256_si128(selected),
                            _mm256_extracti128_si256(selected, 1));
}

// 0xFF in every byte whose value is present in the signature set.
[[nodiscard]] inline __m256i signature_present(const __m256i bytes,
                                               const __m256i table07,
                                               const __m256i table815,
                                               const __m256i one_hot) noexcept {
  const __m256i nibble_mask = _mm256_set1_epi8(0x0F);
  const __m256i low = _mm256_and_si256(bytes, nibble_mask);
  const __m256i high = _mm256_and_si256(_mm256_srli_epi16(bytes, 4), nibble_mask);
  const __m256i from_low = _mm256_shuffle_epi8(table07, low);
  const __m256i from_high = _mm256_shuffle_epi8(table815, low);
  const __m256i upper_half = _mm256_and_si256(high, _mm256_set1_epi8(8));
  const __m256i take_low = _mm256_cmpeq_epi8(upper_half, _mm256_setzero_si256());
  const __m256i row = _mm256_blendv_epi8(from_high, from_low, take_low);
  const __m256i bit = _mm256_shuffle_epi8(one_hot, _mm256_and_si256(high, _mm256_set1_epi8(7)));
  return _mm256_cmpeq_epi8(_mm256_and_si256(row, bit), bit);
}

[[nodiscard]] inline std::uint64_t k3_body(const Tile& tile,
                                           const std::uint8_t* const sig07,
                                           const std::uint8_t* const sig815) noexcept {
  const __m128i x_bytes = pack_low_bytes(tile.x);
  const __m128i q_bytes = pack_low_bytes(tile.q);
  // Bytes 0..31 hold q_0..q_7 four times over, matching lane index modulo 8.
  const __m256i q_lanes = _mm256_broadcastq_epi64(q_bytes);
  const __m256i x_source = _mm256_broadcastq_epi64(x_bytes);
  const __m256i rows_0_3 =
      _mm256_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
                       2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3);
  const __m256i rows_4_7 =
      _mm256_setr_epi8(4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5,
                       6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7);
  const __m256i kappa = _mm256_set1_epi8(static_cast<char>(tile.kappa));
  const __m256i low_lanes = _mm256_add_epi8(
      _mm256_add_epi8(_mm256_shuffle_epi8(x_source, rows_0_3), q_lanes), kappa);
  const __m256i high_lanes = _mm256_add_epi8(
      _mm256_add_epi8(_mm256_shuffle_epi8(x_source, rows_4_7), q_lanes), kappa);

  const __m256i table07 =
      _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sig07)));
  const __m256i table815 =
      _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sig815)));
  const __m256i one_hot =
      _mm256_setr_epi8(1, 2, 4, 8, 16, 32, 64, static_cast<char>(128), 0, 0, 0, 0, 0, 0, 0, 0,
                       1, 2, 4, 8, 16, 32, 64, static_cast<char>(128), 0, 0, 0, 0, 0, 0, 0, 0);

  const std::uint64_t low_bits = static_cast<std::uint32_t>(
      _mm256_movemask_epi8(signature_present(low_lanes, table07, table815, one_hot)));
  const std::uint64_t high_bits = static_cast<std::uint32_t>(
      _mm256_movemask_epi8(signature_present(high_lanes, table07, table815, one_hot)));
  return (low_bits | (high_bits << 32U)) & tile_valid_mask(tile);
}

}  // namespace

std::uint64_t k0_sum_floor(const Tile& tile, const TargetView&) { return k0_body(tile); }

std::uint64_t k1_exact_gather(const Tile& tile, const TargetView& view) {
  return k1_body(tile, view.exact[tile.u]);
}

std::uint64_t k3_signature_shuffle(const Tile& tile, const TargetView& view) {
  return k3_body(tile, view.sig07[tile.u], view.sig815[tile.u]);
}

std::uint64_t k2_coarse_gather(const Tile& tile, const TargetView& view) {
  return k2_body(tile, view.coarse[tile.u]);
}

std::uint64_t k0_sum_floor_batch(const Tile* const tiles, const std::size_t count,
                                 const TargetView&, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k0_body(tiles[index]));
  }
  return checksum;
}

std::uint64_t k1_exact_gather_batch(const Tile* const tiles, const std::size_t count,
                                    const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k1_body(tiles[index], view.exact[tiles[index].u]));
  }
  return checksum;
}

std::uint64_t k3_signature_shuffle_batch(const Tile* const tiles, const std::size_t count,
                                         const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k3_body(tiles[index], view.sig07[tiles[index].u],
                                      view.sig815[tiles[index].u]));
  }
  return checksum;
}

std::uint64_t k2_coarse_gather_batch(const Tile* const tiles, const std::size_t count,
                                     const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k2_body(tiles[index], view.coarse[tiles[index].u]));
  }
  return checksum;
}

bool translation_unit_is_avx2() noexcept {
#if defined(__AVX2__)
  return true;
#else
  return false;
#endif
}

}  // namespace ter::microreport8::avx2
