#pragma once

// TER^2 Radix-13 occupancy guard + packed-tag mask consumer (research spike).
//
// Specification: "the holy markdown files/TER_Radix13_Packed_Tag_Mask_Consumer_
// Architecture.pdf" (23 September 2026).
//
// SCOPE.  Benchmark/test surface only.  Nothing here is linked into ter_t0 or
// into any production reporter.  It reuses the radix-13 builders, shifted
// occupancy views and exact dense tag words unmodified, and adds only the
// occupancy-rank payload decks and the packed consumer.
//
// PROBLEM.  x + q + kappa == y (mod 2^19) with v = i + 8192*a, i the 13-bit low
// position and a the exact 6-bit high tag.  For d = r + 8192*alpha:
//
//     j = (i + r) mod 8192,   c_i = [i + r >= 8192],   b = (a + alpha + c_i) mod 64.
//
// ORDER REVERSAL.  Plain radix-13 turns every guard bit into a position, then
// loads two tag words.  Here the guard word M = O_X & O_Y(shifted) is consumed
// whole:
//
//     p_X = PEXT(M, O_X)              physical candidate bits -> source ranks
//     p_Y = PEXT(M, O_Y)              physical candidate bits -> target ranks
//     v_X = PEXT(deck_X, E[p_X])      source tag bytes, in candidate order
//     v_Y = PEXT(deck_Y, E[p_Y])      target tag bytes, in candidate order
//     keep = lanes with ((a + alpha + c) & 63) == b, or an exception flag
//     R   = PDEP(PEXT(keep, 0x40..), M)   physical positions still possible
//
// and only R reaches the exact resolver.  Because M is a subset of both O_X and
// O_Y, the k-th set bit of M has the same candidate lane k in v_X and v_Y.
//
// PAYLOAD BYTE.  bits 0-5 the exact tag when the low bucket holds exactly one
// distinct tag; bit 7 set when it holds more than one, in which case the tag
// bits are meaningless and the lane is always kept.  Bit 6 is always zero.
// Duplicate postings of one residual value are one tag and never set bit 7.
//
// FAIL-CLOSED.  A word is handed to the exact resolver whole, with no packed
// rejection at all, when any of its candidate ranks is beyond the implemented
// deck width, or when it holds more candidate lanes than one 8-byte packed
// register.  An exception can only cost time; it can never remove a candidate.
//
// RESOLVER.  Every surviving or exceptional position goes through exactly the
// dense radix-13 leaf: rot_64(T_X[i], alpha + c_i) AND T_Y[j].  It emits the
// same witnesses, in the same order, as the plain radix-13 dense kernel.

#include "ter_radix13_locator.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace ter::radix13_packed {

namespace rx = ter::radix;
using G = rx::Geom<13>;

inline constexpr unsigned kPB = 13U;

// Ranks stored per deck word, and deck words per 64-position word.
inline constexpr std::uint32_t kDeckLanes = 8U;
inline constexpr std::uint32_t kMaxDecks = 2U;

inline constexpr std::uint8_t kTagBits6 = 0x3FU;
inline constexpr std::uint8_t kMultiTagFlag = 0x80U;

inline constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
inline constexpr std::uint64_t kLow6 = 0x3F3F3F3F3F3F3F3FULL;
inline constexpr std::uint64_t kBit6 = 0x4040404040404040ULL;
inline constexpr std::uint64_t kBit7 = 0x8080808080808080ULL;

// Target decks are laid out exactly like the shifted occupancy views:
// [shift][kViewWords], with the same 4-word wrap padding, so the deck word for
// view word v is found at the same index as the occupancy word it describes.
inline constexpr std::size_t kTargetDeckWordsPerClass = G::kViewWordsPerClass;

static_assert(G::kR == 8192U && G::kH == 64U, "the packed consumer is specified at R = 8192");
static_assert(G::kTagMask == kTagBits6, "a 6-bit tag must fill the payload tag field");

// ---------------------------------------------------------------------------
// Definitional bit-selection primitives.  They walk the mask bit by bit and
// share nothing with the BMI2 instructions, so the portable consumer and the
// unit tests are an independent authority for the intrinsic path.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr std::uint64_t pext_reference(const std::uint64_t source,
                                                     std::uint64_t mask) noexcept {
  std::uint64_t result = 0U;
  std::uint32_t out = 0U;
  while (mask != 0U) {
    const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(mask));
    mask &= mask - 1U;
    result |= ((source >> bit) & 1U) << out;
    ++out;
  }
  return result;
}

[[nodiscard]] constexpr std::uint64_t pdep_reference(const std::uint64_t source,
                                                     std::uint64_t mask) noexcept {
  std::uint64_t result = 0U;
  std::uint32_t in = 0U;
  while (mask != 0U) {
    const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(mask));
    mask &= mask - 1U;
    result |= ((source >> in) & 1U) << bit;
    ++in;
  }
  return result;
}

// E[p]: byte lane k is 0xFF exactly when bit k of p is set.  2 KiB.
[[nodiscard]] constexpr std::array<std::uint64_t, 256> make_expansion_table() noexcept {
  std::array<std::uint64_t, 256> table{};
  for (std::uint32_t mask = 0U; mask < 256U; ++mask) {
    std::uint64_t word = 0U;
    for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
      if (((mask >> lane) & 1U) != 0U) {
        word |= std::uint64_t{0xFF} << (8U * lane);
      }
    }
    table[mask] = word;
  }
  return table;
}

alignas(64) inline constexpr std::array<std::uint64_t, 256> kExpand = make_expansion_table();

// Packed exact 6-bit relation.  `add` holds alpha + c_k in byte lane k (at most
// 64).  Returns bit 6 of lane k set exactly when the lane must be kept: its
// exact tags satisfy b == (a + alpha + c) mod 64, or either endpoint is a
// multi-tag bucket.  No byte can carry into its neighbour: (a & 63) + 64 <= 127
// and a masked difference is at most 63, so (difference + 63) <= 126.
[[nodiscard]] constexpr std::uint64_t packed_keep_bit6(const std::uint64_t source_tags,
                                                       const std::uint64_t target_tags,
                                                       const std::uint64_t add) noexcept {
  const std::uint64_t expected = ((source_tags & kLow6) + add) & kLow6;
  const std::uint64_t difference = expected ^ (target_tags & kLow6);
  const std::uint64_t nonzero = (difference + kLow6) & kBit6;
  const std::uint64_t match = ~nonzero & kBit6;
  const std::uint64_t flagged = ((source_tags | target_tags) & kBit7) >> 1U;
  return match | flagged;
}

// Payload byte for one dense exact tag word.
[[nodiscard]] constexpr std::uint8_t payload_byte(const std::uint64_t tag_word) noexcept {
  if (tag_word == 0U) {
    return 0U;
  }
  if ((tag_word & (tag_word - 1U)) != 0U) {
    return kMultiTagFlag;
  }
  return static_cast<std::uint8_t>(std::countr_zero(tag_word));
}

// ---------------------------------------------------------------------------
// Decks.  deck0 holds ranks 0-7 of a 64-position word, deck1 ranks 8-15; ranks
// 16 and beyond are not stored and are detected at query time.
// ---------------------------------------------------------------------------
struct SourceDecks {
  std::array<std::uint64_t, G::kWords> deck0{};
  std::array<std::uint64_t, G::kWords> deck1{};
};

void build_source_decks(const rx::SourceClass<kPB>& source, SourceDecks& out) noexcept;

// views: the class's [shift][kViewWords] shifted occupancy views, as produced
// by rx::build_shifted_views<13>.  deck0 / deck1: same shape.
void build_target_decks(const rx::TargetClass<kPB>& target, const std::uint64_t* views,
                        std::uint64_t* deck0, std::uint64_t* deck1) noexcept;

// ---------------------------------------------------------------------------
// Query arguments.  All pointers are fixed by (class, shift) before the query
// runs; nothing is addressed through an individually decoded candidate bit
// until the resolver.
// ---------------------------------------------------------------------------
struct PackedArgs {
  const std::uint64_t* x_occupancy = nullptr;  // kWords, 32-byte aligned
  const std::uint64_t* x_deck0 = nullptr;      // kWords
  const std::uint64_t* x_deck1 = nullptr;      // kWords
  const std::uint64_t* x_tags = nullptr;       // R dense exact tag words
  const std::uint64_t* y_view = nullptr;       // kViewWords, shift-selected
  const std::uint64_t* y_deck0 = nullptr;      // kViewWords, shift-selected
  const std::uint64_t* y_deck1 = nullptr;      // kViewWords, shift-selected
  const std::uint64_t* y_tags = nullptr;       // R dense exact tag words
  std::uint32_t r = 0;
  std::uint32_t alpha = 0;
  std::uint32_t word_offset = 0;
};

// Every counter the specification's section 36 asks for.  Bits are low-bucket
// candidate interactions, i.e. set bits of guard words.
struct PackedCounters {
  std::uint64_t queries = 0;
  std::uint64_t guard_vectors = 0;
  std::uint64_t guard_zero_vectors = 0;
  std::uint64_t nonzero_words = 0;           // nonzero 64-bit guard words
  std::uint64_t candidate_bits = 0;          // total low-bucket interactions
  std::uint64_t rank_0_7_bits = 0;           // max(source, target) rank bucket
  std::uint64_t rank_8_15_bits = 0;
  std::uint64_t rank_16_plus_bits = 0;
  std::uint64_t wide_words = 0;              // more than 8 candidate lanes
  std::uint64_t deck1_words = 0;             // nonzero words that needed deck 1
  std::uint64_t multi_tag_bits = 0;          // either endpoint has >1 distinct tag
  std::uint64_t representation_exception_words = 0;
  std::uint64_t representation_exception_bits = 0;  // whole-word fallback bits
  std::uint64_t carry_seam_words = 0;        // nonzero words split by the carry seam
  std::uint64_t packed_exact_survivors = 0;  // lanes kept by the exact tag test
  std::uint64_t resolver_inputs = 0;         // positions sent to the exact resolver
  std::uint64_t resolver_empty = 0;          // resolver inputs yielding no witness
  std::uint64_t witnesses = 0;
  // Non-exception words whose packed resolve mask differs from the per-bit
  // scalar evaluation of the same exact tags.  Must be zero.
  std::uint64_t packed_scalar_disagreements = 0;

  void merge(const PackedCounters& other) noexcept;
};

// Portable consumer: scalar guard and the definitional pext/pdep above.  It is
// the semantic reference for the intrinsic path, not a timing candidate.
// kTwoDecks selects whether ranks 8-15 are consumed in packed form.
template <bool kTwoDecks>
[[nodiscard]] std::uint32_t route_query_portable(const PackedArgs& args,
                                                 std::uint32_t* out) noexcept;

// Portable instrumented consumer.  Same witnesses as the uncounted one.
template <bool kTwoDecks>
[[nodiscard]] std::uint32_t route_query_portable_counted(const PackedArgs& args,
                                                         std::uint32_t* out,
                                                         PackedCounters& counters) noexcept;

namespace bmi2 {

// AVX2 guard + BMI2 PEXT/PDEP consumer + exact dense resolver.
template <bool kTwoDecks>
[[nodiscard]] std::uint32_t route_query(const PackedArgs& args, std::uint32_t* out) noexcept;

// Guard and packed consumer only: the resolver is replaced by folding each
// word's resolve mask into a checksum.  Differencing against the guard-only
// scan attributes the packed consumer's cost on authentic words.
[[nodiscard]] std::uint64_t front_only(const PackedArgs& args) noexcept;

// Guard only, identical scan and lane skip, no consumer.
[[nodiscard]] std::uint64_t guard_only(const PackedArgs& args) noexcept;

// Queued schedule of the same consumer.  Measured on the authentic fixture,
// the lane-skip schedule above pays for unpredictable control flow: 38% of
// guard vectors are zero and a nonzero word holds about 1.1 candidate bits, so
// "is this vector / lane / word nonzero" branches mispredict constantly.  The
// queued schedule removes those branches without changing any semantics:
//   1. guard: every 256-bit guard vector is compacted, branch-free, into a
//      queue of nonzero 64-bit words (AVX2 permute + popcount advance);
//   2. consume: a fixed-trip-count loop runs consume_word over the queue and
//      appends each nonzero resolve mask to a second queue, branch-free;
//   3. resolve: the exact resolver runs over the (short) resolve queue.
// Word order is preserved, so the witness stream is identical in content and
// order to every other route.
template <bool kTwoDecks>
[[nodiscard]] std::uint32_t route_query_queued(const PackedArgs& args,
                                               std::uint32_t* out) noexcept;

// Stages 1-2 only (resolve queue folded into a checksum), and stage 1 only.
[[nodiscard]] std::uint64_t queued_front_only(const PackedArgs& args) noexcept;
[[nodiscard]] std::uint64_t queued_guard_only(const PackedArgs& args) noexcept;

// Causal control (specification section 26, "no causal win"): the identical
// branch-free queue of nonzero guard words, followed by the plain radix-13
// exact dense leaf on every candidate bit.  No PEXT, PDEP or payload deck.  If
// this is as fast as the packed queued route, the win belongs to the schedule,
// not to packed-tag consumption.
[[nodiscard]] std::uint32_t route_query_queued_dense_control(const PackedArgs& args,
                                                             std::uint32_t* out) noexcept;

// True only when this translation unit was compiled for AVX2 (and therefore,
// under MSVC /arch:AVX2 and GCC/Clang -mavx2 -mbmi2, may issue BMI2).
[[nodiscard]] bool translation_unit_is_bmi2() noexcept;

}  // namespace bmi2

}  // namespace ter::radix13_packed
