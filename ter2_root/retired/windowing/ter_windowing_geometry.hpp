#pragma once

// TER^2 residual windowing research spike: window geometry primitives.
//
// SCOPE.  Benchmark/test surface only.  Nothing here is linked into ter_t0,
// into src/full_ter.cpp, or into any production reporter.  The MicroReport8
// dataset builder and kernels are used as unmodified donors; this header adds
// only the window definitions the windowing experiment needs.
//
// WINDOW DEFINITION.  Residues live on the ring Z / 2^19.  For a nonempty set
// of distinct residues r_0 < r_1 < ... < r_{m-1} the cyclic gaps are
//
//     g_i     = r_{i+1} - r_i          for i in [0, m-1)
//     g_wrap  = M - r_{m-1} + r_0
//
// and G = max(all gaps).  The minimal circular covering window has
//
//     span  = M - G
//     slots = span + 1
//
// and starts at the residue immediately after the largest excluded gap.  This
// is the shortest circular interval containing every element, which is what a
// window-local target representation would have to cover.  A plain
// max(sum) - min(sum) is wrong here: a set clustered around the 2^19 -> 0 wrap
// is extremely local yet has a near-maximal linear range.
//
// SOURCE-BOX BOUND.  If X lies in a circular interval of length span_X starting
// at x0 and Q lies in one of length span_Q starting at q0, then every
// (x + q + kappa) mod M lies in the circular interval of length
// span_X + span_Q starting at (x0 + q0 + kappa) mod M.  This is exact, holds
// across the modular wrap, and needs no sum materialization -- which is the
// point, since a production router cannot afford to build and sort 64 sums
// merely to discover a window.  It is an upper bound, never an equality, so it
// is always stored separately from the exact sum window.

#include "ter_microreport8_kernels.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ter::windowing {

using ter::microreport8::kResidualMask;
using ter::microreport8::kResidualUniverse;
using ter::microreport8::kTileDim;
using ter::microreport8::kTileLanes;
using ter::microreport8::Tile;

inline constexpr std::uint32_t kModulus = kResidualUniverse;  // 2^19 = 524288

struct Window {
  std::uint32_t start = 0U;  // first residue of the covering interval
  std::uint32_t end = 0U;    // last residue, inclusive, mod M
  std::uint32_t span = 0U;   // (end - start) mod M
  std::uint32_t slots = 0U;  // span + 1, the integer slot capacity required
  bool wraps = false;        // true when end < start, i.e. the interval wraps
  bool valid = false;        // false only for an empty input set
};

// Position of `value` relative to a window start.  A residue belongs to the
// window exactly when this is < slots.
[[nodiscard]] inline std::uint32_t normalize(const std::uint32_t value,
                                             const std::uint32_t start) noexcept {
  return (value - start) & kResidualMask;
}

[[nodiscard]] inline bool window_contains(const Window& window,
                                          const std::uint32_t value) noexcept {
  return window.valid && normalize(value, window.start) < window.slots;
}

// Minimal circular covering window of a strictly ascending, distinct set.
[[nodiscard]] inline Window minimal_circular_window(const std::uint32_t* const sorted_distinct,
                                                    const std::size_t count) noexcept {
  Window window;
  if (count == 0U) {
    return window;
  }
  window.valid = true;
  if (count == 1U) {
    window.start = sorted_distinct[0];
    window.end = sorted_distinct[0];
    window.span = 0U;
    window.slots = 1U;
    window.wraps = false;
    return window;
  }
  // The wrap gap is the incumbent, which makes the natural [r_0, r_{m-1}]
  // interval the answer whenever no interior gap beats it.
  std::uint32_t largest_gap = kModulus - sorted_distinct[count - 1U] + sorted_distinct[0];
  std::uint32_t start = sorted_distinct[0];
  std::uint32_t end = sorted_distinct[count - 1U];
  for (std::size_t index = 0U; index + 1U < count; ++index) {
    const std::uint32_t gap = sorted_distinct[index + 1U] - sorted_distinct[index];
    if (gap > largest_gap) {
      largest_gap = gap;
      start = sorted_distinct[index + 1U];
      end = sorted_distinct[index];
    }
  }
  window.start = start;
  window.end = end;
  window.span = kModulus - largest_gap;
  window.slots = window.span + 1U;
  window.wraps = end < start;
  return window;
}

// Window built from an explicit start and span; used by the source-box bound
// and by the value-retiling model, where the interval is known algebraically.
[[nodiscard]] inline Window window_from_start_span(const std::uint32_t start,
                                                   const std::uint32_t span) noexcept {
  Window window;
  window.valid = true;
  window.start = start & kResidualMask;
  window.span = std::min(span, kModulus - 1U);
  window.slots = window.span + 1U;
  window.end = (window.start + window.span) & kResidualMask;
  window.wraps = window.end < window.start;
  return window;
}

// Tight interval for two non-wrapping numeric bins. In this experiment their
// combined span is below M/2: both endpoints occur, and the excluded wrap gap
// exceeds every interior gap, so this is also the minimum circular cover.
[[nodiscard]] inline Window bin_sum_window(const std::uint32_t x_lo,
                                           const std::uint32_t x_hi,
                                           const std::uint32_t q_lo,
                                           const std::uint32_t q_hi,
                                           const std::uint32_t kappa) noexcept {
  return window_from_start_span((x_lo + q_lo + kappa) & kResidualMask,
                                (x_hi - x_lo) + (q_hi - q_lo));
}

// A window identity includes its capacity. Equal starts alone do not imply
// equal target contents or a reusable local bitmap. The caller scopes by Y_u.
[[nodiscard]] inline std::uint64_t window_identity(const Window& window) noexcept {
  return (static_cast<std::uint64_t>(window.start) << 20U) | window.slots;
}

// Collects a tile's valid sums, sorts them ascending and removes duplicates.
// Returns the distinct count; `out` must hold at least kTileLanes entries.
[[nodiscard]] inline std::size_t tile_sorted_sums(const Tile& tile,
                                                  std::uint32_t* const out) noexcept {
  std::size_t count = 0U;
  for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
    for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
      out[count++] = (tile.x[i] + tile.q[j] + tile.kappa) & kResidualMask;
    }
  }
  std::sort(out, out + count);
  return count;
}

[[nodiscard]] inline std::size_t unique_in_place(std::uint32_t* const sorted,
                                                 const std::size_t count) noexcept {
  return static_cast<std::size_t>(std::unique(sorted, sorted + count) - sorted);
}

// Exact minimal circular window over a tile's valid sums.
[[nodiscard]] inline Window tile_exact_window(const Tile& tile) noexcept {
  std::uint32_t sums[kTileLanes];
  const std::size_t total = tile_sorted_sums(tile, sums);
  const std::size_t distinct = unique_in_place(sums, total);
  return minimal_circular_window(sums, distinct);
}

// Cheap algebraic bound: circular span of X plus circular span of Q.  The X and
// Q lanes of a tile are an ascending slice of a sorted family, so their own
// minimal windows are computed the same way and may themselves wrap.
[[nodiscard]] inline Window tile_source_box_window(const Tile& tile) noexcept {
  std::uint32_t values[kTileDim];
  for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
    values[i] = tile.x[i];
  }
  std::sort(values, values + tile.x_count);
  const std::size_t x_distinct = unique_in_place(values, tile.x_count);
  const Window x_window = minimal_circular_window(values, x_distinct);

  std::uint32_t q_values[kTileDim];
  for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
    q_values[j] = tile.q[j];
  }
  std::sort(q_values, q_values + tile.q_count);
  const std::size_t q_distinct = unique_in_place(q_values, tile.q_count);
  const Window q_window = minimal_circular_window(q_values, q_distinct);

  if (!x_window.valid || !q_window.valid) {
    return Window{};
  }
  return window_from_start_span((x_window.start + q_window.start + tile.kappa) & kResidualMask,
                                x_window.span + q_window.span);
}

// ---------------------------------------------------------------------------
// Prefix-counted target family.
//
// One Y_u family indexed so that "how many target residues lie in this circular
// window" is two array reads instead of a scan.  Distinct values and postings
// are counted separately: the membership question is about values, but the TER
// root ultimately needs postings, so multiplicity is never folded away.
// ---------------------------------------------------------------------------
struct FamilyIndex {
  std::vector<std::uint32_t> distinct_prefix;  // size M+1, prefix[v] = |{y < v}|
  std::vector<std::uint32_t> posting_prefix;   // size M+1, multiplicity-aware
  std::uint32_t distinct_total = 0U;
  std::uint32_t posting_total = 0U;

  void build(const std::uint32_t* const values, const std::size_t count) {
    distinct_prefix.assign(kModulus + 1U, 0U);
    posting_prefix.assign(kModulus + 1U, 0U);
    std::vector<std::uint8_t> seen(kModulus, 0U);
    for (std::size_t index = 0U; index < count; ++index) {
      const std::uint32_t value = values[index];
      ++posting_prefix[value + 1U];
      if (seen[value] == 0U) {
        seen[value] = 1U;
        ++distinct_prefix[value + 1U];
      }
    }
    for (std::uint32_t value = 0U; value < kModulus; ++value) {
      distinct_prefix[value + 1U] += distinct_prefix[value];
      posting_prefix[value + 1U] += posting_prefix[value];
    }
    distinct_total = distinct_prefix[kModulus];
    posting_total = posting_prefix[kModulus];
  }

  // Half-open [lo, hi) on the linear range, no wrap.
  [[nodiscard]] std::uint32_t distinct_range(const std::uint32_t lo,
                                             const std::uint32_t hi) const noexcept {
    return distinct_prefix[hi] - distinct_prefix[lo];
  }
  [[nodiscard]] std::uint32_t posting_range(const std::uint32_t lo,
                                            const std::uint32_t hi) const noexcept {
    return posting_prefix[hi] - posting_prefix[lo];
  }

  [[nodiscard]] std::uint32_t distinct_in(const Window& window) const noexcept {
    if (!window.valid) {
      return 0U;
    }
    if (window.slots >= kModulus) {
      return distinct_total;
    }
    if (!window.wraps) {
      return distinct_range(window.start, window.end + 1U);
    }
    return distinct_range(window.start, kModulus) + distinct_range(0U, window.end + 1U);
  }

  [[nodiscard]] std::uint32_t postings_in(const Window& window) const noexcept {
    if (!window.valid) {
      return 0U;
    }
    if (window.slots >= kModulus) {
      return posting_total;
    }
    if (!window.wraps) {
      return posting_range(window.start, window.end + 1U);
    }
    return posting_range(window.start, kModulus) + posting_range(0U, window.end + 1U);
  }
};

// ---------------------------------------------------------------------------
// Range emptiness over the existing exact 2^19-bit Y bitmap.
//
// This is the routing primitive a value-retiled architecture would actually
// have to run: "does Y_u contain any residue in this circular window?". It uses
// the 64 KiB bitmap the dataset already builds, so it needs no new memory, and
// for a narrow window it costs one or two masked word reads -- which is the
// same order as a single pair probe. That equivalence is the whole reason the
// rectangle count has to be charged rather than assumed free.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool range_nonempty(const std::uint32_t* const exact,
                                         const std::uint32_t start,
                                         const std::uint32_t slots) noexcept {
  std::uint32_t remaining = slots;
  std::uint32_t position = start & kResidualMask;
  while (remaining > 0U) {
    const std::uint32_t bit = position & 31U;
    const std::uint32_t take = std::min(32U - bit, remaining);
    const std::uint32_t mask =
        take >= 32U ? 0xFFFFFFFFU : (((std::uint32_t{1} << take) - 1U) << bit);
    if ((exact[position >> 5U] & mask) != 0U) {
      return true;
    }
    position = (position + take) & kResidualMask;
    remaining -= take;
  }
  return false;
}

// One rectangle rejection test, as the routing loop would issue it.
struct RectProbe {
  std::uint32_t start = 0U;  // window start on the residual ring
  std::uint32_t slots = 0U;  // window capacity in slots
  std::uint32_t u = 0U;      // which Y family answers the query
};

// Circular intersection length of two windows, in slots.
[[nodiscard]] inline std::uint32_t window_intersection_slots(const Window& a,
                                                             const Window& b) noexcept {
  if (!a.valid || !b.valid) {
    return 0U;
  }
  // Normalize both relative to a.start: A becomes [0, a.span].  B becomes an
  // interval starting at b0, which may also contribute a piece one turn back.
  const std::uint64_t b0 = normalize(b.start, a.start);
  const std::uint64_t a_hi = a.span;
  std::uint64_t total = 0U;
  const auto overlap = [&](const std::int64_t lo, const std::int64_t hi) {
    const std::int64_t low = std::max<std::int64_t>(lo, 0);
    const std::int64_t high = std::min<std::int64_t>(hi, static_cast<std::int64_t>(a_hi));
    if (high >= low) {
      total += static_cast<std::uint64_t>(high - low + 1);
    }
  };
  overlap(static_cast<std::int64_t>(b0), static_cast<std::int64_t>(b0 + b.span));
  overlap(static_cast<std::int64_t>(b0) - static_cast<std::int64_t>(kModulus),
          static_cast<std::int64_t>(b0) + static_cast<std::int64_t>(b.span) -
              static_cast<std::int64_t>(kModulus));
  const std::uint64_t capped = std::min<std::uint64_t>(total, a.slots);
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(capped, b.slots));
}

}  // namespace ter::windowing
