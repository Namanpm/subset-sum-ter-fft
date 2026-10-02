// TER^2 packed-tag consumer: deck builders and the portable reference route.
//
// Compiled portably.  The portable route uses the definitional pext/pdep, so
// it is an authority for the BMI2 route rather than a copy of it.

#include "ter_radix13_packed.hpp"

#include "ter_radix13_packed_kernel.inl"

#include <array>

namespace ter::radix13_packed {
namespace {

struct ReferenceOps {
  [[nodiscard]] static std::uint64_t pext(const std::uint64_t source,
                                          const std::uint64_t mask) noexcept {
    return pext_reference(source, mask);
  }
  [[nodiscard]] static std::uint64_t pdep(const std::uint64_t source,
                                          const std::uint64_t mask) noexcept {
    return pdep_reference(source, mask);
  }
};

// Appends the payload byte of `position` at occupancy rank `rank`; ranks 16
// and beyond are not stored (the consumer detects them from the rank mask).
inline void place(const std::uint32_t rank, const std::uint8_t byte, std::uint64_t& deck0,
                  std::uint64_t& deck1) noexcept {
  if (rank < kDeckLanes) {
    deck0 |= static_cast<std::uint64_t>(byte) << (8U * rank);
  } else if (rank < 2U * kDeckLanes) {
    deck1 |= static_cast<std::uint64_t>(byte) << (8U * (rank - kDeckLanes));
  }
}

}  // namespace

void PackedCounters::merge(const PackedCounters& other) noexcept {
  queries += other.queries;
  guard_vectors += other.guard_vectors;
  guard_zero_vectors += other.guard_zero_vectors;
  nonzero_words += other.nonzero_words;
  candidate_bits += other.candidate_bits;
  rank_0_7_bits += other.rank_0_7_bits;
  rank_8_15_bits += other.rank_8_15_bits;
  rank_16_plus_bits += other.rank_16_plus_bits;
  wide_words += other.wide_words;
  deck1_words += other.deck1_words;
  multi_tag_bits += other.multi_tag_bits;
  representation_exception_words += other.representation_exception_words;
  representation_exception_bits += other.representation_exception_bits;
  carry_seam_words += other.carry_seam_words;
  packed_exact_survivors += other.packed_exact_survivors;
  resolver_inputs += other.resolver_inputs;
  resolver_empty += other.resolver_empty;
  witnesses += other.witnesses;
  packed_scalar_disagreements += other.packed_scalar_disagreements;
}

void build_source_decks(const rx::SourceClass<kPB>& source, SourceDecks& out) noexcept {
  for (std::uint32_t word = 0U; word < G::kWords; ++word) {
    std::uint64_t deck0 = 0U;
    std::uint64_t deck1 = 0U;
    std::uint32_t rank = 0U;
    for (std::uint64_t bits = source.occupancy[word]; bits != 0U; bits &= bits - 1U) {
      const std::uint32_t position =
          word * 64U + static_cast<std::uint32_t>(std::countr_zero(bits));
      place(rank++, payload_byte(source.tags_dense[position]), deck0, deck1);
    }
    out.deck0[word] = deck0;
    out.deck1[word] = deck1;
  }
}

void build_target_decks(const rx::TargetClass<kPB>& target, const std::uint64_t* const views,
                        std::uint64_t* const deck0, std::uint64_t* const deck1) noexcept {
  std::array<std::uint8_t, G::kR> bytes;
  for (std::uint32_t position = 0U; position < G::kR; ++position) {
    bytes[position] = payload_byte(target.tags_dense[position]);
  }
  for (std::uint32_t shift = 0U; shift < G::kShifts; ++shift) {
    const std::size_t plane = static_cast<std::size_t>(shift) * G::kViewWords;
    const std::uint64_t* const view = views + plane;
    std::uint64_t* const out0 = deck0 + plane;
    std::uint64_t* const out1 = deck1 + plane;
    for (std::uint32_t word = 0U; word < G::kWords; ++word) {
      // Bit k of shifted view word `word` is target position 64*word + k + shift
      // (mod R), so its occupancy rank order is the view's own bit order.
      std::uint64_t low = 0U;
      std::uint64_t high = 0U;
      std::uint32_t rank = 0U;
      for (std::uint64_t bits = view[word]; bits != 0U; bits &= bits - 1U) {
        const std::uint32_t position =
            (word * 64U + static_cast<std::uint32_t>(std::countr_zero(bits)) + shift) &
            G::kPositionMask;
        place(rank++, bytes[position], low, high);
      }
      out0[word] = low;
      out1[word] = high;
    }
    for (std::uint32_t pad = 0U; pad < G::kSliceWords; ++pad) {
      out0[G::kWords + pad] = out0[pad];
      out1[G::kWords + pad] = out1[pad];
    }
  }
}

template <bool kTwoDecks>
std::uint32_t route_query_portable(const PackedArgs& args, std::uint32_t* const out) noexcept {
  return detail::route_scalar_guard<ReferenceOps, kTwoDecks>(args, out);
}

template <bool kTwoDecks>
std::uint32_t route_query_portable_counted(const PackedArgs& args, std::uint32_t* const out,
                                           PackedCounters& counters) noexcept {
  return detail::route_counted<ReferenceOps, kTwoDecks>(args, out, counters);
}

template std::uint32_t route_query_portable<false>(const PackedArgs&, std::uint32_t*) noexcept;
template std::uint32_t route_query_portable<true>(const PackedArgs&, std::uint32_t*) noexcept;
template std::uint32_t route_query_portable_counted<false>(const PackedArgs&, std::uint32_t*,
                                                           PackedCounters&) noexcept;
template std::uint32_t route_query_portable_counted<true>(const PackedArgs&, std::uint32_t*,
                                                          PackedCounters&) noexcept;

}  // namespace ter::radix13_packed
