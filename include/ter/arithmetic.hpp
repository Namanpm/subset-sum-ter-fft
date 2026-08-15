#pragma once

#include "ter/types.hpp"

#include <cstdint>
#include <string>

namespace ter {

struct MergeArithmetic {
  std::uint32_t n = 0;
  std::uint8_t b2 = 0;
  std::uint8_t b1 = 0;
  std::uint64_t m2 = 0;
  std::uint64_t m1 = 0;
  std::uint64_t q = 0;
  std::uint64_t mask_m2 = 0;
  std::uint64_t mask_m1 = 0;
  std::uint64_t mask_q = 0;
  std::uint64_t target_a2 = 0;
  std::uint64_t target_b2 = 0;
  std::uint64_t target_c2 = 0;
  std::uint64_t target_1 = 0;
  QuotientResidue tau = 0;
};

struct ArithmeticResult {
  Status status = Status::InvalidConfiguration;
  MergeArithmetic value{};
  std::string message;
};

[[nodiscard]] ArithmeticResult build_arithmetic(std::uint32_t n,
                                                std::uint8_t b2,
                                                std::uint8_t b1,
                                                std::uint64_t target_a2,
                                                std::uint64_t target_b2,
                                                std::uint64_t target_1);

[[nodiscard]] QuotientResidue quotient_residue(std::uint64_t residue_m1,
                                               std::uint64_t child_target,
                                               const MergeArithmetic& arithmetic) noexcept;

[[nodiscard]] QuotientResidue pair_sum(QuotientResidue left,
                                       QuotientResidue right,
                                       const MergeArithmetic& arithmetic) noexcept;

[[nodiscard]] QuotientResidue required_pair_residue(
    QuotientResidue lookup,
    const MergeArithmetic& arithmetic) noexcept;

}  // namespace ter
