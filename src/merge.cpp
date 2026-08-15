#include "ter/merge.hpp"

#include "ter/arithmetic.hpp"
#include "ter/packed.hpp"
#include "ter/reference_vector.hpp"
#include "ter/validator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ter {
namespace {

using Clock = std::chrono::steady_clock;

struct AcceptedCandidate {
  ReferenceTernaryVector parent;
  std::uint64_t residue_m1 = 0;
  Provenance provenance{};
};

struct PackedCandidateRecord {
  std::uint64_t mask_offset = 0;
  Provenance provenance{};
};

struct PackedCandidateBuffer {
  std::uint32_t words_per_parent = 0;
  std::vector<std::uint64_t> masks;
  std::vector<PackedCandidateRecord> records;
};

bool checked_add(std::uint64_t left,
                 std::uint64_t right,
                 std::uint64_t& result) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(std::uint64_t left,
                      std::uint64_t right,
                      std::uint64_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool checked_accumulate(std::uint64_t& total, std::uint64_t value) noexcept {
  return checked_add(total, value, total);
}

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void record_reject(MergeTelemetry& telemetry, RejectReason reason) {
  switch (reason) {
    case RejectReason::Structural:
    case RejectReason::InvalidInput:
      ++telemetry.structural_rejects;
      break;
    case RejectReason::Distribution:
      ++telemetry.distribution_rejects;
      break;
    case RejectReason::Arithmetic:
      ++telemetry.arithmetic_rejects;
      break;
    case RejectReason::None:
    case RejectReason::Modular:
      break;
  }
}

void finalize_output(std::vector<AcceptedCandidate> accepted,
                     OutputPolicy policy,
                     MergeOutput& output) {
  const auto start = Clock::now();
  std::sort(accepted.begin(), accepted.end(), [](const auto& left, const auto& right) {
    if (left.parent != right.parent) {
      return left.parent < right.parent;
    }
    return left.provenance < right.provenance;
  });

  if (policy == OutputPolicy::Correctness) {
    output.valid_provenance.reserve(accepted.size());
    for (const auto& candidate : accepted) {
      output.valid_provenance.push_back(candidate.provenance);
    }
    std::sort(output.valid_provenance.begin(), output.valid_provenance.end());
  }

  std::size_t begin = 0;
  while (begin < accepted.size()) {
    std::size_t end = begin + 1U;
    while (end < accepted.size() && accepted[end].parent == accepted[begin].parent) {
      ++end;
    }
    output.parents.push_back(ParentRecord{
        std::move(accepted[begin].parent),
        accepted[begin].residue_m1,
        static_cast<std::uint64_t>(end - begin),
    });
    begin = end;
  }
  output.telemetry.raw_valid_triples = accepted.size();
  output.telemetry.unique_parents = output.parents.size();
  output.telemetry.duplicate_parents =
      output.telemetry.raw_valid_triples - output.telemetry.unique_parents;
  for (const auto& parent : output.parents) {
    output.telemetry.output_bytes +=
        sizeof(ParentRecord) + parent.digits.size() * sizeof(std::int8_t);
  }
  output.telemetry.output_ns = elapsed_ns(start, Clock::now());
}

void validate_and_collect(const InputSnapshot& snapshot,
                          const ReferenceEntry& a,
                          const ReferenceEntry& b,
                          const ReferenceEntry& c,
                          Provenance provenance,
                          bool detailed_timing,
                          MergeOutput& output,
                          std::vector<AcceptedCandidate>& accepted) {
  ++output.telemetry.structural_checks;
  const auto validation_start = detailed_timing ? Clock::now() : Clock::time_point{};
  auto checked = audit_candidate(snapshot, a, b, c);
  if (detailed_timing) {
    output.telemetry.validation_ns += elapsed_ns(validation_start, Clock::now());
  }
  if (!checked.valid) {
    record_reject(output.telemetry, checked.reject);
    return;
  }
  accepted.push_back(AcceptedCandidate{
      std::move(checked.parent), checked.residue_m1, provenance});
}

std::span<const std::uint64_t> candidate_positive(const PackedCandidateBuffer& buffer,
                                                  const PackedCandidateRecord& record) {
  return std::span<const std::uint64_t>(buffer.masks)
      .subspan(static_cast<std::size_t>(record.mask_offset), buffer.words_per_parent);
}

std::span<const std::uint64_t> candidate_negative(const PackedCandidateBuffer& buffer,
                                                  const PackedCandidateRecord& record) {
  return std::span<const std::uint64_t>(buffer.masks)
      .subspan(static_cast<std::size_t>(record.mask_offset) + buffer.words_per_parent,
               buffer.words_per_parent);
}

ReferenceTernaryVector unpack_parent(const PackedCandidateBuffer& buffer,
                                     const PackedCandidateRecord& record,
                                     std::uint32_t n) {
  ReferenceTernaryVector digits(n, 0);
  const auto positive = candidate_positive(buffer, record);
  const auto negative = candidate_negative(buffer, record);
  for (std::uint32_t coordinate = 0; coordinate < n; ++coordinate) {
    const std::uint32_t word = coordinate / 64U;
    const std::uint32_t bit = coordinate % 64U;
    const std::uint64_t mask = std::uint64_t{1} << bit;
    if ((positive[word] & mask) != 0U) {
      digits[coordinate] = 1;
    } else if ((negative[word] & mask) != 0U) {
      digits[coordinate] = -1;
    }
  }
  return digits;
}

bool validate_packed_and_collect(const PackedSnapshot& snapshot,
                                 const TerEntry& a,
                                 const TerEntry& b,
                                 const TerEntry& c,
                                 Provenance provenance,
                                 bool detailed_timing,
                                 std::vector<std::uint64_t>& scratch_positive,
                                 std::vector<std::uint64_t>& scratch_negative,
                                 MergeOutput& output,
                                 PackedCandidateBuffer& accepted) {
  ++output.telemetry.structural_checks;
  const auto validation_start = detailed_timing ? Clock::now() : Clock::time_point{};
  const auto& arithmetic = snapshot.spec.arithmetic;
  const auto q_sum = static_cast<QuotientResidue>(
      (static_cast<std::uint64_t>(a.q) + b.q + c.q) & arithmetic.mask_q);
  if (q_sum != arithmetic.tau) {
    if (detailed_timing) {
      output.telemetry.validation_ns += elapsed_ns(validation_start, Clock::now());
    }
    return false;
  }

  const auto& a_list = snapshot.lists[0];
  const auto& b_list = snapshot.lists[1];
  const auto& c_list = snapshot.lists[2];
  std::uint64_t negative_count = 0;
  std::uint64_t positive_count = 0;
  for (std::uint32_t word = 0; word < a_list.words_per_entry; ++word) {
    const auto sum = add_packed_word(
        a_list.positive_words[static_cast<std::size_t>(a.word_offset) + word],
        a_list.negative_words[static_cast<std::size_t>(a.word_offset) + word],
        b_list.positive_words[static_cast<std::size_t>(b.word_offset) + word],
        b_list.negative_words[static_cast<std::size_t>(b.word_offset) + word],
        c_list.positive_words[static_cast<std::size_t>(c.word_offset) + word],
        c_list.negative_words[static_cast<std::size_t>(c.word_offset) + word]);
    const std::uint64_t valid_mask =
        word + 1U == a_list.words_per_entry ? a_list.valid_mask_last
                                           : std::numeric_limits<std::uint64_t>::max();
    if ((sum.invalid & valid_mask) != 0U) {
      ++output.telemetry.structural_rejects;
      if (detailed_timing) {
        output.telemetry.validation_ns += elapsed_ns(validation_start, Clock::now());
      }
      return false;
    }
    scratch_positive[word] = sum.positive & valid_mask;
    scratch_negative[word] = sum.negative & valid_mask;
    positive_count += static_cast<std::uint64_t>(std::popcount(scratch_positive[word]));
    negative_count += static_cast<std::uint64_t>(std::popcount(scratch_negative[word]));
  }
  if (negative_count != snapshot.spec.shape.parent.negative ||
      positive_count != snapshot.spec.shape.parent.positive) {
    ++output.telemetry.distribution_rejects;
    if (detailed_timing) {
      output.telemetry.validation_ns += elapsed_ns(validation_start, Clock::now());
    }
    return false;
  }
  const std::uint64_t offset = accepted.masks.size();
  accepted.records.push_back(PackedCandidateRecord{offset, provenance});
  accepted.masks.insert(
      accepted.masks.end(), scratch_positive.begin(), scratch_positive.end());
  accepted.masks.insert(
      accepted.masks.end(), scratch_negative.begin(), scratch_negative.end());
  if (detailed_timing) {
    output.telemetry.validation_ns += elapsed_ns(validation_start, Clock::now());
  }
  return true;
}

bool finalize_packed_output(PackedCandidateBuffer accepted,
                            const PackedSnapshot& snapshot,
                            OutputPolicy policy,
                            MergeOutput& output) {
  const auto start = Clock::now();
  std::sort(accepted.records.begin(),
            accepted.records.end(),
            [&accepted](const auto& left, const auto& right) {
    const auto left_negative = candidate_negative(accepted, left);
    const auto right_negative = candidate_negative(accepted, right);
    if (!std::equal(left_negative.begin(), left_negative.end(), right_negative.begin())) {
      return std::lexicographical_compare(left_negative.begin(),
                                          left_negative.end(),
                                          right_negative.begin(),
                                          right_negative.end());
    }
    const auto left_positive = candidate_positive(accepted, left);
    const auto right_positive = candidate_positive(accepted, right);
    if (!std::equal(left_positive.begin(), left_positive.end(), right_positive.begin())) {
      return std::lexicographical_compare(left_positive.begin(),
                                          left_positive.end(),
                                          right_positive.begin(),
                                          right_positive.end());
    }
    return left.provenance < right.provenance;
  });
  if (policy == OutputPolicy::Correctness) {
    output.valid_provenance.reserve(accepted.records.size());
    for (const auto& candidate : accepted.records) {
      output.valid_provenance.push_back(candidate.provenance);
    }
    std::sort(output.valid_provenance.begin(), output.valid_provenance.end());
  }

  std::size_t begin = 0;
  while (begin < accepted.records.size()) {
    std::size_t end = begin + 1U;
    const auto equal_parent = [&accepted](const auto& left, const auto& right) {
      const auto left_positive = candidate_positive(accepted, left);
      const auto right_positive = candidate_positive(accepted, right);
      const auto left_negative = candidate_negative(accepted, left);
      const auto right_negative = candidate_negative(accepted, right);
      return std::equal(left_positive.begin(), left_positive.end(), right_positive.begin()) &&
             std::equal(left_negative.begin(), left_negative.end(), right_negative.begin());
    };
    while (end < accepted.records.size() &&
           equal_parent(accepted.records[end], accepted.records[begin])) {
      ++end;
    }
    auto digits = unpack_parent(accepted, accepted.records[begin], snapshot.spec.shape.n);
    const std::uint64_t residue = inner_product_mod(snapshot.instance.residues_m1,
                                                    digits,
                                                    snapshot.spec.arithmetic.mask_m1);
    if (residue != snapshot.spec.arithmetic.target_1) {
      output.message = "unique packed parent failed independent M1 audit";
      return false;
    }
    output.parents.push_back(ParentRecord{
        std::move(digits), residue, static_cast<std::uint64_t>(end - begin)});
    begin = end;
  }
  std::sort(output.parents.begin(), output.parents.end(), [](const auto& left, const auto& right) {
    return left.digits < right.digits;
  });
  output.telemetry.raw_valid_triples = accepted.records.size();
  output.telemetry.unique_parents = output.parents.size();
  output.telemetry.duplicate_parents =
      output.telemetry.raw_valid_triples - output.telemetry.unique_parents;
  for (const auto& parent : output.parents) {
    output.telemetry.output_bytes +=
        sizeof(ParentRecord) + parent.digits.size() * sizeof(std::int8_t);
  }
  output.telemetry.output_ns = elapsed_ns(start, Clock::now());
  return true;
}

}  // namespace

PairOrientation choose_pair_orientation(const InputSnapshot& snapshot) noexcept {
  constexpr std::array<PairOrientation, 3> choices{
      PairOrientation{{0, 1}, 2},
      PairOrientation{{0, 2}, 1},
      PairOrientation{{1, 2}, 0},
  };
  PairOrientation best = choices[0];
  std::uint64_t best_cost = std::numeric_limits<std::uint64_t>::max();
  for (const auto& choice : choices) {
    const std::uint64_t left = snapshot.lists[choice.pair_roles[0]].entries.size();
    const std::uint64_t right = snapshot.lists[choice.pair_roles[1]].entries.size();
    const std::uint64_t cost = left * right;
    if (cost < best_cost ||
        (cost == best_cost && choice.pair_roles < best.pair_roles)) {
      best = choice;
      best_cost = cost;
    }
  }
  return best;
}

MergeOutput run_cubic_oracle_impl(const InputSnapshot& snapshot, OutputPolicy policy) {
  MergeOutput output;
  const auto input_audit = audit_snapshot_children(snapshot);
  if (input_audit.status != Status::Completed) {
    output.status = input_audit.status;
    output.message = input_audit.message;
    return output;
  }
  const std::uint64_t a_size = snapshot.lists[0].entries.size();
  const std::uint64_t b_size = snapshot.lists[1].entries.size();
  const std::uint64_t c_size = snapshot.lists[2].entries.size();
  std::uint64_t pair_work = 0;
  std::uint64_t triple_work = 0;
  constexpr std::uint64_t max_oracle_triples = 1'000'000U;
  if (!checked_multiply(a_size, b_size, pair_work) ||
      !checked_multiply(pair_work, c_size, triple_work) ||
      triple_work > max_oracle_triples) {
    output.status = Status::ResourceLimit;
    output.message = "cubic oracle is restricted to tiny bounded fixtures";
    return output;
  }
  std::uint64_t candidate_records = 0;
  std::uint64_t candidate_digits = 0;
  std::uint64_t output_records = 0;
  std::uint64_t provenance_records = 0;
  std::uint64_t working_budget = 0;
  if (!checked_multiply(triple_work, sizeof(AcceptedCandidate), candidate_records) ||
      !checked_multiply(triple_work, snapshot.spec.shape.n, candidate_digits) ||
      !checked_multiply(triple_work, sizeof(ParentRecord), output_records) ||
      (policy == OutputPolicy::Correctness &&
       !checked_multiply(triple_work, sizeof(Provenance), provenance_records)) ||
      !checked_accumulate(working_budget, candidate_records) ||
      !checked_accumulate(working_budget, candidate_digits) ||
      !checked_accumulate(working_budget, output_records) ||
      !checked_accumulate(working_budget, provenance_records) ||
      working_budget > snapshot.spec.memory_limit_bytes) {
    output.status = Status::ResourceLimit;
    output.message = "cubic oracle output reserve exceeds the declared memory limit";
    return output;
  }
  output.telemetry.candidate_buffer_bytes = candidate_records + candidate_digits;
  output.telemetry.working_set_budget_bytes = working_budget;
  const auto total_start = Clock::now();
  std::vector<AcceptedCandidate> accepted;
  accepted.reserve(static_cast<std::size_t>(triple_work));
  output.parents.reserve(static_cast<std::size_t>(triple_work));
  const auto& a_list = snapshot.lists[0].entries;
  const auto& b_list = snapshot.lists[1].entries;
  const auto& c_list = snapshot.lists[2].entries;
  const auto candidate_start = Clock::now();
  for (const auto& a : a_list) {
    for (const auto& b : b_list) {
      for (const auto& c : c_list) {
        const auto q_sum = static_cast<QuotientResidue>(
            (static_cast<std::uint64_t>(a.q) + b.q + c.q) &
            snapshot.spec.arithmetic.mask_q);
        if (q_sum != snapshot.spec.arithmetic.tau) {
          continue;
        }
        ++output.telemetry.candidate_triples;
        validate_and_collect(snapshot,
                             a,
                             b,
                             c,
                             Provenance{a.id, b.id, c.id},
                             policy == OutputPolicy::Correctness,
                             output,
                             accepted);
      }
    }
  }
  output.telemetry.candidate_loop_ns = elapsed_ns(candidate_start, Clock::now());
  finalize_output(std::move(accepted), policy, output);
  output.status = Status::Completed;
  output.telemetry.total_ns = elapsed_ns(total_start, Clock::now());
  return output;
}

MergeOutput run_pair_baseline_impl(const InputSnapshot& snapshot, OutputPolicy policy) {
  MergeOutput output;
  const auto input_audit = audit_snapshot_children(snapshot);
  if (input_audit.status != Status::Completed) {
    output.status = input_audit.status;
    output.message = input_audit.message;
    return output;
  }
  const auto total_start = Clock::now();
  const auto orientation_start = Clock::now();
  output.telemetry.orientation = choose_pair_orientation(snapshot);
  output.telemetry.orientation_ns = elapsed_ns(orientation_start, Clock::now());

  const std::uint8_t x_role = output.telemetry.orientation.pair_roles[0];
  const std::uint8_t y_role = output.telemetry.orientation.pair_roles[1];
  const std::uint8_t z_role = output.telemetry.orientation.lookup_role;
  const auto& x_list = snapshot.lists[x_role].entries;
  const auto& y_list = snapshot.lists[y_role].entries;
  const auto& z_list = snapshot.lists[z_role].entries;
  std::uint64_t pair_count = 0;
  if (!checked_multiply(static_cast<std::uint64_t>(x_list.size()),
                        static_cast<std::uint64_t>(y_list.size()),
                        pair_count)) {
    output.status = Status::ResourceLimit;
    output.message = "pair count overflow";
    return output;
  }
  const std::uint64_t offset_count = snapshot.spec.arithmetic.q + 1U;
  std::uint64_t offset_bytes = 0;
  std::uint64_t pair_record_bytes = 0;
  std::uint64_t pair_bytes = 0;
  std::uint64_t pair_build_peak = 0;
  if (offset_count > std::numeric_limits<std::size_t>::max() ||
      pair_count > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(offset_count, sizeof(std::uint64_t), offset_bytes) ||
      !checked_multiply(pair_count, sizeof(std::uint64_t), pair_record_bytes) ||
      !checked_add(offset_bytes, pair_record_bytes, pair_bytes) ||
      !checked_add(pair_bytes, offset_bytes, pair_build_peak)) {
    output.status = Status::ResourceLimit;
    output.message = "pair index dimensions overflow";
    return output;
  }
  if (pair_build_peak > snapshot.spec.memory_limit_bytes) {
    output.status = Status::ResourceLimit;
    output.message = "pair-index construction exceeds the declared memory limit";
    return output;
  }
  output.telemetry.working_set_budget_bytes = pair_build_peak;

  const auto pair_start = Clock::now();
  std::vector<std::uint64_t> offsets(static_cast<std::size_t>(offset_count), 0U);
  for (const auto& x : x_list) {
    for (const auto& y : y_list) {
      const QuotientResidue residue = pair_sum(x.q, y.q, snapshot.spec.arithmetic);
      ++offsets[static_cast<std::size_t>(residue) + 1U];
    }
  }
  for (std::size_t residue = 1; residue < offsets.size(); ++residue) {
    offsets[residue] += offsets[residue - 1U];
  }
  std::vector<std::uint64_t> pairs(static_cast<std::size_t>(pair_count));
  {
    auto cursor = offsets;
    for (const auto& x : x_list) {
      for (const auto& y : y_list) {
        const QuotientResidue residue = pair_sum(x.q, y.q, snapshot.spec.arithmetic);
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(x.id) << 32U) | y.id;
        pairs[static_cast<std::size_t>(cursor[residue]++)] = packed;
      }
    }
  }
  output.telemetry.pair_combinations = pair_count;
  output.telemetry.pair_records = pair_count;
  output.telemetry.pair_index_bytes = pair_bytes;
  output.telemetry.pair_build_ns = elapsed_ns(pair_start, Clock::now());

  std::uint64_t modular_candidate_count = 0;
  for (const auto& z : z_list) {
    const QuotientResidue required =
        required_pair_residue(z.q, snapshot.spec.arithmetic);
    const std::uint64_t bucket_size =
        offsets[static_cast<std::size_t>(required) + 1U] - offsets[required];
    if (!checked_accumulate(modular_candidate_count, bucket_size)) {
      output.status = Status::ResourceLimit;
      output.message = "modular candidate count overflow";
      return output;
    }
  }
  std::uint64_t candidate_records = 0;
  std::uint64_t candidate_digits = 0;
  std::uint64_t output_records = 0;
  std::uint64_t provenance_records = 0;
  std::uint64_t candidate_bytes = 0;
  std::uint64_t recovery_peak = pair_bytes;
  if (modular_candidate_count > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(modular_candidate_count,
                        sizeof(AcceptedCandidate),
                        candidate_records) ||
      !checked_multiply(modular_candidate_count,
                        snapshot.spec.shape.n,
                        candidate_digits) ||
      !checked_multiply(modular_candidate_count, sizeof(ParentRecord), output_records) ||
      (policy == OutputPolicy::Correctness &&
       !checked_multiply(modular_candidate_count,
                         sizeof(Provenance),
                         provenance_records)) ||
      !checked_add(candidate_records, candidate_digits, candidate_bytes) ||
      !checked_accumulate(recovery_peak, candidate_bytes) ||
      !checked_accumulate(recovery_peak, output_records) ||
      !checked_accumulate(recovery_peak, provenance_records) ||
      recovery_peak > snapshot.spec.memory_limit_bytes) {
    output.status = Status::ResourceLimit;
    output.message = "candidate and output reserve exceeds the declared memory limit";
    return output;
  }
  output.telemetry.candidate_buffer_bytes = candidate_bytes;
  output.telemetry.working_set_budget_bytes =
      std::max(output.telemetry.working_set_budget_bytes, recovery_peak);
  std::vector<AcceptedCandidate> accepted;
  accepted.reserve(static_cast<std::size_t>(modular_candidate_count));
  output.parents.reserve(static_cast<std::size_t>(modular_candidate_count));
  const auto recovery_start = Clock::now();
  for (const auto& z : z_list) {
    const QuotientResidue required =
        required_pair_residue(z.q, snapshot.spec.arithmetic);
    const std::uint64_t begin = offsets[required];
    const std::uint64_t end = offsets[static_cast<std::size_t>(required) + 1U];
    for (std::uint64_t pair_index = begin; pair_index < end; ++pair_index) {
      const std::uint64_t packed = pairs[static_cast<std::size_t>(pair_index)];
      const EntryId x_id = static_cast<EntryId>(packed >> 32U);
      const EntryId y_id = static_cast<EntryId>(packed);
      std::array<EntryId, 3> ids{};
      ids[x_role] = x_id;
      ids[y_role] = y_id;
      ids[z_role] = z.id;
      ++output.telemetry.candidate_triples;
      validate_and_collect(snapshot,
                           snapshot.lists[0].entries[ids[0]],
                           snapshot.lists[1].entries[ids[1]],
                           snapshot.lists[2].entries[ids[2]],
                           Provenance{ids[0], ids[1], ids[2]},
                           policy == OutputPolicy::Correctness,
                           output,
                           accepted);
    }
  }
  if (output.telemetry.candidate_triples != modular_candidate_count) {
    output.status = Status::InvariantViolation;
    output.message = "scalar recovery did not conserve modular candidates";
    return output;
  }
  const std::uint64_t recovery_and_validation = elapsed_ns(recovery_start, Clock::now());
  output.telemetry.candidate_loop_ns = recovery_and_validation;
  if (policy == OutputPolicy::Correctness) {
    output.telemetry.recovery_ns =
        recovery_and_validation > output.telemetry.validation_ns
            ? recovery_and_validation - output.telemetry.validation_ns
            : 0U;
  }
  finalize_output(std::move(accepted), policy, output);
  output.status = Status::Completed;
  output.telemetry.total_ns = elapsed_ns(total_start, Clock::now());
  return output;
}

MergeOutput run_pair_baseline_packed_impl(const PackedSnapshot& snapshot,
                                          OutputPolicy policy) {
  MergeOutput output;
  const auto input_audit = audit_packed_snapshot(snapshot);
  if (input_audit.status != Status::Completed) {
    output.status = input_audit.status;
    output.message = input_audit.message;
    return output;
  }
  const auto total_start = Clock::now();
  const auto orientation_start = Clock::now();
  constexpr std::array<PairOrientation, 3> choices{
      PairOrientation{{0, 1}, 2},
      PairOrientation{{0, 2}, 1},
      PairOrientation{{1, 2}, 0},
  };
  std::uint64_t best_cost = std::numeric_limits<std::uint64_t>::max();
  for (const auto& choice : choices) {
    const std::uint64_t cost =
        static_cast<std::uint64_t>(snapshot.lists[choice.pair_roles[0]].entries.size()) *
        snapshot.lists[choice.pair_roles[1]].entries.size();
    if (cost < best_cost ||
        (cost == best_cost && choice.pair_roles < output.telemetry.orientation.pair_roles)) {
      output.telemetry.orientation = choice;
      best_cost = cost;
    }
  }
  output.telemetry.orientation_ns = elapsed_ns(orientation_start, Clock::now());

  const std::uint8_t x_role = output.telemetry.orientation.pair_roles[0];
  const std::uint8_t y_role = output.telemetry.orientation.pair_roles[1];
  const std::uint8_t z_role = output.telemetry.orientation.lookup_role;
  const auto& x_list = snapshot.lists[x_role].entries;
  const auto& y_list = snapshot.lists[y_role].entries;
  const auto& z_list = snapshot.lists[z_role].entries;
  std::uint64_t pair_count = 0;
  if (!checked_multiply(static_cast<std::uint64_t>(x_list.size()),
                        static_cast<std::uint64_t>(y_list.size()),
                        pair_count)) {
    output.status = Status::ResourceLimit;
    output.message = "packed pair count overflow";
    return output;
  }
  const std::uint64_t offset_count = snapshot.spec.arithmetic.q + 1U;
  std::uint64_t offset_bytes = 0;
  std::uint64_t pair_record_bytes = 0;
  std::uint64_t pair_bytes = 0;
  std::uint64_t pair_build_peak = 0;
  if (offset_count > std::numeric_limits<std::size_t>::max() ||
      pair_count > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(offset_count, sizeof(std::uint64_t), offset_bytes) ||
      !checked_multiply(pair_count, sizeof(std::uint64_t), pair_record_bytes) ||
      !checked_add(offset_bytes, pair_record_bytes, pair_bytes) ||
      !checked_add(pair_bytes, offset_bytes, pair_build_peak)) {
    output.status = Status::ResourceLimit;
    output.message = "packed pair index dimensions overflow";
    return output;
  }
  if (pair_build_peak > snapshot.spec.memory_limit_bytes) {
    output.status = Status::ResourceLimit;
    output.message = "packed pair-index construction exceeds the declared memory limit";
    return output;
  }
  output.telemetry.working_set_budget_bytes = pair_build_peak;

  const auto pair_start = Clock::now();
  std::vector<std::uint64_t> offsets(static_cast<std::size_t>(offset_count), 0U);
  for (const auto& x : x_list) {
    for (const auto& y : y_list) {
      const QuotientResidue residue = pair_sum(x.q, y.q, snapshot.spec.arithmetic);
      ++offsets[static_cast<std::size_t>(residue) + 1U];
    }
  }
  for (std::size_t residue = 1; residue < offsets.size(); ++residue) {
    offsets[residue] += offsets[residue - 1U];
  }
  std::vector<std::uint64_t> pairs(static_cast<std::size_t>(pair_count));
  {
    auto cursor = offsets;
    for (const auto& x : x_list) {
      for (const auto& y : y_list) {
        const QuotientResidue residue = pair_sum(x.q, y.q, snapshot.spec.arithmetic);
        pairs[static_cast<std::size_t>(cursor[residue]++)] =
            (static_cast<std::uint64_t>(x.id) << 32U) | y.id;
      }
    }
  }
  output.telemetry.pair_combinations = pair_count;
  output.telemetry.pair_records = pair_count;
  output.telemetry.pair_index_bytes = pair_bytes;
  output.telemetry.pair_build_ns = elapsed_ns(pair_start, Clock::now());

  const std::uint32_t words = snapshot.lists[0].words_per_entry;
  std::uint64_t modular_candidate_count = 0;
  for (const auto& z : z_list) {
    const QuotientResidue required =
        required_pair_residue(z.q, snapshot.spec.arithmetic);
    const std::uint64_t bucket_size =
        offsets[static_cast<std::size_t>(required) + 1U] - offsets[required];
    if (modular_candidate_count >
        std::numeric_limits<std::uint64_t>::max() - bucket_size) {
      output.status = Status::ResourceLimit;
      output.message = "packed modular candidate count overflow";
      return output;
    }
    modular_candidate_count += bucket_size;
  }
  const std::uint64_t words_per_candidate = static_cast<std::uint64_t>(words) * 2U;
  const std::uint64_t bytes_per_candidate =
      words_per_candidate * sizeof(std::uint64_t) + sizeof(PackedCandidateRecord);
  if (modular_candidate_count > std::numeric_limits<std::size_t>::max() ||
      (words_per_candidate != 0U &&
       modular_candidate_count >
           std::numeric_limits<std::size_t>::max() / words_per_candidate) ||
      (bytes_per_candidate != 0U &&
       modular_candidate_count >
           std::numeric_limits<std::uint64_t>::max() / bytes_per_candidate)) {
    output.status = Status::ResourceLimit;
    output.message = "packed candidate buffer dimensions overflow";
    return output;
  }
  const std::uint64_t candidate_bytes = modular_candidate_count * bytes_per_candidate;
  std::uint64_t output_records = 0;
  std::uint64_t output_digits = 0;
  std::uint64_t provenance_records = 0;
  std::uint64_t scratch_bytes = 0;
  std::uint64_t recovery_peak = pair_bytes;
  if (!checked_multiply(modular_candidate_count, sizeof(ParentRecord), output_records) ||
      !checked_multiply(modular_candidate_count,
                        snapshot.spec.shape.n,
                        output_digits) ||
      (policy == OutputPolicy::Correctness &&
       !checked_multiply(modular_candidate_count,
                         sizeof(Provenance),
                         provenance_records)) ||
      !checked_multiply(words_per_candidate, sizeof(std::uint64_t), scratch_bytes) ||
      !checked_accumulate(recovery_peak, candidate_bytes) ||
      !checked_accumulate(recovery_peak, output_records) ||
      !checked_accumulate(recovery_peak, output_digits) ||
      !checked_accumulate(recovery_peak, provenance_records) ||
      !checked_accumulate(recovery_peak, scratch_bytes) ||
      recovery_peak > snapshot.spec.memory_limit_bytes) {
    output.status = Status::ResourceLimit;
    output.message =
        "packed candidate and output reserve exceeds the declared memory limit";
    return output;
  }
  output.telemetry.candidate_buffer_bytes = candidate_bytes;
  output.telemetry.working_set_budget_bytes =
      std::max(output.telemetry.working_set_budget_bytes, recovery_peak);
  PackedCandidateBuffer accepted;
  accepted.words_per_parent = words;
  accepted.records.reserve(static_cast<std::size_t>(modular_candidate_count));
  accepted.masks.reserve(
      static_cast<std::size_t>(modular_candidate_count * words_per_candidate));
  output.parents.reserve(static_cast<std::size_t>(modular_candidate_count));
  std::vector<std::uint64_t> scratch_positive(words);
  std::vector<std::uint64_t> scratch_negative(words);
  const auto recovery_start = Clock::now();
  std::uint64_t recovered_candidates = 0;
  for (const auto& z : z_list) {
    const QuotientResidue required =
        required_pair_residue(z.q, snapshot.spec.arithmetic);
    const std::uint64_t begin = offsets[required];
    const std::uint64_t end = offsets[static_cast<std::size_t>(required) + 1U];
    for (std::uint64_t pair_index = begin; pair_index < end; ++pair_index) {
      const std::uint64_t packed_ids = pairs[static_cast<std::size_t>(pair_index)];
      std::array<EntryId, 3> ids{};
      ids[x_role] = static_cast<EntryId>(packed_ids >> 32U);
      ids[y_role] = static_cast<EntryId>(packed_ids);
      ids[z_role] = z.id;
      ++recovered_candidates;
      validate_packed_and_collect(snapshot,
                                  snapshot.lists[0].entries[ids[0]],
                                  snapshot.lists[1].entries[ids[1]],
                                  snapshot.lists[2].entries[ids[2]],
                                  Provenance{ids[0], ids[1], ids[2]},
                                  policy == OutputPolicy::Correctness,
                                  scratch_positive,
                                  scratch_negative,
                                  output,
                                  accepted);
    }
  }
  output.telemetry.candidate_triples = recovered_candidates;
  if (recovered_candidates != modular_candidate_count) {
    output.status = Status::InvariantViolation;
    output.message = "packed recovery did not conserve modular candidates";
    return output;
  }
  const std::uint64_t recovery_and_validation = elapsed_ns(recovery_start, Clock::now());
  output.telemetry.candidate_loop_ns = recovery_and_validation;
  if (policy == OutputPolicy::Correctness) {
    output.telemetry.recovery_ns =
        recovery_and_validation > output.telemetry.validation_ns
            ? recovery_and_validation - output.telemetry.validation_ns
            : 0U;
  }
  if (!finalize_packed_output(std::move(accepted), snapshot, policy, output)) {
    output.parents.clear();
    output.valid_provenance.clear();
    output.status = Status::InvariantViolation;
    return output;
  }
  output.status = Status::Completed;
  output.telemetry.total_ns = elapsed_ns(total_start, Clock::now());
  return output;
}

MergeOutput run_cubic_oracle(const InputSnapshot& snapshot, OutputPolicy policy) {
  try {
    return run_cubic_oracle_impl(snapshot, policy);
  } catch (const std::bad_alloc&) {
    MergeOutput output;
    output.status = Status::ResourceLimit;
    output.message = "cubic oracle allocation failed";
    return output;
  } catch (const std::length_error&) {
    MergeOutput output;
    output.status = Status::ResourceLimit;
    output.message = "cubic oracle allocation size is unsupported";
    return output;
  }
}

MergeOutput run_pair_baseline(const InputSnapshot& snapshot, OutputPolicy policy) {
  try {
    return run_pair_baseline_impl(snapshot, policy);
  } catch (const std::bad_alloc&) {
    MergeOutput output;
    output.status = Status::ResourceLimit;
    output.message = "pair baseline allocation failed";
    return output;
  } catch (const std::length_error&) {
    MergeOutput output;
    output.status = Status::ResourceLimit;
    output.message = "pair baseline allocation size is unsupported";
    return output;
  }
}

MergeOutput run_pair_baseline_packed(const PackedSnapshot& snapshot,
                                     OutputPolicy policy) {
  try {
    return run_pair_baseline_packed_impl(snapshot, policy);
  } catch (const std::bad_alloc&) {
    MergeOutput output;
    output.status = Status::ResourceLimit;
    output.message = "packed pair baseline allocation failed";
    return output;
  } catch (const std::length_error&) {
    MergeOutput output;
    output.status = Status::ResourceLimit;
    output.message = "packed pair baseline allocation size is unsupported";
    return output;
  }
}

}  // namespace ter
