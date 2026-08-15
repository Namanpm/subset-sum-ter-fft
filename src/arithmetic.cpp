#include "ter/arithmetic.hpp"

#include <cstdint>

namespace ter {

ArithmeticResult build_arithmetic(std::uint32_t n,
                                  std::uint8_t b2,
                                  std::uint8_t b1,
                                  std::uint64_t target_a2,
                                  std::uint64_t target_b2,
                                  std::uint64_t target_1) {
  ArithmeticResult result;
  const auto quotient_bits = static_cast<std::uint32_t>(b1) -
                             static_cast<std::uint32_t>(b2);
  if (b2 == 0U || b2 >= b1 || static_cast<std::uint32_t>(b1) >= n || b1 >= 64U ||
      quotient_bits > 32U) {
    result.message =
        "require 0 < b2 < b1 < min(n,64) and a quotient universe of at most 32 bits";
    return result;
  }

  auto& value = result.value;
  value.n = n;
  value.b2 = b2;
  value.b1 = b1;
  value.m2 = std::uint64_t{1} << b2;
  value.m1 = std::uint64_t{1} << b1;
  value.q = std::uint64_t{1} << quotient_bits;
  value.mask_m2 = value.m2 - 1U;
  value.mask_m1 = value.m1 - 1U;
  value.mask_q = value.q - 1U;
  value.target_a2 = target_a2 & value.mask_m2;
  value.target_b2 = target_b2 & value.mask_m2;
  value.target_1 = target_1 & value.mask_m1;
  value.target_c2 =
      (value.target_1 - value.target_a2 - value.target_b2) & value.mask_m2;
  const std::uint64_t quotient_target =
      (value.target_1 - value.target_a2 - value.target_b2 - value.target_c2) &
      value.mask_m1;
  value.tau = static_cast<QuotientResidue>(quotient_target >> b2);
  result.status = Status::Completed;
  return result;
}

QuotientResidue quotient_residue(std::uint64_t residue_m1,
                                 std::uint64_t child_target,
                                 const MergeArithmetic& arithmetic) noexcept {
  const std::uint64_t delta =
      (residue_m1 - child_target) & arithmetic.mask_m1;
  return static_cast<QuotientResidue>(delta >> arithmetic.b2);
}

QuotientResidue pair_sum(QuotientResidue left,
                         QuotientResidue right,
                         const MergeArithmetic& arithmetic) noexcept {
  return static_cast<QuotientResidue>(
      (static_cast<std::uint64_t>(left) + right) & arithmetic.mask_q);
}

QuotientResidue required_pair_residue(
    QuotientResidue lookup,
    const MergeArithmetic& arithmetic) noexcept {
  return static_cast<QuotientResidue>(
      (static_cast<std::uint64_t>(arithmetic.tau) - lookup) & arithmetic.mask_q);
}

}  // namespace ter
