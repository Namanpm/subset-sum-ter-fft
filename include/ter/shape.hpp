#pragma once

#include "ter/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ter {

struct DigitCounts {
  std::uint32_t negative = 0;
  std::uint32_t zero = 0;
  std::uint32_t positive = 0;

  [[nodiscard]] constexpr std::uint64_t total() const noexcept {
    return static_cast<std::uint64_t>(negative) + zero + positive;
  }
};

struct TernaryPatternCount {
  std::array<std::int8_t, 3> digits{};
  std::uint32_t count = 0;
};

struct FiniteTerShape {
  ShapeFamily family = ShapeFamily::FiniteSanity;
  std::uint32_t n = 0;
  DigitCounts half{};
  DigitCounts child{};
  DigitCounts parent{};
  std::vector<TernaryPatternCount> planted_patterns;
  std::array<std::vector<TernaryPatternCount>, 2> planted_half_patterns;
};

struct ShapeResult {
  Status status = Status::InvalidShape;
  FiniteTerShape shape{};
  std::string message;
};

[[nodiscard]] ShapeResult build_shape(ShapeFamily family, std::uint32_t n);

}  // namespace ter
