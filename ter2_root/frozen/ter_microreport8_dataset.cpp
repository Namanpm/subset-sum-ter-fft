#include "ter_microreport8_dataset.hpp"

#include "ter/instance.hpp"
#include "ter/limitations.hpp"
#include "ter_root_capture_adapter.hpp"

#include <algorithm>
#include <chrono>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace ter::microreport8 {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

// ---------------------------------------------------------------------------
// Verbatim transplant from bench/ter_root_pairspace_probe.cpp.
//
// These two functions define the authentic n=72 root fixture. They are copied
// byte-for-byte so this spike observes exactly the same root as the existing
// pair-space probe, and the donor file is left untouched. The behavioural
// identity gate is the observed geometry (Q, |A|, |B|, |C|), checked below.
// Do not "improve" either function: divergence silently changes the dataset.
// ---------------------------------------------------------------------------
void add_value(ter::Instance& instance, std::uint32_t index) {
  std::uint64_t carry = 0;
  const std::size_t base = static_cast<std::size_t>(index) * instance.words_per_value;
  for (std::uint32_t word = 0; word < instance.words_per_value; ++word) {
    const std::uint64_t original = instance.root_target_le[word];
    const std::uint64_t first = original + instance.values_le[base + word];
    const std::uint64_t carry_first = first < original ? 1U : 0U;
    const std::uint64_t second = first + carry;
    const std::uint64_t carry_second = second < first ? 1U : 0U;
    instance.root_target_le[word] = second;
    carry = carry_first | carry_second;
  }
}

ter::Instance benchmark_instance(std::uint32_t n, std::uint64_t seed) {
  auto built =
      ter::build_instance(n, ter::limits::benchmark::kFullTerFixtureModulusBits, seed);
  if (built.status != ter::Status::Completed) {
    return {};
  }
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(), instance.planted_root.end(), std::uint8_t{0});
  std::fill(instance.root_target_le.begin(), instance.root_target_le.end(), 0ULL);
  const std::uint32_t half_width = instance.n / 2U;
  const std::uint32_t planted_per_half = instance.n / 4U;
  for (std::uint32_t half = 0; half < 2U; ++half) {
    const std::uint32_t begin = half * half_width;
    for (std::uint32_t offset = 0; offset < planted_per_half; ++offset) {
      const std::uint32_t coordinate = begin + offset;
      instance.planted_root[coordinate] = 1U;
      add_value(instance, coordinate);
    }
  }
  const std::uint32_t top_word_bits = instance.n % 64U;
  if (top_word_bits != 0U) {
    instance.root_target_le.back() &= (std::uint64_t{1} << top_word_bits) - 1U;
  }
  return instance;
}
// --------------------------- end verbatim transplant -----------------------

[[nodiscard]] std::size_t word_count_for(const std::uint32_t n) noexcept {
  return (static_cast<std::size_t>(n) + 31U) / 32U;
}

void mask_to_width(std::vector<std::uint32_t>& words, const std::uint32_t n) noexcept {
  const std::size_t needed = word_count_for(n);
  words.resize(needed, 0U);
  if ((n % 32U) != 0U) {
    words[needed - 1U] &= (std::uint32_t{1} << (n % 32U)) - 1U;
  }
}

[[nodiscard]] std::uint32_t word_at(const std::vector<std::uint32_t>& words,
                                    const std::size_t index) noexcept {
  return index < words.size() ? words[index] : 0U;
}

[[nodiscard]] double percentile(std::vector<std::uint32_t> values, const double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = fraction * static_cast<double>(values.size() - 1U);
  const std::size_t lower = static_cast<std::size_t>(position);
  const std::size_t upper = lower + 1U < values.size() ? lower + 1U : lower;
  const double weight = position - static_cast<double>(lower);
  return static_cast<double>(values[lower]) * (1.0 - weight) +
         static_cast<double>(values[upper]) * weight;
}

[[nodiscard]] FamilySummary summarize(
    const std::array<std::vector<Posting>, kSplitRadix>& families) {
  FamilySummary summary;
  std::vector<std::uint32_t> sizes;
  sizes.reserve(kSplitRadix);
  for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
    const auto& postings = families[family];
    const auto size = static_cast<std::uint32_t>(postings.size());
    summary.sizes[family] = size;
    sizes.push_back(size);
    summary.postings += size;
    summary.empty_families += size == 0U ? 1U : 0U;
    // Families are stored ascending, so a run scan gives distinct counts and
    // the largest posting multiplicity without another sort.
    std::size_t index = 0U;
    while (index < postings.size()) {
      std::size_t run = index + 1U;
      while (run < postings.size() && postings[run].value == postings[index].value) {
        ++run;
      }
      ++summary.distinct_values;
      const auto multiplicity = static_cast<std::uint32_t>(run - index);
      summary.max_multiplicity = std::max(summary.max_multiplicity, multiplicity);
      index = run;
    }
  }
  summary.duplicate_values = summary.postings - summary.distinct_values;
  summary.minimum = *std::min_element(sizes.begin(), sizes.end());
  summary.maximum = *std::max_element(sizes.begin(), sizes.end());
  summary.mean = static_cast<double>(summary.postings) / static_cast<double>(kSplitRadix);
  summary.median = percentile(sizes, 0.50);
  summary.p90 = percentile(sizes, 0.90);
  summary.p95 = percentile(sizes, 0.95);
  summary.p99 = percentile(sizes, 0.99);
  return summary;
}

}  // namespace

void build_target(const std::vector<Posting>& postings, TargetSet& target) {
  target.exact.assign(kExactWords, 0U);
  target.coarse.assign(kCoarseWords, 0U);
  target.sig07.fill(0U);
  target.sig815.fill(0U);
  target.values.clear();
  target.values.reserve(postings.size());
  target.distinct.clear();
  for (const auto& posting : postings) {
    const std::uint32_t value = posting.value;
    target.values.push_back(value);
    if (!exact_contains(target.exact.data(), value)) {
      target.distinct.push_back(value);
    }
    target.exact[value >> 5U] |= std::uint32_t{1} << (value & 31U);
    const std::uint32_t block = value >> kCoarseShift;
    target.coarse[block >> 5U] |= std::uint32_t{1} << (block & 31U);
    const std::uint32_t byte_value = value & 0xFFU;
    const std::uint32_t low = byte_value & 15U;
    const std::uint32_t high = byte_value >> 4U;
    if (high < 8U) {
      target.sig07[low] = static_cast<std::uint8_t>(target.sig07[low] |
                                                    (std::uint32_t{1} << high));
    } else {
      target.sig815[low] = static_cast<std::uint8_t>(target.sig815[low] |
                                                     (std::uint32_t{1} << (high - 8U)));
    }
  }
  std::sort(target.distinct.begin(), target.distinct.end());
}


// ---------------------------------------------------------------------------
// Three independent computations of S = (cA + cB + cC) mod 2^n.
//
// The donor quotient_low32_from_words() then yields
//   tau = bits[r1, r1+32) of ((T - S) mod 2^n),
// which is exactly production's tau construction. Only the carry-summing step
// is new code, so it is computed three ways whose agreement is required.
// ---------------------------------------------------------------------------
std::vector<std::uint32_t> sum_children_mod_2n_limb32(
    const std::array<std::vector<std::uint32_t>, 3>& child_targets, const std::uint32_t n) {
  const std::size_t words = word_count_for(n);
  std::vector<std::uint32_t> sum(words, 0U);
  std::uint64_t carry = 0U;
  for (std::size_t index = 0U; index < words; ++index) {
    std::uint64_t total = carry;
    for (const auto& child : child_targets) {
      total += word_at(child, index);
    }
    sum[index] = static_cast<std::uint32_t>(total & 0xFFFFFFFFU);
    carry = total >> 32U;
  }
  mask_to_width(sum, n);
  return sum;
}

std::vector<std::uint32_t> sum_children_mod_2n_limb16(
    const std::array<std::vector<std::uint32_t>, 3>& child_targets, const std::uint32_t n) {
  // Deliberately a different radix so a word-boundary or carry-propagation
  // defect cannot be common to both implementations.
  const std::size_t words = word_count_for(n);
  const std::size_t limbs = words * 2U;
  std::vector<std::uint32_t> accumulator(limbs, 0U);
  std::uint32_t carry = 0U;
  for (std::size_t limb = 0U; limb < limbs; ++limb) {
    std::uint32_t total = carry;
    for (const auto& child : child_targets) {
      const std::uint32_t word = word_at(child, limb / 2U);
      total += (limb % 2U) == 0U ? (word & 0xFFFFU) : (word >> 16U);
    }
    accumulator[limb] = total & 0xFFFFU;
    carry = total >> 16U;
  }
  std::vector<std::uint32_t> sum(words, 0U);
  for (std::size_t index = 0U; index < words; ++index) {
    sum[index] = accumulator[index * 2U] | (accumulator[index * 2U + 1U] << 16U);
  }
  mask_to_width(sum, n);
  return sum;
}

bool sum_children_residue_check(
    const std::array<std::vector<std::uint32_t>, 3>& child_targets,
    const std::vector<std::uint32_t>& sum_words,
    const std::uint32_t n) {
  // S == cA + cB + cC - k * 2^n for some k in {0, 1, 2}. Verifying that
  // congruence modulo several primes, with one k satisfying all of them,
  // exercises the carry chain without reusing either limb implementation.
  static constexpr std::array<std::uint64_t, 4> kPrimes{1000003ULL, 1000033ULL, 1000037ULL,
                                                        1000039ULL};
  const auto residue = [](const std::vector<std::uint32_t>& words,
                          const std::uint64_t prime) {
    std::uint64_t value = 0U;
    for (std::size_t index = words.size(); index-- > 0U;) {
      // value * 2^32 mod prime, in two 16-bit steps so nothing overflows.
      value = ((((value << 16U) % prime) << 16U) % prime);
      value = (value + words[index]) % prime;
    }
    return value;
  };
  const auto power_of_two = [](const std::uint32_t exponent, const std::uint64_t prime) {
    std::uint64_t value = 1U % prime;
    for (std::uint32_t step = 0U; step < exponent; ++step) {
      value = (value * 2U) % prime;
    }
    return value;
  };
  std::array<bool, 3> candidate{true, true, true};
  for (const std::uint64_t prime : kPrimes) {
    std::uint64_t children = 0U;
    for (const auto& child : child_targets) {
      children = (children + residue(child, prime)) % prime;
    }
    const std::uint64_t observed = residue(sum_words, prime);
    const std::uint64_t modulus = power_of_two(n, prime);
    for (std::uint32_t k = 0U; k < 3U; ++k) {
      const std::uint64_t expected = (observed + (modulus * k) % prime) % prime;
      candidate[k] = candidate[k] && (expected == children);
    }
  }
  return candidate[0] || candidate[1] || candidate[2];
}

std::unique_ptr<Dataset> build_dataset(const std::uint32_t root_n,
                                       const std::uint64_t root_seed,
                                       const bool run_production_anchor) {
  auto data = std::make_unique<Dataset>();
  data->n = root_n;
  data->seed = root_seed;

  const auto instance = benchmark_instance(root_n, root_seed);
  if (instance.n == 0U) {
    data->message = "failed to build the authentic root probe instance";
    return data;
  }

  FullTerRootCapture capture;
  FullTerConfig config;
  config.family = ShapeFamily::FiniteSanity;
  config.execution_mode = TerExecutionMode::FullTer;
  config.target_mode = TerTargetMode::Random;
  config.seed = root_seed;
  config.attempt_limit = limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = limits::benchmark::kFullTerMemoryBytes;
  config.root_capture = &capture;
  const auto solved = solve_full_ter(instance, config);
  static_cast<void>(solved);
  if (capture.status != Status::Completed) {
    data->message = "root capture did not complete: " + capture.message;
    return data;
  }

  data->r1 = capture.parameters.r1;
  data->quotient_bits = capture.parameters.n - capture.parameters.r1;
  data->a_size = capture.children[0].size();
  data->b_size = capture.children[1].size();
  data->c_size = capture.children[2].size();
  data->pair_universe = data->a_size * data->b_size;

  // Behavioural identity gate for the transplanted fixture. A mismatch is
  // reported with the observed geometry rather than silently accepted.
  data->geometry_matches_reference =
      root_n == kReferenceRootN && root_seed == kReferenceRootSeed &&
      data->quotient_bits == kReferenceQuotientBits && data->a_size == kReferenceASize &&
      data->b_size == kReferenceBSize && data->c_size == kReferenceCSize;
  if (root_n == kReferenceRootN && root_seed == kReferenceRootSeed &&
      !data->geometry_matches_reference) {
    data->message =
        "authentic geometry drifted from the reference n=72 root: observed Q=" +
        std::to_string(data->quotient_bits) + " |A|=" + std::to_string(data->a_size) +
        " |B|=" + std::to_string(data->b_size) + " |C|=" + std::to_string(data->c_size) +
        "; expected Q=47 |A|=19109 |B|=32496 |C|=33505";
    return data;
  }
  if (data->quotient_bits < kProjectionBits) {
    data->message = "captured quotient width is narrower than the k=25 projection";
    return data;
  }

  std::array<std::vector<TerPartitionEntry>, 3> records;
  for (std::size_t group = 0U; group < 3U; ++group) {
    auto converted = root_partition_bench::capture_list_to_records(capture, group);
    if (converted.status != Status::Completed) {
      data->message = "captured list conversion failed: " + converted.message;
      return data;
    }
    records[group] = std::move(converted.records);
  }

  // tau, derived exactly as production derives it. Note this is the
  // quotient-domain target, NOT the raw root-target word.
  std::array<std::vector<std::uint32_t>, 3> child_targets;
  for (std::size_t group = 0U; group < 3U; ++group) {
    child_targets[group] = capture.child_targets_words_le[group];
    mask_to_width(child_targets[group], capture.parameters.n);
  }
  const auto sum32 = sum_children_mod_2n_limb32(child_targets, capture.parameters.n);
  const auto sum16 = sum_children_mod_2n_limb16(child_targets, capture.parameters.n);
  const bool limbs_agree = sum32 == sum16;
  const bool residues_agree =
      sum_children_residue_check(child_targets, sum32, capture.parameters.n);
  data->tau_derivation_verified = limbs_agree && residues_agree;
  data->tau_verification_detail =
      std::string("limb32_vs_limb16=") + (limbs_agree ? "agree" : "DISAGREE") +
      " prime_residue_check=" + (residues_agree ? "pass" : "FAIL");
  if (!data->tau_derivation_verified) {
    data->message = "tau child-target summation failed independent verification: " +
                    data->tau_verification_detail;
    return data;
  }
  data->tau_low32 = root_partition_bench::quotient_low32_from_words(
      capture.root_target_words_le, sum32, capture.parameters.n, capture.parameters.r1);
  data->tau_low25 = data->tau_low32 & kProjectionMask;

  // Low-25 projection and the 6/19 split.
  const auto project = [](const std::vector<TerPartitionEntry>& source,
                          std::vector<TiledQEntry>& destination) {
    destination.clear();
    destination.reserve(source.size());
    for (const auto& record : source) {
      destination.push_back(
          TiledQEntry{static_cast<QuotientResidue>(record.low_k & kProjectionMask),
                      record.entry_id});
    }
  };
  project(records[0], data->a_low25);
  project(records[1], data->b_low25);
  project(records[2], data->c_low25);

  for (const auto& entry : data->a_low25) {
    data->q_family[entry.q & (kSplitRadix - 1U)].push_back(
        Posting{entry.q >> kSplitBits, entry.id});
  }
  for (const auto& entry : data->b_low25) {
    data->x_family[entry.q & (kSplitRadix - 1U)].push_back(
        Posting{entry.q >> kSplitBits, entry.id});
  }
  for (const auto& entry : data->c_low25) {
    // d = (tau - q_c) mod 2^25 is the exact target-side value of a + b == d.
    const std::uint32_t d = (data->tau_low25 - entry.q) & kProjectionMask;
    data->y_family[d & (kSplitRadix - 1U)].push_back(
        Posting{d >> kSplitBits, entry.id});
  }

  // Ascending order within a family. Tiles then hold value-adjacent entries,
  // which is the arrangement any windowed reporter would want. It cannot change
  // lane totals: the union of a microproblem's tiles is X_t x Q_s either way.
  const auto by_value = [](const Posting& left, const Posting& right) {
    return left.value != right.value ? left.value < right.value : left.id < right.id;
  };
  for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
    std::sort(data->q_family[family].begin(), data->q_family[family].end(), by_value);
    std::sort(data->x_family[family].begin(), data->x_family[family].end(), by_value);
    std::sort(data->y_family[family].begin(), data->y_family[family].end(), by_value);
  }

  data->q_summary = summarize(data->q_family);
  data->x_summary = summarize(data->x_family);
  data->y_summary = summarize(data->y_family);

  const auto build_start = Clock::now();
  for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
    build_target(data->y_family[family], data->targets[family]);
  }
  data->target_build_ns = elapsed_ns(build_start);
  for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
    data->view.exact[family] = data->targets[family].exact.data();
    data->view.coarse[family] = data->targets[family].coarse.data();
    data->view.sig07[family] = data->targets[family].sig07.data();
    data->view.sig815[family] = data->targets[family].sig815.data();
  }
  data->per_active_y_bitmap_bytes = kExactWords * sizeof(std::uint32_t);
  data->all_y_bitmap_bytes = data->per_active_y_bitmap_bytes * kSplitRadix;

  // Production anchor. An unmodified count_full_ter_root() configured at
  // exactly kProjectionBits reports low_prefilter_candidates, which is by
  // construction the number of captured triples whose quotients satisfy
  // q_a + q_b + q_c == tau (mod 2^25) using PRODUCTION's own tau. Matching it
  // validates this spike's tau end to end, not merely its internal consistency.
  if (run_production_anchor) {
    FullTerConfig anchor = config;
    anchor.root_capture = nullptr;
    anchor.root_prefilter_bits = kProjectionBits;
    anchor.root_output_policy = RootOutputPolicy::CountOnly;
    const auto counted = count_full_ter_root(capture, anchor);
    data->production_anchor_status =
        counted.status == Status::Completed ? "Completed" : counted.message;
    if (counted.status == Status::Completed) {
      data->production_anchor_available = true;
      data->production_low_prefilter_candidates = counted.telemetry.low_prefilter_candidates;
      data->production_valid_root_count = counted.valid_root_count;
    }
  } else {
    data->production_anchor_status = "skipped by request";
  }

  data->ok = true;
  return data;
}

// ---------------------------------------------------------------------------
// Runtime ISA eligibility: OSXSAVE + AVX + XCR0[2:1] == 11b + AVX2.
// The AVX2 feature bit alone is not enough; an operating system that has not
// enabled YMM state makes the AVX2 path unusable regardless of what CPUID says.
// ---------------------------------------------------------------------------
namespace {

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
void cpuid_count(const std::uint32_t leaf, const std::uint32_t subleaf,
                 std::uint32_t out[4]) noexcept {
#if defined(_MSC_VER)
  int registers[4] = {0, 0, 0, 0};
  __cpuidex(registers, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (std::size_t index = 0U; index < 4U; ++index) {
    out[index] = static_cast<std::uint32_t>(registers[index]);
  }
#else
  unsigned int a = 0U;
  unsigned int b = 0U;
  unsigned int c = 0U;
  unsigned int d = 0U;
  __cpuid_count(leaf, subleaf, a, b, c, d);
  out[0] = a;
  out[1] = b;
  out[2] = c;
  out[3] = d;
#endif
}

[[nodiscard]] std::uint64_t read_xcr0() noexcept {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  std::uint32_t low = 0U;
  std::uint32_t high = 0U;
  __asm__ __volatile__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
  return (static_cast<std::uint64_t>(high) << 32U) | low;
#endif
}
#endif

}  // namespace

IsaReport detect_isa() noexcept {
  IsaReport report;
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
  std::uint32_t registers[4] = {0U, 0U, 0U, 0U};
  cpuid_count(0U, 0U, registers);
  const std::uint32_t maximum_leaf = registers[0];
  if (maximum_leaf == 0U) {
    return report;
  }
  report.cpuid_available = true;

  std::uint32_t brand[4] = {0U, 0U, 0U, 0U};
  cpuid_count(0x80000000U, 0U, brand);
  if (brand[0] >= 0x80000004U) {
    for (std::uint32_t leaf = 0x80000002U; leaf <= 0x80000004U; ++leaf) {
      cpuid_count(leaf, 0U, registers);
      const std::size_t offset = static_cast<std::size_t>(leaf - 0x80000002U) * 16U;
      for (std::size_t word = 0U; word < 4U; ++word) {
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
          report.brand[offset + word * 4U + byte] =
              static_cast<char>((registers[word] >> (8U * byte)) & 0xFFU);
        }
      }
    }
    report.brand[48] = '\0';
  }

  cpuid_count(1U, 0U, registers);
  report.osxsave = (registers[2] & (1U << 27U)) != 0U;
  report.avx = (registers[2] & (1U << 28U)) != 0U;
  if (report.osxsave) {
    // Bit 1 = XMM state, bit 2 = YMM state. Both must be enabled by the OS.
    report.ymm_state_enabled = (read_xcr0() & 0x6U) == 0x6U;
  }
  if (maximum_leaf >= 7U) {
    cpuid_count(7U, 0U, registers);
    report.avx2 = (registers[1] & (1U << 5U)) != 0U;
    report.bmi2 = (registers[1] & (1U << 8U)) != 0U;
    report.avx512f = (registers[1] & (1U << 16U)) != 0U;
  }
  report.usable = report.osxsave && report.avx && report.ymm_state_enabled && report.avx2;
#endif
  return report;
}

}  // namespace ter::microreport8
