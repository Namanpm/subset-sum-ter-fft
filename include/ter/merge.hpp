#pragma once

#include "ter/model.hpp"
#include "ter/packed.hpp"
#include "ter/telemetry.hpp"

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace ter {

enum class OutputPolicy : std::uint8_t {
  Correctness,
  Benchmark,
};

struct Provenance {
  EntryId a = 0;
  EntryId b = 0;
  EntryId c = 0;

  auto operator<=>(const Provenance&) const = default;
};

struct ParentRecord {
  ReferenceTernaryVector digits;
  std::uint64_t residue_m1 = 0;
  std::uint64_t multiplicity = 0;

  bool operator==(const ParentRecord&) const = default;
};

struct MergeOutput {
  Status status = Status::InvariantViolation;
  std::vector<ParentRecord> parents;
  std::vector<Provenance> valid_provenance;
  MergeTelemetry telemetry{};
  std::string message;
};

[[nodiscard]] PairOrientation choose_pair_orientation(const InputSnapshot& snapshot) noexcept;

[[nodiscard]] MergeOutput run_cubic_oracle(const InputSnapshot& snapshot,
                                           OutputPolicy policy);

[[nodiscard]] MergeOutput run_pair_baseline(const InputSnapshot& snapshot,
                                            OutputPolicy policy);

// Exact cache-tiled reference merge.  Locates the same arithmetic candidates as
// run_pair_baseline without a Q-wide offset table or an |X||Y| pair-record
// array, then reuses the same validators and finalizer, so its parents,
// multiplicities and provenance are identical.
[[nodiscard]] MergeOutput run_pair_tiled(const InputSnapshot& snapshot,
                                         OutputPolicy policy);

[[nodiscard]] MergeOutput run_pair_baseline_packed(const PackedSnapshot& snapshot,
                                                   OutputPolicy policy);

// Packed counterpart of run_pair_tiled.  It preserves the existing packed
// validator, canonicalizer, provenance and independent parent-residue audit.
[[nodiscard]] MergeOutput run_pair_tiled_packed(const PackedSnapshot& snapshot,
                                                OutputPolicy policy);

}  // namespace ter
