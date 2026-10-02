// TER^2 packed-tag consumer: AVX2 guard + BMI2 PEXT/PDEP route.
//
// The only translation unit of the packed consumer compiled for AVX2/BMI2.  The
// guard scan is the plain radix-13 lane-skip scan (256-bit AND, movemask of the
// nonempty 64-bit lanes); the consumer and resolver are the shared templates of
// ter_radix13_packed_kernel.inl instantiated with the hardware instructions.

#include "ter_radix13_packed.hpp"

#if defined(__AVX2__) && (defined(_MSC_VER) || defined(__BMI2__))
#define TER_PACKED_HAS_BMI2 1
#include <immintrin.h>
#endif

#include "ter_radix13_packed_kernel.inl"

#include <cstring>

namespace ter::radix13_packed::bmi2 {

#if defined(TER_PACKED_HAS_BMI2)

bool translation_unit_is_bmi2() noexcept { return true; }

namespace {

struct Bmi2Ops {
  [[nodiscard]] static std::uint64_t pext(const std::uint64_t source,
                                          const std::uint64_t mask) noexcept {
    return _pext_u64(source, mask);
  }
  [[nodiscard]] static std::uint64_t pdep(const std::uint64_t source,
                                          const std::uint64_t mask) noexcept {
    return _pdep_u64(source, mask);
  }
};

enum class Mode : std::uint8_t { Resolve, FrontOnly, GuardOnly };

template <bool kTwoDecks, Mode kMode>
[[nodiscard]] inline std::uint64_t scan(const PackedArgs& args, std::uint32_t* const out,
                                        std::uint32_t& found) noexcept {
  const detail::CarryPlan plan = detail::make_carry_plan(args);
  const __m256i zero = _mm256_setzero_si256();
  alignas(32) std::uint64_t lanes[4];
  std::uint64_t checksum = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    // The view and its decks carry 4 words of wrap padding, so base + lane
    // (at most kWords + 3) never crosses the circular seam.
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    const __m256i x =
        _mm256_load_si256(reinterpret_cast<const __m256i*>(args.x_occupancy + word));
    const __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(args.y_view + base));
    const __m256i candidates = _mm256_and_si256(x, y);
    std::uint32_t lane_mask =
        static_cast<std::uint32_t>(~_mm256_movemask_pd(
            _mm256_castsi256_pd(_mm256_cmpeq_epi64(candidates, zero)))) &
        0xFU;
    if (lane_mask == 0U) {
      continue;
    }
    if constexpr (kMode == Mode::GuardOnly) {
      checksum += lane_mask;
    } else {
      _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), candidates);
      do {
        const std::uint32_t lane = static_cast<std::uint32_t>(_tzcnt_u32(lane_mask));
        lane_mask &= lane_mask - 1U;
        const std::uint32_t source_word = word + lane;
        const std::uint64_t resolve = detail::consume_word<Bmi2Ops, kTwoDecks>(
            args, plan, lanes[lane], source_word, base + lane);
        if constexpr (kMode == Mode::Resolve) {
          found = detail::resolve_word(args, resolve, source_word, out, found);
        } else {
          checksum += resolve ^ source_word;
        }
      } while (lane_mask != 0U);
    }
  }
  return checksum;
}

// Branch-free compaction of one 256-bit guard vector.  For a 4-bit mask of
// nonempty 64-bit lanes, `permute` moves those lanes to the front in order
// (as 32-bit index pairs for vpermd) and `lane_bytes` packs their source lane
// numbers, one per byte, in the same order.
struct CompactTables {
  alignas(32) std::uint32_t permute[16][8];
  std::uint32_t lane_bytes[16];
};

[[nodiscard]] constexpr CompactTables make_compact_tables() noexcept {
  CompactTables tables{};
  for (std::uint32_t mask = 0U; mask < 16U; ++mask) {
    std::uint32_t out = 0U;
    for (std::uint32_t lane = 0U; lane < 4U; ++lane) {
      if (((mask >> lane) & 1U) != 0U) {
        tables.permute[mask][2U * out] = 2U * lane;
        tables.permute[mask][2U * out + 1U] = 2U * lane + 1U;
        tables.lane_bytes[mask] |= lane << (8U * out);
        ++out;
      }
    }
  }
  return tables;
}

alignas(32) constexpr CompactTables kCompact = make_compact_tables();

enum class Stages : std::uint8_t { GuardQueue, ConsumeQueue, Resolve, DenseControl };

template <bool kTwoDecks, Stages kStages>
[[nodiscard]] inline std::uint64_t queued(const PackedArgs& args, std::uint32_t* const out,
                                          std::uint32_t& found) noexcept {
  // Stage 1: nonzero guard words, in word order.  Every iteration stores four
  // lanes at the queue tail and advances it by the number of nonempty lanes,
  // so at iteration i the stores reach at most index 4i + 3 < kWords.
  alignas(32) std::uint64_t words[G::kWords];
  std::uint8_t word_index[G::kWords];
  std::uint32_t count = 0U;
  const __m256i zero = _mm256_setzero_si256();
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    const __m256i x =
        _mm256_load_si256(reinterpret_cast<const __m256i*>(args.x_occupancy + word));
    const __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(args.y_view + base));
    const __m256i candidates = _mm256_and_si256(x, y);
    const std::uint32_t lane_mask =
        static_cast<std::uint32_t>(~_mm256_movemask_pd(
            _mm256_castsi256_pd(_mm256_cmpeq_epi64(candidates, zero)))) &
        0xFU;
    const __m256i compacted = _mm256_permutevar8x32_epi32(
        candidates,
        _mm256_load_si256(reinterpret_cast<const __m256i*>(kCompact.permute[lane_mask])));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(words + count), compacted);
    const std::uint32_t lanes = kCompact.lane_bytes[lane_mask] + word * 0x01010101U;
    std::memcpy(word_index + count, &lanes, sizeof(lanes));
    count += static_cast<std::uint32_t>(_mm_popcnt_u32(lane_mask));
  }
  if constexpr (kStages == Stages::GuardQueue) {
    return count ^ words[0];
  } else if constexpr (kStages == Stages::DenseControl) {
    // Ablation: the same queue, then the plain radix-13 exact dense leaf on
    // every candidate bit -- no PEXT, no PDEP, no payload deck.
    for (std::uint32_t index = 0U; index < count; ++index) {
      found = detail::resolve_word(args, words[index], word_index[index], out, found);
    }
    return 0U;
  } else {
    // Stage 2: fixed trip count; resolve masks are queued branch-free.
    const detail::CarryPlan plan = detail::make_carry_plan(args);
    std::uint64_t resolve_masks[G::kWords];
    std::uint8_t resolve_words[G::kWords];
    std::uint32_t pending = 0U;
    for (std::uint32_t index = 0U; index < count; ++index) {
      const std::uint32_t word = word_index[index];
      const std::uint64_t resolve = detail::consume_word<Bmi2Ops, kTwoDecks>(
          args, plan, words[index], word, (word + args.word_offset) & (G::kWords - 1U));
      resolve_masks[pending] = resolve;
      resolve_words[pending] = static_cast<std::uint8_t>(word);
      pending += static_cast<std::uint32_t>(resolve != 0U);
    }
    if constexpr (kStages == Stages::ConsumeQueue) {
      std::uint64_t checksum = pending;
      for (std::uint32_t index = 0U; index < pending; ++index) {
        checksum += resolve_masks[index] ^ resolve_words[index];
      }
      return checksum;
    } else {
      // Stage 3: exact resolution of the survivors and exceptions only.
      for (std::uint32_t index = 0U; index < pending; ++index) {
        found = detail::resolve_word(args, resolve_masks[index], resolve_words[index], out,
                                     found);
      }
      return 0U;
    }
  }
}

}  // namespace

template <bool kTwoDecks>
std::uint32_t route_query_queued(const PackedArgs& args, std::uint32_t* const out) noexcept {
  std::uint32_t found = 0U;
  static_cast<void>(queued<kTwoDecks, Stages::Resolve>(args, out, found));
  return found;
}

std::uint64_t queued_front_only(const PackedArgs& args) noexcept {
  std::uint32_t unused = 0U;
  return queued<true, Stages::ConsumeQueue>(args, nullptr, unused);
}

std::uint32_t route_query_queued_dense_control(const PackedArgs& args,
                                               std::uint32_t* const out) noexcept {
  std::uint32_t found = 0U;
  static_cast<void>(queued<true, Stages::DenseControl>(args, out, found));
  return found;
}

std::uint64_t queued_guard_only(const PackedArgs& args) noexcept {
  std::uint32_t unused = 0U;
  return queued<true, Stages::GuardQueue>(args, nullptr, unused);
}

template <bool kTwoDecks>
std::uint32_t route_query(const PackedArgs& args, std::uint32_t* const out) noexcept {
  std::uint32_t found = 0U;
  static_cast<void>(scan<kTwoDecks, Mode::Resolve>(args, out, found));
  return found;
}

std::uint64_t front_only(const PackedArgs& args) noexcept {
  std::uint32_t unused = 0U;
  return scan<true, Mode::FrontOnly>(args, nullptr, unused);
}

std::uint64_t guard_only(const PackedArgs& args) noexcept {
  std::uint32_t unused = 0U;
  return scan<true, Mode::GuardOnly>(args, nullptr, unused);
}

#else  // no AVX2/BMI2 compilation

// Reports the fact and never substitutes a portable result; the driver fails
// closed on translation_unit_is_bmi2() == false.
bool translation_unit_is_bmi2() noexcept { return false; }

template <bool kTwoDecks>
std::uint32_t route_query(const PackedArgs& args, std::uint32_t* const out) noexcept {
  return route_query_portable<kTwoDecks>(args, out);
}

std::uint64_t front_only(const PackedArgs&) noexcept { return 0U; }
std::uint64_t guard_only(const PackedArgs&) noexcept { return 0U; }

template <bool kTwoDecks>
std::uint32_t route_query_queued(const PackedArgs& args, std::uint32_t* const out) noexcept {
  return route_query_portable<kTwoDecks>(args, out);
}
std::uint64_t queued_front_only(const PackedArgs&) noexcept { return 0U; }
std::uint64_t queued_guard_only(const PackedArgs&) noexcept { return 0U; }
std::uint32_t route_query_queued_dense_control(const PackedArgs& args,
                                               std::uint32_t* const out) noexcept {
  return route_query_portable<true>(args, out);
}

#endif

template std::uint32_t route_query<false>(const PackedArgs&, std::uint32_t*) noexcept;
template std::uint32_t route_query<true>(const PackedArgs&, std::uint32_t*) noexcept;
template std::uint32_t route_query_queued<false>(const PackedArgs&, std::uint32_t*) noexcept;
template std::uint32_t route_query_queued<true>(const PackedArgs&, std::uint32_t*) noexcept;

}  // namespace ter::radix13_packed::bmi2
