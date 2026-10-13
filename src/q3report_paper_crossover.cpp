#include "ter/q3report_paper_crossover.hpp"

#include "ter/arithmetic.hpp"
#include "ter/instance.hpp"
#include "ter/limitations.hpp"
#include "ter/reference_vector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ter {
namespace {

using Clock = std::chrono::steady_clock;
using limits::checked::checked_add_bytes;
using limits::checked::checked_add;
using limits::checked::checked_multiply;
using limits::is_power_of_two;
using limits::power_of_two_exponent;

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

class SplitMix64 {
 public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::uint64_t bounded(const std::uint64_t bound) noexcept {
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

[[nodiscard]] bool counts_equal(const DigitCounts& left, const DigitCounts& right) noexcept {
  return left.negative == right.negative && left.zero == right.zero &&
         left.positive == right.positive;
}

// Saturating exact binomial.  Below `cap` the returned value is exact; at `cap`
// the only claim is "at least cap".  No floating point is involved.
[[nodiscard]] std::uint64_t saturating_binomial(const std::uint32_t population,
                                                std::uint32_t choose,
                                                const std::uint64_t cap) noexcept {
  if (choose > population) {
    return 0U;
  }
  if (choose > population - choose) {
    choose = population - choose;
  }
  std::uint64_t value = 1U;
  for (std::uint32_t step = 1U; step <= choose; ++step) {
    const std::uint64_t numerator = static_cast<std::uint64_t>(population - choose) + step;
    std::uint64_t product = 0;
    if (!checked_multiply(value, numerator, &product)) {
      return cap;
    }
    value = product / step;
    if (value >= cap) {
      return cap;
    }
  }
  return value;
}

[[nodiscard]] std::uint64_t saturating_half_domain(const std::uint32_t half_length,
                                                   const DigitCounts& counts,
                                                   const std::uint64_t cap) noexcept {
  if (counts.total() != half_length) {
    return 0U;
  }
  const std::uint64_t negatives = saturating_binomial(half_length, counts.negative, cap);
  if (negatives >= cap) {
    return cap;
  }
  const std::uint64_t positives =
      saturating_binomial(half_length - counts.negative, counts.positive, cap);
  if (positives >= cap) {
    return cap;
  }
  std::uint64_t total = 0;
  if (!checked_multiply(negatives, positives, &total) || total >= cap) {
    return cap;
  }
  return total;
}

[[nodiscard]] bool next_combination(std::vector<std::uint32_t>& combination,
                                    const std::uint32_t population) noexcept {
  if (combination.empty()) {
    return false;
  }
  for (std::size_t reverse = combination.size(); reverse > 0U; --reverse) {
    const std::size_t index = reverse - 1U;
    const std::uint32_t maximum =
        population - static_cast<std::uint32_t>(combination.size() - index);
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

// Deterministic complete enumeration of one Full TER L3 active half.
[[nodiscard]] std::vector<ReferenceTernaryVector> enumerate_full_ter_halves(
    const std::uint32_t shape_n,
    const DigitCounts& counts,
    const bool second_half,
    const std::uint64_t limit) {
  const std::uint32_t half = shape_n / 2U;
  std::vector<ReferenceTernaryVector> result;
  if (limit == 0U || counts.total() != half) {
    return result;
  }
  const std::uint32_t offset = second_half ? half : 0U;
  std::vector<std::uint32_t> negatives(counts.negative);
  std::iota(negatives.begin(), negatives.end(), 0U);
  bool negative_valid = counts.negative <= half;
  while (negative_valid) {
    std::vector<std::uint32_t> available;
    available.reserve(static_cast<std::size_t>(half) - counts.negative);
    std::size_t cursor = 0;
    for (std::uint32_t coordinate = 0; coordinate < half; ++coordinate) {
      if (cursor < negatives.size() && negatives[cursor] == coordinate) {
        ++cursor;
      } else {
        available.push_back(coordinate);
      }
    }
    const auto remaining = static_cast<std::uint32_t>(available.size());
    if (counts.positive <= remaining) {
      std::vector<std::uint32_t> positives(counts.positive);
      std::iota(positives.begin(), positives.end(), 0U);
      for (;;) {
        ReferenceTernaryVector digits(shape_n, static_cast<std::int8_t>(0));
        for (const std::uint32_t coordinate : negatives) {
          digits[static_cast<std::size_t>(offset) + coordinate] = static_cast<std::int8_t>(-1);
        }
        for (const std::uint32_t index : positives) {
          digits[static_cast<std::size_t>(offset) + available[index]] =
              static_cast<std::int8_t>(1);
        }
        result.push_back(std::move(digits));
        if (static_cast<std::uint64_t>(result.size()) >= limit) {
          return result;
        }
        if (counts.positive == 0U || !next_combination(positives, remaining)) {
          break;
        }
      }
    }
    if (counts.negative == 0U) {
      break;
    }
    negative_valid = next_combination(negatives, half);
  }
  return result;
}

// Deterministic uniform sample of one valid Full TER L3 active half.
void sample_full_ter_half(const std::uint32_t shape_n,
                          const DigitCounts& counts,
                          const bool second_half,
                          SplitMix64& random,
                          std::vector<std::uint32_t>& scratch,
                          ReferenceTernaryVector& digits) {
  const std::uint32_t half = shape_n / 2U;
  const std::uint32_t offset = second_half ? half : 0U;
  scratch.resize(half);
  std::iota(scratch.begin(), scratch.end(), 0U);
  const std::uint32_t drawn = counts.negative + counts.positive;
  for (std::uint32_t index = 0; index < drawn; ++index) {
    const std::uint32_t span = half - index;
    const auto pick =
        index + static_cast<std::uint32_t>(random.bounded(static_cast<std::uint64_t>(span)));
    std::swap(scratch[index], scratch[pick]);
  }
  digits.assign(shape_n, static_cast<std::int8_t>(0));
  for (std::uint32_t index = 0; index < counts.negative; ++index) {
    digits[static_cast<std::size_t>(offset) + scratch[index]] = static_cast<std::int8_t>(-1);
  }
  for (std::uint32_t index = counts.negative; index < drawn; ++index) {
    digits[static_cast<std::size_t>(offset) + scratch[index]] = static_cast<std::int8_t>(1);
  }
}

struct HalfPool {
  std::vector<ReferenceTernaryVector> vectors;
  std::vector<std::uint64_t> residues;
};

[[nodiscard]] bool build_half_pool(const std::vector<std::uint64_t>& coefficient_residues,
                                   const std::uint32_t shape_n,
                                   const DigitCounts& counts,
                                   const bool second_half,
                                   const std::uint32_t target,
                                   const std::uint64_t domain,
                                   const bool domain_saturated,
                                   const std::uint64_t mask_m1,
                                   SplitMix64& random,
                                   HalfPool* const pool) {
  pool->vectors.clear();
  pool->residues.clear();
  if (target == 0U) {
    return false;
  }
  if (!domain_saturated && domain <= target) {
    pool->vectors = enumerate_full_ter_halves(shape_n, counts, second_half, domain);
    if (static_cast<std::uint64_t>(pool->vectors.size()) != domain) {
      return false;
    }
  } else {
    std::vector<std::uint32_t> scratch;
    ReferenceTernaryVector digits;
    std::uint64_t attempts = 0;
    std::uint64_t attempt_ceiling = 0;
    if (!checked_multiply(target, limits::paper_crossover::kPoolSamplingAttemptMultiplier,
                          &attempt_ceiling) ||
        !checked_add(attempt_ceiling, limits::paper_crossover::kPoolSamplingAttemptSlack,
                     &attempt_ceiling)) {
      return false;
    }
    while (pool->vectors.size() < target && attempts < attempt_ceiling) {
      const std::size_t missing = target - pool->vectors.size();
      for (std::size_t index = 0; index < missing; ++index) {
        sample_full_ter_half(shape_n, counts, second_half, random, scratch, digits);
        pool->vectors.push_back(digits);
      }
      attempts += missing;
      std::sort(pool->vectors.begin(), pool->vectors.end());
      pool->vectors.erase(std::unique(pool->vectors.begin(), pool->vectors.end()),
                          pool->vectors.end());
    }
    if (pool->vectors.size() != target) {
      return false;
    }
  }
  pool->residues.reserve(pool->vectors.size());
  for (const auto& vector : pool->vectors) {
    pool->residues.push_back(inner_product_mod(coefficient_residues, vector, mask_m1));
  }
  return true;
}

[[nodiscard]] PaperCrossoverStatus sample_role_list(
    const std::vector<std::uint64_t>& coefficient_residues,
    const std::uint32_t shape_n,
    const FullTerParameters& parameters,
    const MergeArithmetic& arithmetic,
    const std::uint64_t child_target,
    const std::uint64_t requested_list_size,
    const PaperCrossoverSnapshotConfig& config,
    const std::uint64_t domain,
    const bool domain_saturated,
    const std::uint64_t role_seed,
    TerList* const list,
    std::uint32_t* const final_pool,
    std::uint64_t* const compatible_pairs,
    std::uint32_t* const doublings,
    std::string* const message) {
  const std::uint64_t bucket_count = std::uint64_t{1} << arithmetic.b2;
  std::uint64_t pool_ceiling = config.max_half_pool;
  if (!domain_saturated && domain < pool_ceiling) {
    pool_ceiling = domain;
  }
  if (pool_ceiling == 0U) {
    *message = "half-vector domain is empty for the selected Full TER shape";
    return PaperCrossoverStatus::GenerationIncomplete;
  }
  std::uint64_t pool_target = config.initial_half_pool;
  if (pool_target > pool_ceiling) {
    pool_target = pool_ceiling;
  }
  *doublings = 0U;

  HalfPool left;
  HalfPool right;
  std::vector<std::uint64_t> bucket_offsets;
  std::vector<std::uint32_t> bucket_ids;
  std::vector<std::uint64_t> prefix;
  std::vector<std::uint32_t> required_bucket;
  for (;;) {
    SplitMix64 left_random(role_seed ^ 0x9e3779b97f4a7c15ULL);
    SplitMix64 right_random(role_seed ^ 0xc2b2ae3d27d4eb4fULL);
    const auto pool = static_cast<std::uint32_t>(pool_target);
    if (!build_half_pool(coefficient_residues, shape_n, parameters.level3_active_half, false,
                         pool, domain, domain_saturated, arithmetic.mask_m1, left_random,
                         &left) ||
        !build_half_pool(coefficient_residues, shape_n, parameters.level3_active_half, true,
                         pool, domain, domain_saturated, arithmetic.mask_m1, right_random,
                         &right)) {
      *message = "deterministic half-vector pool could not reach its declared size";
      return PaperCrossoverStatus::GenerationIncomplete;
    }
    bucket_offsets.assign(static_cast<std::size_t>(bucket_count) + 1U, 0U);
    for (const auto residue : right.residues) {
      ++bucket_offsets[static_cast<std::size_t>(residue & arithmetic.mask_m2) + 1U];
    }
    for (std::size_t index = 1; index < bucket_offsets.size(); ++index) {
      bucket_offsets[index] += bucket_offsets[index - 1U];
    }
    bucket_ids.assign(right.residues.size(), 0U);
    {
      std::vector<std::uint64_t> cursor = bucket_offsets;
      for (std::size_t id = 0; id < right.residues.size(); ++id) {
        const auto bucket = static_cast<std::size_t>(right.residues[id] & arithmetic.mask_m2);
        bucket_ids[static_cast<std::size_t>(cursor[bucket]++)] = static_cast<std::uint32_t>(id);
      }
    }
    prefix.assign(left.residues.size() + 1U, 0U);
    required_bucket.assign(left.residues.size(), 0U);
    for (std::size_t index = 0; index < left.residues.size(); ++index) {
      const auto needed = static_cast<std::uint32_t>(
          (child_target - left.residues[index]) & arithmetic.mask_m2);
      required_bucket[index] = needed;
      const std::uint64_t available =
          bucket_offsets[static_cast<std::size_t>(needed) + 1U] -
          bucket_offsets[static_cast<std::size_t>(needed)];
      if (!checked_add(prefix[index], available, &prefix[index + 1U])) {
        *message = "compatible pair population overflows";
        return PaperCrossoverStatus::ResourceLimit;
      }
    }
    if (prefix.back() >= requested_list_size) {
      break;
    }
    if (pool_target >= pool_ceiling) {
      *message = "declared half-pool ceiling cannot supply the requested list size";
      return PaperCrossoverStatus::GenerationIncomplete;
    }
    pool_target = std::min(pool_target * 2U, pool_ceiling);
    ++(*doublings);
  }
  *final_pool = static_cast<std::uint32_t>(left.vectors.size());
  *compatible_pairs = prefix.back();

  // Floyd's deterministic sample of exactly `requested_list_size` distinct pair
  // ranks without replacement.
  const std::uint64_t population = prefix.back();
  std::vector<std::uint64_t> ranks;
  ranks.reserve(static_cast<std::size_t>(requested_list_size));
  {
    std::unordered_set<std::uint64_t> chosen;
    chosen.reserve(static_cast<std::size_t>(requested_list_size) * 2U);
    SplitMix64 rank_random(role_seed ^ 0x165667b19e3779f9ULL);
    for (std::uint64_t index = population - requested_list_size; index < population; ++index) {
      const std::uint64_t candidate = rank_random.bounded(index + 1U);
      if (chosen.insert(candidate).second) {
        ranks.push_back(candidate);
      } else {
        chosen.insert(index);
        ranks.push_back(index);
      }
    }
  }
  std::sort(ranks.begin(), ranks.end());

  std::vector<ReferenceTernaryVector> vectors;
  vectors.reserve(ranks.size());
  for (const std::uint64_t rank : ranks) {
    const auto position =
        static_cast<std::size_t>(std::upper_bound(prefix.begin() + 1, prefix.end(), rank) -
                                 (prefix.begin() + 1));
    if (position >= required_bucket.size()) {
      *message = "pair-rank resolution left the compatible population";
      return PaperCrossoverStatus::InternalInvariantFailure;
    }
    const std::uint64_t offset = rank - prefix[position];
    const auto bucket = static_cast<std::size_t>(required_bucket[position]);
    const std::uint64_t base = bucket_offsets[bucket];
    if (base + offset >= bucket_offsets[bucket + 1U]) {
      *message = "pair-rank resolution left its residue bucket";
      return PaperCrossoverStatus::InternalInvariantFailure;
    }
    const auto right_id = static_cast<std::size_t>(bucket_ids[static_cast<std::size_t>(base + offset)]);
    ReferenceTernaryVector digits(shape_n, static_cast<std::int8_t>(0));
    const auto& left_digits = left.vectors[position];
    const auto& right_digits = right.vectors[right_id];
    for (std::size_t coordinate = 0; coordinate < digits.size(); ++coordinate) {
      const int sum = static_cast<int>(left_digits[coordinate]) +
                      static_cast<int>(right_digits[coordinate]);
      if (sum < -1 || sum > 1) {
        *message = "level-3 halves overlapped, which contradicts disjoint half supports";
        return PaperCrossoverStatus::InternalInvariantFailure;
      }
      digits[coordinate] = static_cast<std::int8_t>(sum);
    }
    vectors.push_back(std::move(digits));
  }
  std::sort(vectors.begin(), vectors.end());
  vectors.erase(std::unique(vectors.begin(), vectors.end()), vectors.end());
  if (static_cast<std::uint64_t>(vectors.size()) != requested_list_size) {
    *message = "deduplication removed distinct sampled level-2 vectors";
    return PaperCrossoverStatus::GenerationIncomplete;
  }

  list->target_m2 = child_target;
  list->entries.clear();
  list->residues_m1.clear();
  list->entries.reserve(vectors.size());
  list->residues_m1.reserve(vectors.size());
  for (std::size_t id = 0; id < vectors.size(); ++id) {
    const std::uint64_t residue =
        inner_product_mod(coefficient_residues, vectors[id], arithmetic.mask_m1);
    if ((residue & arithmetic.mask_m2) != child_target) {
      *message = "sampled level-2 vector does not satisfy its child target";
      return PaperCrossoverStatus::InternalInvariantFailure;
    }
    list->residues_m1.push_back(residue);
    list->entries.push_back(ReferenceEntry{static_cast<EntryId>(id),
                                           quotient_residue(residue, child_target, arithmetic),
                                           std::move(vectors[id])});
  }
  return PaperCrossoverStatus::Completed;
}

// ---------------------------------------------------------------------------
// Histogram helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] PaperHistogram build_histogram(std::vector<std::uint64_t> values) {
  PaperHistogram result;
  result.population = values.size();
  if (values.empty()) {
    return result;
  }
  for (const auto value : values) {
    result.zero += value == 0U ? 1U : 0U;
    result.one += value == 1U ? 1U : 0U;
    result.at_most_two += value <= 2U ? 1U : 0U;
    result.at_most_four += value <= 4U ? 1U : 0U;
  }
  std::sort(values.begin(), values.end());
  result.maximum = values.back();
  const auto percentile = [&values](const std::uint64_t numerator) {
    const auto index =
        static_cast<std::size_t>((numerator * (values.size() - 1U)) / 100U);
    return values[index];
  };
  result.p50 = percentile(50U);
  result.p90 = percentile(90U);
  result.p99 = percentile(99U);
  return result;
}

[[nodiscard]] PaperTimingSummary summarize(std::vector<std::uint64_t> samples) {
  PaperTimingSummary summary;
  if (samples.empty()) {
    return summary;
  }
  summary.samples = static_cast<std::uint32_t>(samples.size());
  std::sort(samples.begin(), samples.end());
  summary.minimum_ns = samples.front();
  summary.maximum_ns = samples.back();
  summary.median_ns = samples[samples.size() / 2U];
  return summary;
}

// ---------------------------------------------------------------------------
// Timed method kernels.
// ---------------------------------------------------------------------------

struct DirectFullComponents {
  std::uint64_t setup_ns = 0;
  std::uint64_t pair_count_ns = 0;
  std::uint64_t lookup_ns = 0;
};

// Section 17 DIRECT_FULL_COUNT: the complete Q-sized convolution object plus the
// selected-target lookup.  Counts are exact in 32 bits because the experiment
// refuses to run when |A||B| reaches the chosen NTT prime, which is below 2^31.
[[nodiscard]] std::uint64_t direct_full_method(const std::vector<std::uint32_t>& left,
                                               const std::vector<std::uint32_t>& right,
                                               const std::vector<std::uint32_t>& lookup,
                                               const std::uint64_t q,
                                               const std::uint32_t tau,
                                               std::vector<std::uint32_t>* const counts,
                                               DirectFullComponents* const components) {
  const auto mask = static_cast<std::uint32_t>(q - 1U);
  const auto setup_start = Clock::now();
  counts->assign(static_cast<std::size_t>(q), 0U);
  components->setup_ns = elapsed_ns(setup_start);

  const auto count_start = Clock::now();
  std::uint32_t* const data = counts->data();
  for (const std::uint32_t a : left) {
    for (const std::uint32_t b : right) {
      ++data[(a + b) & mask];
    }
  }
  components->pair_count_ns = elapsed_ns(count_start);

  const auto lookup_start = Clock::now();
  std::uint64_t selected = 0;
  for (const std::uint32_t c : lookup) {
    selected += data[(tau - c) & mask];
  }
  components->lookup_ns = elapsed_ns(lookup_start);
  return selected;
}

struct DirectProductionComponents {
  std::uint64_t index_ns = 0;
  std::uint64_t scan_ns = 0;
};

// Section 17 DIRECT_PRODUCTION: the production-relevant TER arithmetic path.
// No structural validation, no parent construction, no deduplication.
[[nodiscard]] std::uint64_t direct_production_method(
    const std::vector<std::uint32_t>& left,
    const std::vector<std::uint32_t>& right,
    const std::vector<std::uint32_t>& lookup,
    const std::uint64_t q,
    const std::uint32_t tau,
    std::vector<std::uint32_t>* const offsets,
    std::vector<std::uint32_t>* const ids,
    DirectProductionComponents* const components) {
  const auto mask = static_cast<std::uint32_t>(q - 1U);
  const auto index_start = Clock::now();
  offsets->assign(static_cast<std::size_t>(q) + 1U, 0U);
  ids->assign(lookup.size(), 0U);
  std::uint32_t* const offset_data = offsets->data();
  for (const std::uint32_t c : lookup) {
    ++offset_data[static_cast<std::size_t>(c) + 1U];
  }
  for (std::size_t index = 1; index < offsets->size(); ++index) {
    offset_data[index] += offset_data[index - 1U];
  }
  for (std::size_t id = 0; id < lookup.size(); ++id) {
    (*ids)[offset_data[lookup[id]]++] = static_cast<std::uint32_t>(id);
  }
  // Repair the cursor damage in place instead of paying for a second Q-sized array.
  for (std::size_t index = offsets->size() - 1U; index > 0U; --index) {
    offset_data[index] = offset_data[index - 1U];
  }
  offset_data[0] = 0U;
  components->index_ns = elapsed_ns(index_start);

  const auto scan_start = Clock::now();
  std::uint64_t raw = 0;
  for (const std::uint32_t a : left) {
    const std::uint32_t base = (tau - a) & mask;
    for (const std::uint32_t b : right) {
      const std::uint32_t required = (base - b) & mask;
      raw += offset_data[static_cast<std::size_t>(required) + 1U] -
             offset_data[static_cast<std::size_t>(required)];
    }
  }
  components->scan_ns = elapsed_ns(scan_start);
  return raw;
}

struct DirectTiledComponents {
  std::uint64_t setup_ns = 0;
  std::uint64_t scan_ns = 0;
  TiledQ3ReportTelemetry tiling{};
};

// Section 11 DIRECT_PRODUCTION_TILED: the same production-relevant raw triple
// count as DIRECT_PRODUCTION, located through the shared cache-tiled primitive
// instead of a Q-wide random-access index.  It is an additional comparator; it
// never replaces the historical method.
[[nodiscard]] std::uint64_t direct_tiled_method(const std::vector<std::uint32_t>& left,
                                                const std::vector<std::uint32_t>& right,
                                                const std::vector<std::uint32_t>& lookup,
                                                const std::uint64_t q,
                                                const std::uint32_t tau,
                                                const std::uint64_t memory_limit_bytes,
                                                DirectTiledComponents* const components,
                                                PaperCrossoverStatus* const status,
                                                std::string* const message) {
  // The primitive receives caller-owned TiledQEntry vectors, but this paper
  // comparator creates those vectors itself.  Charge them before allocation;
  // otherwise its advertised ceiling would be able to reject the kernel while
  // still permitting this wrapper to exceed it first.
  std::uint64_t input_entries = 0;
  std::uint64_t input_bytes = 0;
  std::uint64_t count_auxiliary = 0;
  std::uint64_t report_auxiliary = 0;
  if (!checked_add(left.size(), right.size(), &input_entries) ||
      !checked_add(input_entries, lookup.size(), &input_entries) ||
      !checked_multiply(input_entries, sizeof(TiledQEntry), &input_bytes) ||
      !tiled_q3report_auxiliary_bytes(left.size(), right.size(), lookup.size(), q,
                                      limits::tiled::kDefaultPreferredTileBits, false,
                                      &count_auxiliary, &report_auxiliary) ||
      !checked_add(input_bytes, count_auxiliary, &input_bytes)) {
    *status = PaperCrossoverStatus::ResourceLimit;
    *message = "tiled comparator memory accounting overflows";
    return 0U;
  }
  if (memory_limit_bytes != 0U &&
      !limits::checked::fits_memory_budget(0U, input_bytes, memory_limit_bytes)) {
    *status = PaperCrossoverStatus::ResourceLimit;
    *message = "tiled comparator inputs and auxiliary memory exceed the declared ceiling";
    return 0U;
  }
  const auto to_entries = [](const std::vector<std::uint32_t>& source) {
    std::vector<TiledQEntry> entries;
    entries.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
      entries.push_back(TiledQEntry{source[index], static_cast<EntryId>(index)});
    }
    return entries;
  };
  const auto a = to_entries(left);
  const auto b = to_entries(right);
  const auto c = to_entries(lookup);
  TiledQ3ReportConfig config;
  config.preferred_tile_bits = limits::tiled::kDefaultPreferredTileBits;
  config.memory_limit_bytes = memory_limit_bytes;
  const auto reported = tiled_q3report_count(a, b, c, tau, q, config);
  components->setup_ns = reported.telemetry.setup_ns;
  components->scan_ns = reported.telemetry.scan_ns;
  components->tiling = reported.telemetry;
  switch (reported.status) {
    case TiledQ3ReportStatus::Completed:
      *status = PaperCrossoverStatus::Completed;
      break;
    case TiledQ3ReportStatus::NotApplicable:
      *status = PaperCrossoverStatus::NotApplicable;
      break;
    case TiledQ3ReportStatus::ResourceLimit:
      *status = PaperCrossoverStatus::ResourceLimit;
      break;
    case TiledQ3ReportStatus::ArithmeticSafetyFailure:
      *status = PaperCrossoverStatus::ArithmeticSafetyFailure;
      break;
    case TiledQ3ReportStatus::InvalidConfiguration:
    case TiledQ3ReportStatus::Cancelled:
    case TiledQ3ReportStatus::StoppedByVisitor:
    case TiledQ3ReportStatus::InvariantViolation:
      *status = PaperCrossoverStatus::InternalInvariantFailure;
      break;
  }
  *message = reported.message;
  return reported.telemetry.raw_triples;
}

struct NttComponents {
  std::uint64_t frequency_build_ns = 0;
  std::uint64_t bit_reversal_ns = 0;
  std::uint64_t forward_ns = 0;
  std::uint64_t pointwise_ns = 0;
  std::uint64_t inverse_ns = 0;
  std::uint64_t reconstruction_ns = 0;
  std::uint64_t lookup_ns = 0;
};

// The transform pair is deliberately asymmetric so that no explicit
// bit-reversal permutation pass is ever executed.
//
//   natural order --[DIF forward]--> bit-reversed spectrum
//                    pointwise multiply (index-wise, order-agnostic)
//   bit-reversed  --[DIT inverse]--> natural order
//
// Gentleman-Sande decimation-in-frequency consumes natural order and leaves its
// output bit-reversed; Cooley-Tukey decimation-in-time consumes bit-reversed
// input and restores natural order.  Composing them cancels the permutation
// exactly, so the scrambling is a side effect of the stage order rather than a
// separate O(Q) scattered-memory pass.  `PaperNttPlan::bit_reverse` is retained as
// declared immutable plan metadata and as the independent test oracle for the
// forward transform's output ordering.

// Gentleman-Sande: stages run from the full span down to 2, the add/subtract
// happens first, and the twiddle multiplies the difference.
void dif_forward_stages(const PaperNttPlan& plan, std::vector<std::uint32_t>& data) {
  const auto prime = static_cast<std::uint32_t>(plan.prime);
  const std::uint64_t length = plan.q;
  const std::uint32_t* const twiddles = plan.forward_twiddles.data();
  std::uint32_t* const values = data.data();
  for (std::uint64_t span = length; span >= 2U; span >>= 1U) {
    const std::uint64_t half = span >> 1U;
    const std::uint64_t base = half - 1U;
    for (std::uint64_t start = 0; start < length; start += span) {
      // offset 0 always has twiddle 1, so its difference needs no multiply.
      {
        const std::uint32_t upper = values[start];
        const std::uint32_t lower = values[start + half];
        std::uint32_t sum = upper + lower;
        if (sum >= prime) {
          sum -= prime;
        }
        values[start] = sum;
        values[start + half] = upper >= lower ? upper - lower : upper + prime - lower;
      }
      for (std::uint64_t offset = 1U; offset < half; ++offset) {
        const std::uint32_t twiddle = twiddles[base + offset];
        const std::uint32_t upper = values[start + offset];
        const std::uint32_t lower = values[start + offset + half];
        std::uint32_t sum = upper + lower;
        if (sum >= prime) {
          sum -= prime;
        }
        const std::uint32_t difference =
            upper >= lower ? upper - lower : upper + prime - lower;
        values[start + offset] = sum;
        values[start + offset + half] = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(difference) * twiddle) % prime);
      }
    }
  }
}

// Cooley-Tukey: stages run from 2 up to the full span, the twiddle multiplies
// the odd leg first, and the add/subtract happens afterwards.  Inverse twiddles
// use the radix-2 identity w^-j = -w^(len/2 - j).
void dit_inverse_stages(const PaperNttPlan& plan, std::vector<std::uint32_t>& data) {
  const auto prime = static_cast<std::uint32_t>(plan.prime);
  const std::uint64_t length = plan.q;
  const std::uint32_t* const twiddles = plan.forward_twiddles.data();
  std::uint32_t* const values = data.data();
  for (std::uint64_t span = 2U; span <= length; span <<= 1U) {
    const std::uint64_t half = span >> 1U;
    const std::uint64_t base = half - 1U;
    for (std::uint64_t start = 0; start < length; start += span) {
      {
        const std::uint32_t even = values[start];
        const std::uint32_t odd = values[start + half];
        std::uint32_t sum = even + odd;
        if (sum >= prime) {
          sum -= prime;
        }
        values[start] = sum;
        values[start + half] = even >= odd ? even - odd : even + prime - odd;
      }
      for (std::uint64_t offset = 1U; offset < half; ++offset) {
        const std::uint32_t twiddle = prime - twiddles[base + half - offset];
        const std::uint32_t even = values[start + offset];
        const auto odd = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(values[start + offset + half]) * twiddle) % prime);
        std::uint32_t sum = even + odd;
        if (sum >= prime) {
          sum -= prime;
        }
        values[start + offset] = sum;
        values[start + offset + half] = even >= odd ? even - odd : even + prime - odd;
      }
    }
  }
}

void forward_transform(const PaperNttPlan& plan,
                       std::vector<std::uint32_t>& data,
                       NttComponents* const components) {
  const auto stage_start = Clock::now();
  dif_forward_stages(plan, data);
  components->forward_ns += elapsed_ns(stage_start);
}

void inverse_transform(const PaperNttPlan& plan,
                       std::vector<std::uint32_t>& data,
                       NttComponents* const components) {
  const auto stage_start = Clock::now();
  dit_inverse_stages(plan, data);
  components->inverse_ns += elapsed_ns(stage_start);
  const auto scale_start = Clock::now();
  const auto prime = static_cast<std::uint64_t>(plan.prime);
  const auto scale = static_cast<std::uint64_t>(plan.inverse_length);
  for (auto& value : data) {
    value = static_cast<std::uint32_t>((static_cast<std::uint64_t>(value) * scale) % prime);
  }
  components->reconstruction_ns += elapsed_ns(scale_start);
}

[[nodiscard]] std::vector<std::uint32_t> ntt_pair_counts_internal(
    const PaperNttPlan& plan,
    const std::vector<std::uint32_t>& left,
    const std::vector<std::uint32_t>& right,
    NttComponents* const components) {
  const auto frequency_start = Clock::now();
  std::vector<std::uint32_t> transformed_left(static_cast<std::size_t>(plan.q), 0U);
  std::vector<std::uint32_t> transformed_right(static_cast<std::size_t>(plan.q), 0U);
  for (const std::uint32_t value : left) {
    ++transformed_left[value];
  }
  for (const std::uint32_t value : right) {
    ++transformed_right[value];
  }
  components->frequency_build_ns += elapsed_ns(frequency_start);

  forward_transform(plan, transformed_left, components);
  forward_transform(plan, transformed_right, components);

  const auto pointwise_start = Clock::now();
  const auto prime = static_cast<std::uint64_t>(plan.prime);
  for (std::size_t index = 0; index < transformed_left.size(); ++index) {
    transformed_left[index] = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(transformed_left[index]) * transformed_right[index]) % prime);
  }
  components->pointwise_ns += elapsed_ns(pointwise_start);
  transformed_right.clear();
  transformed_right.shrink_to_fit();

  inverse_transform(plan, transformed_left, components);
  return transformed_left;
}

}  // namespace

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------

const char* paper_crossover_family_name(const PaperCrossoverFamily family) noexcept {
  switch (family) {
    case PaperCrossoverFamily::FullTerL2PaperScalingEmulation:
      return "FULLTER_L2_PAPER_SCALING_EMULATION";
    case PaperCrossoverFamily::AuthenticFullTer:
      return "AUTHENTIC_FULLTER";
  }
  return "UNKNOWN";
}

const char* paper_crossover_status_name(const PaperCrossoverStatus status) noexcept {
  switch (status) {
    case PaperCrossoverStatus::Completed:
      return "Completed";
    case PaperCrossoverStatus::NotApplicable:
      return "NotApplicable";
    case PaperCrossoverStatus::InvalidConfiguration:
      return "InvalidConfiguration";
    case PaperCrossoverStatus::InvalidSnapshot:
      return "InvalidSnapshot";
    case PaperCrossoverStatus::GenerationIncomplete:
      return "GenerationIncomplete";
    case PaperCrossoverStatus::ResourceLimit:
      return "ResourceLimit";
    case PaperCrossoverStatus::ArithmeticSafetyFailure:
      return "ArithmeticSafetyFailure";
    case PaperCrossoverStatus::InternalInvariantFailure:
      return "InternalInvariantFailure";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Section 2/3.
// ---------------------------------------------------------------------------

PaperScalingPoint derive_paper_scaling_point(const std::uint64_t list_size) noexcept {
  PaperScalingPoint point;
  point.list_size = list_size;
  if (list_size < 2U) {
    return point;
  }
  const PaperAsymptoticReference reference{};
  const double logarithm = std::log2(static_cast<double>(list_size));
  point.effective_n = logarithm / reference.level2_exponent;
  const double quotient_exponent =
      (reference.representation1_exponent - reference.representation2_exponent) / 2.0;
  const double level2_representation_exponent = reference.representation2_exponent / 2.0;
  point.q_bits =
      static_cast<std::uint32_t>(std::llround(quotient_exponent * point.effective_n));
  point.r2 = static_cast<std::uint32_t>(
      std::llround(level2_representation_exponent * point.effective_n));
  point.r1 = point.r2 + point.q_bits;
  point.q = point.q_bits < limits::arithmetic::kMachineWordBits
                ? (std::uint64_t{1} << point.q_bits)
                : 0U;
  return point;
}

std::span<const std::uint64_t> declared_paper_scaling_ladder() noexcept {
  return std::span<const std::uint64_t>(limits::benchmark::kPaperListSizeLadder.data(),
                                        limits::benchmark::kPaperListSizeLadder.size());
}

std::span<const std::uint64_t> declared_paper_scaling_seeds() noexcept {
  return std::span<const std::uint64_t>(limits::benchmark::kPaperSeeds.data(),
                                        limits::benchmark::kPaperSeeds.size());
}

// ---------------------------------------------------------------------------
// Section 11.
// ---------------------------------------------------------------------------

std::uint64_t modular_power(std::uint64_t base,
                            std::uint64_t exponent,
                            const std::uint64_t modulus) noexcept {
  if (modulus <= 1U) {
    return 0U;
  }
  std::uint64_t result = 1U;
  base %= modulus;
  while (exponent != 0U) {
    if ((exponent & 1U) != 0U) {
      result = (result * base) % modulus;
    }
    base = (base * base) % modulus;
    exponent >>= 1U;
  }
  return result;
}

bool is_prime_u32(const std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  if (value < 2U) {
    return false;
  }
  for (const std::uint64_t small : {2U, 3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U, 37U}) {
    if (value == small) {
      return true;
    }
    if (value % small == 0U) {
      return false;
    }
  }
  std::uint64_t odd = value - 1U;
  std::uint32_t twos = 0;
  while ((odd & 1U) == 0U) {
    odd >>= 1U;
    ++twos;
  }
  // {2,3,5,7} is a complete deterministic base set below 3'215'031'751, and
  // 3'215'031'751 exceeds every 32-bit candidate.
  for (const std::uint64_t witness : {2U, 3U, 5U, 7U}) {
    std::uint64_t residue = modular_power(witness, odd, value);
    if (residue == 1U || residue == value - 1U) {
      continue;
    }
    bool composite = true;
    for (std::uint32_t round = 1; round < twos; ++round) {
      residue = (residue * residue) % value;
      if (residue == value - 1U) {
        composite = false;
        break;
      }
    }
    if (composite) {
      return false;
    }
  }
  return true;
}

bool is_primitive_root_u32(const std::uint64_t root,
                           const std::uint64_t prime,
                           const std::span<const std::uint64_t> distinct_prime_factors) noexcept {
  if (prime < 3U || root == 0U || root % prime == 0U || distinct_prime_factors.empty()) {
    return false;
  }
  const std::uint64_t order = prime - 1U;
  for (const std::uint64_t factor : distinct_prime_factors) {
    if (factor < 2U || order % factor != 0U) {
      return false;
    }
    if (modular_power(root, order / factor, prime) == 1U) {
      return false;
    }
  }
  return true;
}

NttPrimeSelection select_paper_crossover_prime(const std::uint64_t q,
                                               const std::uint64_t pair_universe) noexcept {
  NttPrimeSelection selection;
  if (!is_power_of_two(q)) {
    selection.status = PaperCrossoverStatus::NotApplicable;
    return selection;
  }
  if (q <= (std::uint64_t{1} << kPaperNttMaxPower1) && pair_universe < kPaperNttPrime1) {
    selection.status = PaperCrossoverStatus::Completed;
    selection.prime = kPaperNttPrime1;
    selection.primitive_root = kPaperNttRoot1;
    selection.max_power = kPaperNttMaxPower1;
    return selection;
  }
  if (q <= (std::uint64_t{1} << kPaperNttMaxPower2) && pair_universe < kPaperNttPrime2) {
    selection.status = PaperCrossoverStatus::Completed;
    selection.prime = kPaperNttPrime2;
    selection.primitive_root = kPaperNttRoot2;
    selection.max_power = kPaperNttMaxPower2;
    return selection;
  }
  selection.status = q > (std::uint64_t{1} << kPaperNttMaxPower2)
                         ? PaperCrossoverStatus::NotApplicable
                         : PaperCrossoverStatus::ArithmeticSafetyFailure;
  return selection;
}

// ---------------------------------------------------------------------------
// Section 12/13.
// ---------------------------------------------------------------------------

std::uint64_t PaperNttPlan::plan_bytes() const noexcept {
  std::uint64_t total = sizeof(PaperNttPlan);
  total += static_cast<std::uint64_t>(reversal_table.capacity()) * sizeof(std::uint32_t);
  total += static_cast<std::uint64_t>(forward_twiddles.capacity()) * sizeof(std::uint32_t);
  return total;
}

std::uint32_t PaperNttPlan::bit_reverse(const std::uint32_t index) const noexcept {
  const std::uint32_t high = reversal_bits;
  const std::uint32_t low = log_q - high;
  const std::uint32_t low_part = index & ((std::uint32_t{1} << low) - 1U);
  const std::uint32_t high_part = index >> low;
  return (reversal_table[low_part] >> (high - low)) << high | reversal_table[high_part];
}

std::uint32_t PaperNttPlan::forward_twiddle(const std::uint64_t length,
                                       const std::uint64_t offset) const noexcept {
  const std::uint64_t half = length / 2U;
  return forward_twiddles[static_cast<std::size_t>(half - 1U + offset)];
}

std::uint32_t PaperNttPlan::inverse_twiddle(const std::uint64_t length,
                                       const std::uint64_t offset) const noexcept {
  if (offset == 0U) {
    return 1U;
  }
  const std::uint64_t half = length / 2U;
  return static_cast<std::uint32_t>(prime) -
         forward_twiddles[static_cast<std::size_t>(half - 1U + half - offset)];
}

PaperNttPlanResult build_paper_ntt_plan(const std::uint64_t q,
                                        const std::uint64_t prime,
                                        const std::uint64_t primitive_root) {
  PaperNttPlanResult result;
  if (!is_power_of_two(q) || q < 2U || prime < 3U || prime > std::numeric_limits<std::uint32_t>::max() ||
      primitive_root < 2U || primitive_root >= prime) {
    result.message = "NTT plan requires a power-of-two length and a 32-bit prime modulus";
    return result;
  }
  if ((prime - 1U) % q != 0U) {
    result.status = PaperCrossoverStatus::NotApplicable;
    result.message = "the transform length does not divide prime - 1";
    return result;
  }
  try {
    auto& plan = result.value;
    plan.q = q;
    plan.prime = prime;
    plan.primitive_root = primitive_root;
    plan.log_q = power_of_two_exponent(q);
    plan.reversal_bits = (plan.log_q + 1U) / 2U;
    const std::uint32_t table_size = std::uint32_t{1} << plan.reversal_bits;
    plan.reversal_table.resize(table_size);
    for (std::uint32_t index = 0; index < table_size; ++index) {
      std::uint32_t mirrored = 0;
      for (std::uint32_t bit = 0; bit < plan.reversal_bits; ++bit) {
        mirrored |= ((index >> bit) & 1U) << (plan.reversal_bits - 1U - bit);
      }
      plan.reversal_table[index] = mirrored;
    }
    plan.forward_twiddles.resize(static_cast<std::size_t>(q) - 1U);
    for (std::uint64_t span = 2U; span <= q; span <<= 1U) {
      const std::uint64_t half = span >> 1U;
      const std::uint64_t base = half - 1U;
      const std::uint64_t stage_root = modular_power(primitive_root, (prime - 1U) / span, prime);
      std::uint64_t power = 1U;
      for (std::uint64_t offset = 0; offset < half; ++offset) {
        plan.forward_twiddles[static_cast<std::size_t>(base + offset)] =
            static_cast<std::uint32_t>(power);
        power = (power * stage_root) % prime;
      }
    }
    plan.inverse_length =
        static_cast<std::uint32_t>(modular_power(q % prime, prime - 2U, prime));
    result.status = PaperCrossoverStatus::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.value = PaperNttPlan{};
    result.message = "NTT plan allocation failed";
    return result;
  } catch (const std::length_error& error) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.value = PaperNttPlan{};
    result.message = error.what();
    return result;
  }
}

PairCountResult ntt_forward_transform(const PaperNttPlan& plan, std::vector<std::uint32_t> values) {
  PairCountResult result;
  if (plan.q < 2U || plan.forward_twiddles.size() != static_cast<std::size_t>(plan.q) - 1U ||
      values.size() != static_cast<std::size_t>(plan.q)) {
    result.message = "forward transform requires an initialized plan and a length-Q input";
    return result;
  }
  for (const std::uint32_t value : values) {
    if (static_cast<std::uint64_t>(value) >= plan.prime) {
      result.message = "forward transform input leaves the coefficient field";
      return result;
    }
  }
  dif_forward_stages(plan, values);
  result.counts = std::move(values);
  result.status = PaperCrossoverStatus::Completed;
  return result;
}

PairCountResult ntt_cyclic_pair_counts(const PaperNttPlan& plan,
                                       const std::vector<std::uint32_t>& left,
                                       const std::vector<std::uint32_t>& right) {
  PairCountResult result;
  if (plan.q < 2U || plan.forward_twiddles.size() != static_cast<std::size_t>(plan.q) - 1U) {
    result.message = "NTT plan is not initialized";
    return result;
  }
  std::uint64_t pair_universe = 0;
  if (!checked_multiply(left.size(), right.size(), &pair_universe)) {
    result.status = PaperCrossoverStatus::ArithmeticSafetyFailure;
    result.message = "pair universe overflows";
    return result;
  }
  if (pair_universe >= plan.prime) {
    result.status = PaperCrossoverStatus::ArithmeticSafetyFailure;
    result.message = "single-prime exact reconstruction is unsafe; CRT is not implemented";
    return result;
  }
  for (const std::uint32_t value : left) {
    if (static_cast<std::uint64_t>(value) >= plan.q) {
      result.message = "left quotient residue leaves the transform ring";
      return result;
    }
  }
  for (const std::uint32_t value : right) {
    if (static_cast<std::uint64_t>(value) >= plan.q) {
      result.message = "right quotient residue leaves the transform ring";
      return result;
    }
  }
  try {
    NttComponents components;
    result.counts = ntt_pair_counts_internal(plan, left, right, &components);
    result.status = PaperCrossoverStatus::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.counts.clear();
    result.message = "NTT transform allocation failed";
    return result;
  }
}

PairCountResult direct_full_pair_counts(const std::vector<std::uint32_t>& left,
                                        const std::vector<std::uint32_t>& right,
                                        const std::uint64_t q) {
  PairCountResult result;
  if (!is_power_of_two(q) || q > std::numeric_limits<std::size_t>::max()) {
    result.message = "direct cyclic convolution requires a power-of-two length";
    return result;
  }
  std::uint64_t pair_universe = 0;
  if (!checked_multiply(left.size(), right.size(), &pair_universe) ||
      pair_universe > std::numeric_limits<std::uint32_t>::max()) {
    result.status = PaperCrossoverStatus::ArithmeticSafetyFailure;
    result.message = "direct pair counts do not fit the 32-bit count representation";
    return result;
  }
  const auto mask = static_cast<std::uint32_t>(q - 1U);
  for (const std::uint32_t value : left) {
    if (static_cast<std::uint64_t>(value) >= q) {
      result.message = "left quotient residue leaves the counting ring";
      return result;
    }
  }
  for (const std::uint32_t value : right) {
    if (static_cast<std::uint64_t>(value) >= q) {
      result.message = "right quotient residue leaves the counting ring";
      return result;
    }
  }
  try {
    result.counts.assign(static_cast<std::size_t>(q), 0U);
    std::uint32_t* const data = result.counts.data();
    for (const std::uint32_t a : left) {
      for (const std::uint32_t b : right) {
        ++data[(a + b) & mask];
      }
    }
    result.status = PaperCrossoverStatus::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.counts.clear();
    result.message = "direct convolution allocation failed";
    return result;
  }
}

// ---------------------------------------------------------------------------
// Section 6: shape selection.
// ---------------------------------------------------------------------------

PaperShapeSelection select_paper_shape(const PaperScalingPoint& point) {
  PaperShapeSelection selection;
  if (point.list_size < 2U || point.r2 == 0U || point.r2 >= point.r1 || point.q_bits == 0U) {
    selection.message = "paper scaling point is not a usable finite arithmetic configuration";
    return selection;
  }
  std::uint64_t required_capacity = 0;
  if (!checked_multiply(point.list_size, 4U, &required_capacity)) {
    selection.status = PaperCrossoverStatus::ResourceLimit;
    selection.message = "requested list size overflows the capacity heuristic";
    return selection;
  }
  std::uint32_t shape_n =
      ((point.r1 / limits::full_ter::kFiniteDimensionDivisor) + 1U) *
      limits::full_ter::kFiniteDimensionDivisor;
  for (std::uint32_t step = 0;
       step < limits::paper_crossover::kShapeSearchMaximumSteps;
       ++step) {
    selection.search_steps = step + 1U;
    if (shape_n <= point.r1) {
      selection.message = "shape dimension search failed to exceed r1";
      return selection;
    }
    const auto parameters =
        derive_full_ter_parameters(ShapeFamily::PaperRounded, shape_n, shape_n);
    if (parameters.status == Status::Completed &&
        parameters.value.level3_active_half.total() == shape_n / 2U) {
      const std::uint64_t domain =
          saturating_half_domain(shape_n / 2U, parameters.value.level3_active_half,
                                 limits::paper_crossover::kShapeDomainCapacityCap);
      const bool saturated = domain >= limits::paper_crossover::kShapeDomainCapacityCap;
      std::uint64_t pair_domain = 0;
      const bool representable = checked_multiply(domain, domain, &pair_domain);
      const std::uint64_t expected = representable ? (pair_domain >> point.r2)
                                                   : std::numeric_limits<std::uint64_t>::max();
      if (expected >= required_capacity) {
        selection.status = PaperCrossoverStatus::Completed;
        selection.shape_n = shape_n;
        selection.parameters = parameters.value;
        selection.half_domain = domain;
        selection.half_domain_saturated = saturated;
        return selection;
      }
    }
    shape_n += limits::full_ter::kFiniteDimensionDivisor;
  }
  selection.status = PaperCrossoverStatus::GenerationIncomplete;
  selection.message = "no Full TER shape within the declared search bound supplies the requested capacity";
  return selection;
}

// ---------------------------------------------------------------------------
// Sections 7/8: snapshot construction.
// ---------------------------------------------------------------------------

std::uint64_t paper_crossover_fingerprint(const PaperCrossoverSnapshot& snapshot) noexcept {
  std::uint64_t state = 14'695'981'039'346'656'037ULL;
  const auto mix = [&state](const std::uint64_t word) {
    state ^= word;
    state *= 1'099'511'628'211ULL;
  };
  mix(static_cast<std::uint64_t>(snapshot.family));
  mix(snapshot.seed);
  mix(snapshot.requested_list_size);
  mix(snapshot.point.q_bits);
  mix(snapshot.point.r2);
  mix(snapshot.point.r1);
  mix(snapshot.shape_n);
  mix(snapshot.parameters.level2.negative);
  mix(snapshot.parameters.level2.zero);
  mix(snapshot.parameters.level2.positive);
  mix(snapshot.parameters.level3_active_half.negative);
  mix(snapshot.parameters.level3_active_half.zero);
  mix(snapshot.parameters.level3_active_half.positive);
  mix(snapshot.input.instance.n);
  mix(snapshot.input.instance.b1);
  mix(snapshot.input.instance.seed);
  for (const auto residue : snapshot.input.instance.residues_m1) {
    mix(residue);
  }
  mix(snapshot.input.spec.arithmetic.b2);
  mix(snapshot.input.spec.arithmetic.b1);
  mix(snapshot.input.spec.arithmetic.target_a2);
  mix(snapshot.input.spec.arithmetic.target_b2);
  mix(snapshot.input.spec.arithmetic.target_c2);
  mix(snapshot.input.spec.arithmetic.target_1);
  mix(snapshot.input.spec.arithmetic.tau);
  for (const auto& list : snapshot.input.lists) {
    mix(static_cast<std::uint64_t>(list.role));
    mix(list.target_m2);
    mix(list.entries.size());
    for (std::size_t index = 0; index < list.entries.size(); ++index) {
      const auto& entry = list.entries[index];
      mix(entry.id);
      mix(entry.q);
      mix(index < list.residues_m1.size() ? list.residues_m1[index] : 0U);
      for (const auto digit : entry.digits) {
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(digit) + 1));
      }
    }
  }
  return state;
}

namespace {

[[nodiscard]] PaperCrossoverSnapshotResult build_paper_scaling_snapshot_impl(
    const PaperCrossoverSnapshotConfig& config) {
  PaperCrossoverSnapshotResult result;
  if (!limits::paper_crossover::snapshot_configuration_supported(
          config.requested_list_size, config.initial_half_pool, config.max_half_pool,
          config.memory_limit_bytes)) {
    result.message = "paper-scaling snapshot configuration is invalid";
    return result;
  }
  auto point = derive_paper_scaling_point(config.requested_list_size);
  std::uint32_t shape_n = 0;
  FullTerParameters parameters{};
  std::uint64_t half_domain = 0;
  bool half_domain_saturated = false;
  std::uint32_t search_steps = 0;
  if (config.override_shape_n != 0U) {
    if (config.override_r2 == 0U || config.override_r2 >= config.override_r1 ||
        config.override_r1 >= config.override_shape_n) {
      result.message = "deterministic shape override requires 0 < r2 < r1 < shape_n";
      return result;
    }
    const auto derived = derive_full_ter_parameters(ShapeFamily::PaperRounded,
                                                    config.override_shape_n,
                                                    config.override_shape_n);
    if (derived.status != Status::Completed) {
      result.message = "overridden shape has no valid Full TER parameter derivation";
      return result;
    }
    shape_n = config.override_shape_n;
    parameters = derived.value;
    point.r2 = config.override_r2;
    point.r1 = config.override_r1;
    point.q_bits = config.override_r1 - config.override_r2;
    point.q = std::uint64_t{1} << point.q_bits;
    half_domain = saturating_half_domain(shape_n / 2U, parameters.level3_active_half,
                                         limits::paper_crossover::kShapeDomainCapacityCap);
    half_domain_saturated =
        half_domain >= limits::paper_crossover::kShapeDomainCapacityCap;
    search_steps = 0U;
  } else {
    const auto selection = select_paper_shape(point);
    if (selection.status != PaperCrossoverStatus::Completed) {
      result.status = selection.status;
      result.message = selection.message;
      return result;
    }
    shape_n = selection.shape_n;
    parameters = selection.parameters;
    half_domain = selection.half_domain;
    half_domain_saturated = selection.half_domain_saturated;
    search_steps = selection.search_steps;
  }

  const auto instance = build_instance(shape_n, static_cast<std::uint8_t>(point.r1), config.seed);
  if (instance.status != Status::Completed) {
    result.message = "paper-scaling instance construction failed: " + instance.message;
    return result;
  }

  SplitMix64 target_random(config.seed ^ 0x5851f42d4c957f2dULL);
  const auto arithmetic = build_arithmetic(shape_n,
                                           static_cast<std::uint8_t>(point.r2),
                                           static_cast<std::uint8_t>(point.r1),
                                           target_random.next(),
                                           target_random.next(),
                                           target_random.next());
  if (arithmetic.status != Status::Completed) {
    result.message = "paper-scaling merge arithmetic failed: " + arithmetic.message;
    return result;
  }

  auto& snapshot = result.value;
  snapshot.family = PaperCrossoverFamily::FullTerL2PaperScalingEmulation;
  snapshot.seed = config.seed;
  snapshot.requested_list_size = config.requested_list_size;
  snapshot.point = point;
  snapshot.shape_n = shape_n;
  snapshot.parameters = parameters;
  snapshot.generation.shape_n = shape_n;
  snapshot.generation.half_domain = half_domain;
  snapshot.generation.half_domain_saturated = half_domain_saturated;
  snapshot.generation.shape_search_steps = search_steps;

  snapshot.input.instance = instance.value;
  snapshot.input.spec.shape.family = ShapeFamily::PaperRounded;
  snapshot.input.spec.shape.n = shape_n;
  snapshot.input.spec.shape.half = parameters.level3_active_half;
  snapshot.input.spec.shape.child = parameters.level2;
  snapshot.input.spec.shape.parent = parameters.level1;
  snapshot.input.spec.arithmetic = arithmetic.value;
  snapshot.input.spec.memory_limit_bytes = config.memory_limit_bytes;

  constexpr std::array<ListRole, 3> kRoles{ListRole::A, ListRole::B, ListRole::C};
  const std::array<std::uint64_t, 3> child_targets{arithmetic.value.target_a2,
                                                   arithmetic.value.target_b2,
                                                   arithmetic.value.target_c2};
  for (std::size_t role = 0; role < kRoles.size(); ++role) {
    snapshot.input.lists[role].role = kRoles[role];
    const std::uint64_t role_seed =
        config.seed + 0x9e3779b97f4a7c15ULL * (static_cast<std::uint64_t>(role) + 1U);
    const auto status = sample_role_list(snapshot.input.instance.residues_m1,
                                         shape_n,
                                         parameters,
                                         arithmetic.value,
                                         child_targets[role],
                                         config.requested_list_size,
                                         config,
                                         half_domain,
                                         half_domain_saturated,
                                         role_seed,
                                         &snapshot.input.lists[role],
                                         &snapshot.generation.half_pool_size[role],
                                         &snapshot.generation.compatible_pairs[role],
                                         &snapshot.generation.pool_doublings[role],
                                         &result.message);
    if (status != PaperCrossoverStatus::Completed) {
      result.status = status;
      result.value = PaperCrossoverSnapshot{};
      return result;
    }
  }
  snapshot.fingerprint = paper_crossover_fingerprint(snapshot);
  result.status = PaperCrossoverStatus::Completed;
  return result;
}

}  // namespace

PaperCrossoverSnapshotResult build_paper_scaling_snapshot(
    const PaperCrossoverSnapshotConfig& config) {
  try {
    return build_paper_scaling_snapshot_impl(config);
  } catch (const std::bad_alloc&) {
    return {PaperCrossoverStatus::ResourceLimit, {}, "paper-scaling snapshot allocation failed"};
  } catch (const std::length_error& error) {
    return {PaperCrossoverStatus::ResourceLimit, {}, error.what()};
  } catch (const std::invalid_argument& error) {
    return {PaperCrossoverStatus::InternalInvariantFailure, {}, error.what()};
  }
}

PaperCrossoverSnapshotResult adopt_authentic_full_ter_snapshot(InputSnapshot input,
                                                               FullTerParameters parameters,
                                                               const std::uint64_t seed) {
  PaperCrossoverSnapshotResult result;
  const auto& arithmetic = input.spec.arithmetic;
  if (arithmetic.b2 == 0U || arithmetic.b2 >= arithmetic.b1 ||
      static_cast<std::uint32_t>(arithmetic.b1) >= input.instance.n) {
    result.message = "authentic Full TER capture does not satisfy 0 < r2 < r1 < n";
    return result;
  }
  auto& snapshot = result.value;
  snapshot.family = PaperCrossoverFamily::AuthenticFullTer;
  snapshot.seed = seed;
  snapshot.requested_list_size = 0U;
  snapshot.shape_n = input.instance.n;
  snapshot.parameters = std::move(parameters);
  snapshot.point.list_size = 0U;
  snapshot.point.effective_n = 0.0;
  snapshot.point.r2 = arithmetic.b2;
  snapshot.point.r1 = arithmetic.b1;
  snapshot.point.q_bits = static_cast<std::uint32_t>(arithmetic.b1) - arithmetic.b2;
  snapshot.point.q = std::uint64_t{1} << snapshot.point.q_bits;
  snapshot.generation.shape_n = input.instance.n;
  snapshot.input = std::move(input);
  snapshot.fingerprint = paper_crossover_fingerprint(snapshot);
  result.status = PaperCrossoverStatus::Completed;
  return result;
}

// ---------------------------------------------------------------------------
// Section 9: the snapshot audit recomputes everything.
// ---------------------------------------------------------------------------

PaperAuditResult audit_paper_crossover_snapshot(const PaperCrossoverSnapshot& snapshot) {
  PaperAuditResult result;
  const auto& input = snapshot.input;
  const auto& arithmetic = input.spec.arithmetic;
  const std::uint32_t shape_n = snapshot.shape_n;
  const auto& parameters = snapshot.parameters;

  if (shape_n == 0U || shape_n % 2U != 0U || input.instance.n != shape_n ||
      arithmetic.n != shape_n || input.spec.shape.n != shape_n) {
    result.message = "snapshot dimensions are inconsistent";
    return result;
  }
  if (arithmetic.b2 == 0U || arithmetic.b2 >= arithmetic.b1 ||
      static_cast<std::uint32_t>(arithmetic.b1) >= shape_n) {
    result.message = "snapshot violates 0 < r2 < r1 < shape_n";
    return result;
  }
  if (snapshot.point.r2 != arithmetic.b2 || snapshot.point.r1 != arithmetic.b1 ||
      snapshot.point.q_bits != static_cast<std::uint32_t>(arithmetic.b1) - arithmetic.b2 ||
      snapshot.point.q != (std::uint64_t{1} << snapshot.point.q_bits) ||
      arithmetic.q != snapshot.point.q) {
    result.message = "declared paper-scaling widths disagree with the snapshot arithmetic";
    return result;
  }
  if (parameters.level3_active_half.total() != shape_n / 2U ||
      parameters.level2.total() != shape_n ||
      parameters.level2.negative != 2U * parameters.level3_active_half.negative ||
      parameters.level2.positive != 2U * parameters.level3_active_half.positive) {
    result.message = "Full TER level-2 and level-3 half shapes are not consistent";
    return result;
  }
  if (input.instance.residues_m1.size() != shape_n ||
      input.instance.words_per_value != (shape_n + 63U) / 64U ||
      input.instance.values_le.size() !=
          static_cast<std::size_t>(shape_n) * input.instance.words_per_value) {
    result.message = "instance coefficient storage is malformed";
    return result;
  }
  for (std::uint32_t index = 0; index < shape_n; ++index) {
    const std::size_t base = static_cast<std::size_t>(index) * input.instance.words_per_value;
    if (input.instance.residues_m1[index] !=
        (input.instance.values_le[base] & arithmetic.mask_m1)) {
      result.message = "modular coefficient view disagrees with the raw instance values";
      return result;
    }
  }

  const std::uint64_t expected_c2 =
      (arithmetic.target_1 - arithmetic.target_a2 - arithmetic.target_b2) & arithmetic.mask_m2;
  if (arithmetic.target_c2 != expected_c2) {
    result.message = "child targets do not close the level-1 target relationship";
    return result;
  }
  const std::uint64_t quotient_target =
      (arithmetic.target_1 - arithmetic.target_a2 - arithmetic.target_b2 -
       arithmetic.target_c2) &
      arithmetic.mask_m1;
  if (arithmetic.tau != static_cast<QuotientResidue>(quotient_target >> arithmetic.b2)) {
    result.message = "tau is not the exact quotient of the residual level-1 target";
    return result;
  }

  constexpr std::array<ListRole, 3> kRoles{ListRole::A, ListRole::B, ListRole::C};
  const std::array<std::uint64_t, 3> child_targets{arithmetic.target_a2, arithmetic.target_b2,
                                                   arithmetic.target_c2};
  for (std::size_t role = 0; role < kRoles.size(); ++role) {
    const auto& list = input.lists[role];
    if (list.role != kRoles[role] || list.target_m2 != child_targets[role] ||
        list.entries.size() != list.residues_m1.size() || list.entries.empty()) {
      result.message = "child-list role, target, or residue metadata is invalid";
      return result;
    }
    for (std::size_t index = 0; index < list.entries.size(); ++index) {
      const auto& entry = list.entries[index];
      if (entry.id != index || entry.digits.size() != shape_n) {
        result.message = "child entry identity or vector length is invalid";
        return result;
      }
      if (index > 0U && !(list.entries[index - 1U].digits < entry.digits)) {
        result.message = "child list is not strictly canonical";
        return result;
      }
      const std::span<const std::int8_t> digits(entry.digits);
      const auto first = count_digits(digits.first(shape_n / 2U));
      const auto second = count_digits(digits.subspan(shape_n / 2U));
      const auto whole = count_digits(digits);
      if (!first.has_value() || !second.has_value() || !whole.has_value()) {
        result.message = "child entry contains a non-ternary digit";
        return result;
      }
      if (!counts_equal(*first, parameters.level3_active_half) ||
          !counts_equal(*second, parameters.level3_active_half)) {
        result.message = "child entry halves violate the exact Full TER level-3 half shape";
        return result;
      }
      if (!counts_equal(*whole, parameters.level2)) {
        result.message = "child entry violates the exact Full TER level-2 distribution";
        return result;
      }
      const std::uint64_t residue =
          inner_product_mod(input.instance.residues_m1, entry.digits, arithmetic.mask_m1);
      if (residue != list.residues_m1[index]) {
        result.message = "recomputed child residue disagrees with stored metadata";
        return result;
      }
      if ((residue & arithmetic.mask_m2) != list.target_m2) {
        result.message = "child residue does not satisfy its level-2 target";
        return result;
      }
      if (entry.q != quotient_residue(residue, list.target_m2, arithmetic)) {
        result.message = "stored child quotient disagrees with canonical quotient arithmetic";
        return result;
      }
      if (static_cast<std::uint64_t>(entry.q) >= arithmetic.q) {
        result.message = "child quotient leaves the declared quotient universe";
        return result;
      }
    }
  }
  if (snapshot.fingerprint != paper_crossover_fingerprint(snapshot)) {
    result.message = "snapshot fingerprint does not match its current contents";
    return result;
  }
  result.status = PaperCrossoverStatus::Completed;
  return result;
}

// ---------------------------------------------------------------------------
// Sections 16-20: the measurement.
// ---------------------------------------------------------------------------

namespace {

struct MethodBudget {
  std::uint64_t direct_full = 0;
  std::uint64_t direct_production = 0;
  std::uint64_t direct_tiled = 0;
  std::uint64_t ntt_plan = 0;
  std::uint64_t ntt_run = 0;
  std::uint64_t ntt_peak = 0;
  bool representable = false;
};

[[nodiscard]] MethodBudget compute_budgets(const std::uint64_t q,
                                           const std::uint32_t log_q,
                                           const std::uint64_t a_size,
                                           const std::uint64_t b_size,
                                           const std::uint64_t c_size) noexcept {
  MethodBudget budget;
  const std::uint64_t element = sizeof(std::uint32_t);
  std::uint64_t direct_full = 0;
  std::uint64_t direct_production = 0;
  std::uint64_t plan = sizeof(PaperNttPlan);
  std::uint64_t run = 0;
  const std::uint64_t reversal_entries = std::uint64_t{1} << ((log_q + 1U) / 2U);
  if (!checked_add_bytes(&direct_full, q, element) ||
      !checked_add_bytes(&direct_full, c_size, element) ||
      !checked_add_bytes(&direct_production, q + 1U, element) ||
      !checked_add_bytes(&direct_production, c_size, element) ||
      !checked_add_bytes(&plan, q - 1U, element) ||
      !checked_add_bytes(&plan, reversal_entries, element) ||
      !checked_add_bytes(&run, q, element) || !checked_add_bytes(&run, q, element) ||
      !checked_add_bytes(&run, c_size, element)) {
    return budget;
  }
  std::uint64_t peak = 0;
  if (!checked_add(plan, run, &peak)) {
    return budget;
  }
  // The tiled comparator's fixed auxiliary requirement, computed by the shared
  // primitive so the two never disagree.  It is evaluated independently of every
  // other method and is never summed with them.
  std::uint64_t tiled_count_bytes = 0;
  std::uint64_t tiled_report_bytes = 0;
  std::uint64_t tiled_input_entries = 0;
  if (!tiled_q3report_auxiliary_bytes(a_size, b_size, c_size, q,
                                      limits::tiled::kDefaultPreferredTileBits, false,
                                      &tiled_count_bytes,
                                      &tiled_report_bytes) ||
      !checked_add(a_size, b_size, &tiled_input_entries) ||
      !checked_add(tiled_input_entries, c_size, &tiled_input_entries) ||
      !checked_add_bytes(&tiled_count_bytes, tiled_input_entries, sizeof(TiledQEntry))) {
    return budget;
  }
  budget.direct_full = direct_full;
  budget.direct_production = direct_production;
  budget.direct_tiled = tiled_count_bytes;
  budget.ntt_plan = plan;
  budget.ntt_run = run;
  budget.ntt_peak = peak;
  budget.representable = true;
  return budget;
}

[[nodiscard]] PaperCrossoverResult run_paper_crossover_experiment_impl(
    const PaperCrossoverSnapshot& snapshot,
    const PaperCrossoverConfig& config) {
  PaperCrossoverResult result;
  auto& telemetry = result.telemetry;
  if (!limits::paper_crossover::run_configuration_supported(config.memory_limit_bytes,
                                                             config.timed_repetitions)) {
    result.status = PaperCrossoverStatus::InvalidConfiguration;
    result.message = "the experiment requires a nonzero memory ceiling and repetition count";
    return result;
  }
  const auto audited = audit_paper_crossover_snapshot(snapshot);
  if (audited.status != PaperCrossoverStatus::Completed) {
    result.status = audited.status;
    result.message = "snapshot audit failed: " + audited.message;
    return result;
  }

  const auto& input = snapshot.input;
  const auto& arithmetic = input.spec.arithmetic;
  telemetry.family = snapshot.family;
  telemetry.seed = snapshot.seed;
  telemetry.fingerprint = snapshot.fingerprint;
  telemetry.requested_list_size = snapshot.requested_list_size;
  telemetry.a_size = input.lists[0].entries.size();
  telemetry.b_size = input.lists[1].entries.size();
  telemetry.c_size = input.lists[2].entries.size();
  telemetry.effective_n = snapshot.point.effective_n;
  telemetry.shape_n = snapshot.shape_n;
  telemetry.level2 = snapshot.parameters.level2;
  telemetry.level3_active_half = snapshot.parameters.level3_active_half;
  telemetry.r2 = arithmetic.b2;
  telemetry.r1 = arithmetic.b1;
  telemetry.q_bits = snapshot.point.q_bits;
  telemetry.q = arithmetic.q;
  telemetry.tau = arithmetic.tau;

  if (!checked_multiply(telemetry.a_size, telemetry.b_size, &telemetry.pair_universe)) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.message = "pair universe overflows";
    return result;
  }
  telemetry.lambda = static_cast<long double>(telemetry.pair_universe) /
                     static_cast<long double>(arithmetic.q);
  telemetry.poisson_support_estimate = 1.0L - std::exp(-telemetry.lambda);

  telemetry.prime = select_paper_crossover_prime(arithmetic.q, telemetry.pair_universe);
  if (telemetry.prime.status != PaperCrossoverStatus::Completed) {
    result.status = telemetry.prime.status;
    result.message = telemetry.prime.status == PaperCrossoverStatus::NotApplicable
                         ? "the required transform length exceeds the verified 2^27 ceiling"
                         : "single-prime exact reconstruction is unsafe; CRT is not implemented";
    telemetry.direct_full.status = result.status;
    telemetry.direct_production.status = result.status;
    telemetry.ntt_warm.status = result.status;
    telemetry.ntt_cold.status = result.status;
    return result;
  }

  std::array<std::vector<std::uint32_t>, 3> quotients;
  for (std::size_t role = 0; role < quotients.size(); ++role) {
    quotients[role].reserve(input.lists[role].entries.size());
    for (const auto& entry : input.lists[role].entries) {
      quotients[role].push_back(entry.q);
    }
  }
  const auto tau = static_cast<std::uint32_t>(arithmetic.tau);
  const auto mask = static_cast<std::uint32_t>(arithmetic.q - 1U);

  const auto budget = compute_budgets(arithmetic.q, snapshot.point.q_bits, telemetry.a_size,
                                      telemetry.b_size, telemetry.c_size);
  if (!budget.representable) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.message = "method memory accounting overflows";
    return result;
  }
  telemetry.ntt_plan_bytes = budget.ntt_plan;
  telemetry.ntt_run_bytes = budget.ntt_run;
  telemetry.ntt_peak_bytes = budget.ntt_peak;
  telemetry.direct_full.peak_bytes = budget.direct_full;
  telemetry.direct_production.peak_bytes = budget.direct_production;
  telemetry.direct_tiled.peak_bytes = budget.direct_tiled;
  telemetry.ntt_warm.peak_bytes = budget.ntt_peak;
  telemetry.ntt_cold.peak_bytes = budget.ntt_peak;

  // Each method's ceiling is evaluated independently.  Peaks are never summed and
  // no method's rejection short-circuits another method's evaluation.
  bool any_rejected = false;
  const auto evaluate = [&config, &any_rejected](PaperMethodOutcome* const outcome,
                                                 const std::uint64_t bytes) {
    if (!limits::checked::fits_memory_budget(0U, bytes, config.memory_limit_bytes)) {
      outcome->status = PaperCrossoverStatus::ResourceLimit;
      outcome->message = "method peak exceeds the declared memory ceiling";
      any_rejected = true;
    }
  };
  evaluate(&telemetry.direct_full, budget.direct_full);
  evaluate(&telemetry.direct_production, budget.direct_production);
  evaluate(&telemetry.direct_tiled, budget.direct_tiled);
  evaluate(&telemetry.ntt_warm, budget.ntt_peak);
  evaluate(&telemetry.ntt_cold, budget.ntt_peak);
  if (any_rejected) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.message = "at least one method cannot fit the declared memory ceiling";
    return result;
  }

  // ---- PHASE A: correctness only, never timed. ----
  if (config.run_correctness_phase) {
    auto direct = direct_full_pair_counts(quotients[0], quotients[1], arithmetic.q);
    if (direct.status != PaperCrossoverStatus::Completed) {
      result.status = direct.status;
      result.message = "direct reference convolution failed: " + direct.message;
      return result;
    }
    std::uint64_t conserved = 0;
    std::uint64_t occupied = 0;
    for (const std::uint32_t value : direct.counts) {
      conserved += value;
      occupied += value != 0U ? 1U : 0U;
    }
    telemetry.pair_conservation_passed = conserved == telemetry.pair_universe;
    telemetry.occupied_pair_residues = occupied;
    telemetry.support_fraction =
        static_cast<long double>(occupied) / static_cast<long double>(arithmetic.q);

    std::vector<std::uint64_t> weighted;
    weighted.reserve(quotients[2].size());
    std::uint64_t selected_direct = 0;
    for (const std::uint32_t c : quotients[2]) {
      const std::uint32_t value = direct.counts[(tau - c) & mask];
      weighted.push_back(value);
      selected_direct += value;
    }
    telemetry.selected_weighted = build_histogram(std::move(weighted));

    DirectProductionComponents production_components;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> ids;
    const std::uint64_t raw_production = direct_production_method(
        quotients[0], quotients[1], quotients[2], arithmetic.q, tau, &offsets, &ids,
        &production_components);
    offsets.clear();
    offsets.shrink_to_fit();
    ids.clear();
    ids.shrink_to_fit();
    telemetry.raw_arithmetic_triples = raw_production;

    // Four-way exact agreement: direct_full_pair_counts, DIRECT_PRODUCTION,
    // DIRECT_PRODUCTION_TILED, and (below) the NTT selected-target sum.
    DirectTiledComponents tiled_components;
    PaperCrossoverStatus tiled_status = PaperCrossoverStatus::InvalidConfiguration;
    std::string tiled_message;
    const std::uint64_t raw_tiled =
        direct_tiled_method(quotients[0], quotients[1], quotients[2], arithmetic.q, tau,
                            config.memory_limit_bytes, &tiled_components, &tiled_status,
                            &tiled_message);
    if (tiled_status != PaperCrossoverStatus::Completed) {
      result.status = tiled_status;
      result.message = "tiled reference count failed: " + tiled_message;
      return result;
    }
    if (raw_tiled != raw_production || raw_tiled != selected_direct) {
      result.status = PaperCrossoverStatus::InternalInvariantFailure;
      result.message = "DIRECT_PRODUCTION_TILED disagrees with the direct authorities";
      return result;
    }
    if (tiled_components.tiling.logical_pairs != telemetry.pair_universe) {
      result.status = PaperCrossoverStatus::InternalInvariantFailure;
      result.message = "tiled logical pair universe disagrees with |A||B|";
      return result;
    }

    auto plan = build_paper_ntt_plan(arithmetic.q, telemetry.prime.prime, telemetry.prime.primitive_root);
    if (plan.status != PaperCrossoverStatus::Completed) {
      result.status = plan.status;
      result.message = "NTT plan construction failed: " + plan.message;
      return result;
    }
    auto warm = ntt_cyclic_pair_counts(plan.value, quotients[0], quotients[1]);
    if (warm.status != PaperCrossoverStatus::Completed) {
      result.status = warm.status;
      result.message = "NTT reference convolution failed: " + warm.message;
      return result;
    }
    telemetry.exactness_passed = warm.counts == direct.counts;
    std::uint64_t selected_ntt = 0;
    for (const std::uint32_t c : quotients[2]) {
      selected_ntt += warm.counts[(tau - c) & mask];
    }
    telemetry.triple_conservation_passed = raw_production == selected_direct &&
                                           raw_production == selected_ntt &&
                                           raw_production == raw_tiled;
    direct.counts.clear();
    direct.counts.shrink_to_fit();

    {
      auto cold_plan =
          build_paper_ntt_plan(arithmetic.q, telemetry.prime.prime, telemetry.prime.primitive_root);
      if (cold_plan.status != PaperCrossoverStatus::Completed) {
        result.status = cold_plan.status;
        result.message = "cold NTT plan construction failed: " + cold_plan.message;
        return result;
      }
      auto cold = ntt_cyclic_pair_counts(cold_plan.value, quotients[0], quotients[1]);
      if (cold.status != PaperCrossoverStatus::Completed) {
        result.status = cold.status;
        result.message = "cold NTT convolution failed: " + cold.message;
        return result;
      }
      telemetry.cold_warm_identical = cold.counts == warm.counts;
    }

    if (!telemetry.exactness_passed) {
      result.status = PaperCrossoverStatus::InternalInvariantFailure;
      result.message = "NTT coefficients disagree with the direct cyclic convolution";
      return result;
    }
    if (!telemetry.pair_conservation_passed || !telemetry.triple_conservation_passed ||
        !telemetry.cold_warm_identical) {
      result.status = PaperCrossoverStatus::InternalInvariantFailure;
      result.message = "conservation or cold/warm identity failed";
      return result;
    }
  }

  const std::uint32_t total_repetitions = config.warmup_repetitions + config.timed_repetitions;
  std::vector<std::uint64_t> direct_full_totals;
  std::vector<std::uint64_t> direct_full_setup;
  std::vector<std::uint64_t> direct_full_count;
  std::vector<std::uint64_t> direct_full_lookup;
  std::vector<std::uint64_t> direct_production_totals;
  std::vector<std::uint64_t> direct_production_index;
  std::vector<std::uint64_t> direct_production_scan;
  std::vector<std::uint64_t> direct_tiled_totals;
  std::vector<std::uint64_t> direct_tiled_setup;
  std::vector<std::uint64_t> direct_tiled_scan;
  direct_full_totals.reserve(config.timed_repetitions);
  direct_production_totals.reserve(config.timed_repetitions);
  direct_tiled_totals.reserve(config.timed_repetitions);

  // ---- PHASE B: direct timing only.  No NTT buffer is alive here. ----
  std::uint64_t direct_full_selected = 0;
  std::uint64_t direct_production_raw = 0;
  std::uint64_t direct_tiled_raw = 0;
  for (std::uint32_t repetition = 0; repetition < total_repetitions; ++repetition) {
    const bool measured = repetition >= config.warmup_repetitions;
    {
      std::vector<std::uint32_t> counts;
      DirectFullComponents components;
      const auto start = Clock::now();
      direct_full_selected = direct_full_method(quotients[0], quotients[1], quotients[2],
                                                arithmetic.q, tau, &counts, &components);
      const std::uint64_t total = elapsed_ns(start);
      if (measured) {
        direct_full_totals.push_back(total);
        direct_full_setup.push_back(components.setup_ns);
        direct_full_count.push_back(components.pair_count_ns);
        direct_full_lookup.push_back(components.lookup_ns);
      }
    }
    {
      std::vector<std::uint32_t> offsets;
      std::vector<std::uint32_t> ids;
      DirectProductionComponents components;
      const auto start = Clock::now();
      direct_production_raw = direct_production_method(quotients[0], quotients[1], quotients[2],
                                                       arithmetic.q, tau, &offsets, &ids,
                                                       &components);
      const std::uint64_t total = elapsed_ns(start);
      if (measured) {
        direct_production_totals.push_back(total);
        direct_production_index.push_back(components.index_ns);
        direct_production_scan.push_back(components.scan_ns);
      }
    }
    {
      // Independently benchmarked: its own allocation, its own timing state.
      DirectTiledComponents components;
      PaperCrossoverStatus tiled_status = PaperCrossoverStatus::InvalidConfiguration;
      std::string tiled_message;
      const auto start = Clock::now();
      direct_tiled_raw =
          direct_tiled_method(quotients[0], quotients[1], quotients[2], arithmetic.q, tau,
                              config.memory_limit_bytes, &components, &tiled_status,
                              &tiled_message);
      const std::uint64_t total = elapsed_ns(start);
      if (tiled_status != PaperCrossoverStatus::Completed) {
        telemetry.direct_tiled.status = tiled_status;
        telemetry.direct_tiled.message = tiled_message;
        result.status = tiled_status;
        result.message = "timed tiled comparator failed: " + tiled_message;
        return result;
      }
      if (measured) {
        direct_tiled_totals.push_back(total);
        direct_tiled_setup.push_back(components.setup_ns);
        direct_tiled_scan.push_back(components.scan_ns);
        telemetry.direct_tiled_tiling = components.tiling;
      }
    }
  }
  if (config.run_correctness_phase &&
      (direct_full_selected != telemetry.raw_arithmetic_triples ||
       direct_production_raw != telemetry.raw_arithmetic_triples ||
       direct_tiled_raw != telemetry.raw_arithmetic_triples)) {
    result.status = PaperCrossoverStatus::InternalInvariantFailure;
    result.message = "timed direct paths disagree with the untimed correctness phase";
    return result;
  }
  if (direct_tiled_raw != direct_production_raw) {
    result.status = PaperCrossoverStatus::InternalInvariantFailure;
    result.message = "timed tiled comparator disagrees with timed DIRECT_PRODUCTION";
    return result;
  }
  telemetry.raw_arithmetic_triples = direct_production_raw;
  telemetry.direct_full.status = PaperCrossoverStatus::Completed;
  telemetry.direct_full.total = summarize(direct_full_totals);
  telemetry.direct_production.status = PaperCrossoverStatus::Completed;
  telemetry.direct_production.total = summarize(direct_production_totals);
  telemetry.direct_tiled.status = PaperCrossoverStatus::Completed;
  telemetry.direct_tiled.total = summarize(direct_tiled_totals);
  telemetry.components.direct_tiled_setup = summarize(std::move(direct_tiled_setup));
  telemetry.components.direct_tiled_scan = summarize(std::move(direct_tiled_scan));
  telemetry.components.direct_full_setup = summarize(std::move(direct_full_setup));
  telemetry.components.direct_full_pair_count = summarize(std::move(direct_full_count));
  telemetry.components.direct_full_lookup = summarize(std::move(direct_full_lookup));
  telemetry.components.direct_production_index = summarize(std::move(direct_production_index));
  telemetry.components.direct_production_scan = summarize(std::move(direct_production_scan));

  // ---- PHASE C: NTT timing only.  No direct Q-sized array is alive here. ----
  std::vector<std::uint64_t> plan_build;
  std::vector<std::uint64_t> warm_totals;
  std::vector<std::uint64_t> cold_totals;
  std::vector<std::uint64_t> frequency_build;
  std::vector<std::uint64_t> bit_reversal;
  std::vector<std::uint64_t> forward;
  std::vector<std::uint64_t> pointwise;
  std::vector<std::uint64_t> inverse;
  std::vector<std::uint64_t> reconstruction;
  std::vector<std::uint64_t> target_lookup;
  std::vector<std::uint64_t> transform_totals;

  PaperNttPlanResult resident_plan;
  {
    const auto start = Clock::now();
    resident_plan =
        build_paper_ntt_plan(arithmetic.q, telemetry.prime.prime, telemetry.prime.primitive_root);
    static_cast<void>(elapsed_ns(start));
  }
  if (resident_plan.status != PaperCrossoverStatus::Completed) {
    result.status = resident_plan.status;
    result.message = "resident NTT plan construction failed: " + resident_plan.message;
    return result;
  }
  telemetry.ntt_plan_bytes = resident_plan.value.plan_bytes();
  telemetry.ntt_run_bytes = budget.ntt_run;
  if (!checked_add(telemetry.ntt_plan_bytes, telemetry.ntt_run_bytes, &telemetry.ntt_peak_bytes)) {
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.message = "NTT memory accounting overflows";
    return result;
  }
  telemetry.ntt_warm.peak_bytes = telemetry.ntt_peak_bytes;
  telemetry.ntt_cold.peak_bytes = telemetry.ntt_peak_bytes;
  if (!limits::checked::fits_memory_budget(0U, telemetry.ntt_peak_bytes,
                                            config.memory_limit_bytes)) {
    telemetry.ntt_warm.status = PaperCrossoverStatus::ResourceLimit;
    telemetry.ntt_cold.status = PaperCrossoverStatus::ResourceLimit;
    result.status = PaperCrossoverStatus::ResourceLimit;
    result.message = "the realized NTT plan exceeds the declared memory ceiling";
    return result;
  }

  std::uint64_t warm_selected = 0;
  for (std::uint32_t repetition = 0; repetition < total_repetitions; ++repetition) {
    const bool measured = repetition >= config.warmup_repetitions;
    NttComponents components;
    const auto start = Clock::now();
    auto counts = ntt_pair_counts_internal(resident_plan.value, quotients[0], quotients[1],
                                           &components);
    const auto lookup_start = Clock::now();
    std::uint64_t selected = 0;
    for (const std::uint32_t c : quotients[2]) {
      selected += counts[(tau - c) & mask];
    }
    components.lookup_ns = elapsed_ns(lookup_start);
    const std::uint64_t total = elapsed_ns(start);
    warm_selected = selected;
    if (measured) {
      warm_totals.push_back(total);
      frequency_build.push_back(components.frequency_build_ns);
      bit_reversal.push_back(components.bit_reversal_ns);
      forward.push_back(components.forward_ns);
      pointwise.push_back(components.pointwise_ns);
      inverse.push_back(components.inverse_ns);
      reconstruction.push_back(components.reconstruction_ns);
      target_lookup.push_back(components.lookup_ns);
      transform_totals.push_back(components.bit_reversal_ns + components.forward_ns +
                                 components.pointwise_ns + components.inverse_ns +
                                 components.reconstruction_ns);
    }
  }
  if (config.run_correctness_phase && warm_selected != telemetry.raw_arithmetic_triples) {
    result.status = PaperCrossoverStatus::InternalInvariantFailure;
    result.message = "timed NTT selected-target sum disagrees with the direct triple count";
    return result;
  }
  resident_plan.value.reversal_table.clear();
  resident_plan.value.reversal_table.shrink_to_fit();
  resident_plan.value.forward_twiddles.clear();
  resident_plan.value.forward_twiddles.shrink_to_fit();

  for (std::uint32_t repetition = 0; repetition < total_repetitions; ++repetition) {
    const bool measured = repetition >= config.warmup_repetitions;
    NttComponents components;
    const auto start = Clock::now();
    auto cold_plan =
        build_paper_ntt_plan(arithmetic.q, telemetry.prime.prime, telemetry.prime.primitive_root);
    const std::uint64_t build_ns = elapsed_ns(start);
    if (cold_plan.status != PaperCrossoverStatus::Completed) {
      result.status = cold_plan.status;
      result.message = "cold NTT plan construction failed: " + cold_plan.message;
      return result;
    }
    auto counts =
        ntt_pair_counts_internal(cold_plan.value, quotients[0], quotients[1], &components);
    std::uint64_t selected = 0;
    for (const std::uint32_t c : quotients[2]) {
      selected += counts[(tau - c) & mask];
    }
    const std::uint64_t total = elapsed_ns(start);
    if (config.run_correctness_phase && selected != telemetry.raw_arithmetic_triples) {
      result.status = PaperCrossoverStatus::InternalInvariantFailure;
      result.message = "cold NTT selected-target sum disagrees with the direct triple count";
      return result;
    }
    if (measured) {
      cold_totals.push_back(total);
      plan_build.push_back(build_ns);
    }
  }

  telemetry.ntt_warm.status = PaperCrossoverStatus::Completed;
  telemetry.ntt_warm.total = summarize(warm_totals);
  telemetry.ntt_cold.status = PaperCrossoverStatus::Completed;
  telemetry.ntt_cold.total = summarize(cold_totals);
  telemetry.components.ntt_plan_build = summarize(std::move(plan_build));
  telemetry.components.ntt_frequency_build = summarize(std::move(frequency_build));
  telemetry.components.ntt_bit_reversal = summarize(std::move(bit_reversal));
  telemetry.components.ntt_forward = summarize(std::move(forward));
  telemetry.components.ntt_pointwise = summarize(std::move(pointwise));
  telemetry.components.ntt_inverse = summarize(std::move(inverse));
  telemetry.components.ntt_reconstruction = summarize(std::move(reconstruction));
  telemetry.components.ntt_target_lookup = summarize(std::move(target_lookup));
  telemetry.components.ntt_transform = summarize(std::move(transform_totals));
  // Full TER performs three L2->L1 ternary merges with the same r1/r2/Q during one
  // attempt, so one Q-specific plan is amortized across exactly three merges.
  telemetry.ntt_three_merge_amortized_ns =
      telemetry.ntt_warm.total.median_ns + telemetry.components.ntt_plan_build.median_ns / 3U;

  result.status = PaperCrossoverStatus::Completed;
  return result;
}

}  // namespace

PaperCrossoverResult run_paper_crossover_experiment(const PaperCrossoverSnapshot& snapshot,
                                                    const PaperCrossoverConfig& config) {
  try {
    return run_paper_crossover_experiment_impl(snapshot, config);
  } catch (const std::bad_alloc&) {
    return {PaperCrossoverStatus::ResourceLimit, {}, "paper crossover allocation failed"};
  } catch (const std::length_error& error) {
    return {PaperCrossoverStatus::ResourceLimit, {}, error.what()};
  } catch (const std::exception& error) {
    return {PaperCrossoverStatus::InternalInvariantFailure, {},
            std::string{"paper crossover internal exception: "} + error.what()};
  }
}

}  // namespace ter
