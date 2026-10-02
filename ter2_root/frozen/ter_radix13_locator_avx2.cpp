// TER^2 radix occupancy locator: AVX2 kernels.
//
// This is the only translation unit of the spike compiled for AVX2.  It must
// reproduce the portable kernels' witness stream bit for bit at every radix;
// the test binary asserts that on the authentic classes and on adversarial
// synthetic ones, for every instantiated radix.

#include "ter_radix13_locator.hpp"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace ter::radix::avx2 {

#if defined(__AVX2__)

bool translation_unit_is_avx2() noexcept { return true; }

namespace {

// One 256-bit guard step: AND the source occupancy slice with the rotated
// target occupancy view.  The view carries 4 words of wrap padding so the
// unaligned load never crosses the circular seam.
template <unsigned PB>
[[nodiscard]] inline __m256i guard_slice(const RouteArgs<PB>& args,
                                         const std::uint32_t word) noexcept {
  const std::uint32_t base = (word + args.word_offset) & (Geom<PB>::kWords - 1U);
  const __m256i x =
      _mm256_load_si256(reinterpret_cast<const __m256i*>(args.x_occupancy + word));
  const __m256i y =
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(args.y_view + base));
  return _mm256_and_si256(x, y);
}

}  // namespace

template <unsigned PB>
std::uint64_t guard_only(const RouteArgs<PB>& args) noexcept {
  using G = Geom<PB>;
  std::uint64_t nonzero = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const __m256i candidates = guard_slice<PB>(args, word);
    nonzero += (_mm256_testz_si256(candidates, candidates) == 0) ? 1U : 0U;
  }
  return nonzero;
}

template <unsigned PB>
std::uint64_t extract_only(const RouteArgs<PB>& args) noexcept {
  using G = Geom<PB>;
  std::uint64_t accumulator = 0U;
  alignas(32) std::uint64_t lanes[4];
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const __m256i candidates = guard_slice<PB>(args, word);
    if (_mm256_testz_si256(candidates, candidates) != 0) {
      continue;
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), candidates);
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = lanes[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(_tzcnt_u64(bits));
        bits &= bits - 1U;
        const std::uint32_t sum = position_base + bit + args.r;
        accumulator += (sum & G::kPositionMask) + (sum >> G::kPositionBits);
      }
    }
  }
  return accumulator;
}

template <unsigned PB>
std::uint32_t route_query_compact(const RouteArgs<PB>& args,
                                  std::uint32_t* const out) noexcept {
  using G = Geom<PB>;
  std::uint32_t found = 0U;
  alignas(32) std::uint64_t lanes[4];
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const __m256i candidates = guard_slice<PB>(args, word);
    if (_mm256_testz_si256(candidates, candidates) != 0) {
      continue;
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), candidates);
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = lanes[lane];
      const std::uint32_t source_word = word + lane;
      const std::uint32_t position_base = source_word * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(_tzcnt_u64(bits));
        bits &= bits - 1U;
        const std::uint32_t position = position_base + bit;
        const std::uint32_t sum = position + args.r;
        const std::uint32_t j = sum & G::kPositionMask;
        const std::uint32_t carry = sum >> G::kPositionBits;
        const std::uint32_t xr =
            args.x_prefix[source_word] +
            static_cast<std::uint32_t>(_mm_popcnt_u64(args.x_occupancy[source_word] &
                                                      ((std::uint64_t{1} << bit) - 1U)));
        const std::uint32_t yr = args.y_bucket_rank[j];
        std::uint64_t exact =
            rotate_tags<PB>(args.x_tags[xr], (args.alpha + carry) & G::kTagMask) &
            args.y_tags[yr];
        while (exact != 0U) {
          const std::uint32_t b = static_cast<std::uint32_t>(_tzcnt_u64(exact));
          exact &= exact - 1U;
          out[found++] =
              pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
        }
      }
    }
  }
  return found;
}

template <unsigned PB>
std::uint32_t route_query_dense(const RouteArgs<PB>& args,
                                std::uint32_t* const out) noexcept {
  using G = Geom<PB>;
  std::uint32_t found = 0U;
  alignas(32) std::uint64_t lanes[4];
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const __m256i candidates = guard_slice<PB>(args, word);
    if (_mm256_testz_si256(candidates, candidates) != 0) {
      continue;
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), candidates);
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = lanes[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(_tzcnt_u64(bits));
        bits &= bits - 1U;
        const std::uint32_t position = position_base + bit;
        const std::uint32_t sum = position + args.r;
        const std::uint32_t j = sum & G::kPositionMask;
        const std::uint32_t carry = sum >> G::kPositionBits;
        std::uint64_t exact =
            rotate_tags<PB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
            args.y_tags[j];
        while (exact != 0U) {
          const std::uint32_t b = static_cast<std::uint32_t>(_tzcnt_u64(exact));
          exact &= exact - 1U;
          out[found++] =
              pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
        }
      }
    }
  }
  return found;
}

template <unsigned PB>
std::uint32_t route_query_dense_counted(const RouteArgs<PB>& args, std::uint32_t* const out,
                                        Counters& counters) noexcept {
  using G = Geom<PB>;
  std::uint32_t found = 0U;
  alignas(32) std::uint64_t lanes[4];
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const __m256i candidates = guard_slice<PB>(args, word);
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), candidates);
    ++counters.guard_vectors;
    const std::uint32_t population = static_cast<std::uint32_t>(
        _mm_popcnt_u64(lanes[0]) + _mm_popcnt_u64(lanes[1]) + _mm_popcnt_u64(lanes[2]) +
        _mm_popcnt_u64(lanes[3]));
    ++counters.guard_popcount_histogram[popcount_bucket(population)];
    if (population == 0U) {
      ++counters.guard_zero_vectors;
      continue;
    }
    counters.emitted_buckets += population;
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = lanes[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(_tzcnt_u64(bits));
        bits &= bits - 1U;
        const std::uint32_t position = position_base + bit;
        const std::uint32_t sum = position + args.r;
        const std::uint32_t j = sum & G::kPositionMask;
        const std::uint32_t carry = sum >> G::kPositionBits;
        std::uint64_t exact =
            rotate_tags<PB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
            args.y_tags[j];
        if (exact != 0U) {
          ++counters.nonempty_buckets;
        }
        while (exact != 0U) {
          const std::uint32_t b = static_cast<std::uint32_t>(_tzcnt_u64(exact));
          exact &= exact - 1U;
          ++counters.witnesses;
          out[found++] =
              pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
        }
      }
    }
  }
  return found;
}

template <unsigned PB>
std::uint32_t route_query_skip(const RouteArgs<PB>& args, std::uint32_t* const out) noexcept {
  using G = Geom<PB>;
  std::uint32_t positions[G::kExtractCapacity];
  std::uint32_t count = 0U;
  alignas(32) std::uint64_t lanes[4];
  const __m256i zero = _mm256_setzero_si256();
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const __m256i candidates = guard_slice<PB>(args, word);
    // Bit L of lane_mask is set exactly when 64-bit lane L is nonempty.
    const std::uint32_t lane_mask =
        static_cast<std::uint32_t>(~_mm256_movemask_pd(
            _mm256_castsi256_pd(_mm256_cmpeq_epi64(candidates, zero)))) &
        0xFU;
    if (lane_mask == 0U) {
      continue;
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), candidates);
    std::uint32_t remaining = lane_mask;
    do {
      const std::uint32_t lane = static_cast<std::uint32_t>(_tzcnt_u32(remaining));
      remaining &= remaining - 1U;
      std::uint64_t bits = lanes[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      do {
        const std::uint32_t bit = static_cast<std::uint32_t>(_tzcnt_u64(bits));
        bits &= bits - 1U;
        positions[count++] = position_base + bit;
      } while (bits != 0U);
    } while (remaining != 0U);
  }
  // Fixed trip count and no unpredictable back-edge: the two tag loads of one
  // iteration do not wait on the previous iteration's branch resolution.
  std::uint32_t found = 0U;
  for (std::uint32_t index = 0U; index < count; ++index) {
    const std::uint32_t position = positions[index];
    const std::uint32_t sum = position + args.r;
    const std::uint32_t j = sum & G::kPositionMask;
    const std::uint32_t carry = sum >> G::kPositionBits;
    std::uint64_t exact =
        rotate_tags<PB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
        args.y_tags[j];
    while (exact != 0U) {
      const std::uint32_t b = static_cast<std::uint32_t>(_tzcnt_u64(exact));
      exact &= exact - 1U;
      out[found++] = pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
    }
  }
  return found;
}

#else  // !__AVX2__

// The AVX2 routers are the proposed architecture.  When this translation unit
// was not compiled for AVX2 they report that fact and never silently substitute
// a portable result; the driver fails closed on it.
bool translation_unit_is_avx2() noexcept { return false; }

template <unsigned PB>
std::uint64_t guard_only(const RouteArgs<PB>& args) noexcept {
  return guard_only_portable<PB>(args);
}
template <unsigned PB>
std::uint64_t extract_only(const RouteArgs<PB>& args) noexcept {
  return extract_only_portable<PB>(args);
}
template <unsigned PB>
std::uint32_t route_query_compact(const RouteArgs<PB>& args,
                                  std::uint32_t* const out) noexcept {
  return route_query_compact_portable<PB>(args, out);
}
template <unsigned PB>
std::uint32_t route_query_dense(const RouteArgs<PB>& args,
                                std::uint32_t* const out) noexcept {
  return route_query_dense_portable<PB>(args, out);
}
template <unsigned PB>
std::uint32_t route_query_dense_counted(const RouteArgs<PB>& args, std::uint32_t* const out,
                                        Counters& counters) noexcept {
  return route_query_dense_portable_counted<PB>(args, out, counters);
}
template <unsigned PB>
std::uint32_t route_query_skip(const RouteArgs<PB>& args, std::uint32_t* const out) noexcept {
  return route_query_batched_portable<PB>(args, out);
}

#endif

#define TER_RADIX_AVX2_INSTANTIATE(PB)                                                  \
  template std::uint64_t guard_only<PB>(const RouteArgs<PB>&) noexcept;                  \
  template std::uint64_t extract_only<PB>(const RouteArgs<PB>&) noexcept;                \
  template std::uint32_t route_query_compact<PB>(const RouteArgs<PB>&,                  \
                                                 std::uint32_t*) noexcept;              \
  template std::uint32_t route_query_dense<PB>(const RouteArgs<PB>&,                    \
                                               std::uint32_t*) noexcept;                \
  template std::uint32_t route_query_dense_counted<PB>(const RouteArgs<PB>&,            \
                                                       std::uint32_t*, Counters&) noexcept; \
  template std::uint32_t route_query_skip<PB>(const RouteArgs<PB>&, std::uint32_t*) noexcept;

TER_RADIX_AVX2_INSTANTIATE(13)
TER_RADIX_AVX2_INSTANTIATE(14)
TER_RADIX_AVX2_INSTANTIATE(15)
TER_RADIX_AVX2_INSTANTIATE(16)

#undef TER_RADIX_AVX2_INSTANTIATE

}  // namespace ter::radix::avx2
