// TER^2 translated-tag locator research spike: focused correctness tests.
//
// Every test compares against exhaustive scalar truth over the same inputs.  The
// truth here never uses the bit-sliced representation, the shifted views or the
// translation table: it recomputes (x + d) mod 2^19 and asks the group directory
// directly, so a defect in the fast machinery cannot hide in both.
//
// Coverage follows the task's required list: shift and carry boundaries, tag and
// position wrap, empty and degenerate families, duplicate residuals, all four
// primary/excess witness combinations, all-hit and no-hit geometries, a
// pathologically concentrated bucket geometry, an exhaustive addressing and
// carry identity over every (r, i), exhaustive shifted-view correctness, and
// exhaustive translation-table correctness.

#include "ter_microreport8_dataset.hpp"
#include "ter_translated_tag_dataset.hpp"
#include "ter_translated_tag_locator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace tt = ter::translated_tag;

std::uint64_t g_checks = 0U;
std::uint64_t g_failures = 0U;

void check(const bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    if (g_failures <= 40U) {
      std::cerr << "FAIL: " << what << '\n';
    }
  }
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

constexpr std::uint32_t kMask = tt::kResidualMask;

// Exhaustive truth for one query over one (X, Y) pair of families: every
// (x group, y group) whose exact residuals satisfy y == (x + d) mod 2^19.
[[nodiscard]] std::vector<std::uint64_t> truth_pairs(
    const std::vector<tt::ResidualGroup>& x_groups,
    const std::vector<tt::ResidualGroup>& y_groups, const std::uint32_t d) {
  std::vector<std::uint64_t> pairs;
  for (std::uint32_t x = 0U; x < x_groups.size(); ++x) {
    const std::uint32_t wanted = (x_groups[x].value + d) & kMask;
    for (std::uint32_t y = 0U; y < y_groups.size(); ++y) {
      if (y_groups[y].value == wanted) {
        pairs.push_back((static_cast<std::uint64_t>(x) << 32U) | y);
      }
    }
  }
  std::sort(pairs.begin(), pairs.end());
  return pairs;
}

struct Harness {
  std::unique_ptr<tt::SourceClass> source = std::make_unique<tt::SourceClass>();
  std::unique_ptr<tt::TargetClass> target = std::make_unique<tt::TargetClass>();
  tt::AlignedWords translation{tt::kTranslationWords};
  tt::AlignedWords views{tt::kShiftedViewWords};
  std::vector<std::uint32_t> hits = std::vector<std::uint32_t>(tt::kHitCapacity, 0U);

  void load(std::vector<std::uint32_t> x_values, std::vector<std::uint32_t> y_values) {
    std::sort(x_values.begin(), x_values.end());
    std::sort(y_values.begin(), y_values.end());
    tt::build_source_class(x_values.data(), x_values.size(), *source);
    tt::build_target_class(y_values.data(), y_values.size(), *target);
    tt::build_translation_table(*source, translation.data());
    tt::build_shifted_views(*target, views.data());
  }
};

// Runs one query three ways (scalar definition, portable bit slicing, AVX2) and
// then checks that primary plus the two complementary overflow loops cover the
// exhaustive truth exactly once each.
void run_query(Harness& harness, const std::uint32_t d, const bool avx2_usable,
               const std::string& label) {
  const tt::QueryPlan plan = tt::make_query_plan(d);
  const std::uint32_t portable_count =
      tt::route_query_portable(harness.source->occupancy.data(), harness.translation.data(),
                               harness.views.data(), plan, harness.hits.data());
  std::vector<std::uint32_t> portable(harness.hits.begin(),
                                      harness.hits.begin() + portable_count);
  std::sort(portable.begin(), portable.end());

  const std::vector<std::uint32_t> scalar =
      tt::route_query_scalar(*harness.source, *harness.target, d);
  check(portable == scalar, label + ": portable router disagrees with the scalar definition");

  if (avx2_usable) {
    const std::uint32_t avx2_count =
        tt::avx2::route_query(harness.source->occupancy.data(), harness.translation.data(),
                              harness.views.data(), plan, harness.hits.data());
    std::vector<std::uint32_t> avx2(harness.hits.begin(), harness.hits.begin() + avx2_count);
    std::sort(avx2.begin(), avx2.end());
    check(avx2 == portable, label + ": AVX2 router disagrees with the portable router");
  }

  // Primary coverage, decoded to (x group, y group).
  std::vector<std::uint64_t> coverage;
  for (const std::uint32_t packed : portable) {
    const std::uint32_t position = packed >> 2U;
    const std::uint32_t x_layer = (packed >> 1U) & 1U;
    const std::uint32_t y_layer = packed & 1U;
    const std::uint32_t x_group = harness.source->group_at[x_layer][position];
    const std::uint32_t y_group =
        harness.target->group_at[y_layer][(position + plan.r) & tt::kPositionMask];
    check(x_group != tt::kNoGroup, label + ": primary hit with no source group");
    check(y_group != tt::kNoGroup, label + ": primary hit with no target group");
    if (x_group == tt::kNoGroup || y_group == tt::kNoGroup) {
      continue;
    }
    check(((harness.source->groups[x_group].value + d) & kMask) ==
              harness.target->groups[y_group].value,
          label + ": primary emission violates exact residual equality");
    coverage.push_back((static_cast<std::uint64_t>(x_group) << 32U) | y_group);
  }
  const std::size_t primary_count = coverage.size();

  // Complement 1: (X_excess, Y_all).
  for (const std::uint32_t x_group : harness.source->excess) {
    const std::uint32_t value = (harness.source->groups[x_group].value + d) & kMask;
    const std::uint32_t y_group = tt::lookup_all(*harness.target, value);
    if (y_group != tt::kNoGroup) {
      coverage.push_back((static_cast<std::uint64_t>(x_group) << 32U) | y_group);
    }
  }
  // Complement 2: (X_primary, Y_excess).  Searching only the primary source
  // layers is what keeps (X_excess, Y_excess) from being counted twice.
  for (const std::uint32_t y_group : harness.target->excess) {
    const std::uint32_t value = (harness.target->groups[y_group].value - d) & kMask;
    const std::uint32_t x_group = tt::lookup_primary(*harness.source, value);
    if (x_group != tt::kNoGroup) {
      coverage.push_back((static_cast<std::uint64_t>(x_group) << 32U) | y_group);
    }
  }
  static_cast<void>(primary_count);

  std::sort(coverage.begin(), coverage.end());
  const auto duplicate = std::adjacent_find(coverage.begin(), coverage.end());
  check(duplicate == coverage.end(),
        label + ": primary and overflow coverage are not disjoint");

  const std::vector<std::uint64_t> expected =
      truth_pairs(harness.source->groups, harness.target->groups, d);
  check(coverage == expected, label + ": total coverage differs from exhaustive truth");
}

void run_case(const std::string& label, std::vector<std::uint32_t> x_values,
              std::vector<std::uint32_t> y_values, const std::vector<std::uint32_t>& queries,
              const bool avx2_usable) {
  Harness harness;
  harness.load(std::move(x_values), std::move(y_values));
  for (const std::uint32_t d : queries) {
    run_query(harness, d, avx2_usable, label + " d=" + std::to_string(d));
  }
}

[[nodiscard]] std::uint32_t compose(const std::uint32_t position, const std::uint32_t tag) noexcept {
  return (tag << tt::kPositionBits) | position;
}

// -------------------------------------------------------------------------
// Exhaustive addressing and carry identity for every (r, i).
// -------------------------------------------------------------------------
void test_addressing_identity() {
  for (std::uint32_t r = 0U; r < tt::kR; ++r) {
    const tt::QueryPlan plan = tt::make_query_plan(compose(r, 0U));
    check(plan.r == r, "query plan lost r");
    check(plan.delta == (r & 63U), "query plan lost delta");
    check(plan.word_offset == (r >> 6U), "query plan lost the word offset");
    for (std::uint32_t i = 0U; i < tt::kR; ++i) {
      const std::uint32_t expected_carry = (i + r) >= tt::kR ? 1U : 0U;
      const std::uint32_t slice = i / tt::kVectorBits;
      const std::uint32_t within = i % tt::kVectorBits;
      std::uint32_t observed_carry = 0U;
      if (plan.slice_mode[slice] == 0U) {
        observed_carry = 0U;
      } else if (plan.slice_mode[slice] == 1U) {
        observed_carry = 1U;
      } else {
        const std::uint64_t low = plan.low_mask[within / 64U];
        observed_carry = ((low >> (within % 64U)) & 1U) != 0U ? 0U : 1U;
      }
      if (observed_carry != expected_carry) {
        check(false, "carry identity broken at r=" + std::to_string(r) + " i=" +
                         std::to_string(i));
        return;
      }
    }
  }
  ++g_checks;  // the loop above only records failures
}

// -------------------------------------------------------------------------
// Exhaustive shifted-view correctness: for every r and every position, the view
// word must supply Y[(i + r) mod 2048] of the requested plane.
// -------------------------------------------------------------------------
void test_shifted_views() {
  std::uint64_t state = 0x5EED0D1D0000ULL;
  std::vector<std::uint32_t> values;
  for (std::uint32_t index = 0U; index < 600U; ++index) {
    values.push_back(static_cast<std::uint32_t>(next_random(state) & kMask));
  }
  std::sort(values.begin(), values.end());
  auto target = std::make_unique<tt::TargetClass>();
  tt::build_target_class(values.data(), values.size(), *target);
  tt::AlignedWords views(tt::kShiftedViewWords);
  tt::build_shifted_views(*target, views.data());

  std::uint64_t mismatches = 0U;
  for (std::uint32_t r = 0U; r < tt::kR; ++r) {
    const std::uint32_t delta = r & 63U;
    const std::uint32_t offset = r >> 6U;
    for (std::uint32_t layer = 0U; layer < tt::kLayers; ++layer) {
      for (std::uint32_t plane = 0U; plane < tt::kPlanes; ++plane) {
        const std::uint64_t* const view =
            views.data() + tt::shifted_view_index(delta, layer, plane) + offset;
        const std::uint64_t* const source =
            target->plane.data() + tt::target_plane_index(layer, plane);
        for (std::uint32_t i = 0U; i < tt::kR; ++i) {
          const std::uint32_t j = (i + r) & tt::kPositionMask;
          const std::uint32_t observed =
              static_cast<std::uint32_t>((view[i / 64U] >> (i % 64U)) & 1U);
          const std::uint32_t expected =
              static_cast<std::uint32_t>((source[j / 64U] >> (j % 64U)) & 1U);
          mismatches += observed != expected ? 1U : 0U;
        }
      }
    }
  }
  check(mismatches == 0U, "shifted views do not reproduce the rotated plane (" +
                              std::to_string(mismatches) + " mismatches)");
}

// -------------------------------------------------------------------------
// Exhaustive translation-table correctness against scalar exact tag arithmetic.
// -------------------------------------------------------------------------
void test_translation_table() {
  std::uint64_t state = 0xA11CE0FDULL;
  std::vector<std::uint32_t> values;
  for (std::uint32_t index = 0U; index < 700U; ++index) {
    values.push_back(static_cast<std::uint32_t>(next_random(state) & kMask));
  }
  std::sort(values.begin(), values.end());
  auto source = std::make_unique<tt::SourceClass>();
  tt::build_source_class(values.data(), values.size(), *source);
  tt::AlignedWords translation(tt::kTranslationWords);
  tt::build_translation_table(*source, translation.data());

  std::uint64_t mismatches = 0U;
  std::uint64_t occupied_mismatches = 0U;
  for (std::uint32_t layer = 0U; layer < tt::kLayers; ++layer) {
    for (std::uint32_t position = 0U; position < tt::kR; ++position) {
      const std::uint32_t group = source->group_at[layer][position];
      const std::uint32_t slice = position / tt::kVectorBits;
      const std::uint32_t within = position % tt::kVectorBits;
      const std::uint32_t word = within / 64U;
      const std::uint32_t offset = within % 64U;
      for (std::uint32_t alpha = 0U; alpha < tt::kH; ++alpha) {
        std::uint32_t observed = 0U;
        for (std::uint32_t bit = 0U; bit < tt::kTagBits; ++bit) {
          const std::uint64_t plane =
              translation.data()[tt::translation_index(alpha, layer, slice, bit) + word];
          observed |= static_cast<std::uint32_t>((plane >> offset) & 1U) << bit;
        }
        if (group == tt::kNoGroup) {
          // Unoccupied positions are excluded by the occupancy intersection, so
          // their tag bits are irrelevant, but a zero-tag class must translate
          // to zero rather than to garbage.
          mismatches += observed != 0U ? 1U : 0U;
        } else {
          const std::uint32_t tag = source->groups[group].value >> tt::kPositionBits;
          occupied_mismatches += observed != ((tag + alpha) & tt::kTagMask) ? 1U : 0U;
        }
      }
    }
  }
  check(occupied_mismatches == 0U,
        "translated tags disagree with scalar (z_x + alpha) mod 256 (" +
            std::to_string(occupied_mismatches) + " mismatches)");
  check(mismatches == 0U, "unoccupied positions acquired nonzero translated tags");
}

// -------------------------------------------------------------------------
// Focused geometry cases from the task's required list.
// -------------------------------------------------------------------------
void test_required_geometries(const bool avx2_usable) {
  const std::vector<std::uint32_t> boundary_shifts{0U,   1U,   63U,  64U,   65U,
                                                   127U, 128U, 255U, 256U,  257U,
                                                   511U, 512U, 1023U, 1024U, 2047U};
  const std::vector<std::uint32_t> boundary_tags{0U, 1U, 2U, 127U, 128U, 254U, 255U};
  std::vector<std::uint32_t> queries;
  for (const std::uint32_t r : boundary_shifts) {
    for (const std::uint32_t alpha : boundary_tags) {
      queries.push_back(compose(r, alpha));
    }
  }

  // A dense family across the whole universe.
  std::uint64_t state = 0xC0FFEEULL;
  std::vector<std::uint32_t> dense_x;
  std::vector<std::uint32_t> dense_y;
  for (std::uint32_t index = 0U; index < 500U; ++index) {
    dense_x.push_back(static_cast<std::uint32_t>(next_random(state) & kMask));
    dense_y.push_back(static_cast<std::uint32_t>(next_random(state) & kMask));
  }
  run_case("dense", dense_x, dense_y, queries, avx2_usable);

  // Empty families.
  run_case("empty_x", {}, dense_y, queries, avx2_usable);
  run_case("empty_y", dense_x, {}, queries, avx2_usable);
  run_case("empty_both", {}, {}, queries, avx2_usable);

  // Single value, and two values sharing one low position.
  run_case("single", {compose(0U, 0U)}, {compose(0U, 0U)}, queries, avx2_usable);
  run_case("two_same_position", {compose(7U, 3U), compose(7U, 9U)},
           {compose(7U, 3U), compose(7U, 9U)}, queries, avx2_usable);
  // Three values at one position: the third is excess on both sides, exercising
  // (X_excess, Y_excess), which must be reported exactly once.
  run_case("three_same_position", {compose(11U, 1U), compose(11U, 2U), compose(11U, 3U)},
           {compose(11U, 1U), compose(11U, 2U), compose(11U, 3U)}, queries, avx2_usable);

  // Position wrap 2047 -> 0 and tag wrap 255 -> 0, chosen so the witness only
  // exists if both wraps are handled.
  run_case("position_wrap", {compose(2047U, 5U)}, {compose(0U, 6U)},
           {compose(1U, 0U)}, avx2_usable);
  run_case("tag_wrap", {compose(3U, 255U)}, {compose(4U, 0U)}, {compose(1U, 0U)},
           avx2_usable);
  run_case("both_wrap", {compose(2047U, 255U)}, {compose(0U, 0U)}, {compose(1U, 0U)},
           avx2_usable);

  // All-hit geometry: Y is X translated by exactly d.
  {
    std::vector<std::uint32_t> x;
    for (std::uint32_t index = 0U; index < 400U; ++index) {
      x.push_back(static_cast<std::uint32_t>(next_random(state) & kMask));
    }
    const std::uint32_t d = compose(701U, 133U);
    std::vector<std::uint32_t> y;
    for (const std::uint32_t value : x) {
      y.push_back((value + d) & kMask);
    }
    run_case("all_hit", x, y, {d}, avx2_usable);
  }

  // No-hit geometry: disjoint tag ranges make every residual impossible.
  {
    std::vector<std::uint32_t> x;
    std::vector<std::uint32_t> y;
    for (std::uint32_t index = 0U; index < 300U; ++index) {
      x.push_back(compose(index, 0U));
      y.push_back(compose(index, 200U));
    }
    run_case("no_hit", x, y, {compose(0U, 1U), compose(5U, 3U)}, avx2_usable);
  }

  // Highly concentrated bucket geometry: one position holds 200 distinct tags,
  // so 198 of them are excess and only the complement can find them.
  {
    std::vector<std::uint32_t> x;
    std::vector<std::uint32_t> y;
    for (std::uint32_t tag = 0U; tag < 200U; ++tag) {
      x.push_back(compose(64U, tag));
      y.push_back(compose(64U, tag));
    }
    run_case("concentrated", x, y, queries, avx2_usable);
    run_case("concentrated_zero_shift", x, y, {0U, compose(0U, 1U), compose(0U, 255U)},
             avx2_usable);
  }

  // Large overflow: every position carries four values.
  {
    std::vector<std::uint32_t> x;
    std::vector<std::uint32_t> y;
    for (std::uint32_t position = 0U; position < 512U; ++position) {
      for (std::uint32_t tag = 0U; tag < 4U; ++tag) {
        x.push_back(compose(position, tag * 37U));
        y.push_back(compose(position, tag * 37U));
      }
    }
    run_case("large_overflow", x, y, {0U, compose(1U, 0U), compose(2047U, 255U)},
             avx2_usable);
  }

  // Duplicate residual values: grouping must collapse them into one group with a
  // posting span of the right size, and never into two groups.
  {
    std::vector<std::uint32_t> x{compose(5U, 7U), compose(5U, 7U), compose(5U, 7U),
                                 compose(9U, 1U)};
    std::vector<std::uint32_t> y{compose(6U, 7U), compose(6U, 7U)};
    Harness harness;
    harness.load(x, y);
    check(harness.source->groups.size() == 2U, "duplicate X residuals were not grouped");
    check(harness.source->groups[0].count == 3U ||
              harness.source->groups[1].count == 3U,
          "duplicate X residual group lost its posting multiplicity");
    check(harness.target->groups.size() == 1U, "duplicate Y residuals were not grouped");
    check(harness.target->groups[0].count == 2U,
          "duplicate Y residual group lost its posting multiplicity");
    run_query(harness, compose(1U, 0U), avx2_usable, "duplicates");
  }

  // Randomized differential sweep over mixed densities.
  for (std::uint32_t trial = 0U; trial < 200U; ++trial) {
    const std::uint32_t x_count = static_cast<std::uint32_t>(next_random(state) % 900U);
    const std::uint32_t y_count = static_cast<std::uint32_t>(next_random(state) % 900U);
    const std::uint32_t spread =
        static_cast<std::uint32_t>(1U + (next_random(state) % tt::kResidualUniverse));
    std::vector<std::uint32_t> x;
    std::vector<std::uint32_t> y;
    for (std::uint32_t index = 0U; index < x_count; ++index) {
      x.push_back(static_cast<std::uint32_t>(next_random(state) % spread) & kMask);
    }
    for (std::uint32_t index = 0U; index < y_count; ++index) {
      y.push_back(static_cast<std::uint32_t>(next_random(state) % spread) & kMask);
    }
    const std::uint32_t d = static_cast<std::uint32_t>(next_random(state) & kMask);
    run_case("random_" + std::to_string(trial), x, y, {d}, avx2_usable);
  }
}

// -------------------------------------------------------------------------
// Authentic end-to-end sample: a handful of real microproblems, checked against
// brute-force enumeration of the real posting pairs.
// -------------------------------------------------------------------------
void test_authentic_sample(const bool avx2_usable) {
  const auto data = ter::microreport8::build_dataset(ter::microreport8::kReferenceRootN,
                                                     ter::microreport8::kReferenceRootSeed,
                                                     false);
  if (!data->ok) {
    check(false, "authentic dataset build failed: " + data->message);
    return;
  }
  check(data->geometry_matches_reference, "authentic geometry drifted from the reference root");
  check(data->tau_low25 == 5005954U, "authentic tau_low25 drifted");

  auto source = std::make_unique<tt::SourceClass>();
  auto target = std::make_unique<tt::TargetClass>();
  tt::AlignedWords translation(tt::kTranslationWords);
  tt::AlignedWords views(tt::kShiftedViewWords);
  std::vector<std::uint32_t> hits(tt::kHitCapacity, 0U);

  const std::array<std::uint32_t, 4> sample_t{0U, 17U, 41U, 63U};
  const std::array<std::uint32_t, 4> sample_s{0U, 5U, 33U, 63U};
  for (const std::uint32_t t : sample_t) {
    std::vector<std::uint32_t> x_values;
    for (const auto& posting : data->x_family[t]) {
      x_values.push_back(posting.value);
    }
    tt::build_source_class(x_values.data(), x_values.size(), *source);
    tt::build_translation_table(*source, translation.data());
    for (const std::uint32_t s : sample_s) {
      const std::uint32_t u = (s + t) & 63U;
      const std::uint32_t kappa = (s + t) >> 6U;
      std::vector<std::uint32_t> y_values;
      for (const auto& posting : data->y_family[u]) {
        y_values.push_back(posting.value);
      }
      tt::build_target_class(y_values.data(), y_values.size(), *target);
      tt::build_shifted_views(*target, views.data());

      std::uint64_t located = 0U;
      std::uint64_t brute = 0U;
      const auto& qs = data->q_family[s];
      std::uint32_t previous = 0xFFFFFFFFU;
      for (const auto& posting : qs) {
        if (posting.value == previous) {
          continue;  // one query per distinct residual
        }
        previous = posting.value;
        const std::uint32_t d = (posting.value + kappa) & kMask;
        const tt::QueryPlan plan = tt::make_query_plan(d);
        std::uint32_t found = tt::route_query_portable(source->occupancy.data(),
                                                       translation.data(), views.data(), plan,
                                                       hits.data());
        if (avx2_usable) {
          std::vector<std::uint32_t> portable(hits.begin(), hits.begin() + found);
          std::sort(portable.begin(), portable.end());
          const std::uint32_t avx2_found =
              tt::avx2::route_query(source->occupancy.data(), translation.data(), views.data(),
                                    plan, hits.data());
          std::vector<std::uint32_t> avx2(hits.begin(), hits.begin() + avx2_found);
          std::sort(avx2.begin(), avx2.end());
          check(avx2 == portable, "authentic sample: AVX2 and portable routers disagree");
          found = avx2_found;
        }
        for (std::uint32_t index = 0U; index < found; ++index) {
          const std::uint32_t packed = hits[index];
          const std::uint32_t position = packed >> 2U;
          const std::uint32_t x_group = source->group_at[(packed >> 1U) & 1U][position];
          const std::uint32_t y_group =
              target->group_at[packed & 1U][(position + plan.r) & tt::kPositionMask];
          check(x_group != tt::kNoGroup && y_group != tt::kNoGroup,
                "authentic sample: primary hit without a directory entry");
          if (x_group == tt::kNoGroup || y_group == tt::kNoGroup) {
            continue;
          }
          check(((source->groups[x_group].value + d) & kMask) ==
                    target->groups[y_group].value,
                "authentic sample: primary emission violates residual equality");
          ++located;
        }
        for (const std::uint32_t x_group : source->excess) {
          const std::uint32_t value = (source->groups[x_group].value + d) & kMask;
          if (tt::lookup_all(*target, value) != tt::kNoGroup) {
            ++located;
          }
        }
        for (const std::uint32_t y_group : target->excess) {
          const std::uint32_t value = (target->groups[y_group].value - d) & kMask;
          if (tt::lookup_primary(*source, value) != tt::kNoGroup) {
            ++located;
          }
        }
        // Brute force over the same distinct residuals, bitmap-free.
        for (const auto& x_group : source->groups) {
          const std::uint32_t wanted = (x_group.value + d) & kMask;
          if (std::binary_search(y_values.begin(), y_values.end(), wanted)) {
            ++brute;
          }
        }
      }
      check(located == brute, "authentic sample (s=" + std::to_string(s) + ", t=" +
                                  std::to_string(t) + "): located " +
                                  std::to_string(located) + " group witnesses, brute force " +
                                  std::to_string(brute));
    }
  }
}

// -------------------------------------------------------------------------
// The authority tables must reproduce the donor's low-25 projection exactly.
// -------------------------------------------------------------------------
void test_authority_tables() {
  const auto data = ter::microreport8::build_dataset(ter::microreport8::kReferenceRootN,
                                                     ter::microreport8::kReferenceRootSeed,
                                                     false);
  if (!data->ok) {
    check(false, "authentic dataset build failed: " + data->message);
    return;
  }
  const tt::AuthorityTables tables = tt::build_authority_tables(
      ter::microreport8::kReferenceRootN, ter::microreport8::kReferenceRootSeed);
  check(tables.ok, "authority table build failed: " + tables.message);
  if (!tables.ok) {
    return;
  }
  check(tables.tau_low32 == data->tau_low32, "authority tau_low32 disagrees with the donor");
  check(tables.n == 72U, "authority n drifted");
  check(tables.residue_words == 3U, "authority residue width unexpected for n=72");
  check(tables.shape_available, "expected parent shape unavailable");

  const std::array<const std::vector<ter::TiledQEntry>*, 3> donor{
      &data->a_low25, &data->b_low25, &data->c_low25};
  std::uint64_t mismatches = 0U;
  for (std::size_t group = 0U; group < 3U; ++group) {
    check(tables.quotient[group].size() == donor[group]->size(),
          "authority list size disagrees with the donor");
    const std::size_t count = std::min(tables.quotient[group].size(), donor[group]->size());
    for (std::size_t index = 0U; index < count; ++index) {
      if ((tables.quotient[group][index] & ter::microreport8::kProjectionMask) !=
          (*donor[group])[index].q) {
        ++mismatches;
      }
    }
  }
  check(mismatches == 0U, "authority capture low-25 projection differs from the donor");
}

}  // namespace

int main() {
  const ter::microreport8::IsaReport isa = ter::microreport8::detect_isa();
  const bool avx2_usable = isa.usable && tt::avx2::translation_unit_is_avx2();
  std::cout << "translated-tag locator tests: isa_executed="
            << (avx2_usable ? "AVX2" : "portable") << '\n';

  test_addressing_identity();
  std::cout << "  addressing/carry identity done\n";
  test_shifted_views();
  std::cout << "  shifted views done\n";
  test_translation_table();
  std::cout << "  translation table done\n";
  test_required_geometries(avx2_usable);
  std::cout << "  required geometries done\n";
  test_authentic_sample(avx2_usable);
  std::cout << "  authentic sample done\n";
  test_authority_tables();
  std::cout << "  authority tables done\n";

  std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
  if (g_failures != 0U) {
    std::cerr << "translated-tag locator tests FAILED\n";
    return EXIT_FAILURE;
  }
  std::cout << "translated-tag locator tests passed\n";
  return EXIT_SUCCESS;
}
