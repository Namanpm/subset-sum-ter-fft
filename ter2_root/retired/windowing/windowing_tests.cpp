// Correctness tests for the TER^2 residual windowing spike's geometry.
//
// The windowing result rests entirely on two claims: that the reported window
// really is the minimal circular interval covering a tile's sums, and that the
// source-box bound never underestimates it. Both are checked here against an
// independent algorithm that shares no code with the implementation, over
// hand-built boundary cases and a randomized differential sweep.
//
// Authentic n=72 coverage is not duplicated here: ter_windowing_probe checks
// the normalization contract and the box bound on every one of the 9,890,364
// real tiles and refuses to issue a verdict if either is ever violated.

#include "ter_microreport8_dataset.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_windowing_geometry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ter::microreport8;
using namespace ter::windowing;

std::uint64_t g_checks = 0;

void require(const bool condition, const std::string_view message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class SplitMix64 {
 public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Independent minimal-window algorithm.
//
// A minimal covering interval can always be slid forward until its start
// coincides with a member of the set, so trying every member as a start and
// keeping the smallest resulting span is exhaustive. This is O(m^2) and shares
// no logic with the cyclic-gap implementation, so a defect cannot hide in both.
// ---------------------------------------------------------------------------
[[nodiscard]] Window brute_force_window(const std::vector<std::uint32_t>& values) {
  Window best;
  if (values.empty()) {
    return best;
  }
  best.valid = true;
  bool have = false;
  for (const std::uint32_t start : values) {
    std::uint32_t span = 0U;
    for (const std::uint32_t value : values) {
      span = std::max(span, normalize(value, start));
    }
    if (!have || span < best.span || (span == best.span && start < best.start)) {
      best.start = start;
      best.span = span;
      have = true;
    }
  }
  best.slots = best.span + 1U;
  best.end = (best.start + best.span) & kResidualMask;
  best.wraps = best.end < best.start;
  return best;
}

[[nodiscard]] Window window_of(std::vector<std::uint32_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return minimal_circular_window(values.data(), values.size());
}

void check_set(const std::vector<std::uint32_t>& raw, const std::string_view label) {
  std::vector<std::uint32_t> values = raw;
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  const Window actual = minimal_circular_window(values.data(), values.size());
  const Window expected = brute_force_window(values);
  require(actual.valid == expected.valid, std::string(label) + ": validity");
  require(actual.span == expected.span, std::string(label) + ": span");
  require(actual.slots == expected.slots, std::string(label) + ": slots");
  // Coverage is the property that matters; a tie in start is permitted only if
  // the window still covers every member.
  for (const std::uint32_t value : values) {
    require(normalize(value, actual.start) < actual.slots,
            std::string(label) + ": member outside reported window");
  }
  require(actual.wraps == (actual.end < actual.start), std::string(label) + ": wrap flag");
  require(actual.end == ((actual.start + actual.span) & kResidualMask),
          std::string(label) + ": end derived from start and span");
}

void minimal_window_matches_an_independent_algorithm() {
  const std::uint32_t last = kModulus - 1U;

  check_set({7U}, "single value");
  check_set({0U}, "single value at 0");
  check_set({last}, "single value at 2^19-1");
  check_set({0U, 1U}, "adjacent at the bottom");
  check_set({last - 1U, last}, "adjacent at the top");

  // Window starting exactly at 0 and ending exactly at 2^19-1.
  check_set({0U, 5U, 12U, 99U}, "window starting at 0");
  check_set({last - 40U, last - 3U, last}, "window ending at 2^19-1");

  // The decisive case: a set straddling the 2^19-1 -> 0 wrap is extremely local
  // even though its linear range is nearly the whole modulus.
  check_set({last - 2U, last - 1U, last, 0U, 1U, 2U}, "window crossing the wrap");
  {
    const Window window = window_of({last - 2U, last - 1U, last, 0U, 1U, 2U});
    require(window.slots == 6U, "wrap window is 6 slots, not 524286");
    require(window.start == last - 2U, "wrap window starts just after the largest gap");
    require(window.wraps, "wrap window is flagged as wrapping");
  }

  // A set at both extremes with nothing between: still minimal across the wrap.
  check_set({0U, last}, "two values at opposite ends");
  {
    const Window window = window_of({0U, last});
    require(window.slots == 2U, "0 and 2^19-1 are two adjacent slots on the ring");
  }

  // Maximum spread: three values a third of the ring apart.
  check_set({0U, kModulus / 3U, (2U * kModulus) / 3U}, "evenly spread triple");

  // Duplicates must not change the answer.
  check_set({5U, 5U, 5U, 9U, 9U}, "duplicate members");

  // Dense contiguous run and a full-capacity 64-slot run.
  {
    std::vector<std::uint32_t> run;
    for (std::uint32_t offset = 0U; offset < 64U; ++offset) {
      run.push_back((last - 31U + offset) & kResidualMask);
    }
    check_set(run, "64-slot run across the wrap");
    const Window window = window_of(run);
    require(window.slots == 64U, "contiguous 64-run occupies exactly 64 slots");
  }
}

void randomized_differential_window_sweep() {
  SplitMix64 generator(0x5EED0D1D0000ULL);
  for (std::uint32_t trial = 0U; trial < 4000U; ++trial) {
    const std::size_t count = 1U + static_cast<std::size_t>(generator.next() % 64U);
    // Alternate between values spread over the whole ring and values clustered
    // in a narrow band that is deliberately placed across the wrap.
    const bool clustered = (trial & 1U) == 0U;
    const std::uint32_t base = static_cast<std::uint32_t>(generator.next() & kResidualMask);
    const std::uint32_t width =
        clustered ? 1U + static_cast<std::uint32_t>(generator.next() % 4096U) : kModulus;
    std::vector<std::uint32_t> values;
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      values.push_back(
          (base + static_cast<std::uint32_t>(generator.next() % width)) & kResidualMask);
    }
    check_set(values, "randomized window");
  }
}

// ---------------------------------------------------------------------------
// Tile-level geometry.
// ---------------------------------------------------------------------------
[[nodiscard]] Tile make_tile(const std::vector<std::uint32_t>& xs,
                             const std::vector<std::uint32_t>& qs,
                             const std::uint32_t kappa) {
  Tile tile{};
  for (std::size_t index = 0U; index < xs.size(); ++index) {
    tile.x[index] = xs[index];
  }
  for (std::size_t index = 0U; index < qs.size(); ++index) {
    tile.q[index] = qs[index];
  }
  tile.x_count = static_cast<std::uint8_t>(xs.size());
  tile.q_count = static_cast<std::uint8_t>(qs.size());
  tile.kappa = static_cast<std::uint8_t>(kappa);
  tile.u = 0U;
  return tile;
}

void check_tile_geometry(const Tile& tile, const std::string_view label) {
  std::vector<std::uint32_t> sums;
  for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
    for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
      sums.push_back((tile.x[i] + tile.q[j] + tile.kappa) & kResidualMask);
    }
  }
  const Window exact = tile_exact_window(tile);
  const Window expected = brute_force_window(sums);
  require(exact.span == expected.span, std::string(label) + ": tile window span");

  // Contract 1: every valid sum normalizes into [0, slots).
  for (const std::uint32_t sum : sums) {
    require(normalize(sum, exact.start) < exact.slots,
            std::string(label) + ": sum outside the exact window");
  }

  // Contract 2: the source-box bound never underestimates, and covers the sums.
  const Window box = tile_source_box_window(tile);
  require(box.slots >= exact.slots, std::string(label) + ": box bound underestimates");
  for (const std::uint32_t sum : sums) {
    require(normalize(sum, box.start) <= box.span,
            std::string(label) + ": sum outside the source-box window");
  }
}

void tile_windows_and_box_bounds_hold() {
  const std::uint32_t last = kModulus - 1U;

  for (std::uint32_t kappa = 0U; kappa <= 1U; ++kappa) {
    const std::string suffix = " (kappa=" + std::to_string(kappa) + ")";

    check_tile_geometry(make_tile({0U}, {0U}, kappa), "single lane" + suffix);
    check_tile_geometry(make_tile({0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U},
                                  {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}, kappa),
                        "dense low tile" + suffix);
    check_tile_geometry(
        make_tile({last - 3U, last - 2U, last - 1U, last}, {0U, 1U, 2U, 3U}, kappa),
        "tile straddling the wrap" + suffix);
    check_tile_geometry(make_tile({last}, {last}, kappa), "double top value" + suffix);
    check_tile_geometry(make_tile({0U, 262144U}, {0U, 262144U}, kappa),
                        "half-modulus spread" + suffix);
    check_tile_geometry(make_tile({5U, 5U, 5U, 5U}, {9U, 9U, 9U, 9U}, kappa),
                        "duplicate x and q" + suffix);

    // Every partial dimension.
    for (std::uint32_t x_count = 1U; x_count <= kTileDim; ++x_count) {
      for (std::uint32_t q_count = 1U; q_count <= kTileDim; ++q_count) {
        std::vector<std::uint32_t> xs;
        std::vector<std::uint32_t> qs;
        for (std::uint32_t index = 0U; index < x_count; ++index) {
          xs.push_back((last - 100U + index * 7U) & kResidualMask);
        }
        for (std::uint32_t index = 0U; index < q_count; ++index) {
          qs.push_back((last - 50U + index * 11U) & kResidualMask);
        }
        check_tile_geometry(make_tile(xs, qs, kappa),
                            "partial " + std::to_string(x_count) + "x" +
                                std::to_string(q_count) + suffix);
      }
    }
  }

  SplitMix64 generator(0xA11CE0FDULL);
  for (std::uint32_t trial = 0U; trial < 3000U; ++trial) {
    const std::uint32_t x_count = 1U + static_cast<std::uint32_t>(generator.next() % kTileDim);
    const std::uint32_t q_count = 1U + static_cast<std::uint32_t>(generator.next() % kTileDim);
    const std::uint32_t base = static_cast<std::uint32_t>(generator.next() & kResidualMask);
    const std::uint32_t spread =
        1U + static_cast<std::uint32_t>(generator.next() % (kModulus / 8U));
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> qs;
    for (std::uint32_t index = 0U; index < x_count; ++index) {
      xs.push_back((base + static_cast<std::uint32_t>(generator.next() % spread)) &
                   kResidualMask);
    }
    for (std::uint32_t index = 0U; index < q_count; ++index) {
      qs.push_back((base + static_cast<std::uint32_t>(generator.next() % spread)) &
                   kResidualMask);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(qs.begin(), qs.end());
    check_tile_geometry(make_tile(xs, qs, generator.next() & 1U), "randomized tile");
  }
}

// ---------------------------------------------------------------------------
// FamilyIndex: window counting, and the separation of value membership from
// posting multiplicity. Losing multiplicity here would silently turn a
// value-existence question into a wrong triple count downstream.
// ---------------------------------------------------------------------------
void family_index_counts_windows_and_preserves_multiplicity() {
  const std::uint32_t last = kModulus - 1U;
  std::vector<std::uint32_t> values{10U, 10U, 10U, 25U, last, last, 0U, 300U};
  FamilyIndex index;
  index.build(values.data(), values.size());

  require(index.posting_total == values.size(), "posting total equals the posting count");
  require(index.distinct_total == 5U, "distinct total counts values, not postings");

  const auto linear_distinct = [&](const Window& window) {
    std::uint32_t seen = 0U;
    for (const std::uint32_t value : {0U, 10U, 25U, 300U, last}) {
      seen += window_contains(window, value) ? 1U : 0U;
    }
    return seen;
  };
  const auto linear_postings = [&](const Window& window) {
    std::uint32_t seen = 0U;
    for (const std::uint32_t value : values) {
      seen += window_contains(window, value) ? 1U : 0U;
    }
    return seen;
  };

  const std::array<Window, 8> windows{
      window_from_start_span(0U, 0U),          // single slot at 0
      window_from_start_span(10U, 0U),         // single slot on a triple posting
      window_from_start_span(0U, 25U),         // [0, 25]
      window_from_start_span(11U, 13U),        // strictly between members
      window_from_start_span(last, 1U),        // wraps: {last, 0}
      window_from_start_span(last - 5U, 20U),  // wraps across the top
      window_from_start_span(0U, kModulus - 1U),  // the whole ring
      window_from_start_span(301U, 100U),      // empty region
  };
  for (std::size_t index2 = 0U; index2 < windows.size(); ++index2) {
    const Window& window = windows[index2];
    require(index.distinct_in(window) == linear_distinct(window),
            "distinct_in matches a linear scan for window " + std::to_string(index2));
    require(index.postings_in(window) == linear_postings(window),
            "postings_in matches a linear scan for window " + std::to_string(index2));
  }

  // A value just outside either edge must not be counted.
  const Window tight = window_from_start_span(10U, 0U);
  require(index.distinct_in(tight) == 1U, "tight window sees exactly the value at its start");
  require(index.postings_in(tight) == 3U, "tight window preserves multiplicity 3");
  require(index.distinct_in(window_from_start_span(11U, 0U)) == 0U,
          "value just above the window edge is excluded");
  require(index.distinct_in(window_from_start_span(9U, 0U)) == 0U,
          "value just below the window edge is excluded");

  // Postings must remain reconstructible: the membership answer identifies the
  // value, and expanding that value back over the posting list reproduces its
  // exact multiplicity.
  std::uint32_t expanded = 0U;
  for (const std::uint32_t value : values) {
    expanded += value == 10U ? 1U : 0U;
  }
  require(expanded == index.postings_in(tight),
          "posting expansion reproduces the exact multiplicity");
}

void window_intersection_is_exact() {
  const std::uint32_t last = kModulus - 1U;
  const auto brute = [](const Window& a, const Window& b) {
    std::uint32_t seen = 0U;
    for (std::uint32_t offset = 0U; offset < a.slots; ++offset) {
      seen += window_contains(b, (a.start + offset) & kResidualMask) ? 1U : 0U;
    }
    return seen;
  };
  const std::array<Window, 6> windows{
      window_from_start_span(0U, 0U),      window_from_start_span(0U, 63U),
      window_from_start_span(32U, 63U),    window_from_start_span(last - 10U, 40U),
      window_from_start_span(last, 5U),    window_from_start_span(1000U, 255U)};
  for (const Window& a : windows) {
    for (const Window& b : windows) {
      require(window_intersection_slots(a, b) == brute(a, b),
              "window intersection matches a slot-by-slot count");
      require(window_intersection_slots(a, b) == window_intersection_slots(b, a),
              "window intersection is symmetric");
    }
  }
}

// A tile whose sums are all congruent mod 256, and a tile whose sums all live
// in one coarse block: neither may confuse the window computation, which is
// about numeric locality on the ring rather than about any bitmap layout.
void pathological_residue_patterns_do_not_confuse_the_window() {
  std::vector<std::uint32_t> xs;
  std::vector<std::uint32_t> qs;
  for (std::uint32_t index = 0U; index < kTileDim; ++index) {
    xs.push_back(index * 256U);
    qs.push_back(index * 256U * 8U);
  }
  check_tile_geometry(make_tile(xs, qs, 0U), "all sums congruent mod 256");

  std::vector<std::uint32_t> tight_x{0U, 1U, 2U, 3U};
  std::vector<std::uint32_t> tight_q{0U, 1U, 2U, 3U};
  const Tile tile = make_tile(tight_x, tight_q, 1U);
  check_tile_geometry(tile, "all sums inside one coarse block");
  require(tile_exact_window(tile).slots <= 8U,
          "a tile confined to one coarse block has a window of at most 8 slots");
}

void full_ring_bounds_and_window_identity() {
  const Window wide = window_from_start_span(123U, 2U * kModulus);
  require(wide.slots == kModulus, "a circular window cannot exceed one ring");
  require(window_identity(window_from_start_span(9U, 3U)) !=
              window_identity(window_from_start_span(9U, 4U)),
          "same start with different capacity is not the same window");
  for (std::uint32_t value = 0; value < kModulus; ++value) {
    require(window_contains(wide, value), "full-ring bound covers every residue");
  }
}

void rectangle_rejection_matches_independent_truth() {
  SplitMix64 rng(0xFACE1977ULL);
  for (std::uint32_t trial = 0; trial < 1200U; ++trial) {
    const std::uint32_t width = 1U << (4U + trial % 13U);
    const std::uint32_t xb = static_cast<std::uint32_t>(rng.next() % (kModulus / width)) * width;
    const std::uint32_t qb = static_cast<std::uint32_t>(rng.next() % (kModulus / width)) * width;
    std::vector<std::uint32_t> xs, qs;
    for (std::uint32_t i = 0; i < 1U + trial % 8U; ++i) {
      xs.push_back(xb + static_cast<std::uint32_t>(rng.next() % width));
      qs.push_back(qb + static_cast<std::uint32_t>(rng.next() % width));
    }
    std::sort(xs.begin(), xs.end());
    std::sort(qs.begin(), qs.end());
    const Tile tile = make_tile(xs, qs, trial % 2U);
    const Window window = bin_sum_window(xs.front(), xs.back(), qs.front(), qs.back(), tile.kappa);
    require(window.slots == tile_exact_window(tile).slots, "bin endpoints give exact sum window");
    std::vector<Posting> ys{{window.start, 71U}, {window.start, 72U}, {window.end, 73U}};
    if (trial % 3U == 0) ys = {{(window.start - 1U) & kResidualMask, 74U},
                              {(window.end + 1U) & kResidualMask, 75U}};
    TargetSet target;
    build_target(ys, target);
    const bool retained = range_nonempty(target.exact.data(), window.start, window.slots);
    const auto truth = brute_report8(tile, target.values.data(), target.values.size());
    require(retained || truth == 0U, "rectangle rejection never discards a true lane");
    bool expected = false;
    for (const Posting& y : ys) {
      const auto distance = (static_cast<std::uint64_t>(y.value) + kModulus - window.start) % kModulus;
      expected = expected || distance < window.slots;
    }
    require(retained == expected, "bitmap range query agrees with raw postings");
    for (const auto x : xs) for (const auto q : qs) {
      const auto sum = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) + q + tile.kappa) % kModulus);
      require(window_contains(window, sum), "every original rectangle lane is covered");
    }
  }
}

}  // namespace

int main() {
  std::cout << "windowing tests: modulus=" << kModulus << '\n';

  minimal_window_matches_an_independent_algorithm();
  randomized_differential_window_sweep();
  tile_windows_and_box_bounds_hold();
  family_index_counts_windows_and_preserves_multiplicity();
  window_intersection_is_exact();
  pathological_residue_patterns_do_not_confuse_the_window();
  full_ring_bounds_and_window_identity();
  rectangle_rejection_matches_independent_truth();

  std::cout << "windowing tests passed (" << g_checks << " assertions)\n";
  return 0;
}
