#include "ter/shape.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ter {
namespace {

void add_pattern(std::vector<TernaryPatternCount>& patterns,
                 std::array<std::int8_t, 3> digits,
                 std::uint32_t count) {
  if (count != 0U) {
    patterns.push_back(TernaryPatternCount{digits, count});
  }
}

bool build_zero_parent_cancellations(std::uint32_t count,
                                     std::array<std::uint32_t, 3>& negative_degree,
                                     std::array<std::uint32_t, 3>& positive_degree,
                                     std::array<std::array<std::uint32_t, 3>, 3>& edges) {
  const std::uint32_t base = count / 3U;
  const std::uint32_t remainder = count % 3U;
  for (std::size_t child = 0; child < 3U; ++child) {
    negative_degree[child] = base + (child < remainder ? 1U : 0U);
    const std::size_t shifted = (child + 2U) % 3U;
    positive_degree[child] = base + (shifted < remainder ? 1U : 0U);
  }

  const auto r0 = static_cast<std::int64_t>(negative_degree[0]);
  const auto r1 = static_cast<std::int64_t>(negative_degree[1]);
  const auto r2 = static_cast<std::int64_t>(negative_degree[2]);
  const auto c0 = static_cast<std::int64_t>(positive_degree[0]);
  const auto c1 = static_cast<std::int64_t>(positive_degree[1]);
  const auto c2 = static_cast<std::int64_t>(positive_degree[2]);

  for (std::int64_t e01 = 0; e01 <= r0; ++e01) {
    const std::int64_t e02 = r0 - e01;
    const std::int64_t e21 = c1 - e01;
    const std::int64_t e20 = r2 - e21;
    const std::int64_t e10 = c0 - e20;
    const std::int64_t e12 = r1 - e10;
    if (e02 < 0 || e21 < 0 || e20 < 0 || e10 < 0 || e12 < 0) {
      continue;
    }
    if (e02 + e12 != c2) {
      continue;
    }
    edges = {};
    edges[0][1] = static_cast<std::uint32_t>(e01);
    edges[0][2] = static_cast<std::uint32_t>(e02);
    edges[1][0] = static_cast<std::uint32_t>(e10);
    edges[1][2] = static_cast<std::uint32_t>(e12);
    edges[2][0] = static_cast<std::uint32_t>(e20);
    edges[2][1] = static_cast<std::uint32_t>(e21);
    return true;
  }
  return false;
}

bool build_patterns_for_marginals(std::uint32_t length,
                                  std::uint32_t child_negative,
                                  std::uint32_t child_positive,
                                  std::uint32_t parent_negative,
                                  std::uint32_t parent_positive,
                                  std::vector<TernaryPatternCount>& patterns) {
  const std::int64_t cancellation_count =
      3 * static_cast<std::int64_t>(child_negative) -
      static_cast<std::int64_t>(parent_negative);
  if (static_cast<std::uint64_t>(parent_negative) + parent_positive > length) {
    return false;
  }
  const std::uint32_t parent_zero = length - parent_negative - parent_positive;
  if (cancellation_count < 0 ||
      cancellation_count > static_cast<std::int64_t>(parent_zero)) {
    return false;
  }

  const auto cancellations = static_cast<std::uint32_t>(cancellation_count);
  std::array<std::uint32_t, 3> negative_degree{};
  std::array<std::uint32_t, 3> positive_degree{};
  std::array<std::array<std::uint32_t, 3>, 3> edges{};
  if (!build_zero_parent_cancellations(
          cancellations, negative_degree, positive_degree, edges)) {
    return false;
  }

  for (std::size_t child = 0; child < 3U; ++child) {
    if (negative_degree[child] > child_negative ||
        positive_degree[child] > child_positive) {
      return false;
    }
  }

  patterns.clear();
  patterns.reserve(13U);

  for (std::size_t child = 0; child < 3U; ++child) {
    std::array<std::int8_t, 3> negative_pattern{};
    negative_pattern[child] = -1;
    add_pattern(patterns,
                negative_pattern,
                child_negative - negative_degree[child]);

    std::array<std::int8_t, 3> positive_pattern{};
    positive_pattern[child] = 1;
    add_pattern(patterns,
                positive_pattern,
                child_positive - positive_degree[child]);
  }

  for (std::size_t negative_child = 0; negative_child < 3U; ++negative_child) {
    for (std::size_t positive_child = 0; positive_child < 3U; ++positive_child) {
      if (negative_child == positive_child) {
        continue;
      }
      std::array<std::int8_t, 3> cancellation_pattern{};
      cancellation_pattern[negative_child] = -1;
      cancellation_pattern[positive_child] = 1;
      add_pattern(patterns, cancellation_pattern, edges[negative_child][positive_child]);
    }
  }

  const std::uint32_t used_zero_parent = cancellations;
  add_pattern(patterns,
              std::array<std::int8_t, 3>{0, 0, 0},
              parent_zero - used_zero_parent);

  std::sort(patterns.begin(), patterns.end(), [](const auto& left, const auto& right) {
    return left.digits < right.digits;
  });
  return true;
}

bool build_planted_patterns(FiniteTerShape& shape) {
  if (!build_patterns_for_marginals(shape.n,
                                    shape.child.negative,
                                    shape.child.positive,
                                    shape.parent.negative,
                                    shape.parent.positive,
                                    shape.planted_patterns)) {
    return false;
  }

  const std::uint32_t first_parent_negative = shape.parent.negative / 2U;
  const std::uint32_t first_parent_positive = first_parent_negative + shape.n / 12U;
  const std::uint32_t second_parent_negative =
      shape.parent.negative - first_parent_negative;
  const std::uint32_t second_parent_positive =
      shape.parent.positive - first_parent_positive;
  return build_patterns_for_marginals(shape.n / 2U,
                                      shape.half.negative,
                                      shape.half.positive,
                                      first_parent_negative,
                                      first_parent_positive,
                                      shape.planted_half_patterns[0]) &&
         build_patterns_for_marginals(shape.n / 2U,
                                      shape.half.negative,
                                      shape.half.positive,
                                      second_parent_negative,
                                      second_parent_positive,
                                      shape.planted_half_patterns[1]);
}

std::uint32_t nearest_even_w2(std::uint32_t n) {
  constexpr std::uint64_t denominator = 20'000U;
  const std::uint64_t numerator = 59U * static_cast<std::uint64_t>(n);
  const std::uint64_t half_down = (numerator + (denominator / 2U) - 1U) / denominator;
  return static_cast<std::uint32_t>(2U * half_down);
}

std::uint32_t nearest_w1(std::uint32_t n) {
  constexpr std::uint64_t denominator = 2'500U;
  const std::uint64_t numerator = 39U * static_cast<std::uint64_t>(n);
  return static_cast<std::uint32_t>(
      (numerator + (denominator / 2U) - 1U) / denominator);
}

bool assign_counts(FiniteTerShape& shape,
                   std::uint32_t child_negative,
                   std::uint32_t parent_negative) {
  const std::uint32_t child_positive = child_negative + shape.n / 18U;
  const std::uint32_t parent_positive = parent_negative + shape.n / 6U;
  if (child_negative % 2U != 0U ||
      static_cast<std::uint64_t>(child_negative) + child_positive > shape.n ||
      static_cast<std::uint64_t>(parent_negative) + parent_positive > shape.n) {
    return false;
  }

  shape.child = DigitCounts{
      child_negative,
      shape.n - child_negative - child_positive,
      child_positive,
  };
  shape.half = DigitCounts{
      child_negative / 2U,
      shape.n / 2U - child_negative / 2U - child_positive / 2U,
      child_positive / 2U,
  };
  shape.parent = DigitCounts{
      parent_negative,
      shape.n - parent_negative - parent_positive,
      parent_positive,
  };
  return shape.half.total() == shape.n / 2U;
}

}  // namespace

ShapeResult build_shape(ShapeFamily family, std::uint32_t n) {
  ShapeResult result;
  result.shape.family = family;
  result.shape.n = n;

  if (n == 0U || n % 36U != 0U) {
    result.status = Status::InvalidShape;
    result.message = "TER finite shapes require positive n divisible by 36";
    return result;
  }

  bool counts_valid = false;
  switch (family) {
    case ShapeFamily::FiniteSanity:
      counts_valid = assign_counts(result.shape, 2U, 2U);
      break;
    case ShapeFamily::PaperRounded:
      counts_valid = assign_counts(result.shape, nearest_even_w2(n), nearest_w1(n));
      break;
  }

  if (!counts_valid || !build_planted_patterns(result.shape)) {
    result.status = Status::InvalidShape;
    result.message = "finite TER marginals have no exact ternary parent decomposition";
    return result;
  }

  result.status = Status::Completed;
  return result;
}

}  // namespace ter
