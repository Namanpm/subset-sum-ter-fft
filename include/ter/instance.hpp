#pragma once

#include "ter/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ter {

struct Instance {
  std::uint32_t n = 0;
  std::uint8_t b1 = 0;
  std::uint64_t seed = 0;
  std::uint32_t words_per_value = 0;
  std::vector<std::uint64_t> values_le;
  std::vector<std::uint64_t> residues_m1;
  std::vector<std::uint8_t> planted_root;
  std::vector<std::uint64_t> root_target_le;
};

struct InstanceResult {
  Status status = Status::InvalidConfiguration;
  Instance value{};
  std::string message;
};

[[nodiscard]] InstanceResult build_instance(std::uint32_t n,
                                            std::uint8_t b1,
                                            std::uint64_t seed);

}  // namespace ter
