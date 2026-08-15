#include "ter/validator.hpp"

#include "ter/reference_vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ter {

OperationResult audit_snapshot_children(const InputSnapshot& snapshot) {
  const auto& shape = snapshot.spec.shape;
  const auto& arithmetic = snapshot.spec.arithmetic;
  if (snapshot.instance.n != shape.n || arithmetic.n != shape.n ||
      snapshot.instance.residues_m1.size() != shape.n) {
    return {Status::InvariantViolation, "snapshot dimensions disagree"};
  }

  constexpr std::array<ListRole, 3> expected_roles{
      ListRole::A,
      ListRole::B,
      ListRole::C,
  };
  const std::array<std::uint64_t, 3> expected_targets{
      arithmetic.target_a2,
      arithmetic.target_b2,
      arithmetic.target_c2,
  };

  for (std::size_t list_index = 0; list_index < snapshot.lists.size(); ++list_index) {
    const auto& list = snapshot.lists[list_index];
    if (list.role != expected_roles[list_index] ||
        list.target_m2 != expected_targets[list_index] ||
        list.entries.size() != list.residues_m1.size()) {
      return {Status::InvariantViolation, "child list metadata disagrees with merge spec"};
    }

    for (std::size_t entry_index = 0; entry_index < list.entries.size(); ++entry_index) {
      const auto& entry = list.entries[entry_index];
      if (entry.id != entry_index || entry.digits.size() != shape.n) {
        return {Status::InvariantViolation, "child entry identity or length is invalid"};
      }
      const auto counts = count_digits(entry.digits);
      if (!counts.has_value() || counts->negative != shape.child.negative ||
          counts->zero != shape.child.zero || counts->positive != shape.child.positive ||
          !has_balanced_halves(entry.digits, shape.half)) {
        return {Status::InvariantViolation, "child entry violates TER distribution"};
      }
      const std::uint64_t audited_residue = inner_product_mod(
          snapshot.instance.residues_m1, entry.digits, arithmetic.mask_m1);
      if (audited_residue != list.residues_m1[entry_index] ||
          (audited_residue & arithmetic.mask_m2) != list.target_m2 ||
          quotient_residue(audited_residue, list.target_m2, arithmetic) != entry.q) {
        return {Status::InvariantViolation, "child entry arithmetic metadata is invalid"};
      }
      if (entry_index > 0U &&
          !(list.entries[entry_index - 1U].digits < entry.digits)) {
        return {Status::InvariantViolation,
                "child list is not strictly canonical or contains duplicates"};
      }
    }
  }
  return {Status::Completed, {}};
}

CandidateValidation audit_candidate(const InputSnapshot& snapshot,
                                    const ReferenceEntry& a,
                                    const ReferenceEntry& b,
                                    const ReferenceEntry& c) {
  CandidateValidation result;
  const auto& arithmetic = snapshot.spec.arithmetic;
  const auto& shape = snapshot.spec.shape;
  if (a.digits.size() != shape.n || b.digits.size() != shape.n ||
      c.digits.size() != shape.n) {
    return result;
  }

  const auto q_sum = static_cast<QuotientResidue>(
      (static_cast<std::uint64_t>(a.q) + b.q + c.q) & arithmetic.mask_q);
  if (q_sum != arithmetic.tau) {
    result.reject = RejectReason::Modular;
    return result;
  }

  result.parent.resize(shape.n);
  DigitCounts counts;
  for (std::size_t coordinate = 0; coordinate < shape.n; ++coordinate) {
    const auto da = a.digits[coordinate];
    const auto db = b.digits[coordinate];
    const auto dc = c.digits[coordinate];
    if (da < -1 || da > 1 || db < -1 || db > 1 || dc < -1 || dc > 1) {
      result.parent.clear();
      result.reject = RejectReason::Structural;
      return result;
    }
    const int sum = static_cast<int>(da) + static_cast<int>(db) + static_cast<int>(dc);
    if (sum < -1 || sum > 1) {
      result.parent.clear();
      result.reject = RejectReason::Structural;
      return result;
    }
    const auto parent_digit = static_cast<std::int8_t>(sum);
    result.parent[coordinate] = parent_digit;
    if (parent_digit == -1) {
      ++counts.negative;
    } else if (parent_digit == 1) {
      ++counts.positive;
    } else {
      ++counts.zero;
    }
  }

  if (counts.negative != shape.parent.negative || counts.zero != shape.parent.zero ||
      counts.positive != shape.parent.positive) {
    result.parent.clear();
    result.reject = RejectReason::Distribution;
    return result;
  }
  result.residue_m1 = inner_product_mod(snapshot.instance.residues_m1,
                                        result.parent,
                                        arithmetic.mask_m1);
  if (result.residue_m1 != arithmetic.target_1) {
    result.parent.clear();
    result.reject = RejectReason::Arithmetic;
    return result;
  }
  result.valid = true;
  result.reject = RejectReason::None;
  return result;
}

}  // namespace ter
