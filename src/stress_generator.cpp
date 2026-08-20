#include "ter/stress_generator.hpp"

#include "ter/limitations.hpp"
#include "ter/validator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ter {
namespace {

constexpr std::uint8_t kMaximumPrefilterBits = 12U;

[[nodiscard]] bool list_sizes_valid(const TerStressConfig& config) noexcept {
  return std::all_of(config.requested_list_sizes.begin(), config.requested_list_sizes.end(),
                     [&config](const std::uint32_t size) {
                       return size != 0U && size <= config.candidate_pool_size;
                     });
}

[[nodiscard]] bool config_valid(const TerStressConfig& config) noexcept {
  if (!list_sizes_valid(config) || config.candidate_pool_size == 0U ||
      config.prefilter_bits == 0U || config.prefilter_bits > kMaximumPrefilterBits ||
      config.hot_mass_tolerance_percent > 100U ||
      config.target_hot_mass_percent > 100U || config.required_distinct_witnesses == 0U) {
    return false;
  }
  if (config.bucket_profile == BucketProfile::HotBuckets &&
      (config.hot_bucket_count == 0U || config.hot_bucket_count >= (1U << config.prefilter_bits))) {
    return false;
  }
  const bool equal_sizes = config.requested_list_sizes[0] == config.requested_list_sizes[1] &&
                           config.requested_list_sizes[1] == config.requested_list_sizes[2];
  switch (config.mode) {
    case StressMode::BucketSkew:
      return equal_sizes;
    case StressMode::GroupSizeSkew:
      return !equal_sizes && config.bucket_profile == BucketProfile::Uniform;
    case StressMode::SolutionMultiplicity:
      return config.solution_mode != StressSolutionMode::NoPlantedSolution;
    case StressMode::QuotientBoundary:
      return config.quotient_boundary_bits != 0U &&
             config.quotient_boundary_bits <= limits::arithmetic::kMaximumQuotientBits &&
             config.source.b1 > config.source.b2 &&
             static_cast<std::uint32_t>(config.source.b1) - config.source.b2 ==
                 config.quotient_boundary_bits;
    case StressMode::ArithmeticStructure:
      return config.bucket_profile == BucketProfile::HotBuckets;
    case StressMode::Composite:
      return true;
  }
  return true;
}

// The source pool and the selected fixture coexist while selection is in
// progress. Reserve the latter before asking the existing generator to spend
// the remaining budget; allocation failure is not our resource policy.
[[nodiscard]] bool selected_fixture_reserve(const TerStressConfig& config,
                                            std::uint64_t& reserve_bytes) noexcept {
  const std::uint64_t n = config.source.n;
  const std::uint64_t words = (n + 63U) / 64U;
  std::uint64_t instance_bytes = 0U;
  std::uint64_t selected_entries = 0U;
  std::uint64_t list_bytes = 0U;
  std::uint64_t selection_bytes = 0U;
  if (!limits::checked::checked_multiply(n, words, &instance_bytes) ||
      !limits::checked::checked_multiply(instance_bytes, sizeof(std::uint64_t), &instance_bytes) ||
      !limits::checked::checked_add_bytes(&instance_bytes, n, sizeof(std::uint64_t)) ||
      !limits::checked::checked_add_bytes(&instance_bytes, n, sizeof(std::uint8_t)) ||
      !limits::checked::checked_add_bytes(&instance_bytes, words, sizeof(std::uint64_t)) ||
      !limits::checked::checked_add_bytes(&instance_bytes, n, 3U)) {
    return false;
  }
  for (const std::uint32_t size : config.requested_list_sizes) {
    if (!limits::checked::checked_add(selected_entries, size, &selected_entries)) {
      return false;
    }
  }
  std::uint64_t bytes_per_entry = sizeof(ReferenceEntry) + sizeof(std::uint64_t);
  if (!limits::checked::checked_add(bytes_per_entry, n, &bytes_per_entry) ||
      !limits::checked::checked_multiply(selected_entries, bytes_per_entry, &list_bytes) ||
      !limits::checked::checked_multiply(config.candidate_pool_size, 3U,
                                         &selection_bytes) ||
      !limits::checked::checked_multiply(selection_bytes, sizeof(std::uint32_t),
                                         &selection_bytes) ||
      !limits::checked::checked_add_bytes(
          &selection_bytes, std::uint64_t{1} << config.prefilter_bits,
          sizeof(std::uint64_t))) {
    return false;
  }
  reserve_bytes = instance_bytes;
  return limits::checked::checked_add(reserve_bytes, list_bytes, &reserve_bytes) &&
         limits::checked::checked_add(reserve_bytes, selection_bytes, &reserve_bytes);
}

[[nodiscard]] std::uint32_t bucket_for(const QuotientResidue q,
                                        const std::uint8_t bits) noexcept {
  return static_cast<std::uint32_t>(q) & ((std::uint32_t{1} << bits) - 1U);
}

[[nodiscard]] bool is_hot_bucket(const std::uint32_t bucket,
                                  const TerStressConfig& config) noexcept {
  return bucket < config.hot_bucket_count;
}

[[nodiscard]] bool append_if_new(std::vector<std::uint32_t>& selected,
                                 const std::uint32_t index) {
  if (std::find(selected.begin(), selected.end(), index) != selected.end()) {
    return false;
  }
  selected.push_back(index);
  return true;
}

[[nodiscard]] std::vector<std::uint32_t> select_uniform(
    const TerList& source,
    const std::uint32_t requested,
    const std::uint8_t bits,
    const std::vector<std::uint32_t>& required) {
  const std::uint32_t bucket_count = std::uint32_t{1} << bits;
  std::vector<std::vector<std::uint32_t>> bucket_indices(bucket_count);
  for (std::uint32_t index = 0U; index < source.entries.size(); ++index) {
    bucket_indices[bucket_for(source.entries[index].q, bits)].push_back(index);
  }
  std::vector<std::uint32_t> cursor(bucket_count, 0U);
  std::vector<std::uint32_t> selected;
  selected.reserve(requested);
  for (const std::uint32_t index : required) {
    if (index >= source.entries.size() || !append_if_new(selected, index)) {
      return {};
    }
  }
  if (selected.size() > requested) {
    return {};
  }
  while (selected.size() < requested) {
    bool progress = false;
    for (std::uint32_t bucket = 0U; bucket < bucket_count && selected.size() < requested;
         ++bucket) {
      auto& next = cursor[bucket];
      const auto& candidates = bucket_indices[bucket];
      while (next < candidates.size() &&
             std::find(selected.begin(), selected.end(), candidates[next]) != selected.end()) {
        ++next;
      }
      if (next < candidates.size()) {
        selected.push_back(candidates[next++]);
        progress = true;
      }
    }
    if (!progress) {
      return {};
    }
  }
  std::sort(selected.begin(), selected.end());
  return selected;
}

[[nodiscard]] std::vector<std::uint32_t> select_hot(
    const TerList& source,
    const std::uint32_t requested,
    const TerStressConfig& config,
    const std::vector<std::uint32_t>& required) {
  std::vector<std::uint32_t> hot;
  std::vector<std::uint32_t> cool;
  for (std::uint32_t index = 0U; index < source.entries.size(); ++index) {
    const bool hot_bucket = is_hot_bucket(bucket_for(source.entries[index].q,
                                                      config.prefilter_bits),
                                          config);
    (hot_bucket ? hot : cool).push_back(index);
  }
  const std::uint32_t wanted_hot = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(requested) * config.target_hot_mass_percent + 99U) / 100U);
  std::vector<std::uint32_t> selected;
  selected.reserve(requested);
  for (const std::uint32_t index : required) {
    if (index >= source.entries.size() || !append_if_new(selected, index)) {
      return {};
    }
  }
  if (selected.size() > requested) {
    return {};
  }
  const auto append_group = [&selected](const std::vector<std::uint32_t>& candidates,
                                        const std::uint32_t wanted) {
    for (const std::uint32_t index : candidates) {
      if (selected.size() >= wanted) {
        break;
      }
      (void)append_if_new(selected, index);
    }
  };
  std::uint32_t selected_hot = 0U;
  for (const std::uint32_t index : selected) {
    if (is_hot_bucket(bucket_for(source.entries[index].q, config.prefilter_bits), config)) {
      ++selected_hot;
    }
  }
  if (selected_hot < wanted_hot) {
    const auto total_needed = static_cast<std::uint32_t>(selected.size()) + wanted_hot - selected_hot;
    append_group(hot, total_needed);
  }
  append_group(cool, requested);
  append_group(hot, requested);
  if (selected.size() != requested) {
    return {};
  }
  std::sort(selected.begin(), selected.end());
  return selected;
}

[[nodiscard]] std::uint32_t find_vector(const TerList& list,
                                         const ReferenceTernaryVector& vector) {
  const auto found = std::lower_bound(list.entries.begin(), list.entries.end(), vector,
                                      [](const ReferenceEntry& entry,
                                         const ReferenceTernaryVector& digits) {
                                        return entry.digits < digits;
                                      });
  if (found == list.entries.end() || found->digits != vector) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return found->id;
}

[[nodiscard]] bool make_selected_list(TerList& destination,
                                      const TerList& source,
                                      const std::vector<std::uint32_t>& selection) {
  destination.role = source.role;
  destination.target_m2 = source.target_m2;
  destination.entries.reserve(selection.size());
  destination.residues_m1.reserve(selection.size());
  for (const std::uint32_t index : selection) {
    if (index >= source.entries.size()) {
      return false;
    }
    ReferenceEntry entry = source.entries[index];
    entry.id = static_cast<EntryId>(destination.entries.size());
    destination.entries.push_back(std::move(entry));
    destination.residues_m1.push_back(source.residues_m1[index]);
  }
  return true;
}

void measure_buckets(const InputSnapshot& snapshot,
                     const TerStressConfig& config,
                     StressGenerationTelemetry& telemetry) {
  const std::uint32_t bucket_count = std::uint32_t{1} << config.prefilter_bits;
  std::vector<std::uint64_t> counts(bucket_count, 0U);
  for (const auto& list : snapshot.lists) {
    for (const auto& entry : list.entries) {
      ++counts[bucket_for(entry.q, config.prefilter_bits)];
    }
  }
  auto& report = telemetry.buckets;
  report.bucket_count = bucket_count;
  for (const std::uint64_t count : counts) {
    report.entry_count += count;
    report.maximum_occupancy = std::max(report.maximum_occupancy, count);
    if (count != 0U) {
      ++report.nonempty_bucket_count;
      report.collision_count += count - 1U;
    }
  }
  if (report.entry_count == 0U) {
    return;
  }
  report.mean_occupancy = static_cast<double>(report.entry_count) /
                          static_cast<double>(bucket_count);
  double entropy = 0.0;
  for (const std::uint64_t count : counts) {
    const double difference = static_cast<double>(count) - report.mean_occupancy;
    report.occupancy_variance += difference * difference;
    if (count != 0U) {
      const double probability = static_cast<double>(count) /
                                 static_cast<double>(report.entry_count);
      entropy -= probability * std::log2(probability);
    }
  }
  report.occupancy_variance /= static_cast<double>(bucket_count);
  report.normalized_entropy = entropy / static_cast<double>(config.prefilter_bits);
  std::uint64_t hot_count = 0U;
  for (std::uint32_t bucket = 0U; bucket < config.hot_bucket_count; ++bucket) {
    hot_count += counts[bucket];
  }
  report.hot_mass_percent = static_cast<std::uint32_t>(
      (hot_count * 100U + report.entry_count / 2U) / report.entry_count);
}

[[nodiscard]] bool fixture_bytes(const InputSnapshot& snapshot, std::uint64_t& bytes) noexcept {
  bytes = 0U;
  for (const auto& list : snapshot.lists) {
    std::uint64_t list_bytes = 0U;
    for (const auto& entry : list.entries) {
      std::uint64_t entry_bytes = sizeof(ReferenceEntry) + sizeof(std::uint64_t);
      if (!limits::checked::checked_add(entry_bytes, entry.digits.size(), &entry_bytes) ||
          !limits::checked::checked_add(list_bytes, entry_bytes, &list_bytes)) {
        return false;
      }
    }
    if (!limits::checked::checked_add(bytes, list_bytes, &bytes)) {
      return false;
    }
  }
  return true;
}

}  // namespace

QuotientBoundaryReport describe_quotient_boundary(const std::uint8_t quotient_bits) noexcept {
  const bool native = quotient_bits != 0U &&
                      quotient_bits <= limits::arithmetic::kMaximumQuotientBits;
  return {quotient_bits, native, !native};
}

StressSnapshotResult build_stress_snapshot(const TerStressConfig& config) {
  StressSnapshotResult result;
  if (!config_valid(config)) {
    result.message = "stress generator configuration is invalid";
    return result;
  }
  SnapshotConfig source_config = config.source;
  source_config.requested_list_size = config.candidate_pool_size;
  std::uint64_t selected_reserve = 0U;
  std::uint64_t source_budget = 0U;
  if (!selected_fixture_reserve(config, selected_reserve) ||
      !limits::checked::remaining_memory(selected_reserve,
                                         source_config.memory_limit_bytes,
                                         &source_budget) ||
      source_budget == 0U) {
    result.status = Status::ResourceLimit;
    result.message = "source pool and selected stress fixture exceed the declared memory limit";
    return result;
  }
  source_config.memory_limit_bytes = source_budget;
  const bool require_plant = config.solution_mode != StressSolutionMode::NoPlantedSolution;
  if (require_plant) {
    source_config.mode = GeneratorMode::PlantedMitm;
    source_config.plant_valid_triple = true;
  } else {
    source_config.mode = GeneratorMode::MitmPerformance;
    source_config.plant_valid_triple = false;
  }
  auto source = build_snapshot(source_config);
  result.telemetry.source_attempts = source.telemetry.attempts;
  if (source.status != Status::Completed) {
    result.status = source.status;
    result.message = "stress source generation failed: " + source.message;
    return result;
  }
  const std::uint8_t quotient_bits = static_cast<std::uint8_t>(
      source.snapshot.spec.arithmetic.b1 - source.snapshot.spec.arithmetic.b2);
  if (config.prefilter_bits > quotient_bits) {
    result.message = "stress prefilter width exceeds the actual quotient width";
    return result;
  }

  // Multiple/Dense modes make their witness lower bound true by deliberately
  // retaining distinct oracle-certified source triples, not by relabelling
  // accidental final-list hits as plants.  The oracle's existing cap remains
  // the explicit resource boundary for this small-fixture operation.
  std::array<std::vector<std::uint32_t>, 3> required_ids;
  std::vector<std::array<ReferenceTernaryVector, 3>> required_witness_vectors;
  if (config.solution_mode == StressSolutionMode::MultiplePlantedSolutions ||
      config.solution_mode == StressSolutionMode::DenseSolutionStress) {
    const auto source_oracle = run_cubic_oracle(source.snapshot, OutputPolicy::Correctness);
    if (source_oracle.status != Status::Completed) {
      result.status = source_oracle.status;
      result.message = "source witness certification failed: " + source_oracle.message;
      return result;
    }
    if (source_oracle.valid_provenance.size() < config.required_distinct_witnesses) {
      result.status = Status::GenerationIncomplete;
      result.message = "source pool lacks the requested distinct witness lower bound";
      return result;
    }
    for (std::uint32_t witness_index = 0U;
         witness_index < config.required_distinct_witnesses; ++witness_index) {
      const auto& witness = source_oracle.valid_provenance[witness_index];
      const std::array<std::uint32_t, 3> ids{witness.a, witness.b, witness.c};
      std::array<ReferenceTernaryVector, 3> vectors;
      for (std::size_t role = 0U; role < ids.size(); ++role) {
        if (ids[role] >= source.snapshot.lists[role].entries.size()) {
          result.status = Status::InvariantViolation;
          result.message = "source oracle returned an out-of-range provenance ID";
          return result;
        }
        required_ids[role].push_back(ids[role]);
        vectors[role] = source.snapshot.lists[role].entries[ids[role]].digits;
      }
      required_witness_vectors.push_back(std::move(vectors));
    }
  } else if (require_plant) {
    for (std::size_t role = 0U; role < required_ids.size(); ++role) {
      const auto id = find_vector(source.snapshot.lists[role], source.snapshot.planted_vectors[role]);
      if (id == std::numeric_limits<std::uint32_t>::max()) {
        result.status = Status::InvariantViolation;
        result.message = "source fixture lost its planted child";
        return result;
      }
      required_ids[role].push_back(id);
    }
    required_witness_vectors.push_back(source.snapshot.planted_vectors);
  }
  for (auto& ids : required_ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }

  auto& snapshot = result.snapshot;
  snapshot.instance = source.snapshot.instance;
  snapshot.spec = source.snapshot.spec;
  snapshot.planted_vectors = source.snapshot.planted_vectors;
  result.telemetry.requested_list_sizes = config.requested_list_sizes;
  result.telemetry.quotient_bits = quotient_bits;
  result.telemetry.prefilter_bits = config.prefilter_bits;

  for (std::size_t role = 0U; role < snapshot.lists.size(); ++role) {
    const auto selected = config.bucket_profile == BucketProfile::Uniform
                              ? select_uniform(source.snapshot.lists[role],
                                               config.requested_list_sizes[role],
                                               config.prefilter_bits,
                                               required_ids[role])
                              : select_hot(source.snapshot.lists[role],
                                           config.requested_list_sizes[role], config,
                                           required_ids[role]);
    if (selected.size() != config.requested_list_sizes[role] ||
        !make_selected_list(snapshot.lists[role], source.snapshot.lists[role], selected)) {
      result.status = Status::GenerationIncomplete;
      result.message = "source pool cannot achieve the requested quotient/list geometry";
      return result;
    }
    result.telemetry.actual_list_sizes[role] =
        static_cast<std::uint32_t>(snapshot.lists[role].entries.size());
  }
  // Release the full candidate pool before any final local oracle/output work.
  source.snapshot = InputSnapshot{};

  snapshot.has_planted_triple = !required_witness_vectors.empty();
  if (snapshot.has_planted_triple) {
    snapshot.planted_vectors = required_witness_vectors.front();
    for (std::size_t role = 0U; role < snapshot.lists.size(); ++role) {
      const auto id = find_vector(snapshot.lists[role], snapshot.planted_vectors[role]);
      if (id == std::numeric_limits<std::uint32_t>::max()) {
        result.status = Status::GenerationIncomplete;
        result.message = "selection cannot retain the requested planted witness";
        return result;
      }
      snapshot.planted_entry_ids[role] = id;
    }
  }
  const auto child_audit = audit_snapshot_children(snapshot);
  if (child_audit.status != Status::Completed) {
    result.status = child_audit.status;
    result.message = "stress fixture audit failed: " + child_audit.message;
    return result;
  }
  if (snapshot.has_planted_triple) {
    const auto planted = audit_candidate(snapshot,
                                         snapshot.lists[0].entries[snapshot.planted_entry_ids[0]],
                                         snapshot.lists[1].entries[snapshot.planted_entry_ids[1]],
                                         snapshot.lists[2].entries[snapshot.planted_entry_ids[2]]);
    if (!planted.valid) {
      result.status = Status::InvariantViolation;
      result.message = "independent planted-witness audit failed";
      return result;
    }
    result.telemetry.deliberately_planted_witnesses =
        static_cast<std::uint32_t>(required_witness_vectors.size());
    result.telemetry.guaranteed_planted_witnesses =
        result.telemetry.deliberately_planted_witnesses;
  }
  for (const auto& witness_vectors : required_witness_vectors) {
    std::array<ReferenceEntry, 3> entries;
    for (std::size_t role = 0U; role < entries.size(); ++role) {
      const auto id = find_vector(snapshot.lists[role], witness_vectors[role]);
      if (id == std::numeric_limits<std::uint32_t>::max()) {
        result.status = Status::InvariantViolation;
        result.message = "final fixture lost a deliberately retained witness";
        return result;
      }
      entries[role] = snapshot.lists[role].entries[id];
    }
    if (!audit_candidate(snapshot, entries[0], entries[1], entries[2]).valid) {
      result.status = Status::InvariantViolation;
      result.message = "independent final witness audit failed";
      return result;
    }
  }
  measure_buckets(snapshot, config, result.telemetry);
  if (config.bucket_profile == BucketProfile::HotBuckets) {
    const std::uint32_t actual = result.telemetry.buckets.hot_mass_percent;
    const std::uint32_t requested = config.target_hot_mass_percent;
    const std::uint32_t difference = actual > requested ? actual - requested : requested - actual;
    if (difference > config.hot_mass_tolerance_percent) {
      result.status = Status::GenerationIncomplete;
      result.message = "measured hot-bucket mass misses the configured tolerance";
      return result;
    }
  }
  if (!fixture_bytes(snapshot, result.telemetry.fixture_bytes) ||
      !limits::checked::fits_memory_budget(0U, result.telemetry.fixture_bytes,
                                            config.source.memory_limit_bytes)) {
    result.status = Status::ResourceLimit;
    result.message = "selected stress fixture exceeds the declared memory limit";
    return result;
  }

  if (config.solution_mode == StressSolutionMode::MultiplePlantedSolutions ||
      config.solution_mode == StressSolutionMode::DenseSolutionStress) {
    const auto oracle = run_cubic_oracle(snapshot, OutputPolicy::Correctness);
    if (oracle.status != Status::Completed) {
      result.status = oracle.status;
      result.message = "multiplicity certification failed: " + oracle.message;
      return result;
    }
    result.certified_witnesses = oracle.valid_provenance;
    result.telemetry.certified_witnesses =
        static_cast<std::uint32_t>(std::min<std::size_t>(oracle.valid_provenance.size(),
                                                         std::numeric_limits<std::uint32_t>::max()));
    result.telemetry.exact_solution_count_known = true;
    result.telemetry.exact_solution_count = oracle.telemetry.raw_valid_triples;
    if (result.telemetry.certified_witnesses < config.required_distinct_witnesses) {
      result.status = Status::GenerationIncomplete;
      result.message = "fixture did not contain the requested certified witness lower bound";
      return result;
    }
  }
  result.status = Status::Completed;
  return result;
}

}  // namespace ter
