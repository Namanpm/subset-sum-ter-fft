#include "ter/generator.hpp"

#include "ter/arithmetic.hpp"
#include "ter/limitations.hpp"
#include "ter/reference_vector.hpp"
#include "ter/validator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ter {
namespace {

using limits::checked::checked_accumulate;
using limits::checked::checked_add;
using limits::checked::checked_multiply;

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) noexcept {
    if (bound <= 1U) {
      return 0U;
    }
    const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
    for (;;) {
      const std::uint64_t value = next();
      if (value >= threshold) {
        return value % bound;
      }
    }
  }

 private:
  std::uint64_t state_;
};

struct HalfVector {
  ReferenceTernaryVector digits;
  std::uint64_t residue_m1 = 0;
  std::uint64_t residue_m2 = 0;
};

struct HalfResidueIndex {
  std::vector<std::uint32_t> offsets;
  std::vector<std::uint32_t> ids;
};

struct ListGenerationResult {
  Status status = Status::GenerationIncomplete;
  TerList list{};
  std::string message;
};

bool snapshot_resident_budget(std::uint32_t n,
                              std::uint32_t requested_list_size,
                              std::uint64_t& bytes) noexcept {
  const std::uint64_t words = (static_cast<std::uint64_t>(n) + 63U) / 64U;
  std::uint64_t instance_words = 0;
  std::uint64_t instance_word_bytes = 0;
  std::uint64_t child_entry_bytes = sizeof(ReferenceEntry) + sizeof(std::uint64_t);
  std::uint64_t all_child_entries = 0;
  bytes = sizeof(InputSnapshot);
  if (!checked_multiply(n, words, instance_words) ||
      !checked_multiply(instance_words,
                        sizeof(std::uint64_t),
                        instance_word_bytes) ||
      !checked_accumulate(bytes, instance_word_bytes) ||
      !checked_accumulate(bytes,
                          static_cast<std::uint64_t>(n) * sizeof(std::uint64_t)) ||
      !checked_accumulate(bytes,
                          static_cast<std::uint64_t>(n) * sizeof(std::uint8_t)) ||
      !checked_accumulate(bytes, words * sizeof(std::uint64_t)) ||
      !checked_accumulate(bytes, static_cast<std::uint64_t>(n) * 3U) ||
      !checked_accumulate(child_entry_bytes, n) ||
      !checked_multiply(requested_list_size,
                        child_entry_bytes,
                        all_child_entries) ||
      !checked_multiply(all_child_entries, 3U, all_child_entries) ||
      !checked_accumulate(bytes, all_child_entries)) {
    return false;
  }
  return true;
}

bool materialized_snapshot_bytes(const InputSnapshot& snapshot,
                                 std::uint64_t& bytes) noexcept {
  bytes = sizeof(InputSnapshot);
  const auto add_vector = [&bytes](std::size_t count, std::size_t element_size) {
    std::uint64_t vector_bytes = 0;
    return checked_multiply(static_cast<std::uint64_t>(count),
                            static_cast<std::uint64_t>(element_size),
                            vector_bytes) &&
           checked_accumulate(bytes, vector_bytes);
  };
  if (!add_vector(snapshot.instance.values_le.size(), sizeof(std::uint64_t)) ||
      !add_vector(snapshot.instance.residues_m1.size(), sizeof(std::uint64_t)) ||
      !add_vector(snapshot.instance.planted_root.size(), sizeof(std::uint8_t)) ||
      !add_vector(snapshot.instance.root_target_le.size(), sizeof(std::uint64_t))) {
    return false;
  }
  for (const auto& planted : snapshot.planted_vectors) {
    if (!add_vector(planted.size(), sizeof(std::int8_t))) {
      return false;
    }
  }
  for (const auto& list : snapshot.lists) {
    if (!add_vector(list.entries.size(), sizeof(ReferenceEntry)) ||
        !add_vector(list.residues_m1.size(), sizeof(std::uint64_t))) {
      return false;
    }
    for (const auto& entry : list.entries) {
      if (!add_vector(entry.digits.size(), sizeof(std::int8_t))) {
        return false;
      }
    }
  }
  return true;
}

std::uint64_t exhaustive_half_domain_size(const FiniteTerShape& shape) noexcept {
  const auto factorial = [](std::uint32_t value) {
    std::uint64_t result = 1U;
    for (std::uint32_t factor = 2U; factor <= value; ++factor) {
      result *= factor;
    }
    return result;
  };
  const std::uint32_t half_size = shape.n / 2U;
  return factorial(half_size) /
         (factorial(shape.half.negative) * factorial(shape.half.zero) *
          factorial(shape.half.positive));
}

bool mitm_memory_fits(const InputSnapshot& snapshot,
                      const SnapshotConfig& config,
                      std::uint32_t pool_size,
                      bool force_planted) noexcept {
  const std::uint64_t half_size = snapshot.spec.shape.n / 2U;
  const std::uint64_t pool_entries = pool_size;
  const std::uint64_t offset_count = snapshot.spec.arithmetic.m2 + 1U;
  constexpr std::uint64_t conservative_set_node_overhead = 96U;
  std::uint64_t half_record_bytes = sizeof(HalfVector);
  std::uint64_t sampled_node_bytes = sizeof(ReferenceTernaryVector) +
                                     conservative_set_node_overhead;
  if (!checked_accumulate(half_record_bytes, half_size) ||
      !checked_accumulate(sampled_node_bytes, half_size)) {
    return false;
  }
  std::uint64_t left_pool_bytes = 0;
  std::uint64_t right_generation_bytes = 0;
  std::uint64_t index_offset_bytes = 0;
  std::uint64_t index_id_bytes = 0;
  std::uint64_t pool_and_index_peak = 0;
  std::uint64_t prefix_bytes = 0;
  std::uint64_t output_entry_bytes = sizeof(ReferenceEntry) +
                                     sizeof(ReferenceTernaryVector) +
                                     sizeof(std::uint64_t);
  if (!checked_accumulate(output_entry_bytes,
                          static_cast<std::uint64_t>(snapshot.spec.shape.n) * 2U) ||
      !checked_multiply(pool_entries, half_record_bytes, left_pool_bytes) ||
      !checked_add(half_record_bytes, sampled_node_bytes, right_generation_bytes) ||
      !checked_multiply(pool_entries, right_generation_bytes, right_generation_bytes) ||
      !checked_accumulate(right_generation_bytes, left_pool_bytes) ||
      !checked_multiply(offset_count,
                        sizeof(std::uint32_t) * 2U,
                        index_offset_bytes) ||
      !checked_multiply(pool_entries, sizeof(std::uint32_t), index_id_bytes) ||
      !checked_add(left_pool_bytes, left_pool_bytes, pool_and_index_peak) ||
      !checked_accumulate(pool_and_index_peak, index_offset_bytes) ||
      !checked_accumulate(pool_and_index_peak, index_id_bytes) ||
      !checked_multiply(pool_entries + 1U, sizeof(std::uint64_t), prefix_bytes)) {
    return false;
  }
  std::uint64_t output_bytes = 0;
  std::uint64_t rank_set_bytes = 0;
  std::uint64_t resident_bytes = 0;
  if (!checked_multiply(config.requested_list_size,
                        output_entry_bytes,
                        output_bytes) ||
      !checked_multiply(config.requested_list_size,
                        sizeof(std::uint64_t) + 64U,
                        rank_set_bytes) ||
      (force_planted &&
       !checked_multiply(rank_set_bytes, 2U, rank_set_bytes)) ||
      !checked_accumulate(pool_and_index_peak, prefix_bytes) ||
      !checked_accumulate(pool_and_index_peak, output_bytes) ||
      !checked_accumulate(pool_and_index_peak, rank_set_bytes) ||
      !materialized_snapshot_bytes(snapshot, resident_bytes) ||
      !checked_accumulate(right_generation_bytes, resident_bytes) ||
      !checked_accumulate(pool_and_index_peak, resident_bytes)) {
    return false;
  }
  return std::max(right_generation_bytes, pool_and_index_peak) <=
         config.memory_limit_bytes;
}

std::uint64_t role_seed(std::uint64_t seed, std::size_t role, std::uint64_t lane) noexcept {
  SplitMix64 mixer(seed ^ (0x6a09e667f3bcc909ULL * (role + 1U)) ^ lane);
  return mixer.next();
}

std::array<ReferenceTernaryVector, 3> materialize_planted_vectors(
    const FiniteTerShape& shape) {
  std::array<ReferenceTernaryVector, 3> result;
  for (auto& vector : result) {
    vector.reserve(shape.n);
  }
  for (const auto& half_patterns : shape.planted_half_patterns) {
    for (const auto& pattern : half_patterns) {
      for (std::uint32_t copy = 0; copy < pattern.count; ++copy) {
        for (std::size_t child = 0; child < 3U; ++child) {
          result[child].push_back(pattern.digits[child]);
        }
      }
    }
  }
  return result;
}

ReferenceTernaryVector sample_half(std::uint32_t half_size,
                                   const DigitCounts& counts,
                                   SplitMix64& random) {
  ReferenceTernaryVector digits(half_size, 0);
  std::vector<std::uint32_t> positions(half_size);
  std::iota(positions.begin(), positions.end(), 0U);
  const std::uint32_t selected_count = counts.negative + counts.positive;
  for (std::uint32_t index = 0; index < selected_count; ++index) {
    const std::uint32_t remaining = half_size - index;
    const std::uint32_t offset = static_cast<std::uint32_t>(random.bounded(remaining));
    std::swap(positions[index], positions[index + offset]);
  }
  for (std::uint32_t index = 0; index < counts.negative; ++index) {
    digits[positions[index]] = -1;
  }
  for (std::uint32_t index = counts.negative; index < selected_count; ++index) {
    digits[positions[index]] = 1;
  }
  return digits;
}

HalfVector make_half_record(const InputSnapshot& snapshot,
                            std::span<const std::int8_t> digits,
                            std::size_t coordinate_offset) {
  const auto residues = std::span<const std::uint64_t>(snapshot.instance.residues_m1)
                            .subspan(coordinate_offset, digits.size());
  const std::uint64_t residue_m1 =
      inner_product_mod(residues, digits, snapshot.spec.arithmetic.mask_m1);
  return HalfVector{ReferenceTernaryVector(digits.begin(), digits.end()),
                    residue_m1,
                    residue_m1 & snapshot.spec.arithmetic.mask_m2};
}

bool next_combination(std::vector<std::uint32_t>& combination, std::uint32_t n) {
  if (combination.empty()) {
    return false;
  }
  for (std::size_t reverse = combination.size(); reverse > 0U; --reverse) {
    const std::size_t index = reverse - 1U;
    const std::uint32_t maximum =
        n - static_cast<std::uint32_t>(combination.size() - index);
    if (combination[index] < maximum) {
      ++combination[index];
      for (std::size_t tail = index + 1U; tail < combination.size(); ++tail) {
        combination[tail] = combination[tail - 1U] + 1U;
      }
      return true;
    }
  }
  return false;
}

template <typename Callback>
void for_each_combination(std::uint32_t n, std::uint32_t k, Callback&& callback) {
  if (k == 0U) {
    callback(std::span<const std::uint32_t>{});
    return;
  }
  if (k > n) {
    return;
  }
  std::vector<std::uint32_t> combination(k);
  std::iota(combination.begin(), combination.end(), 0U);
  do {
    callback(std::span<const std::uint32_t>(combination));
  } while (next_combination(combination, n));
}

std::vector<ReferenceTernaryVector> enumerate_half_digits(std::uint32_t half_size,
                                                          const DigitCounts& counts) {
  std::vector<ReferenceTernaryVector> result;
  for_each_combination(half_size, counts.negative, [&](std::span<const std::uint32_t> neg) {
    std::vector<std::uint32_t> available;
    available.reserve(half_size - counts.negative);
    std::size_t negative_cursor = 0;
    for (std::uint32_t coordinate = 0; coordinate < half_size; ++coordinate) {
      if (negative_cursor < neg.size() && neg[negative_cursor] == coordinate) {
        ++negative_cursor;
      } else {
        available.push_back(coordinate);
      }
    }
    for_each_combination(static_cast<std::uint32_t>(available.size()),
                         counts.positive,
                         [&](std::span<const std::uint32_t> pos_indexes) {
      ReferenceTernaryVector digits(half_size, 0);
      for (const std::uint32_t coordinate : neg) {
        digits[coordinate] = -1;
      }
      for (const std::uint32_t position : pos_indexes) {
        digits[available[position]] = 1;
      }
      result.push_back(std::move(digits));
    });
  });
  return result;
}

std::vector<HalfVector> make_exhaustive_half_pool(const InputSnapshot& snapshot,
                                                  std::size_t coordinate_offset) {
  const std::uint32_t half_size = snapshot.spec.shape.n / 2U;
  auto digits = enumerate_half_digits(half_size, snapshot.spec.shape.half);
  std::vector<HalfVector> result;
  result.reserve(digits.size());
  for (const auto& vector : digits) {
    result.push_back(make_half_record(snapshot, vector, coordinate_offset));
  }
  return result;
}

std::optional<std::vector<HalfVector>> make_sampled_half_pool(
    const InputSnapshot& snapshot,
    std::size_t coordinate_offset,
    std::uint32_t requested,
    std::uint64_t seed,
    const ReferenceTernaryVector* forced,
    const SnapshotConfig& config,
    GenerationTelemetry& telemetry) {
  std::set<ReferenceTernaryVector> unique;
  if (forced != nullptr) {
    unique.insert(*forced);
  }
  SplitMix64 random(seed);
  const std::uint32_t half_size = snapshot.spec.shape.n / 2U;
  while (unique.size() < requested) {
    if (!limits::attempts::within_budget(telemetry.attempts, config.attempt_limit)) {
      return std::nullopt;
    }
    ++telemetry.attempts;
    unique.insert(sample_half(half_size, snapshot.spec.shape.half, random));
  }
  std::vector<HalfVector> result;
  result.reserve(unique.size());
  for (const auto& vector : unique) {
    result.push_back(make_half_record(snapshot, vector, coordinate_offset));
  }
  telemetry.half_pool_entries += result.size();
  return result;
}

std::optional<HalfResidueIndex> build_half_index(const std::vector<HalfVector>& entries,
                                                 const MergeArithmetic& arithmetic,
                                                 std::uint64_t memory_limit_bytes) {
  const std::uint64_t offset_count = arithmetic.m2 + 1U;
  std::uint64_t offset_bytes = 0;
  std::uint64_t id_bytes = 0;
  std::uint64_t peak_bytes = 0;
  if (offset_count > std::numeric_limits<std::size_t>::max() ||
      entries.size() > std::numeric_limits<std::uint32_t>::max() ||
      !checked_multiply(offset_count,
                        sizeof(std::uint32_t) * 2U,
                        offset_bytes) ||
      !checked_multiply(entries.size(), sizeof(std::uint32_t), id_bytes) ||
      !checked_add(offset_bytes, id_bytes, peak_bytes) ||
      !limits::checked::fits_memory_budget(0U, peak_bytes, memory_limit_bytes)) {
    return std::nullopt;
  }
  HalfResidueIndex index;
  index.offsets.assign(static_cast<std::size_t>(offset_count), 0U);
  index.ids.resize(entries.size());
  for (const auto& entry : entries) {
    ++index.offsets[static_cast<std::size_t>(entry.residue_m2) + 1U];
  }
  for (std::size_t residue = 1; residue < index.offsets.size(); ++residue) {
    index.offsets[residue] += index.offsets[residue - 1U];
  }
  auto cursor = index.offsets;
  for (std::uint32_t id = 0; id < entries.size(); ++id) {
    const auto residue = static_cast<std::size_t>(entries[id].residue_m2);
    index.ids[cursor[residue]++] = id;
  }
  return index;
}

ReferenceTernaryVector concatenate(const HalfVector& first, const HalfVector& second) {
  ReferenceTernaryVector result;
  result.reserve(first.digits.size() + second.digits.size());
  result.insert(result.end(), first.digits.begin(), first.digits.end());
  result.insert(result.end(), second.digits.begin(), second.digits.end());
  return result;
}

ListGenerationResult finalize_list(ListRole role,
                                   std::uint64_t target,
                                   std::vector<ReferenceTernaryVector> vectors,
                                   const InputSnapshot& snapshot,
                                   const SnapshotConfig& config) {
  std::sort(vectors.begin(), vectors.end());
  vectors.erase(std::unique(vectors.begin(), vectors.end()), vectors.end());
  const std::uint64_t estimated_bytes = vectors.size() *
      (static_cast<std::uint64_t>(snapshot.spec.shape.n) + sizeof(ReferenceEntry) +
       sizeof(std::uint64_t));
  if (!limits::checked::fits_memory_budget(0U, estimated_bytes, config.memory_limit_bytes) ||
      vectors.size() > std::numeric_limits<EntryId>::max()) {
    return {Status::ResourceLimit, {}, "child list exceeds the declared memory or ID limit"};
  }

  ListGenerationResult result;
  result.list.role = role;
  result.list.target_m2 = target;
  result.list.entries.reserve(vectors.size());
  result.list.residues_m1.reserve(vectors.size());
  for (auto& digits : vectors) {
    const std::uint64_t residue = inner_product_mod(snapshot.instance.residues_m1,
                                                    digits,
                                                    snapshot.spec.arithmetic.mask_m1);
    if ((residue & snapshot.spec.arithmetic.mask_m2) != target) {
      return {Status::InvariantViolation, {}, "generator emitted a child with the wrong M2 target"};
    }
    const auto id = static_cast<EntryId>(result.list.entries.size());
    result.list.entries.push_back(
        ReferenceEntry{id,
                       quotient_residue(residue, target, snapshot.spec.arithmetic),
                       std::move(digits)});
    result.list.residues_m1.push_back(residue);
  }
  result.status = Status::Completed;
  return result;
}

std::vector<std::uint64_t> compatible_prefix(const std::vector<HalfVector>& left,
                                             const HalfResidueIndex& right_index,
                                             std::uint64_t target,
                                             const MergeArithmetic& arithmetic,
                                             bool& overflow) {
  std::vector<std::uint64_t> prefix(left.size() + 1U, 0U);
  overflow = false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const std::uint64_t complement =
        (target - left[index].residue_m2) & arithmetic.mask_m2;
    const std::uint64_t count =
        right_index.offsets[static_cast<std::size_t>(complement) + 1U] -
        right_index.offsets[static_cast<std::size_t>(complement)];
    if (prefix[index] > std::numeric_limits<std::uint64_t>::max() - count) {
      overflow = true;
      return {};
    }
    prefix[index + 1U] = prefix[index] + count;
  }
  return prefix;
}

std::optional<std::uint64_t> find_pair_rank(
    const std::vector<HalfVector>& left,
    const std::vector<HalfVector>& right,
    const HalfResidueIndex& right_index,
    const std::vector<std::uint64_t>& prefix,
    std::uint64_t target,
    const MergeArithmetic& arithmetic,
    const ReferenceTernaryVector& forced_left,
    const ReferenceTernaryVector& forced_right) {
  const auto left_it = std::lower_bound(left.begin(), left.end(), forced_left,
                                        [](const HalfVector& record, const auto& digits) {
    return record.digits < digits;
  });
  const auto right_it = std::lower_bound(right.begin(), right.end(), forced_right,
                                         [](const HalfVector& record, const auto& digits) {
    return record.digits < digits;
  });
  if (left_it == left.end() || left_it->digits != forced_left || right_it == right.end() ||
      right_it->digits != forced_right) {
    return std::nullopt;
  }
  const std::size_t left_id = static_cast<std::size_t>(left_it - left.begin());
  const std::uint32_t right_id = static_cast<std::uint32_t>(right_it - right.begin());
  const std::uint64_t complement =
      (target - left_it->residue_m2) & arithmetic.mask_m2;
  const std::uint32_t begin = right_index.offsets[static_cast<std::size_t>(complement)];
  const std::uint32_t end = right_index.offsets[static_cast<std::size_t>(complement) + 1U];
  for (std::uint32_t offset = begin; offset < end; ++offset) {
    if (right_index.ids[offset] == right_id) {
      return prefix[left_id] + (offset - begin);
    }
  }
  return std::nullopt;
}

std::set<std::uint64_t> sample_ranks(std::uint64_t population,
                                     std::uint64_t sample_count,
                                     SplitMix64& random) {
  std::set<std::uint64_t> selected;
  if (sample_count == population) {
    for (std::uint64_t rank = 0; rank < population; ++rank) {
      selected.insert(rank);
    }
    return selected;
  }
  for (std::uint64_t cursor = population - sample_count; cursor < population; ++cursor) {
    const std::uint64_t candidate = random.bounded(cursor + 1U);
    if (!selected.insert(candidate).second) {
      selected.insert(cursor);
    }
  }
  return selected;
}

std::vector<ReferenceTernaryVector> vectors_for_ranks(
    const std::set<std::uint64_t>& ranks,
    const std::vector<std::uint64_t>& prefix,
    const std::vector<HalfVector>& left,
    const std::vector<HalfVector>& right,
    const HalfResidueIndex& right_index,
    std::uint64_t target,
    const MergeArithmetic& arithmetic) {
  std::vector<ReferenceTernaryVector> vectors;
  vectors.reserve(ranks.size());
  for (const std::uint64_t rank : ranks) {
    const auto upper = std::upper_bound(prefix.begin(), prefix.end(), rank);
    const std::size_t left_id = static_cast<std::size_t>(upper - prefix.begin() - 1);
    const std::uint64_t complement =
        (target - left[left_id].residue_m2) & arithmetic.mask_m2;
    const std::uint32_t begin = right_index.offsets[static_cast<std::size_t>(complement)];
    const std::uint64_t within = rank - prefix[left_id];
    const std::uint32_t right_id = right_index.ids[begin + static_cast<std::uint32_t>(within)];
    vectors.push_back(concatenate(left[left_id], right[right_id]));
  }
  return vectors;
}

std::vector<ReferenceTernaryVector> vectors_for_all_ranks(
    std::uint64_t population,
    const std::vector<std::uint64_t>& prefix,
    const std::vector<HalfVector>& left,
    const std::vector<HalfVector>& right,
    const HalfResidueIndex& right_index,
    std::uint64_t target,
    const MergeArithmetic& arithmetic) {
  std::vector<ReferenceTernaryVector> vectors;
  vectors.reserve(static_cast<std::size_t>(population));
  for (std::uint64_t rank = 0; rank < population; ++rank) {
    const auto upper = std::upper_bound(prefix.begin(), prefix.end(), rank);
    const std::size_t left_id = static_cast<std::size_t>(upper - prefix.begin() - 1);
    const std::uint64_t complement =
        (target - left[left_id].residue_m2) & arithmetic.mask_m2;
    const std::uint32_t begin =
        right_index.offsets[static_cast<std::size_t>(complement)];
    const std::uint64_t within = rank - prefix[left_id];
    const std::uint32_t right_id =
        right_index.ids[begin + static_cast<std::uint32_t>(within)];
    vectors.push_back(concatenate(left[left_id], right[right_id]));
  }
  return vectors;
}

ListGenerationResult generate_exhaustive(ListRole role,
                                         std::uint64_t target,
                                         const InputSnapshot& snapshot,
                                         const SnapshotConfig& config,
                                         GenerationTelemetry& telemetry) {
  const std::uint64_t domain_size = exhaustive_half_domain_size(snapshot.spec.shape);
  const std::uint64_t half_size = snapshot.spec.shape.n / 2U;
  const std::uint64_t offset_count = snapshot.spec.arithmetic.m2 + 1U;
  std::uint64_t half_record_bytes = sizeof(HalfVector) + half_size;
  std::uint64_t temporary_half_bytes = sizeof(ReferenceTernaryVector) + half_size;
  std::uint64_t one_pool_bytes = 0;
  std::uint64_t second_pool_peak = 0;
  std::uint64_t index_bytes = 0;
  std::uint64_t prefix_bytes = 0;
  std::uint64_t resident_bytes = 0;
  std::uint64_t pre_output_peak = 0;
  if (!checked_multiply(domain_size, half_record_bytes, one_pool_bytes) ||
      !checked_add(half_record_bytes, temporary_half_bytes, second_pool_peak) ||
      !checked_multiply(domain_size, second_pool_peak, second_pool_peak) ||
      !checked_accumulate(second_pool_peak, one_pool_bytes) ||
      !checked_multiply(offset_count,
                        sizeof(std::uint32_t) * 2U,
                        index_bytes) ||
      !checked_accumulate(index_bytes,
                          domain_size * sizeof(std::uint32_t)) ||
      !checked_multiply(domain_size + 1U,
                        sizeof(std::uint64_t),
                        prefix_bytes) ||
      !materialized_snapshot_bytes(snapshot, resident_bytes) ||
      !checked_add(resident_bytes, second_pool_peak, second_pool_peak) ||
      !checked_add(resident_bytes, one_pool_bytes * 2U, pre_output_peak) ||
      !checked_accumulate(pre_output_peak, index_bytes) ||
      !checked_accumulate(pre_output_peak, prefix_bytes) ||
      !limits::checked::fits_memory_budget(
          0U, std::max(second_pool_peak, pre_output_peak), config.memory_limit_bytes)) {
    return {Status::ResourceLimit,
            {},
            "exhaustive half pools and residue index exceed the memory limit"};
  }
  const auto left = make_exhaustive_half_pool(snapshot, 0U);
  const auto right = make_exhaustive_half_pool(snapshot, snapshot.spec.shape.n / 2U);
  telemetry.half_pool_entries += left.size() + right.size();
  const auto right_index =
      build_half_index(right, snapshot.spec.arithmetic, config.memory_limit_bytes);
  if (!right_index.has_value()) {
    return {Status::ResourceLimit, {}, "exhaustive residue index exceeds memory limit"};
  }
  bool overflow = false;
  const auto prefix = compatible_prefix(
      left, *right_index, target, snapshot.spec.arithmetic, overflow);
  if (overflow) {
    return {Status::ResourceLimit, {}, "compatible pair population overflow"};
  }
  const std::uint64_t population = prefix.back();
  std::uint64_t output_entry_bytes = sizeof(ReferenceTernaryVector) +
                                     sizeof(ReferenceEntry) +
                                     sizeof(std::uint64_t) +
                                     snapshot.spec.shape.n;
  std::uint64_t output_bytes = 0;
  std::uint64_t exact_peak = pre_output_peak;
  if (population > std::numeric_limits<std::size_t>::max() ||
      population > std::numeric_limits<EntryId>::max() ||
      !checked_multiply(population, output_entry_bytes, output_bytes) ||
      !checked_accumulate(exact_peak, output_bytes) ||
      !limits::checked::fits_memory_budget(0U, exact_peak, config.memory_limit_bytes)) {
    return {Status::ResourceLimit,
            {},
            "complete exhaustive child slice exceeds the memory or ID limit"};
  }
  telemetry.compatible_pairs += population;
  auto vectors = vectors_for_all_ranks(population,
                                       prefix,
                                       left,
                                       right,
                                       *right_index,
                                       target,
                                       snapshot.spec.arithmetic);
  telemetry.accepted_entries += vectors.size();
  return finalize_list(role, target, std::move(vectors), snapshot, config);
}

ListGenerationResult generate_rejection(ListRole role,
                                        std::uint64_t target,
                                        const InputSnapshot& snapshot,
                                        const SnapshotConfig& config,
                                        GenerationTelemetry& telemetry,
                                        std::size_t role_index) {
  std::uint64_t resident_bytes = 0;
  std::uint64_t bytes_per_entry = sizeof(ReferenceTernaryVector) * 2U +
                                  sizeof(ReferenceEntry) +
                                  sizeof(std::uint64_t) + 96U;
  std::uint64_t temporary_bytes = 0;
  std::uint64_t peak_bytes = 0;
  if (!checked_accumulate(bytes_per_entry,
                          static_cast<std::uint64_t>(snapshot.spec.shape.n) * 2U) ||
      !checked_multiply(config.requested_list_size,
                        bytes_per_entry,
                        temporary_bytes) ||
      !materialized_snapshot_bytes(snapshot, resident_bytes) ||
      !checked_add(resident_bytes, temporary_bytes, peak_bytes) ||
      !limits::checked::fits_memory_budget(0U, peak_bytes, config.memory_limit_bytes)) {
    return {Status::ResourceLimit,
            {},
            "rejection set, copy, and resident lists exceed the memory limit"};
  }
  std::set<ReferenceTernaryVector> accepted;
  SplitMix64 random(role_seed(config.seed, role_index, 0x72656a656374ULL));
  const std::uint32_t half_size = snapshot.spec.shape.n / 2U;
  while (accepted.size() < config.requested_list_size) {
    if (!limits::attempts::within_budget(telemetry.attempts, config.attempt_limit)) {
      return {Status::GenerationIncomplete, {}, "rejection generator exhausted its attempt budget"};
    }
    ++telemetry.attempts;
    auto first = sample_half(half_size, snapshot.spec.shape.half, random);
    auto second = sample_half(half_size, snapshot.spec.shape.half, random);
    first.insert(first.end(), second.begin(), second.end());
    const std::uint64_t residue = inner_product_mod(snapshot.instance.residues_m1,
                                                    first,
                                                    snapshot.spec.arithmetic.mask_m1);
    if ((residue & snapshot.spec.arithmetic.mask_m2) == target) {
      accepted.insert(std::move(first));
    }
  }
  std::vector<ReferenceTernaryVector> vectors(accepted.begin(), accepted.end());
  telemetry.accepted_entries += vectors.size();
  return finalize_list(role, target, std::move(vectors), snapshot, config);
}

ListGenerationResult generate_mitm(ListRole role,
                                   std::uint64_t target,
                                   const InputSnapshot& snapshot,
                                   const SnapshotConfig& config,
                                   GenerationTelemetry& telemetry,
                                   std::size_t role_index,
                                   bool force_planted) {
  std::uint32_t pool_size = std::max<std::uint32_t>(config.half_pool_size, 1U);
  const auto& planted = snapshot.planted_vectors[role_index];
  const std::size_t half_size = snapshot.spec.shape.n / 2U;
  const auto half_offset = static_cast<std::ptrdiff_t>(half_size);
  const ReferenceTernaryVector planted_left(planted.begin(), planted.begin() + half_offset);
  const ReferenceTernaryVector planted_right(planted.begin() + half_offset, planted.end());

  for (;;) {
    if (!mitm_memory_fits(snapshot, config, pool_size, force_planted)) {
      return {Status::ResourceLimit,
              {},
              "MITM pools, residue index, or output reserve exceed the memory limit"};
    }
    const auto left = make_sampled_half_pool(snapshot,
                                             0U,
                                             pool_size,
                                             role_seed(config.seed, role_index, 0x6c656674ULL),
                                             force_planted ? &planted_left : nullptr,
                                             config,
                                             telemetry);
    const auto right = make_sampled_half_pool(snapshot,
                                              half_size,
                                              pool_size,
                                              role_seed(config.seed, role_index, 0x7269676874ULL),
                                              force_planted ? &planted_right : nullptr,
                                              config,
                                              telemetry);
    if (!left.has_value() || !right.has_value()) {
      return {Status::GenerationIncomplete, {}, "MITM half-pool generation exhausted attempts"};
    }
    const auto right_index =
        build_half_index(*right, snapshot.spec.arithmetic, config.memory_limit_bytes);
    if (!right_index.has_value()) {
      return {Status::ResourceLimit, {}, "MITM residue index exceeds memory limit"};
    }
    bool overflow = false;
    const auto prefix = compatible_prefix(
        *left, *right_index, target, snapshot.spec.arithmetic, overflow);
    if (overflow) {
      return {Status::ResourceLimit, {}, "compatible pair population overflow"};
    }
    const std::uint64_t population = prefix.back();
    telemetry.compatible_pairs += population;
    if (population >= config.requested_list_size) {
      std::optional<std::uint64_t> planted_rank;
      if (force_planted) {
        planted_rank = find_pair_rank(*left,
                                      *right,
                                      *right_index,
                                      prefix,
                                      target,
                                      snapshot.spec.arithmetic,
                                      planted_left,
                                      planted_right);
        if (!planted_rank.has_value()) {
          return {Status::InvariantViolation, {}, "planted compatible pair vanished from MITM pools"};
        }
      }

      SplitMix64 rank_random(role_seed(config.seed, role_index, 0x72616e6bULL));
      std::set<std::uint64_t> ranks;
      if (planted_rank.has_value()) {
        ranks.insert(*planted_rank);
        const std::uint64_t remaining = config.requested_list_size - 1U;
        const auto compressed = sample_ranks(population - 1U, remaining, rank_random);
        for (const std::uint64_t rank : compressed) {
          ranks.insert(rank >= *planted_rank ? rank + 1U : rank);
        }
      } else {
        ranks = sample_ranks(population, config.requested_list_size, rank_random);
      }
      auto vectors = vectors_for_ranks(ranks,
                                       prefix,
                                       *left,
                                       *right,
                                       *right_index,
                                       target,
                                       snapshot.spec.arithmetic);
      telemetry.accepted_entries += vectors.size();
      return finalize_list(role, target, std::move(vectors), snapshot, config);
    }

    if (pool_size >= config.max_half_pool_size) {
      return {Status::GenerationIncomplete, {}, "MITM pools cannot supply the requested unique list size"};
    }
    const std::uint64_t doubled = static_cast<std::uint64_t>(pool_size) * 2U;
    pool_size = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(doubled, config.max_half_pool_size));
  }
}

std::array<std::uint64_t, 3> child_targets(const MergeArithmetic& arithmetic) {
  return {arithmetic.target_a2, arithmetic.target_b2, arithmetic.target_c2};
}

std::array<ListRole, 3> list_roles() {
  return {ListRole::A, ListRole::B, ListRole::C};
}

}  // namespace

SnapshotResult build_snapshot_impl(const SnapshotConfig& config) {
  SnapshotResult result;
  if (!limits::generator::nonzero_requested_list_size(config.requested_list_size) ||
      !limits::full_ter::runtime_limit_supported(config.memory_limit_bytes) ||
      !limits::generator::half_pool_range_supported(config.half_pool_size,
                                                     config.max_half_pool_size) ||
      !limits::arithmetic::quotient_arithmetic_supported(config.n, config.b2, config.b1) ||
      (config.mode == GeneratorMode::PlantedMitm && !config.plant_valid_triple)) {
    result.status = Status::InvalidConfiguration;
    result.message = "snapshot generator configuration is invalid";
    return result;
  }
  if (config.mode == GeneratorMode::ExhaustiveReference &&
      !limits::generator::exhaustive_reference_dimension_supported(config.n)) {
    result.status = Status::ResourceLimit;
    result.message = "exhaustive reference generation is restricted to n <= 36";
    return result;
  }
  std::uint64_t resident_snapshot_bytes = 0;
  if (!snapshot_resident_budget(config.n,
                                config.requested_list_size,
                                resident_snapshot_bytes) ||
      !limits::checked::fits_memory_budget(0U, resident_snapshot_bytes,
                                            config.memory_limit_bytes)) {
    result.status = Status::ResourceLimit;
    result.message =
        "density-one instance and three resident child lists exceed the memory limit";
    return result;
  }
  std::uint64_t requested_entry_bytes = sizeof(ReferenceEntry) +
                                        sizeof(ReferenceTernaryVector) + 128U;
  if (!checked_accumulate(requested_entry_bytes,
                          static_cast<std::uint64_t>(config.n) * 2U) ||
      !checked_multiply(config.requested_list_size,
                        requested_entry_bytes,
                        requested_entry_bytes) ||
      !limits::checked::fits_memory_budget(0U, requested_entry_bytes,
                                            config.memory_limit_bytes)) {
    result.status = Status::ResourceLimit;
    result.message = "requested child-list materialization exceeds the memory limit";
    return result;
  }

  const auto shape_result = build_shape(config.family, config.n);
  if (shape_result.status != Status::Completed) {
    result.status = shape_result.status;
    result.message = shape_result.message;
    return result;
  }
  auto instance_result = build_instance(config.n, config.b1, config.seed);
  if (instance_result.status != Status::Completed) {
    result.status = instance_result.status;
    result.message = instance_result.message;
    return result;
  }

  auto& snapshot = result.snapshot;
  snapshot.instance = std::move(instance_result.value);
  snapshot.spec.shape = shape_result.shape;
  snapshot.spec.memory_limit_bytes = config.memory_limit_bytes;
  snapshot.planted_vectors = materialize_planted_vectors(snapshot.spec.shape);

  std::uint64_t target_a2 = 0;
  std::uint64_t target_b2 = 0;
  std::uint64_t target_1 = 0;
  const std::uint64_t provisional_m2_mask = (std::uint64_t{1} << config.b2) - 1U;
  const std::uint64_t provisional_m1_mask = (std::uint64_t{1} << config.b1) - 1U;
  if (config.plant_valid_triple) {
    const std::uint64_t residue_a = inner_product_mod(snapshot.instance.residues_m1,
                                                      snapshot.planted_vectors[0],
                                                      provisional_m1_mask);
    const std::uint64_t residue_b = inner_product_mod(snapshot.instance.residues_m1,
                                                      snapshot.planted_vectors[1],
                                                      provisional_m1_mask);
    const std::uint64_t residue_c = inner_product_mod(snapshot.instance.residues_m1,
                                                      snapshot.planted_vectors[2],
                                                      provisional_m1_mask);
    target_a2 = residue_a & provisional_m2_mask;
    target_b2 = residue_b & provisional_m2_mask;
    target_1 = (residue_a + residue_b + residue_c) & provisional_m1_mask;
  } else {
    SplitMix64 targets(config.seed ^ 0xa54ff53a5f1d36f1ULL);
    target_a2 = targets.next() & provisional_m2_mask;
    target_b2 = targets.next() & provisional_m2_mask;
    target_1 = targets.next() & provisional_m1_mask;
  }

  const auto arithmetic_result = build_arithmetic(
      config.n, config.b2, config.b1, target_a2, target_b2, target_1);
  if (arithmetic_result.status != Status::Completed) {
    result.status = arithmetic_result.status;
    result.message = arithmetic_result.message;
    return result;
  }
  snapshot.spec.arithmetic = arithmetic_result.value;
  if (config.plant_valid_triple) {
    const std::uint64_t planted_c_residue = inner_product_mod(snapshot.instance.residues_m1,
                                                              snapshot.planted_vectors[2],
                                                              snapshot.spec.arithmetic.mask_m1);
    if ((planted_c_residue & snapshot.spec.arithmetic.mask_m2) !=
        snapshot.spec.arithmetic.target_c2) {
      result.status = Status::InvariantViolation;
      result.message = "planted target derivation failed";
      return result;
    }
  }

  const auto targets = child_targets(snapshot.spec.arithmetic);
  const auto roles = list_roles();
  for (std::size_t role_index = 0; role_index < 3U; ++role_index) {
    ListGenerationResult generated;
    switch (config.mode) {
      case GeneratorMode::ExhaustiveReference:
        generated = generate_exhaustive(
            roles[role_index], targets[role_index], snapshot, config, result.telemetry);
        break;
      case GeneratorMode::RejectionReference:
        generated = generate_rejection(roles[role_index],
                                       targets[role_index],
                                       snapshot,
                                       config,
                                       result.telemetry,
                                       role_index);
        break;
      case GeneratorMode::MitmPerformance:
      case GeneratorMode::PlantedMitm:
        generated = generate_mitm(roles[role_index],
                                  targets[role_index],
                                  snapshot,
                                  config,
                                  result.telemetry,
                                  role_index,
                                  config.mode == GeneratorMode::PlantedMitm);
        break;
    }
    if (generated.status != Status::Completed) {
      result.status = generated.status;
      result.message = generated.message;
      return result;
    }
    snapshot.lists[role_index] = std::move(generated.list);
  }

  snapshot.has_planted_triple = config.plant_valid_triple;
  if (config.plant_valid_triple) {
    for (std::size_t role_index = 0; role_index < 3U; ++role_index) {
      const auto& entries = snapshot.lists[role_index].entries;
      const auto found = std::lower_bound(entries.begin(),
                                          entries.end(),
                                          snapshot.planted_vectors[role_index],
                                          [](const ReferenceEntry& entry, const auto& digits) {
        return entry.digits < digits;
      });
      if (found == entries.end() || found->digits != snapshot.planted_vectors[role_index]) {
        snapshot.has_planted_triple = false;
        break;
      }
      snapshot.planted_entry_ids[role_index] = found->id;
    }
  }
  if (config.mode == GeneratorMode::PlantedMitm && !snapshot.has_planted_triple) {
    result.status = Status::InvariantViolation;
    result.message = "PLANTED_MITM did not retain the planted triple";
    return result;
  }

  const auto audit = audit_snapshot_children(snapshot);
  if (audit.status != Status::Completed) {
    result.status = audit.status;
    result.message = audit.message;
    return result;
  }
  result.status = Status::Completed;
  return result;
}

SnapshotResult build_snapshot(const SnapshotConfig& config) {
  try {
    return build_snapshot_impl(config);
  } catch (const std::bad_alloc&) {
    SnapshotResult result;
    result.status = Status::ResourceLimit;
    result.message = "snapshot generation allocation failed";
    return result;
  } catch (const std::length_error&) {
    SnapshotResult result;
    result.status = Status::ResourceLimit;
    result.message = "snapshot generation allocation size is unsupported";
    return result;
  }
}

}  // namespace ter
