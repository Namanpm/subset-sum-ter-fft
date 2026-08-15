#pragma once

#include "ter/shape.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ter {

using ReferenceTernaryVector = std::vector<std::int8_t>;

[[nodiscard]] std::optional<DigitCounts> count_digits(
    std::span<const std::int8_t> digits) noexcept;

[[nodiscard]] bool has_balanced_halves(std::span<const std::int8_t> digits,
                                       const DigitCounts& expected_half) noexcept;

[[nodiscard]] std::uint64_t inner_product_mod(
    std::span<const std::uint64_t> residues,
    std::span<const std::int8_t> digits,
    std::uint64_t modulus_mask);

}  // namespace ter
