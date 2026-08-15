#include "ter/reference_vector.hpp"

#include <cstdint>
#include <stdexcept>

namespace ter {

std::optional<DigitCounts> count_digits(std::span<const std::int8_t> digits) noexcept {
  DigitCounts counts;
  for (const std::int8_t digit : digits) {
    switch (digit) {
      case -1:
        ++counts.negative;
        break;
      case 0:
        ++counts.zero;
        break;
      case 1:
        ++counts.positive;
        break;
      default:
        return std::nullopt;
    }
  }
  return counts;
}

bool has_balanced_halves(std::span<const std::int8_t> digits,
                         const DigitCounts& expected_half) noexcept {
  if (digits.size() % 2U != 0U || expected_half.total() != digits.size() / 2U) {
    return false;
  }
  const std::size_t half_size = digits.size() / 2U;
  const auto first = count_digits(digits.first(half_size));
  const auto second = count_digits(digits.subspan(half_size));
  const auto matches = [&expected_half](const std::optional<DigitCounts>& counts) {
    return counts.has_value() && counts->negative == expected_half.negative &&
           counts->zero == expected_half.zero && counts->positive == expected_half.positive;
  };
  return matches(first) && matches(second);
}

std::uint64_t inner_product_mod(std::span<const std::uint64_t> residues,
                                std::span<const std::int8_t> digits,
                                std::uint64_t modulus_mask) {
  if (residues.size() != digits.size()) {
    throw std::invalid_argument("inner product inputs must have identical lengths");
  }
  std::uint64_t sum = 0;
  for (std::size_t index = 0; index < digits.size(); ++index) {
    switch (digits[index]) {
      case -1:
        sum = (sum - residues[index]) & modulus_mask;
        break;
      case 0:
        break;
      case 1:
        sum = (sum + residues[index]) & modulus_mask;
        break;
      default:
        throw std::invalid_argument("inner product requires ternary digits");
    }
  }
  return sum;
}

}  // namespace ter
