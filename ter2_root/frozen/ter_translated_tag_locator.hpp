#pragma once

// TER^2 translated-tag locator research spike: exact set-alignment locator.
//
// SCOPE.  Benchmark/test surface only.  Nothing here is linked into ter_t0, into
// src/full_ter.cpp, or into any production reporter.  It answers one question:
// can exact translated 11-bit-position + 8-bit-tag set alignment locate the
// authentic n=72 TER^2 residual witnesses faster than pair/tile enumeration?
//
// PROBLEM.  After the exact low-25 TER^2 decomposition (see
// ter_microreport8_dataset.hpp) each microproblem is
//
//     x + q + kappa  ==  y   (mod 2^19),   x in X_t, q in Q_s, y in Y_u.
//
// REPRESENTATION.  Every 19-bit residual v splits exactly as
//
//     v = i + R*z,   i = v mod 2048   (11 exact position bits)
//                    z = v / 2048     (8  exact tag bits)
//
// so position + tag is the complete residual, never a fingerprint.  For a query
// d = (q + kappa) mod 2^19 written as d = r + R*alpha,
//
//     j   = (i + r) mod R                     the only possible target position
//     c_i = [i + r >= R]                      the exact carry into the tag
//     z_y = (z_x + alpha + c_i) mod 256       the required target tag
//
// LAYERS.  At most L = 2 distinct residuals per position live in the primary
// bit-sliced layers, chosen deterministically as the two smallest values at that
// position.  Everything else goes to an explicit excess list, and the excess is
// covered by the two exact complementary loops in the driver, never by the
// primary path.
//
// CONTRACT.  The primary path is exact in both directions: it reports every
// (X_primary, Y_primary) witness (zero false negatives) and never reports a
// position whose 19-bit residuals differ (zero exact-residual false positives).

#include "ter_microreport8_kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ter::translated_tag {

using ter::microreport8::kResidualBits;
using ter::microreport8::kResidualMask;
using ter::microreport8::kResidualUniverse;

// Frozen for this spike (section 26 of the task): R, L and the tag width are
// not parameters here.  Changing them is a separate experiment.
inline constexpr std::uint32_t kPositionBits = 11U;
inline constexpr std::uint32_t kR = 1U << kPositionBits;  // 2048
inline constexpr std::uint32_t kPositionMask = kR - 1U;
inline constexpr std::uint32_t kTagBits = 8U;
inline constexpr std::uint32_t kH = 1U << kTagBits;  // 256
inline constexpr std::uint32_t kTagMask = kH - 1U;
inline constexpr std::uint32_t kLayers = 2U;
inline constexpr std::uint32_t kWords = kR / 64U;         // 32 uint64 per plane
inline constexpr std::uint32_t kViewWords = 2U * kWords;  // 64, duplicated for wrap
inline constexpr std::uint32_t kVectorBits = 256U;
inline constexpr std::uint32_t kSlices = kR / kVectorBits;  // 8 vector slices
inline constexpr std::uint32_t kSliceWords = kVectorBits / 64U;  // 4
inline constexpr std::uint32_t kPlanes = 1U + kTagBits;  // occupancy + 8 tag planes

static_assert(kPositionBits + kTagBits == kResidualBits,
              "position and tag bits must reconstruct the whole residual");
static_assert(kSlices * kSliceWords == kWords, "slice decomposition must tile a plane");

inline constexpr std::uint32_t kNoGroup = 0xFFFFFFFFU;

// ---------------------------------------------------------------------------
// Flat table addressing.  Shared by the portable and the AVX2 translation units
// so a layout change cannot desynchronize them.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr std::size_t occupancy_index(const std::uint32_t layer,
                                                    const std::uint32_t slice) noexcept {
  return (static_cast<std::size_t>(layer) * kSlices + slice) * kSliceWords;
}

[[nodiscard]] constexpr std::size_t source_tag_index(const std::uint32_t layer,
                                                     const std::uint32_t slice,
                                                     const std::uint32_t bit) noexcept {
  return ((static_cast<std::size_t>(layer) * kSlices + slice) * kTagBits + bit) * kSliceWords;
}

[[nodiscard]] constexpr std::size_t target_plane_index(const std::uint32_t layer,
                                                       const std::uint32_t plane) noexcept {
  return (static_cast<std::size_t>(layer) * kPlanes + plane) * kWords;
}

[[nodiscard]] constexpr std::size_t translation_index(const std::uint32_t alpha,
                                                      const std::uint32_t layer,
                                                      const std::uint32_t slice,
                                                      const std::uint32_t bit) noexcept {
  return (((static_cast<std::size_t>(alpha) * kLayers + layer) * kSlices + slice) * kTagBits +
          bit) *
         kSliceWords;
}

[[nodiscard]] constexpr std::size_t shifted_view_index(const std::uint32_t delta,
                                                       const std::uint32_t layer,
                                                       const std::uint32_t plane) noexcept {
  return ((static_cast<std::size_t>(delta) * kLayers + layer) * kPlanes + plane) * kViewWords;
}

// Source-side (X) translation table: [alpha][layer][slice][tag bit][4 words].
inline constexpr std::size_t kTranslationWords =
    static_cast<std::size_t>(kH) * kLayers * kSlices * kTagBits * kSliceWords;
// Target-side (Y) shifted views: [delta][layer][plane][64 words].
inline constexpr std::size_t kShiftedViewWords =
    static_cast<std::size_t>(64) * kLayers * kPlanes * kViewWords;

// One distinct residual value with the complete posting span that produced it.
// Multiplicity is never collapsed: the span is what a match expands to.
struct ResidualGroup {
  std::uint32_t value = 0;
  std::uint32_t begin = 0;
  std::uint32_t count = 0;
};

// Source class: the bit-sliced X_t representation the router translates.
struct SourceClass {
  std::vector<ResidualGroup> groups;  // ascending by value
  // position -> group id per layer, kNoGroup when that layer is empty there.
  std::array<std::array<std::uint32_t, kR>, kLayers> group_at{};
  // Slice-major so one slice's whole tag set is contiguous: [layer][slice][4].
  std::array<std::uint64_t, static_cast<std::size_t>(kLayers) * kSlices * kSliceWords>
      occupancy{};
  // [layer][slice][tag bit][4]; this is the alpha = 0 translation.
  std::array<std::uint64_t,
             static_cast<std::size_t>(kLayers) * kSlices * kTagBits * kSliceWords>
      tag{};
  std::vector<std::uint32_t> excess;  // group ids beyond layer 1 at their position
  std::uint32_t primary_values = 0;
  std::array<std::uint32_t, kLayers> layer_values{};
  std::uint64_t postings = 0;
};

// Target class: the Y_u representation the shifted views are cut from.
struct TargetClass {
  std::vector<ResidualGroup> groups;  // ascending by value
  std::array<std::array<std::uint32_t, kR>, kLayers> group_at{};
  // Plane-major, undupliated: [layer][plane][32 words]; plane 0 is occupancy,
  // plane 1+b is tag bit b.
  std::array<std::uint64_t, static_cast<std::size_t>(kLayers) * kPlanes * kWords> plane{};
  std::vector<std::uint32_t> excess;
  std::uint32_t primary_values = 0;
  std::array<std::uint32_t, kLayers> layer_values{};
  std::uint64_t postings = 0;
};

// Work counters.  The counted and uncounted routers are separate instantiations
// so the timed total never pays a counter update or a null check.
struct Counters {
  std::uint64_t occupancy_vectors = 0;
  std::uint64_t occupancy_zero_exits = 0;
  std::uint64_t tag_plane_comparisons = 0;
  std::uint64_t tag_early_zero_exits = 0;
  std::uint64_t uniform_carry_slices = 0;
  std::uint64_t mixed_carry_slices = 0;
  std::uint64_t final_masks = 0;
  std::uint64_t primary_group_matches = 0;

  void merge(const Counters& other) noexcept {
    occupancy_vectors += other.occupancy_vectors;
    occupancy_zero_exits += other.occupancy_zero_exits;
    tag_plane_comparisons += other.tag_plane_comparisons;
    tag_early_zero_exits += other.tag_early_zero_exits;
    uniform_carry_slices += other.uniform_carry_slices;
    mixed_carry_slices += other.mixed_carry_slices;
    final_masks += other.final_masks;
    primary_group_matches += other.primary_group_matches;
  }
};

// Per-query addressing, computed once and shared by every layer pair.  Slice
// carry modes: 0 = every position uses alpha, 1 = every position uses alpha+1,
// 2 = the single slice the carry threshold falls inside.
struct QueryPlan {
  std::uint32_t r = 0;
  std::uint32_t delta = 0;
  std::uint32_t word_offset = 0;
  std::uint32_t alpha_low = 0;
  std::uint32_t alpha_high = 0;
  std::array<std::uint8_t, kSlices> slice_mode{};
  std::array<std::uint64_t, kSliceWords> low_mask{};  // bits below the threshold
};

[[nodiscard]] inline QueryPlan make_query_plan(const std::uint32_t d) noexcept {
  QueryPlan plan;
  plan.r = d & kPositionMask;
  plan.alpha_low = d >> kPositionBits;
  plan.alpha_high = (plan.alpha_low + 1U) & kTagMask;
  plan.delta = plan.r & 63U;
  plan.word_offset = plan.r >> 6U;
  // First position that carries.  r == 0 never carries, so the threshold sits
  // past the last position and every slice is uniform.
  const std::uint32_t threshold = plan.r == 0U ? kR : kR - plan.r;
  for (std::uint32_t slice = 0U; slice < kSlices; ++slice) {
    const std::uint32_t low = slice * kVectorBits;
    if (low + kVectorBits <= threshold) {
      plan.slice_mode[slice] = 0U;
    } else if (low >= threshold) {
      plan.slice_mode[slice] = 1U;
    } else {
      plan.slice_mode[slice] = 2U;
      const std::uint32_t split = threshold - low;  // 1..255
      for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
        const std::uint32_t base = word * 64U;
        if (split >= base + 64U) {
          plan.low_mask[word] = ~std::uint64_t{0};
        } else if (split <= base) {
          plan.low_mask[word] = 0U;
        } else {
          plan.low_mask[word] = (std::uint64_t{1} << (split - base)) - 1U;
        }
      }
    }
  }
  return plan;
}

// A surviving primary position, packed as (position << 2) | (x layer << 1) | y layer.
[[nodiscard]] inline std::uint32_t pack_hit(const std::uint32_t position,
                                            const std::uint32_t x_layer,
                                            const std::uint32_t y_layer) noexcept {
  return (position << 2U) | (x_layer << 1U) | y_layer;
}

// Upper bound on hits from one query: every position of every layer pair.
inline constexpr std::size_t kHitCapacity =
    static_cast<std::size_t>(kR) * kLayers * kLayers;

// ---------------------------------------------------------------------------
// Construction.  Inputs are ascending residual values with duplicates adjacent,
// which is exactly how the authentic families are stored.
// ---------------------------------------------------------------------------
void build_source_class(const std::uint32_t* values, std::size_t count, SourceClass& out);
void build_target_class(const std::uint32_t* values, std::size_t count, TargetClass& out);

// [alpha][layer][slice][bit][4]; alpha 0 is the class's own tags.  Written once
// per source class and reused across every query in that class's scope.
void build_translation_table(const SourceClass& source, std::uint64_t* out) noexcept;

// [delta][layer][plane][64]; the 64-word duplication makes an arbitrary whole
// word rotation a contiguous load.
void build_shifted_views(const TargetClass& target, std::uint64_t* out) noexcept;

// ---------------------------------------------------------------------------
// Exact directory lookups used by the two complementary overflow loops.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint32_t lookup_primary(const SourceClass& source,
                                                  const std::uint32_t value) noexcept {
  const std::uint32_t position = value & kPositionMask;
  for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
    const std::uint32_t group = source.group_at[layer][position];
    if (group != kNoGroup && source.groups[group].value == value) {
      return group;
    }
  }
  return kNoGroup;
}

[[nodiscard]] inline std::uint32_t lookup_all(const TargetClass& target,
                                              const std::uint32_t value) noexcept {
  const std::uint32_t position = value & kPositionMask;
  for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
    const std::uint32_t group = target.group_at[layer][position];
    if (group != kNoGroup && target.groups[group].value == value) {
      return group;
    }
  }
  for (const std::uint32_t group : target.excess) {
    if (target.groups[group].value == value) {
      return group;
    }
  }
  return kNoGroup;
}

// ---------------------------------------------------------------------------
// Routers.  All three produce the identical hit multiset for identical inputs.
// ---------------------------------------------------------------------------

// Definitional scalar reference (task section 25 step 1).  It uses only the
// group directory and ordinary modular arithmetic: no tags, no shifted views,
// no bit slicing.  A defect in the bit-sliced machinery cannot hide here.
[[nodiscard]] std::vector<std::uint32_t> route_query_scalar(const SourceClass& source,
                                                            const TargetClass& target,
                                                            std::uint32_t d);

// Portable bit-sliced router over 4-word slices.  Defines the semantics the
// AVX2 translation unit must reproduce bit for bit.
[[nodiscard]] std::uint32_t route_query_portable(const std::uint64_t* occupancy,
                                                 const std::uint64_t* translation,
                                                 const std::uint64_t* views,
                                                 const QueryPlan& plan,
                                                 std::uint32_t* out) noexcept;
[[nodiscard]] std::uint32_t route_query_portable_counted(const std::uint64_t* occupancy,
                                                         const std::uint64_t* translation,
                                                         const std::uint64_t* views,
                                                         const QueryPlan& plan,
                                                         std::uint32_t* out,
                                                         Counters& counters) noexcept;

namespace avx2 {

[[nodiscard]] std::uint32_t route_query(const std::uint64_t* occupancy,
                                        const std::uint64_t* translation,
                                        const std::uint64_t* views,
                                        const QueryPlan& plan,
                                        std::uint32_t* out) noexcept;
[[nodiscard]] std::uint32_t route_query_counted(const std::uint64_t* occupancy,
                                                const std::uint64_t* translation,
                                                const std::uint64_t* views,
                                                const QueryPlan& plan,
                                                std::uint32_t* out,
                                                Counters& counters) noexcept;

// True only when this translation unit was actually compiled for AVX2; it is
// resolved there, not by the caller's preprocessor state.
[[nodiscard]] bool translation_unit_is_avx2() noexcept;

}  // namespace avx2

// ---------------------------------------------------------------------------
// Aligned storage for the two large auxiliary tables.  Every byte it hands out
// is charged in the spike's memory accounting.
// ---------------------------------------------------------------------------
class AlignedWords {
 public:
  AlignedWords() = default;
  explicit AlignedWords(std::size_t words) { reset(words); }
  AlignedWords(const AlignedWords&) = delete;
  AlignedWords& operator=(const AlignedWords&) = delete;
  ~AlignedWords() { release(); }

  void reset(std::size_t words);
  void release() noexcept;

  [[nodiscard]] std::uint64_t* data() noexcept { return data_; }
  [[nodiscard]] const std::uint64_t* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return words_; }
  [[nodiscard]] std::size_t bytes() const noexcept { return words_ * sizeof(std::uint64_t); }

 private:
  std::uint64_t* data_ = nullptr;
  std::size_t words_ = 0;
};

}  // namespace ter::translated_tag
