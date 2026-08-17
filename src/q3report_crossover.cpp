#include "ter/q3report_crossover.hpp"

#include "ter/arithmetic.hpp"
#include "ter/generator.hpp"
#include "ter/instance.hpp"
#include "ter/reference_vector.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace ter {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(const Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left,
                                    const std::uint64_t right,
                                    std::uint64_t* const out) {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  *out = left * right;
  return true;
}

[[nodiscard]] bool checked_add(const std::uint64_t left,
                               const std::uint64_t right,
                               std::uint64_t* const out) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *out = left + right;
  return true;
}

[[nodiscard]] bool is_power_of_two(const std::uint64_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

// Same exact single-prime cyclic NTT modulus already validated by the frozen
// Q3REPORT experiment (src/q3report_audit.cpp). Reused, not reinvented: its
// 2-adicity (23) is the hard ceiling on every representable Q in this
// benchmark. Points that require a larger Q are reported NotApplicable
// rather than silently CRT-extended, per the crossover experiment's scope
// (minimal exact machinery only).
constexpr std::uint64_t kNttPrime = 998'244'353ULL;
constexpr std::uint64_t kNttPrimitiveRoot = 3ULL;
constexpr std::uint32_t kNttMaximumPower = 23U;

std::uint64_t mod_pow(std::uint64_t base, std::uint64_t exponent, const std::uint64_t modulus) {
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

CrossoverStatus translate_generator_status(const Status status) {
  switch (status) {
    case Status::Completed:
      return CrossoverStatus::Completed;
    case Status::GenerationIncomplete:
      return CrossoverStatus::GenerationIncomplete;
    case Status::ResourceLimit:
      return CrossoverStatus::ResourceLimit;
    case Status::InvalidConfiguration:
    case Status::InvalidShape:
      return CrossoverStatus::InvalidConfiguration;
    default:
      return CrossoverStatus::InternalInvariantFailure;
  }
}

CrossoverHistogram histogram(const std::vector<std::uint64_t>& values) {
  CrossoverHistogram result;
  result.population = values.size();
  if (values.empty()) {
    return result;
  }
  std::vector<std::uint64_t> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  result.maximum = sorted.back();
  for (const auto value : values) {
    result.zero += value == 0U;
    result.one += value == 1U;
    result.at_most_two += value <= 2U;
    result.at_most_four += value <= 4U;
  }
  const auto percentile = [&sorted](const std::uint64_t numerator) {
    const auto index = static_cast<std::size_t>((numerator * (sorted.size() - 1U)) / 100U);
    return sorted[index];
  };
  result.p50 = percentile(50U);
  result.p90 = percentile(90U);
  result.p99 = percentile(99U);
  return result;
}

// Population/zero/max only, in one O(Q) pass with no sort. The full pair-
// count array has Q entries (up to 2^23); sorting it just to report unused
// percentiles would dominate wall-clock at large Q for no benefit -- the
// selected-target (unique/C-weighted) histograms carry the percentiles that
// are actually reported.
CrossoverHistogram population_histogram(const std::vector<std::uint64_t>& values) {
  CrossoverHistogram result;
  result.population = values.size();
  for (const auto value : values) {
    result.zero += value == 0U;
    result.one += value == 1U;
    result.at_most_two += value <= 2U;
    result.at_most_four += value <= 4U;
    result.maximum = std::max(result.maximum, value);
  }
  return result;
}

void apply_plan(const NttPlan& plan, std::vector<std::uint64_t>* const data, const bool inverse) {
  auto& values = *data;
  for (std::size_t index = 0; index < plan.length; ++index) {
    const auto target = plan.bit_reversal[index];
    if (index < target) {
      std::swap(values[index], values[target]);
    }
  }
  std::size_t stage = 0;
  for (std::size_t length = 2; length <= plan.length; length <<= 1U, ++stage) {
    const auto root = inverse ? plan.inverse_root[stage] : plan.forward_root[stage];
    const std::size_t half = length / 2U;
    for (std::size_t start = 0; start < plan.length; start += length) {
      std::uint64_t omega = 1U;
      for (std::size_t offset = 0; offset < half; ++offset) {
        const auto even = values[start + offset];
        const auto odd = (values[start + offset + half] * omega) % kNttPrime;
        values[start + offset] = (even + odd) % kNttPrime;
        values[start + offset + half] = (even + kNttPrime - odd) % kNttPrime;
        omega = (omega * root) % kNttPrime;
      }
    }
  }
  if (inverse) {
    for (auto& value : values) {
      value = (value * plan.inverse_length) % kNttPrime;
    }
  }
}

}  // namespace

CrossoverSnapshotResult build_scaling_snapshot(const CrossoverSnapshotConfig& config) {
  CrossoverSnapshotResult result;
  if (config.list_size == 0U || config.r2 == 0U || config.r2 >= config.r1 ||
      config.r1 >= config.n || config.r1 >= 64U ||
      static_cast<std::uint32_t>(config.r1 - config.r2) > kNttMaximumPower) {
    result.status = CrossoverStatus::InvalidConfiguration;
    result.message =
        "crossover snapshot configuration is invalid or requires a Q beyond the "
        "supported single-prime transform length (2^23)";
    return result;
  }

  SnapshotConfig generator_config;
  generator_config.family = ShapeFamily::FiniteSanity;
  generator_config.n = config.n;
  generator_config.b2 = config.r2;
  generator_config.b1 = config.r1;
  generator_config.seed = config.seed;
  generator_config.requested_list_size = config.list_size;
  generator_config.mode = GeneratorMode::MitmPerformance;
  generator_config.plant_valid_triple = false;
  generator_config.half_pool_size = config.half_pool_size;
  generator_config.max_half_pool_size = config.max_half_pool_size;
  generator_config.attempt_limit = config.attempt_limit;
  generator_config.memory_limit_bytes = config.memory_limit_bytes;

  const auto generated = build_snapshot(generator_config);
  if (generated.status != Status::Completed) {
    result.status = translate_generator_status(generated.status);
    result.message = generated.message.empty()
                          ? "TER_SCALING_EMULATION generation did not complete"
                          : generated.message;
    return result;
  }

  result.value.input = generated.snapshot;
  result.value.seed = config.seed;

  std::uint64_t fingerprint = 0x9E3779B97F4A7C15ULL ^ config.seed;
  const auto mix = [&fingerprint](const std::uint64_t value) {
    fingerprint ^= value + 0x9E3779B97F4A7C15ULL + (fingerprint << 6) + (fingerprint >> 2);
  };
  const auto& arithmetic = result.value.input.spec.arithmetic;
  mix(arithmetic.n);
  mix(arithmetic.b1);
  mix(arithmetic.b2);
  mix(arithmetic.q);
  mix(arithmetic.tau);
  for (const auto& list : result.value.input.lists) {
    mix(list.target_m2);
    for (const auto& entry : list.entries) {
      mix(entry.id);
      mix(entry.q);
      for (const auto digit : entry.digits) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(digit)));
      }
    }
  }
  result.value.fingerprint = fingerprint;
  result.status = CrossoverStatus::Completed;
  return result;
}

CrossoverStatus audit_scaling_snapshot(const CrossoverSnapshot& snapshot,
                                       std::string* const message) {
  const auto& input = snapshot.input;
  const auto& arithmetic = input.spec.arithmetic;
  const auto& shape = input.spec.shape;

  if (arithmetic.b2 == 0U || arithmetic.b2 >= arithmetic.b1 || arithmetic.q == 0U ||
      !is_power_of_two(arithmetic.q)) {
    *message = "crossover snapshot arithmetic parameters are invalid";
    return CrossoverStatus::InternalInvariantFailure;
  }

  const std::array<std::uint64_t, 3> child_targets_m2 = {
      arithmetic.target_a2, arithmetic.target_b2, arithmetic.target_c2};

  for (std::size_t role = 0; role < 3U; ++role) {
    const auto& list = input.lists[role];
    if (list.target_m2 != child_targets_m2[role]) {
      *message = "list child target does not match the frozen arithmetic target";
      return CrossoverStatus::InternalInvariantFailure;
    }
    std::unordered_set<EntryId> seen_ids;
    seen_ids.reserve(list.entries.size());
    for (const auto& entry : list.entries) {
      if (!seen_ids.insert(entry.id).second) {
        *message = "duplicate EntryId within a scaling list";
        return CrossoverStatus::InternalInvariantFailure;
      }
      const auto counts = count_digits(entry.digits);
      if (!counts.has_value() || counts->negative != shape.child.negative ||
          counts->zero != shape.child.zero || counts->positive != shape.child.positive) {
        *message = "scaling entry does not have the exact required L2 digit distribution";
        return CrossoverStatus::InternalInvariantFailure;
      }
      const auto residue =
          inner_product_mod(input.instance.residues_m1, entry.digits, arithmetic.mask_m1);
      if ((residue & arithmetic.mask_m2) != child_targets_m2[role]) {
        *message = "recomputed residue does not satisfy the child target modulo 2^r2";
        return CrossoverStatus::InternalInvariantFailure;
      }
      const auto recomputed_q = quotient_residue(residue, child_targets_m2[role], arithmetic);
      if (recomputed_q != entry.q) {
        *message =
            "recomputed target-aware quotient residue disagrees with the stored quotient";
        return CrossoverStatus::InternalInvariantFailure;
      }
    }
  }
  return CrossoverStatus::Completed;
}

CrossoverStatus build_ntt_plan(const std::uint64_t length,
                               NttPlan* const plan,
                               std::string* const message) {
  if (!is_power_of_two(length) || length > (std::uint64_t{1} << kNttMaximumPower)) {
    *message = "NTT length is unsupported by the exact single-prime transform";
    return CrossoverStatus::NotApplicable;
  }
  plan->length = length;
  plan->bit_reversal.assign(static_cast<std::size_t>(length), 0U);
  for (std::size_t index = 1, reversed = 0; index < length; ++index) {
    std::size_t bit = static_cast<std::size_t>(length) >> 1U;
    for (; (reversed & bit) != 0U; bit >>= 1U) {
      reversed ^= bit;
    }
    reversed ^= bit;
    plan->bit_reversal[index] = static_cast<std::uint32_t>(reversed);
  }
  const auto stages = static_cast<std::size_t>(std::countr_zero(length));
  plan->forward_root.assign(stages, 0U);
  plan->inverse_root.assign(stages, 0U);
  std::size_t stage = 0;
  for (std::size_t stage_length = 2; stage_length <= length; stage_length <<= 1U, ++stage) {
    const auto root = mod_pow(kNttPrimitiveRoot, (kNttPrime - 1U) / stage_length, kNttPrime);
    plan->forward_root[stage] = root;
    plan->inverse_root[stage] = mod_pow(root, kNttPrime - 2U, kNttPrime);
  }
  plan->inverse_length = mod_pow(length, kNttPrime - 2U, kNttPrime);
  return CrossoverStatus::Completed;
}

CrossoverRunResult run_crossover_point(const CrossoverSnapshot& snapshot,
                                       const NttPlan* const plan,
                                       const std::uint64_t memory_limit_bytes) {
  CrossoverRunResult out;

  std::string audit_message;
  const auto audit_status = audit_scaling_snapshot(snapshot, &audit_message);
  if (audit_status != CrossoverStatus::Completed) {
    out.status = audit_status;
    out.message = audit_message;
    return out;
  }

  const auto& input = snapshot.input;
  const auto& arithmetic = input.spec.arithmetic;
  const auto q = arithmetic.q;

  if (!is_power_of_two(q) || q > (std::uint64_t{1} << kNttMaximumPower)) {
    out.status = CrossoverStatus::NotApplicable;
    out.message = "Q exceeds the supported single-prime transform length (2^23)";
    return out;
  }
  if (plan != nullptr && plan->length != q) {
    out.status = CrossoverStatus::InvalidConfiguration;
    out.message = "supplied NTT plan length does not match the snapshot's Q";
    return out;
  }

  std::array<std::vector<std::uint64_t>, 3> quotients;
  for (std::size_t role = 0; role < 3U; ++role) {
    quotients[role].reserve(input.lists[role].entries.size());
    for (const auto& entry : input.lists[role].entries) {
      quotients[role].push_back(entry.q);
    }
  }

  auto& report = out.report;
  report.a_size = quotients[0].size();
  report.b_size = quotients[1].size();
  report.c_size = quotients[2].size();
  report.r2 = arithmetic.b2;
  report.r1 = arithmetic.b1;
  report.q = q;

  std::uint64_t pair_bound = 0;
  if (!checked_multiply(report.a_size, report.b_size, &pair_bound)) {
    out.status = CrossoverStatus::ResourceLimit;
    out.message = "|A|*|B| overflows 64-bit arithmetic";
    return out;
  }
  if (pair_bound >= kNttPrime) {
    out.status = CrossoverStatus::ArithmeticSafetyFailure;
    out.message = "|A|*|B| does not satisfy the strict single-prime exactness bound";
    return out;
  }
  report.lambda = static_cast<long double>(pair_bound) / static_cast<long double>(q);
  report.predicted_support_fraction = 1.0L - std::exp(-report.lambda);

  std::uint64_t direct_bytes = 0;
  std::uint64_t ntt_per_run_bytes = 0;
  std::uint64_t ntt_plan_bytes = 0;
  if (!checked_multiply(q, sizeof(std::uint64_t), &direct_bytes) ||
      !checked_multiply(q, sizeof(std::uint64_t) * 2U, &ntt_per_run_bytes)) {
    out.status = CrossoverStatus::ResourceLimit;
    out.message = "count-array byte accounting overflows";
    return out;
  }
  if (plan == nullptr) {
    ntt_plan_bytes = q * sizeof(std::uint32_t) +
                     2U * static_cast<std::uint64_t>(std::countr_zero(q)) * sizeof(std::uint64_t);
  }
  std::uint64_t total_bytes = 0;
  if (!checked_add(direct_bytes, ntt_per_run_bytes, &total_bytes) ||
      !checked_add(total_bytes, ntt_plan_bytes, &total_bytes) ||
      total_bytes > memory_limit_bytes) {
    out.status = CrossoverStatus::ResourceLimit;
    out.message = "direct/NTT count-array buffers exceed the declared memory ceiling";
    return out;
  }
  report.memory.direct_bytes = direct_bytes;
  report.memory.ntt_per_run_bytes = ntt_per_run_bytes;
  report.memory.ntt_plan_bytes = ntt_plan_bytes;

  auto& timing = report.timing;

  // DIRECT count-only path: naive O(|A|*|B|) cyclic pair-count build.
  const auto direct_setup_start = Clock::now();
  std::vector<std::uint64_t> direct_pair_counts(static_cast<std::size_t>(q), 0U);
  timing.direct_setup_ns = elapsed_ns(direct_setup_start);

  const auto direct_count_start = Clock::now();
  for (const auto a : quotients[0]) {
    for (const auto b : quotients[1]) {
      ++direct_pair_counts[static_cast<std::size_t>((a + b) & (q - 1U))];
    }
  }
  timing.direct_count_ns = elapsed_ns(direct_count_start);

  std::vector<std::uint64_t> direct_weighted;
  direct_weighted.reserve(quotients[2].size());
  const auto direct_lookup_start = Clock::now();
  for (const auto c : quotients[2]) {
    const auto target =
        required_pair_residue(static_cast<QuotientResidue>(c), arithmetic);
    direct_weighted.push_back(direct_pair_counts[target]);
  }
  timing.direct_lookup_ns = elapsed_ns(direct_lookup_start);

  // NTT count-only path.
  NttPlan local_plan;
  const NttPlan* active_plan = plan;
  if (active_plan == nullptr) {
    const auto plan_start = Clock::now();
    std::string plan_message;
    const auto plan_status = build_ntt_plan(q, &local_plan, &plan_message);
    timing.ntt_plan_build_ns = elapsed_ns(plan_start);
    if (plan_status != CrossoverStatus::Completed) {
      out.status = plan_status;
      out.message = plan_message;
      return out;
    }
    active_plan = &local_plan;
  }

  const auto frequency_start = Clock::now();
  std::vector<std::uint64_t> left(static_cast<std::size_t>(q), 0U);
  std::vector<std::uint64_t> right(static_cast<std::size_t>(q), 0U);
  for (const auto value : quotients[0]) {
    ++left[static_cast<std::size_t>(value)];
  }
  for (const auto value : quotients[1]) {
    ++right[static_cast<std::size_t>(value)];
  }
  timing.ntt_frequency_ns = elapsed_ns(frequency_start);

  const auto transform_start = Clock::now();
  apply_plan(*active_plan, &left, false);
  apply_plan(*active_plan, &right, false);
  for (std::size_t index = 0; index < left.size(); ++index) {
    left[index] = (left[index] * right[index]) % kNttPrime;
  }
  apply_plan(*active_plan, &left, true);
  timing.ntt_transform_ns = elapsed_ns(transform_start);

  const auto reconstruction_start = Clock::now();
  bool coefficient_overflow = false;
  for (const auto value : left) {
    coefficient_overflow = coefficient_overflow || value > pair_bound;
  }
  timing.ntt_reconstruction_ns = elapsed_ns(reconstruction_start);
  if (coefficient_overflow) {
    out.status = CrossoverStatus::InternalInvariantFailure;
    out.message = "reconstructed NTT coefficient exceeds the proven exact bound |A|*|B|";
    return out;
  }
  std::vector<std::uint64_t> ntt_pair_counts = std::move(left);

  std::vector<std::uint64_t> ntt_weighted;
  std::vector<std::uint64_t> ntt_unique;
  ntt_weighted.reserve(quotients[2].size());
  ntt_unique.reserve(quotients[2].size());
  std::vector<bool> seen(static_cast<std::size_t>(q), false);
  const auto ntt_lookup_start = Clock::now();
  for (const auto c : quotients[2]) {
    const auto target =
        required_pair_residue(static_cast<QuotientResidue>(c), arithmetic);
    const auto value = ntt_pair_counts[target];
    ntt_weighted.push_back(value);
    if (!seen[target]) {
      seen[target] = true;
      ntt_unique.push_back(value);
    }
  }
  timing.ntt_lookup_ns = elapsed_ns(ntt_lookup_start);

  report.direct_matches_ntt =
      direct_pair_counts == ntt_pair_counts && direct_weighted == ntt_weighted;
  if (!report.direct_matches_ntt) {
    out.status = CrossoverStatus::InternalInvariantFailure;
    out.message = "direct count-only path disagrees with the exact NTT count-only path";
    return out;
  }
  report.exactness_passed = true;

  report.all_residues = population_histogram(ntt_pair_counts);
  report.occupied_pair_residues = report.all_residues.population - report.all_residues.zero;
  report.unique_selected_targets = ntt_unique.size();
  report.unique_selected = histogram(ntt_unique);
  report.c_weighted_selected = histogram(ntt_weighted);

  out.status = CrossoverStatus::Completed;
  return out;
}

}  // namespace ter
