// TER^2 packed-tag consumer: the word consumer and exact resolver, shared by
// the portable translation unit and the BMI2 translation unit.
//
// Every function here is a template over an Ops type that supplies pext and
// pdep.  Each translation unit defines its own Ops in an anonymous namespace,
// so each instantiation has internal linkage and the BMI2 code can never be
// selected by the linker for a portable caller (or the reverse).  The consumer
// logic itself exists exactly once.

#include "ter_radix13_packed.hpp"

#include <bit>
#include <cstdint>

namespace ter::radix13_packed::detail {

// Carry geometry of one query.  c_i = [i >= threshold] with threshold = R - r.
// Words entirely below the seam word carry 0, words above it carry 1, and the
// one seam word (if any) carries 1 exactly on the bits of seam_mask.  r = 0
// gives threshold = R, i.e. seam_word = kWords, so no word carries.
struct CarryPlan {
  std::uint32_t seam_word = 0;
  std::uint64_t seam_mask = 0;
  std::uint64_t alpha_bytes = 0;
};

[[nodiscard]] inline CarryPlan make_carry_plan(const PackedArgs& args) noexcept {
  CarryPlan plan;
  const std::uint32_t threshold = G::kR - args.r;  // 1 .. R
  plan.seam_word = threshold >> 6U;
  plan.seam_mask = ~std::uint64_t{0} << (threshold & 63U);
  plan.alpha_bytes = static_cast<std::uint64_t>(args.alpha) * kOnes;
  return plan;
}

// Physical carry bits of source word `word`.  Used by the instrumented path
// and the tests; the timed consumer folds the same rule into `add` directly.
[[nodiscard]] inline std::uint64_t carry_bits(const CarryPlan& plan,
                                              const std::uint32_t word) noexcept {
  if (word < plan.seam_word) {
    return 0U;
  }
  return word == plan.seam_word ? plan.seam_mask : ~std::uint64_t{0};
}

// Two-deck payload extraction for ranks 0..15.  The caller guarantees at most
// 8 candidate lanes in total, so when deck 0 contributes 8 lanes deck 1
// contributes none and the (masked) shift is irrelevant.
template <class Ops>
[[nodiscard]] inline std::uint64_t extract_two_decks(const std::uint64_t deck0,
                                                     const std::uint64_t deck1,
                                                     const std::uint64_t ranks) noexcept {
  const std::uint64_t low = ranks & 0xFFU;
  const std::uint64_t high = (ranks >> 8U) & 0xFFU;
  const std::uint64_t from0 = Ops::pext(deck0, kExpand[low]);
  const std::uint64_t from1 = Ops::pext(deck1, kExpand[high]);
  const std::uint32_t shift = (8U * static_cast<std::uint32_t>(std::popcount(low))) & 63U;
  return from0 | (from1 << shift);
}

// Consumes one nonzero guard word M of source word `word` / target view word
// `view_word`, and returns the physical positions of M that must reach the
// exact resolver: exact packed survivors, multi-tag lanes, and -- whole word,
// with no packed rejection -- any word the packed representation cannot hold.
template <class Ops, bool kTwoDecks>
[[nodiscard]] inline std::uint64_t consume_word(const PackedArgs& args, const CarryPlan& plan,
                                                const std::uint64_t m,
                                                const std::uint32_t word,
                                                const std::uint32_t view_word) noexcept {
  const std::uint64_t source_occupancy = args.x_occupancy[word];
  const std::uint64_t target_occupancy = args.y_view[view_word];
  const std::uint64_t source_ranks = Ops::pext(m, source_occupancy);
  const std::uint64_t target_ranks = Ops::pext(m, target_occupancy);

  std::uint64_t source_tags = 0U;
  std::uint64_t target_tags = 0U;
  if (((source_ranks | target_ranks) >> 8U) == 0U) {
    // Every selected rank is in deck 0.  popcount(M) = popcount(source_ranks)
    // <= 8, so all candidate lanes fit one packed register.
    source_tags = Ops::pext(args.x_deck0[word], kExpand[source_ranks]);
    target_tags = Ops::pext(args.y_deck0[view_word], kExpand[target_ranks]);
  } else {
    if constexpr (!kTwoDecks) {
      return m;  // rank >= 8 without a second deck: whole-word fallback
    } else {
      if (((source_ranks | target_ranks) >> 16U) != 0U || std::popcount(m) > 8) {
        return m;  // rank >= 16, or more lanes than one register: fallback
      }
      source_tags = extract_two_decks<Ops>(args.x_deck0[word], args.x_deck1[word], source_ranks);
      target_tags =
          extract_two_decks<Ops>(args.y_deck0[view_word], args.y_deck1[view_word], target_ranks);
    }
  }

  // alpha in every lane, plus 1 in the lanes whose low position carries.  The
  // above-seam case is branch-free; only the single seam word branches.
  std::uint64_t add =
      plan.alpha_bytes +
      (kOnes & (std::uint64_t{0} - static_cast<std::uint64_t>(word > plan.seam_word)));
  if (word == plan.seam_word) {
    add += kExpand[Ops::pext(plan.seam_mask, m)] & kOnes;
  }

  const std::uint64_t keep = packed_keep_bit6(source_tags, target_tags, add);
  // Lanes beyond popcount(M) are zero bytes that may compare equal; PDEP only
  // deposits the first popcount(M) lane bits, so they can never be emitted.
  return Ops::pdep(Ops::pext(keep, kBit6), m);
}

// The exact resolver: the plain radix-13 dense leaf, applied only to `mask`.
[[nodiscard]] inline std::uint32_t resolve_word(const PackedArgs& args, std::uint64_t mask,
                                                const std::uint32_t word, std::uint32_t* const out,
                                                std::uint32_t found) noexcept {
  const std::uint32_t position_base = word * 64U;
  while (mask != 0U) {
    const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(mask));
    mask &= mask - 1U;
    const std::uint32_t position = position_base + bit;
    const std::uint32_t sum = position + args.r;
    const std::uint32_t j = sum & G::kPositionMask;
    const std::uint32_t carry = sum >> G::kPositionBits;
    std::uint64_t exact =
        rx::rotate_tags<kPB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
        args.y_tags[j];
    while (exact != 0U) {
      const std::uint32_t b = static_cast<std::uint32_t>(std::countr_zero(exact));
      exact &= exact - 1U;
      out[found++] = rx::pack_hit<kPB>(position, (b - args.alpha - carry) & G::kTagMask, b);
    }
  }
  return found;
}

// Scalar-guard route over single 64-bit words.  The BMI2 route replaces only
// the guard scan (AVX2 slices plus lane skip); the consumer and resolver are
// these same templates.
template <class Ops, bool kTwoDecks>
[[nodiscard]] inline std::uint32_t route_scalar_guard(const PackedArgs& args,
                                                      std::uint32_t* const out) noexcept {
  const CarryPlan plan = make_carry_plan(args);
  std::uint32_t found = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; ++word) {
    const std::uint32_t view_word = (word + args.word_offset) & (G::kWords - 1U);
    const std::uint64_t m = args.x_occupancy[word] & args.y_view[view_word];
    if (m == 0U) {
      continue;
    }
    found = resolve_word(args, consume_word<Ops, kTwoDecks>(args, plan, m, word, view_word),
                         word, out, found);
  }
  return found;
}

// Instrumented route.  The resolve mask comes from the very same consume_word;
// the counters are derived independently, per candidate bit, from the dense
// exact tag words, and every non-exception word's resolve mask is checked
// against that scalar evaluation (disagreements must stay zero).
template <class Ops, bool kTwoDecks>
[[nodiscard]] inline std::uint32_t route_counted(const PackedArgs& args, std::uint32_t* const out,
                                                 PackedCounters& counters) noexcept {
  const CarryPlan plan = make_carry_plan(args);
  ++counters.queries;
  std::uint32_t found = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; ++word) {
    if ((word & 3U) == 0U) {
      ++counters.guard_vectors;
      std::uint64_t any = 0U;
      for (std::uint32_t lane = 0U; lane < 4U; ++lane) {
        const std::uint32_t view = (word + lane + args.word_offset) & (G::kWords - 1U);
        any |= args.x_occupancy[word + lane] & args.y_view[view];
      }
      if (any == 0U) {
        ++counters.guard_zero_vectors;
      }
    }
    const std::uint32_t view_word = (word + args.word_offset) & (G::kWords - 1U);
    const std::uint64_t source_occupancy = args.x_occupancy[word];
    const std::uint64_t target_occupancy = args.y_view[view_word];
    const std::uint64_t m = source_occupancy & target_occupancy;
    if (m == 0U) {
      continue;
    }
    const std::uint32_t lanes = static_cast<std::uint32_t>(std::popcount(m));
    ++counters.nonzero_words;
    counters.candidate_bits += lanes;
    if (lanes > 8U) {
      ++counters.wide_words;
    }
    const std::uint64_t carry = carry_bits(plan, word);
    if (carry != 0U && carry != ~std::uint64_t{0}) {
      ++counters.carry_seam_words;
    }

    // Scalar, per-bit truth for this word.
    std::uint64_t scalar_keep = 0U;
    std::uint32_t max_rank = 0U;
    for (std::uint64_t bits = m; bits != 0U; bits &= bits - 1U) {
      const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
      const std::uint64_t below = (std::uint64_t{1} << bit) - 1U;
      const std::uint32_t source_rank =
          static_cast<std::uint32_t>(std::popcount(source_occupancy & below));
      const std::uint32_t target_rank =
          static_cast<std::uint32_t>(std::popcount(target_occupancy & below));
      const std::uint32_t rank = source_rank > target_rank ? source_rank : target_rank;
      max_rank = rank > max_rank ? rank : max_rank;
      if (rank < 8U) {
        ++counters.rank_0_7_bits;
      } else if (rank < 16U) {
        ++counters.rank_8_15_bits;
      } else {
        ++counters.rank_16_plus_bits;
      }
      const std::uint32_t position = word * 64U + bit;
      const std::uint32_t sum = position + args.r;
      const std::uint32_t c = sum >> G::kPositionBits;
      const std::uint8_t a = payload_byte(args.x_tags[position]);
      const std::uint8_t b = payload_byte(args.y_tags[sum & G::kPositionMask]);
      if (((a | b) & kMultiTagFlag) != 0U) {
        ++counters.multi_tag_bits;
        scalar_keep |= std::uint64_t{1} << bit;
      } else if (((static_cast<std::uint32_t>(a) + args.alpha + c) & G::kTagMask) == b) {
        ++counters.packed_exact_survivors;
        scalar_keep |= std::uint64_t{1} << bit;
      }
    }

    const bool representable =
        max_rank < (kTwoDecks ? 16U : 8U) && (lanes <= 8U);
    if (max_rank >= 8U && representable) {
      ++counters.deck1_words;
    }
    const std::uint64_t resolve = consume_word<Ops, kTwoDecks>(args, plan, m, word, view_word);
    if (!representable) {
      ++counters.representation_exception_words;
      counters.representation_exception_bits += lanes;
      if (resolve != m) {
        ++counters.packed_scalar_disagreements;  // must fall back whole
      }
    } else if (resolve != scalar_keep) {
      ++counters.packed_scalar_disagreements;
    }

    counters.resolver_inputs += static_cast<std::uint64_t>(std::popcount(resolve));
    for (std::uint64_t bits = resolve; bits != 0U; bits &= bits - 1U) {
      const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
      const std::uint32_t before = found;
      found = resolve_word(args, std::uint64_t{1} << bit, word, out, found);
      if (found == before) {
        ++counters.resolver_empty;
      }
      counters.witnesses += found - before;
    }
  }
  return found;
}

}  // namespace ter::radix13_packed::detail
