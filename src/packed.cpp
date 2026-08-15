#include "ter/packed.hpp"

#include "ter/reference_vector.hpp"
#include "ter/validator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>

namespace ter {

PackedSumWord add_packed_word(std::uint64_t a_positive,
                              std::uint64_t a_negative,
                              std::uint64_t b_positive,
                              std::uint64_t b_negative,
                              std::uint64_t c_positive,
                              std::uint64_t c_negative) noexcept {
  const std::uint64_t positive_any = a_positive | b_positive | c_positive;
  const std::uint64_t negative_any = a_negative | b_negative | c_negative;
  const std::uint64_t positive_two =
      (a_positive & b_positive) | (a_positive & c_positive) |
      (b_positive & c_positive);
  const std::uint64_t negative_two =
      (a_negative & b_negative) | (a_negative & c_negative) |
      (b_negative & c_negative);
  const std::uint64_t invalid =
      (positive_two & ~negative_any) | (negative_two & ~positive_any);
  const std::uint64_t exactly_one_positive = positive_any & ~positive_two;
  const std::uint64_t exactly_one_negative = negative_any & ~negative_two;
  const std::uint64_t parent_positive =
      (exactly_one_positive & ~negative_any) | (positive_two & negative_any);
  const std::uint64_t parent_negative =
      (exactly_one_negative & ~positive_any) | (negative_two & positive_any);
  return PackedSumWord{parent_positive, parent_negative, invalid};
}

ReferenceTernaryVector unpack_entry(const PackedTerList& list,
                                    EntryId id,
                                    std::uint32_t n) {
  if (id >= list.entries.size()) {
    throw std::out_of_range("packed TER entry ID is out of range");
  }
  ReferenceTernaryVector result(n, 0);
  const auto& entry = list.entries[id];
  for (std::uint32_t coordinate = 0; coordinate < n; ++coordinate) {
    const std::uint32_t word = coordinate / 64U;
    const std::uint32_t bit = coordinate % 64U;
    const std::uint64_t mask = std::uint64_t{1} << bit;
    const std::size_t offset = static_cast<std::size_t>(entry.word_offset) + word;
    if ((list.positive_words[offset] & mask) != 0U) {
      result[coordinate] = 1;
    } else if ((list.negative_words[offset] & mask) != 0U) {
      result[coordinate] = -1;
    }
  }
  return result;
}

PackedSnapshotResult pack_snapshot_impl(const InputSnapshot& snapshot) {
  PackedSnapshotResult result;
  const auto child_audit = audit_snapshot_children(snapshot);
  if (child_audit.status != Status::Completed) {
    result.status = child_audit.status;
    result.message = child_audit.message;
    return result;
  }

  result.snapshot.instance = snapshot.instance;
  result.snapshot.spec = snapshot.spec;
  result.snapshot.has_planted_triple = snapshot.has_planted_triple;
  result.snapshot.planted_entry_ids = snapshot.planted_entry_ids;
  const std::uint64_t words =
      (static_cast<std::uint64_t>(snapshot.spec.shape.n) + 63U) / 64U;
  if (words > std::numeric_limits<std::uint32_t>::max()) {
    result.status = Status::ResourceLimit;
    result.message = "packed vector width exceeds the V1 word-offset representation";
    return result;
  }
  const std::uint32_t words_per_entry = static_cast<std::uint32_t>(words);
  const std::uint32_t remainder = snapshot.spec.shape.n % 64U;
  const std::uint64_t valid_mask_last =
      remainder == 0U ? std::numeric_limits<std::uint64_t>::max()
                      : (std::uint64_t{1} << remainder) - 1U;

  for (std::size_t role = 0; role < snapshot.lists.size(); ++role) {
    const auto& source = snapshot.lists[role];
    auto& destination = result.snapshot.lists[role];
    const std::uint64_t total_words = words * source.entries.size();
    if (total_words > std::numeric_limits<std::uint32_t>::max() ||
        total_words > std::numeric_limits<std::size_t>::max()) {
      result.status = Status::ResourceLimit;
      result.message = "packed list exceeds the V1 word-offset representation";
      return result;
    }
    destination.role = source.role;
    destination.target_m2 = source.target_m2;
    destination.words_per_entry = words_per_entry;
    destination.valid_mask_last = valid_mask_last;
    destination.entries.reserve(source.entries.size());
    destination.positive_words.assign(static_cast<std::size_t>(total_words), 0U);
    destination.negative_words.assign(static_cast<std::size_t>(total_words), 0U);
    destination.residues_m1 = source.residues_m1;

    for (const auto& entry : source.entries) {
      const auto offset = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(entry.id) * words_per_entry);
      destination.entries.push_back(TerEntry{entry.id, entry.q, offset});
      for (std::uint32_t coordinate = 0; coordinate < snapshot.spec.shape.n;
           ++coordinate) {
        const std::uint32_t word = coordinate / 64U;
        const std::uint32_t bit = coordinate % 64U;
        const std::uint64_t mask = std::uint64_t{1} << bit;
        const std::size_t location = static_cast<std::size_t>(offset) + word;
        if (entry.digits[coordinate] == 1) {
          destination.positive_words[location] |= mask;
        } else if (entry.digits[coordinate] == -1) {
          destination.negative_words[location] |= mask;
        }
      }
    }
  }
  result.status = Status::Completed;
  return result;
}

PackedSnapshotResult pack_snapshot(const InputSnapshot& snapshot) {
  try {
    return pack_snapshot_impl(snapshot);
  } catch (const std::bad_alloc&) {
    PackedSnapshotResult result;
    result.status = Status::ResourceLimit;
    result.message = "packed snapshot allocation failed";
    return result;
  } catch (const std::length_error&) {
    PackedSnapshotResult result;
    result.status = Status::ResourceLimit;
    result.message = "packed snapshot allocation size is unsupported";
    return result;
  }
}

OperationResult audit_packed_snapshot(const PackedSnapshot& snapshot) {
  const auto& shape = snapshot.spec.shape;
  const auto& arithmetic = snapshot.spec.arithmetic;
  if (snapshot.instance.n != shape.n || arithmetic.n != shape.n ||
      snapshot.instance.residues_m1.size() != shape.n) {
    return {Status::InvariantViolation, "packed snapshot dimensions disagree"};
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
  const std::uint32_t expected_words = (shape.n + 63U) / 64U;
  const std::uint32_t remainder = shape.n % 64U;
  const std::uint64_t expected_last_mask =
      remainder == 0U ? std::numeric_limits<std::uint64_t>::max()
                      : (std::uint64_t{1} << remainder) - 1U;
  for (std::size_t list_index = 0; list_index < snapshot.lists.size(); ++list_index) {
    const auto& list = snapshot.lists[list_index];
    if (list.role != expected_roles[list_index] ||
        list.target_m2 != expected_targets[list_index] ||
        list.words_per_entry != expected_words ||
        list.valid_mask_last != expected_last_mask ||
        list.entries.size() != list.residues_m1.size() ||
        list.positive_words.size() != list.entries.size() * expected_words ||
        list.negative_words.size() != list.positive_words.size()) {
      return {Status::InvariantViolation, "packed list dimensions are invalid"};
    }
    ReferenceTernaryVector previous;
    for (std::size_t id = 0; id < list.entries.size(); ++id) {
      const auto& entry = list.entries[id];
      if (entry.id != id || entry.word_offset != id * expected_words) {
        return {Status::InvariantViolation, "packed entry identity or offset is invalid"};
      }
      for (std::uint32_t word = 0; word < expected_words; ++word) {
        const std::size_t offset = static_cast<std::size_t>(entry.word_offset) + word;
        if ((list.positive_words[offset] & list.negative_words[offset]) != 0U) {
          return {Status::InvariantViolation, "packed masks overlap"};
        }
        if (word + 1U == expected_words &&
            ((list.positive_words[offset] | list.negative_words[offset]) &
             ~list.valid_mask_last) != 0U) {
          return {Status::InvariantViolation, "packed masks set unused high bits"};
        }
      }
      auto digits = unpack_entry(list, entry.id, shape.n);
      const auto counts = count_digits(digits);
      if (!counts.has_value() || counts->negative != shape.child.negative ||
          counts->zero != shape.child.zero || counts->positive != shape.child.positive ||
          !has_balanced_halves(digits, shape.half)) {
        return {Status::InvariantViolation, "packed entry violates TER distribution"};
      }
      const std::uint64_t residue = inner_product_mod(
          snapshot.instance.residues_m1, digits, arithmetic.mask_m1);
      if (residue != list.residues_m1[id] ||
          (residue & arithmetic.mask_m2) != list.target_m2 ||
          quotient_residue(residue, list.target_m2, arithmetic) != entry.q) {
        return {Status::InvariantViolation, "packed entry arithmetic metadata is invalid"};
      }
      if (id > 0U && !(previous < digits)) {
        return {Status::InvariantViolation,
                "packed list is not strictly canonical or contains duplicates"};
      }
      previous = std::move(digits);
    }
  }
  return {Status::Completed, {}};
}

}  // namespace ter
