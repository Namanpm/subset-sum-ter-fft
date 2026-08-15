#include "ter/shape.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expect_feasible_pattern(const ter::FiniteTerShape& shape) {
  std::uint64_t total = 0;
  std::array<std::uint64_t, 3> negatives{};
  std::array<std::uint64_t, 3> positives{};
  std::uint64_t parent_negative = 0;
  std::uint64_t parent_positive = 0;

  for (const auto& pattern : shape.planted_patterns) {
    total += pattern.count;
    int parent_digit = 0;
    for (std::size_t child = 0; child < 3; ++child) {
      const auto digit = pattern.digits[child];
      expect(digit >= -1 && digit <= 1, "feasibility pattern has ternary digits");
      negatives[child] += digit == -1 ? pattern.count : 0U;
      positives[child] += digit == 1 ? pattern.count : 0U;
      parent_digit += digit;
    }
    expect(parent_digit >= -1 && parent_digit <= 1,
           "feasibility pattern produces a ternary parent digit");
    parent_negative += parent_digit == -1 ? pattern.count : 0U;
    parent_positive += parent_digit == 1 ? pattern.count : 0U;
  }

  expect(total == shape.n, "feasibility pattern covers exactly n coordinates");
  for (std::size_t child = 0; child < 3; ++child) {
    expect(negatives[child] == shape.child.negative,
           "feasibility pattern has exact child negative count");
    expect(positives[child] == shape.child.positive,
           "feasibility pattern has exact child positive count");
  }
  expect(parent_negative == shape.parent.negative,
         "feasibility pattern has exact parent negative count");
  expect(parent_positive == shape.parent.positive,
         "feasibility pattern has exact parent positive count");
}

void finite_sanity_has_exact_counts() {
  const auto result = ter::build_shape(ter::ShapeFamily::FiniteSanity, 36);
  expect(result.status == ter::Status::Completed, "FINITE_SANITY n=36 is valid");
  expect(result.shape.child.negative == 2, "FINITE_SANITY child negative count is 2");
  expect(result.shape.child.positive == 4, "FINITE_SANITY child positive count is 4");
  expect(result.shape.child.zero == 30, "FINITE_SANITY child zero count is 30");
  expect(result.shape.half.negative == 1, "FINITE_SANITY half negative count is 1");
  expect(result.shape.half.positive == 2, "FINITE_SANITY half positive count is 2");
  expect(result.shape.parent.negative == 2, "FINITE_SANITY parent negative count is 2");
  expect(result.shape.parent.positive == 8, "FINITE_SANITY parent positive count is 8");
  expect_feasible_pattern(result.shape);
}

void invalid_shape_is_not_a_completed_empty_result() {
  const auto result = ter::build_shape(ter::ShapeFamily::FiniteSanity, 37);
  expect(result.status == ter::Status::InvalidShape,
         "n not divisible by 36 reports InvalidShape");
  expect(!result.message.empty(), "invalid shape carries a diagnostic");
}

void paper_rounded_uses_declared_rounding_and_exact_feasibility() {
  const auto result = ter::build_shape(ter::ShapeFamily::PaperRounded, 360);
  expect(result.status == ter::Status::Completed, "PAPER_ROUNDED n=360 is feasible");
  expect(result.shape.child.negative == 2, "PAPER_ROUNDED child negatives round to 2");
  expect(result.shape.child.positive == 22, "PAPER_ROUNDED child positives are 22");
  expect(result.shape.parent.negative == 6, "PAPER_ROUNDED parent negatives round to 6");
  expect(result.shape.parent.positive == 66, "PAPER_ROUNDED parent positives are 66");
  expect_feasible_pattern(result.shape);
}

}  // namespace

int main() {
  finite_sanity_has_exact_counts();
  invalid_shape_is_not_a_completed_empty_result();
  paper_rounded_uses_declared_rounding_and_exact_feasibility();

  if (failures != 0) {
    std::cerr << failures << " shape assertion(s) failed\n";
    return 1;
  }
  std::cout << "shape tests passed\n";
  return 0;
}
