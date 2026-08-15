#pragma once

#include "ter/arithmetic.hpp"
#include "ter/instance.hpp"
#include "ter/reference_vector.hpp"
#include "ter/shape.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ter {

struct ReferenceEntry {
  EntryId id = 0;
  QuotientResidue q = 0;
  ReferenceTernaryVector digits;

  bool operator==(const ReferenceEntry&) const = default;
};

struct TerList {
  ListRole role = ListRole::A;
  std::uint64_t target_m2 = 0;
  std::vector<ReferenceEntry> entries;
  std::vector<std::uint64_t> residues_m1;
};

struct MergeSpec {
  FiniteTerShape shape{};
  MergeArithmetic arithmetic{};
  std::uint64_t memory_limit_bytes = 0;
};

struct InputSnapshot {
  Instance instance{};
  MergeSpec spec{};
  std::array<TerList, 3> lists{};
  bool has_planted_triple = false;
  std::array<EntryId, 3> planted_entry_ids{};
  std::array<ReferenceTernaryVector, 3> planted_vectors{};
};

struct OperationResult {
  Status status = Status::InvariantViolation;
  std::string message;
};

}  // namespace ter
