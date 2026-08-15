#pragma once

#include "ter/types.hpp"

#include <array>
#include <cstdint>

namespace ter {

struct PairOrientation {
  std::array<std::uint8_t, 2> pair_roles{0, 1};
  std::uint8_t lookup_role = 2;
};

struct MergeTelemetry {
  PairOrientation orientation{};
  std::uint64_t pair_combinations = 0;
  std::uint64_t pair_records = 0;
  std::uint64_t candidate_triples = 0;
  std::uint64_t structural_checks = 0;
  std::uint64_t structural_rejects = 0;
  std::uint64_t distribution_rejects = 0;
  std::uint64_t arithmetic_rejects = 0;
  std::uint64_t raw_valid_triples = 0;
  std::uint64_t unique_parents = 0;
  std::uint64_t duplicate_parents = 0;
  std::uint64_t pair_index_bytes = 0;
  std::uint64_t candidate_buffer_bytes = 0;
  std::uint64_t output_bytes = 0;
  std::uint64_t working_set_budget_bytes = 0;
  std::uint64_t orientation_ns = 0;
  std::uint64_t pair_build_ns = 0;
  std::uint64_t candidate_loop_ns = 0;
  std::uint64_t recovery_ns = 0;
  std::uint64_t validation_ns = 0;
  std::uint64_t output_ns = 0;
  std::uint64_t total_ns = 0;
};

}  // namespace ter
