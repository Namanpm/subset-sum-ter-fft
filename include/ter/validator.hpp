#pragma once

#include "ter/model.hpp"

namespace ter {

enum class RejectReason : std::uint8_t {
  None,
  Modular,
  Structural,
  Distribution,
  Arithmetic,
  InvalidInput,
};

struct CandidateValidation {
  bool valid = false;
  RejectReason reject = RejectReason::InvalidInput;
  ReferenceTernaryVector parent;
  std::uint64_t residue_m1 = 0;
};

[[nodiscard]] OperationResult audit_snapshot_children(const InputSnapshot& snapshot);

[[nodiscard]] CandidateValidation audit_candidate(const InputSnapshot& snapshot,
                                                  const ReferenceEntry& a,
                                                  const ReferenceEntry& b,
                                                  const ReferenceEntry& c);

}  // namespace ter
