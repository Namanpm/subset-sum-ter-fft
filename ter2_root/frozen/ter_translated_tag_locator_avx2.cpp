// TER^2 translated-tag locator research spike: AVX2 primary router.
//
// This translation unit, and only this translation unit, is compiled with the
// AVX2 option.  The builders, the portable router, the scalar reference, the
// benchmark driver and the runtime dispatch are compiled portably, so the rest
// of the executable cannot silently acquire AVX2 instruction selection.
//
// Nothing here is reachable unless microreport8::detect_isa().usable is true,
// which requires OSXSAVE, AVX, XCR0[2:1] == 11b and AVX2 -- not merely the AVX2
// feature bit.
//
// Instruction set used: AVX (vptest), AVX2 (vpand, vpandn, vpxor, vpor).  No
// AVX-512, no BMI2, no gathers: every load in the routing loop is a contiguous
// 256-bit load, which is the whole point of precomputing the shifted views.
//
// A 2048-position plane is exactly eight 256-bit slices.  Per query and layer
// pair the router intersects occupancies, then removes positions whose exact
// 8-bit tags differ, one tag plane at a time, checking for an empty mask
// between planes.  Surviving positions carry all 11 position bits and all 8 tag
// bits, hence the complete 19-bit residual, so every survivor is an exact
// witness rather than a candidate.

#include "ter_translated_tag_locator.hpp"

#include <bit>
#include <immintrin.h>

namespace ter::translated_tag::avx2 {
namespace {

template <bool kCount>
[[nodiscard]] std::uint32_t route_impl(const std::uint64_t* const occupancy,
                                       const std::uint64_t* const translation,
                                       const std::uint64_t* const views,
                                       const QueryPlan& plan,
                                       std::uint32_t* const out,
                                       Counters* const counters) noexcept {
  std::uint32_t hits = 0U;
  const __m256i carry_low =
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(plan.low_mask.data()));
  for (std::uint32_t y_layer = 0U; y_layer < kLayers; ++y_layer) {
    const std::uint64_t* const y_occupancy =
        views + shifted_view_index(plan.delta, y_layer, 0U) + plan.word_offset;
    const std::uint64_t* const y_tag_base =
        views + shifted_view_index(plan.delta, y_layer, 1U) + plan.word_offset;
    for (std::uint32_t x_layer = 0U; x_layer < kLayers; ++x_layer) {
      const std::uint64_t* const x_occupancy = occupancy + occupancy_index(x_layer, 0U);
      for (std::uint32_t slice = 0U; slice < kSlices; ++slice) {
        const std::size_t slice_at = static_cast<std::size_t>(slice) * kSliceWords;
        __m256i mask = _mm256_and_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x_occupancy + slice_at)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y_occupancy + slice_at)));
        if constexpr (kCount) {
          ++counters->occupancy_vectors;
        }
        if (_mm256_testz_si256(mask, mask) != 0) {
          if constexpr (kCount) {
            ++counters->occupancy_zero_exits;
          }
          continue;
        }
        const std::uint8_t mode = plan.slice_mode[slice];
        if constexpr (kCount) {
          if (mode == 2U) {
            ++counters->mixed_carry_slices;
          } else {
            ++counters->uniform_carry_slices;
          }
        }
        const std::uint64_t* const low_tags =
            translation + translation_index(plan.alpha_low, x_layer, slice, 0U);
        const std::uint64_t* const high_tags =
            translation + translation_index(plan.alpha_high, x_layer, slice, 0U);
        const std::uint64_t* const y_tag = y_tag_base + slice_at;
        bool empty = false;
        for (std::uint32_t bit = 0U; bit < kTagBits; ++bit) {
          if constexpr (kCount) {
            ++counters->tag_plane_comparisons;
          }
          const __m256i target_tag = _mm256_loadu_si256(
              reinterpret_cast<const __m256i*>(y_tag + static_cast<std::size_t>(bit) *
                                                           kViewWords));
          __m256i source_tag;
          if (mode == 0U) {
            source_tag = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                low_tags + static_cast<std::size_t>(bit) * kSliceWords));
          } else if (mode == 1U) {
            source_tag = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                high_tags + static_cast<std::size_t>(bit) * kSliceWords));
          } else {
            // Only the slice the carry threshold falls inside pays this blend.
            const __m256i below = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                low_tags + static_cast<std::size_t>(bit) * kSliceWords));
            const __m256i above = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                high_tags + static_cast<std::size_t>(bit) * kSliceWords));
            source_tag = _mm256_or_si256(_mm256_and_si256(below, carry_low),
                                         _mm256_andnot_si256(carry_low, above));
          }
          mask = _mm256_andnot_si256(_mm256_xor_si256(source_tag, target_tag), mask);
          if (_mm256_testz_si256(mask, mask) != 0) {
            if constexpr (kCount) {
              ++counters->tag_early_zero_exits;
            }
            empty = true;
            break;
          }
        }
        if (empty) {
          continue;
        }
        if constexpr (kCount) {
          ++counters->final_masks;
        }
        alignas(32) std::uint64_t words[kSliceWords];
        _mm256_store_si256(reinterpret_cast<__m256i*>(words), mask);
        for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
          std::uint64_t bits = words[word];
          while (bits != 0U) {
            const std::uint32_t offset = static_cast<std::uint32_t>(std::countr_zero(bits));
            bits &= bits - 1U;
            const std::uint32_t position = slice * kVectorBits + word * 64U + offset;
            out[hits] = pack_hit(position, x_layer, y_layer);
            ++hits;
            if constexpr (kCount) {
              ++counters->primary_group_matches;
            }
          }
        }
      }
    }
  }
  return hits;
}

}  // namespace

std::uint32_t route_query(const std::uint64_t* const occupancy,
                          const std::uint64_t* const translation,
                          const std::uint64_t* const views,
                          const QueryPlan& plan,
                          std::uint32_t* const out) noexcept {
  return route_impl<false>(occupancy, translation, views, plan, out, nullptr);
}

std::uint32_t route_query_counted(const std::uint64_t* const occupancy,
                                  const std::uint64_t* const translation,
                                  const std::uint64_t* const views,
                                  const QueryPlan& plan,
                                  std::uint32_t* const out,
                                  Counters& counters) noexcept {
  return route_impl<true>(occupancy, translation, views, plan, out, &counters);
}

bool translation_unit_is_avx2() noexcept {
#if defined(__AVX2__)
  return true;
#else
  return false;
#endif
}

}  // namespace ter::translated_tag::avx2
