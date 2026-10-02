#pragma once

// TER^2 radix occupancy locator research spike.
//
// SCOPE.  Benchmark/test surface only.  Nothing here is linked into ter_t0, into
// src/full_ter.cpp, or into any production reporter.  It does not modify the
// translated-tag locator, the MicroReport8 donor dataset, the frozen K2
// comparator, or any previously recorded spike result: it is an additional
// comparator path measured under the identical matched scope.
//
// PROBLEM.  After the exact low-25 TER^2 decomposition each microproblem is
//
//     x + q + kappa  ==  y   (mod 2^19),   x in X_t, q in Q_s, y in Y_u.
//
// REPRESENTATION.  The translated-tag spike splits the 19-bit residual as
// 11 position bits + 8 tag bits, which needs 256 tag values per position and
// therefore a capped two-layer bit-plane representation plus explicit excess
// lists.  This spike splits it as
//
//     v = i + R*a,   i = v mod R   (PositionBits exact position bits)
//                    a = v / R     (19 - PositionBits exact tag bits)
//
// so the complete tag universe at one position fits in a single uint64_t.
// Layers, bit planes, excess lists and shifted tag views all disappear; what
// remains per position is one exact tag word.
//
// For a query d = (q + kappa) mod 2^19 written as d = r + R*alpha,
//
//     j   = (i + r) mod R                     the only possible target position
//     c_i = [i + r >= R]                      the exact carry into the tag
//     b   = (a + alpha + c_i) mod H           the required target tag
//
// GUARD.  G_r[i] = O_X[i] AND O_Y[(i + r) mod R] over R positions, read as
// R/256 256-bit vectors per query.  Every set bit is a compatible bucket.
//
// LEAF.  E_i = rot_H(T_X[i], alpha + c_i) AND T_Y[j].  Zero means the whole
// bucket interaction is certainly empty; every set bit b is an exact residual
// witness with a = (b - alpha - c_i) mod H.
//
// CONTRACT.  Exact in both directions.  Zero false negatives and zero exact
// residual false positives, with no excess or overflow path of any kind: the
// tag word at a position is the complete set of residuals at that position.
//
// WHY R IS A POWER OF TWO.  Three independent parts of the construction require
// it, and all three fail together for a non-power-of-two radix:
//
//   1. the split v = i + R*a is a bit mask and a shift, not a division;
//   2. the shifted views decompose a rotation by r into a whole-word rotation
//      by r/64 plus a bit shift by r mod 64, which needs 64 | R so that the
//      ring is a whole number of machine words;
//   3. the carry rule c_i = [i + r >= R] is exact only because the residual
//      universe wraps at 2^19 and R divides 2^19, so wrapping the universe
//      does not also displace the position.
//
// For R = 32628, for example, R mod 64 = 52 and 2^19 mod R = 2240: the ring is
// 509.8 words long and a universe wrap displaces the position by 2240, so the
// single-rotation guard and the single-rotation leaf both cease to exist.

#include "ter_translated_tag_locator.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ter::radix {

namespace tt = ter::translated_tag;

using ter::microreport8::kResidualBits;
using ter::microreport8::kResidualMask;

// ---------------------------------------------------------------------------
// Geometry.  Everything else is derived from the position width, so a radix
// sweep changes one number and cannot desynchronize the tables from the
// kernels that read them.
// ---------------------------------------------------------------------------
template <unsigned PositionBits>
struct Geom {
  static constexpr std::uint32_t kPositionBits = PositionBits;
  static constexpr std::uint32_t kR = 1U << PositionBits;
  static constexpr std::uint32_t kPositionMask = kR - 1U;
  static constexpr std::uint32_t kTagBits = kResidualBits - PositionBits;
  static constexpr std::uint32_t kH = 1U << kTagBits;
  static constexpr std::uint32_t kTagMask = kH - 1U;
  // The tag word holds exactly H bits.  At H = 64 that is the whole word.
  static constexpr std::uint64_t kTagWordMask =
      kH >= 64U ? ~std::uint64_t{0} : ((std::uint64_t{1} << (kH & 63U)) - 1U);

  static constexpr std::uint32_t kWords = kR / 64U;
  static constexpr std::uint32_t kSliceWords = 4U;                 // 256-bit vector
  static constexpr std::uint32_t kSlices = kWords / kSliceWords;   // vectors per query
  static constexpr std::uint32_t kViewWords = kWords + kSliceWords;  // wrap pad
  static constexpr std::uint32_t kShifts = 64U;
  static constexpr std::size_t kViewWordsPerClass =
      static_cast<std::size_t>(kShifts) * kViewWords;

  // A query maps every source residual to exactly one target residual, so it
  // emits at most one bucket per occupied source position and at most one
  // witness per distinct source value.  Both bounds are enforced at build time
  // against these capacities, never assumed.
  static constexpr std::uint32_t kExtractCapacity = 8192U;
  static constexpr std::uint32_t kHitCapacity = 65536U;

  static_assert(PositionBits >= 6U, "the occupancy ring must be a whole machine word");
  static_assert(PositionBits < kResidualBits, "the tag must keep at least one bit");
  static_assert(kH <= 64U, "the tag universe at one position must fit in one word");
  static_assert(kSlices * kSliceWords == kWords, "slices must tile the occupancy plane");
  static_assert(kPositionBits + 2U * kTagBits <= 32U, "a witness must pack into 32 bits");
  static_assert((kResidualMask + 1U) % kR == 0U,
                "R must divide the residual universe or the carry rule is not exact");
};

inline constexpr std::uint32_t kNoGroup = tt::kNoGroup;

// Rotation within the H-bit tag universe.  At H = 64 this is the machine
// rotate; below it, the word is masked so the vacated high bits stay clear.
template <unsigned PB>
[[nodiscard]] inline std::uint64_t rotate_tags(const std::uint64_t word,
                                               const std::uint32_t amount) noexcept {
  using G = Geom<PB>;
  if constexpr (G::kH >= 64U) {
    return std::rotl(word, static_cast<int>(amount));
  } else {
    // amount is already reduced mod H by the caller, so amount < H <= 32 and
    // neither shift can reach the machine word width.
    return ((word << amount) | (word >> (G::kH - amount))) & G::kTagWordMask;
  }
}

// Packed witness: position, then source tag, then target tag.
template <unsigned PB>
[[nodiscard]] constexpr std::uint32_t pack_hit(const std::uint32_t position,
                                               const std::uint32_t source_tag,
                                               const std::uint32_t target_tag) noexcept {
  return (position << (2U * Geom<PB>::kTagBits)) | (source_tag << Geom<PB>::kTagBits) |
         target_tag;
}
template <unsigned PB>
[[nodiscard]] constexpr std::uint32_t hit_position(const std::uint32_t packed) noexcept {
  return packed >> (2U * Geom<PB>::kTagBits);
}
template <unsigned PB>
[[nodiscard]] constexpr std::uint32_t hit_source_tag(const std::uint32_t packed) noexcept {
  return (packed >> Geom<PB>::kTagBits) & Geom<PB>::kTagMask;
}
template <unsigned PB>
[[nodiscard]] constexpr std::uint32_t hit_target_tag(const std::uint32_t packed) noexcept {
  return packed & Geom<PB>::kTagMask;
}

// ---------------------------------------------------------------------------
// Classes.
//
// Two exact tag representations are built for each class so the leaf kernels
// can be measured against each other on identical data:
//
//   COMPACT  tags indexed by the rank of the occupied position.  This is the
//            representation the proposal specifies: it needs a rank directory
//            on the target side and a popcount rank on the source side, and it
//            puts two dependent loads on the leaf critical path.  Its size is
//            proportional to the occupied positions, not to R.
//   DENSE    tags indexed directly by the position.  It removes the rank
//            directory, the popcount and the load-to-load dependency, but it
//            costs 8*R bytes per class and so scales with the radix.
//
// Both are exact and must produce byte-identical witness streams.  Which one
// wins is a cache question that changes with R, which is why both are kept.
// ---------------------------------------------------------------------------

template <unsigned PB>
struct SourceClass {
  using G = Geom<PB>;
  std::vector<tt::ResidualGroup> groups;  // ascending by value

  alignas(32) std::array<std::uint64_t, G::kWords> occupancy{};
  std::array<std::uint32_t, G::kWords> occupied_prefix{};

  std::vector<std::uint64_t> tags_compact;  // one per occupied position, ascending
  std::vector<std::uint64_t> tags_dense;    // R entries, zero where unoccupied

  // Cold, touched only once an exact residual witness exists.  Position-major
  // slot -> group index in the ascending-by-value directory.
  std::vector<std::uint32_t> group_base;  // R entries
  std::vector<std::uint32_t> group_ids;

  std::uint32_t occupied = 0;  // occupied positions
  std::uint32_t distinct = 0;  // distinct 19-bit residual values
  std::uint64_t postings = 0;
};

template <unsigned PB>
struct TargetClass {
  using G = Geom<PB>;
  std::vector<tt::ResidualGroup> groups;  // ascending by value

  alignas(32) std::array<std::uint64_t, G::kWords> occupancy{};
  std::vector<std::uint32_t> bucket_rank;  // R entries, rank of occupied position

  std::vector<std::uint64_t> tags_compact;
  std::vector<std::uint64_t> tags_dense;

  std::vector<std::uint32_t> group_base;
  std::vector<std::uint32_t> group_ids;

  std::uint32_t occupied = 0;
  std::uint32_t distinct = 0;
  std::uint64_t postings = 0;
};

// ---------------------------------------------------------------------------
// Per-query addressing.  Derived once, shared by every leaf kernel.
// ---------------------------------------------------------------------------
template <unsigned PB>
struct QueryPlan {
  std::uint32_t r = 0;
  std::uint32_t alpha = 0;
  std::uint32_t shift = 0;        // r mod 64, selects the shifted occupancy view
  std::uint32_t word_offset = 0;  // r / 64, rotates the view by whole words
};

template <unsigned PB>
[[nodiscard]] inline QueryPlan<PB> make_query_plan(const std::uint32_t d) noexcept {
  QueryPlan<PB> plan;
  plan.r = d & Geom<PB>::kPositionMask;
  plan.alpha = d >> Geom<PB>::kPositionBits;
  plan.shift = plan.r & 63U;
  plan.word_offset = plan.r >> 6U;
  return plan;
}

// ---------------------------------------------------------------------------
// Work counters.  The counted and uncounted kernels are separate instantiations
// so a timed run never pays a counter update.
// ---------------------------------------------------------------------------
struct Counters {
  std::uint64_t guard_vectors = 0;
  std::uint64_t guard_zero_vectors = 0;
  std::uint64_t emitted_buckets = 0;   // L, the leaf count
  std::uint64_t nonempty_buckets = 0;  // leaves whose tag intersection survived
  std::uint64_t witnesses = 0;         // exact residual witnesses reported
  std::array<std::uint64_t, 12> guard_popcount_histogram{};

  void merge(const Counters& other) noexcept {
    guard_vectors += other.guard_vectors;
    guard_zero_vectors += other.guard_zero_vectors;
    emitted_buckets += other.emitted_buckets;
    nonempty_buckets += other.nonempty_buckets;
    witnesses += other.witnesses;
    for (std::size_t index = 0U; index < guard_popcount_histogram.size(); ++index) {
      guard_popcount_histogram[index] += other.guard_popcount_histogram[index];
    }
  }
};

[[nodiscard]] constexpr std::size_t popcount_bucket(const std::uint32_t population) noexcept {
  if (population <= 4U) {
    return population;
  }
  if (population <= 8U) {
    return 5U;
  }
  if (population <= 16U) {
    return 6U;
  }
  if (population <= 32U) {
    return 7U;
  }
  if (population <= 64U) {
    return 8U;
  }
  if (population <= 128U) {
    return 9U;
  }
  if (population <= 256U) {
    return 10U;
  }
  return 11U;
}

// ---------------------------------------------------------------------------
// Construction.  Inputs are ascending residual values with duplicates adjacent,
// which is exactly how the authentic families are stored.  Returns false if the
// class exceeds a representable or buffered bound, which the driver treats as
// fail-closed: it never becomes an empty result.
// ---------------------------------------------------------------------------
template <unsigned PB>
[[nodiscard]] bool build_source_class(const std::uint32_t* values, std::size_t count,
                                      SourceClass<PB>& out);
template <unsigned PB>
[[nodiscard]] bool build_target_class(const std::uint32_t* values, std::size_t count,
                                      TargetClass<PB>& out);

// [shift][kViewWords]; the 4-word tail duplication makes an arbitrary
// whole-word rotation a contiguous, unaligned-safe 256-bit load.  Occupancy
// only: exact tag words are never shifted.
template <unsigned PB>
void build_shifted_views(const TargetClass<PB>& target, std::uint64_t* out) noexcept;

// ---------------------------------------------------------------------------
// Exact value -> group index, used only after a witness exists.
// ---------------------------------------------------------------------------
template <unsigned PB>
[[nodiscard]] inline std::uint32_t source_group(const SourceClass<PB>& source,
                                                const std::uint32_t position,
                                                const std::uint32_t tag) noexcept {
  const std::uint64_t word = source.tags_dense[position];
  if (((word >> tag) & 1U) == 0U) {
    return kNoGroup;
  }
  const std::uint32_t within = static_cast<std::uint32_t>(
      std::popcount(word & ((std::uint64_t{1} << tag) - 1U)));
  return source.group_ids[source.group_base[position] + within];
}

template <unsigned PB>
[[nodiscard]] inline std::uint32_t target_group(const TargetClass<PB>& target,
                                                const std::uint32_t position,
                                                const std::uint32_t tag) noexcept {
  const std::uint64_t word = target.tags_dense[position];
  if (((word >> tag) & 1U) == 0U) {
    return kNoGroup;
  }
  const std::uint32_t within = static_cast<std::uint32_t>(
      std::popcount(word & ((std::uint64_t{1} << tag) - 1U)));
  return target.group_ids[target.group_base[position] + within];
}

// ---------------------------------------------------------------------------
// Routers.  Every one of them produces the identical witness multiset.
// ---------------------------------------------------------------------------

// Definitional scalar reference: group directory plus ordinary modular
// arithmetic, no occupancy words, no shifted views, no bit tricks.  A defect in
// the bit-parallel machinery cannot hide here.
template <unsigned PB>
[[nodiscard]] std::vector<std::uint32_t> route_query_scalar(const SourceClass<PB>& source,
                                                            const TargetClass<PB>& target,
                                                            std::uint32_t d);

// Arguments common to every bit-parallel kernel.  The view pointer already
// selects the shift plane, so the kernel only applies the whole-word rotation.
template <unsigned PB>
struct RouteArgs {
  const std::uint64_t* x_occupancy = nullptr;    // kWords
  const std::uint32_t* x_prefix = nullptr;       // kWords, compact kernel only
  const std::uint64_t* x_tags = nullptr;         // compact or dense
  const std::uint64_t* y_view = nullptr;         // kViewWords, already shift-selected
  const std::uint32_t* y_bucket_rank = nullptr;  // R, compact kernel only
  const std::uint64_t* y_tags = nullptr;         // compact or dense
  std::uint32_t r = 0;
  std::uint32_t alpha = 0;
  std::uint32_t word_offset = 0;
};

// Guard only: AND plus zero test over the ring.  Emits nothing.  Attributes the
// guard phase cost with no leaf work at all.
template <unsigned PB>
[[nodiscard]] std::uint64_t guard_only_portable(const RouteArgs<PB>& args) noexcept;

// Guard plus bit extraction plus the position/carry arithmetic, with no tag
// load, no rotation and no emission.  Subtracting the guard from this isolates
// what it costs to turn set guard bits into bucket locations, which is the one
// cost the original proposal folded into a single leaf constant.
template <unsigned PB>
[[nodiscard]] std::uint64_t extract_only_portable(const RouteArgs<PB>& args) noexcept;

// Leaf kernel A, the representation the proposal specifies.
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_compact_portable(const RouteArgs<PB>& args,
                                                         std::uint32_t* out) noexcept;

// Leaf kernel B: tags addressed directly by position.
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_dense_portable(const RouteArgs<PB>& args,
                                                       std::uint32_t* out) noexcept;

template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_dense_portable_counted(const RouteArgs<PB>& args,
                                                               std::uint32_t* out,
                                                               Counters& counters) noexcept;

// Leaf kernel C: the same exact work as kernel B, split into an extraction pass
// that materializes the compatible positions and a resolution pass with a fixed
// trip count, so the two tag loads of one iteration do not sit behind an
// unpredictable loop back-edge.
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_batched_portable(const RouteArgs<PB>& args,
                                                         std::uint32_t* out) noexcept;

namespace avx2 {

template <unsigned PB>
[[nodiscard]] std::uint64_t guard_only(const RouteArgs<PB>& args) noexcept;
template <unsigned PB>
[[nodiscard]] std::uint64_t extract_only(const RouteArgs<PB>& args) noexcept;
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_compact(const RouteArgs<PB>& args,
                                                std::uint32_t* out) noexcept;
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_dense(const RouteArgs<PB>& args,
                                              std::uint32_t* out) noexcept;
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_dense_counted(const RouteArgs<PB>& args,
                                                      std::uint32_t* out,
                                                      Counters& counters) noexcept;

// Leaf kernel D: kernel C plus a lane skip.  The measured cost of turning guard
// bits into bucket locations is dominated by unpredictable short loops, and at
// these densities most 64-bit lanes of a nonzero guard vector hold no bit at
// all.  A vector compare plus movemask names the nonempty lanes directly.
template <unsigned PB>
[[nodiscard]] std::uint32_t route_query_skip(const RouteArgs<PB>& args,
                                             std::uint32_t* out) noexcept;

// True only when this translation unit was actually compiled for AVX2; it is
// resolved there, not by the caller's preprocessor state.
[[nodiscard]] bool translation_unit_is_avx2() noexcept;

}  // namespace avx2

}  // namespace ter::radix

// The original spike measured PositionBits = 13 under this name.  Keeping the
// alias means every existing call site, test and recorded result continues to
// designate exactly the same instantiation.
namespace ter::radix13 {
inline constexpr unsigned kBits = 13U;
}  // namespace ter::radix13
