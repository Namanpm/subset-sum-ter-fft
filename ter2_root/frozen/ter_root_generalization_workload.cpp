#include "ter_root_generalization_workload.hpp"

#include "ter/full_ter.hpp"
#include "ter/instance.hpp"
#include "ter/limitations.hpp"
#include "ter/shape.hpp"
#include "ter_root_capture_adapter.hpp"
#include "ter_root_snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ter::root_generalization {
namespace {

// ---------------------------------------------------------------------------
// Verbatim transplant from bench/ter_root_pairspace_probe.cpp, identical to the
// copies in bench/ter_microreport8_dataset.cpp and
// bench/ter_translated_tag_dataset.cpp.
//
// Used ONLY to diagnose a capture that did not complete: the donor dataset
// builder discards the solver's status, so the exact same solve is repeated to
// learn it instead of guessing.  Do not "improve" either function: divergence
// silently changes which instance is solved.
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

[[nodiscard]] const char* solver_status_name(const Status status) noexcept {
  switch (status) {
    case Status::Completed:
      return "Completed";
    case Status::Found:
      return "Found";
    case Status::NoWitnessThisAttempt:
      return "NoWitnessThisAttempt";
    case Status::AttemptLimitReached:
      return "AttemptLimitReached";
    case Status::InvalidConfiguration:
      return "InvalidConfiguration";
    case Status::InvalidShape:
      return "InvalidShape";
    case Status::GenerationIncomplete:
      return "GenerationIncomplete";
    case Status::ResourceLimit:
      return "ResourceLimit";
    case Status::Cancelled:
      return "Cancelled";
    case Status::InvariantViolation:
      return "InvariantViolation";
    case Status::NotApplicable:
      return "NotApplicable";
    case Status::UnsupportedWidth:
      return "UnsupportedWidth";
  }
  return "Unknown";
}

// Repeats the donor's exact solve (same instance, same config, same capture
// seam) and returns the solver's own terminal status and message.
struct CaptureDiagnosis {
  Status status = Status::InvalidConfiguration;
  bool capture_completed = false;
  std::string message;
  std::uint64_t peak_working_bytes = 0;
};

[[nodiscard]] CaptureDiagnosis diagnose_capture(const std::uint32_t n, const std::uint64_t seed) {
  CaptureDiagnosis diagnosis;
  const auto instance = benchmark_instance(n, seed);
  if (instance.n == 0U) {
    diagnosis.message = "instance construction failed";
    return diagnosis;
  }
  FullTerRootCapture capture;
  FullTerConfig config;
  config.family = ShapeFamily::FiniteSanity;
  config.execution_mode = TerExecutionMode::FullTer;
  config.target_mode = TerTargetMode::Random;
  config.seed = seed;
  config.attempt_limit = limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = limits::benchmark::kFullTerMemoryBytes;
  config.root_capture = &capture;
  const auto solved = solve_full_ter(instance, config);
  diagnosis.status = solved.status;
  diagnosis.capture_completed = capture.status == Status::Completed;
  diagnosis.message = solved.message;
  diagnosis.peak_working_bytes = solved.telemetry.peak_working_bytes;
  return diagnosis;
}

using Clock = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// SplitMix64: the repository's declared deterministic generator.
struct SplitMix64 {
  std::uint64_t state = 0;
  [[nodiscard]] std::uint64_t next() noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }
  // Unbiased value in [0, bound) by rejection; bound > 0.
  [[nodiscard]] std::uint64_t below(const std::uint64_t bound) noexcept {
    const std::uint64_t limit = ~std::uint64_t{0} - (~std::uint64_t{0} % bound);
    std::uint64_t value = next();
    while (value >= limit) {
      value = next();
    }
    return value % bound;
  }
};

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

// Distinct generator stream per (family, scale, seed, list) so no two workloads
// or lists share a sequence.
[[nodiscard]] std::uint64_t stream_seed(const WorkloadFamily family, const double scale,
                                        const std::uint64_t seed,
                                        const std::uint32_t list) noexcept {
  const auto scale_milli = static_cast<std::uint64_t>(std::llround(scale * 1000.0));
  return mix64(mix64(mix64(seed) ^ (static_cast<std::uint64_t>(family) << 56U)) ^
               (scale_milli << 8U) ^ list);
}

// Family size f of `total` postings spread as evenly as possible over 64.
[[nodiscard]] std::uint32_t even_share(const std::uint64_t total, const std::uint32_t family) {
  return static_cast<std::uint32_t>((total * (family + 1U)) / kFamilies -
                                    (total * family) / kFamilies);
}

void sort_family(std::vector<mr8::Posting>& postings) {
  std::sort(postings.begin(), postings.end(),
            [](const mr8::Posting& left, const mr8::Posting& right) {
              return left.value != right.value ? left.value < right.value
                                               : left.id < right.id;
            });
}

// Builds the Y targets and view exactly as the donor does, through the donor's
// single exported rule (mr8::build_target).  Families must already be sorted.
void finish_synthetic_dataset(mr8::Dataset& data) {
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    mr8::build_target(data.y_family[family], data.targets[family]);
  }
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    data.view.exact[family] = data.targets[family].exact.data();
    data.view.coarse[family] = data.targets[family].coarse.data();
    data.view.sig07[family] = data.targets[family].sig07.data();
    data.view.sig815[family] = data.targets[family].sig815.data();
  }
  data.per_active_y_bitmap_bytes = mr8::kExactWords * sizeof(std::uint32_t);
  data.all_y_bitmap_bytes = data.per_active_y_bitmap_bytes * kFamilies;
  data.ok = true;
}

// Unsigned 128-bit value as two words; full quotients never exceed 69 bits.
struct Wide {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
};

[[nodiscard]] Wide wide_from_words(const std::span<const std::uint32_t> words) noexcept {
  Wide value;
  for (std::size_t word = 0U; word < words.size() && word < 4U; ++word) {
    const std::uint64_t part = words[word];
    if (word < 2U) {
      value.lo |= part << (32U * word);
    } else {
      value.hi |= part << (32U * (word - 2U));
    }
  }
  return value;
}

[[nodiscard]] Wide mask_bits(Wide value, const std::uint32_t bits) noexcept {
  if (bits >= 128U) {
    return value;
  }
  if (bits > 64U) {
    value.hi &= (std::uint64_t{1} << (bits - 64U)) - 1U;
  } else {
    value.hi = 0U;
    if (bits < 64U) {
      value.lo &= (std::uint64_t{1} << bits) - 1U;
    }
  }
  return value;
}

[[nodiscard]] Wide subtract(const Wide left, const Wide right) noexcept {
  Wide out;
  out.lo = left.lo - right.lo;
  out.hi = left.hi - right.hi - (left.lo < right.lo ? 1U : 0U);
  return out;
}

// (((a - b) mod 2^n) >> shift) mod 2^width, n <= 128, shift < n.
[[nodiscard]] Wide shifted_difference(const std::span<const std::uint32_t> a,
                                      const std::span<const std::uint32_t> b,
                                      const std::uint32_t n, const std::uint32_t shift,
                                      const std::uint32_t width) noexcept {
  Wide value = mask_bits(subtract(mask_bits(wide_from_words(a), n), mask_bits(wide_from_words(b), n)), n);
  if (shift >= 64U) {
    value.lo = value.hi >> (shift - 64U);
    value.hi = 0U;
  } else if (shift > 0U) {
    value.lo = (value.lo >> shift) | (value.hi << (64U - shift));
    value.hi >>= shift;
  }
  return mask_bits(value, width);
}

void fill_sizes(Workload& workload) {
  const mr8::Dataset& data = *workload.data;
  workload.a_size = 0U;
  workload.b_size = 0U;
  workload.c_size = 0U;
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    workload.a_size += data.q_family[family].size();
    workload.b_size += data.x_family[family].size();
    workload.c_size += data.y_family[family].size();
  }
  workload.pair_universe = workload.a_size * workload.b_size;
}

[[nodiscard]] std::string hex(const std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const auto digit = static_cast<std::uint32_t>((value >> shift) & 0xFU);
    if (digit != 0U || started || shift == 0) {
      out.push_back(kDigits[digit]);
      started = true;
    }
  }
  return out;
}

[[nodiscard]] std::string scale_label(const double scale) {
  const auto milli = static_cast<long long>(std::llround(scale * 1000.0));
  std::string text = std::to_string(milli / 1000) + ".";
  const long long fraction = milli % 1000;
  text += std::to_string(fraction / 100) + std::to_string((fraction / 10) % 10);
  return text;
}

}  // namespace

const char* family_label(const WorkloadFamily family) noexcept {
  switch (family) {
    case WorkloadFamily::AuthenticFullTer:
      return "AUTHENTIC_FULLTER";
    case WorkloadFamily::SyntheticUniform:
      return "SYNTHETIC_UNIFORM";
    case WorkloadFamily::ResidualProfileEmulation:
      return "RESIDUAL_PROFILE_EMULATION";
  }
  return "UNKNOWN";
}

const char* status_label(const WorkloadStatus status) noexcept {
  switch (status) {
    case WorkloadStatus::Completed:
      return "Completed";
    case WorkloadStatus::ResourceLimit:
      return "ResourceLimit";
    case WorkloadStatus::NotApplicable:
      return "NotApplicable";
    case WorkloadStatus::Failed:
      return "Failed";
  }
  return "Unknown";
}

std::uint64_t fingerprint(const mr8::Dataset& data) noexcept {
  std::uint64_t hash = 0x243F6A8885A308D3ULL;
  const auto absorb = [&hash](const std::array<std::vector<mr8::Posting>, kFamilies>& lists,
                              const std::uint64_t tag) {
    for (std::uint32_t family = 0U; family < kFamilies; ++family) {
      hash = mix64(hash ^ (tag << 32U) ^ family ^ (lists[family].size() << 8U));
      for (const mr8::Posting& posting : lists[family]) {
        hash = mix64(hash ^ ((static_cast<std::uint64_t>(posting.value) << 32U) | posting.id));
      }
    }
  };
  absorb(data.q_family, 1U);
  absorb(data.x_family, 2U);
  absorb(data.y_family, 3U);
  return hash;
}

std::unique_ptr<Workload> capture_authentic(const std::uint32_t n, const std::uint64_t seed,
                                            const bool with_authority) {
  auto workload = std::make_unique<Workload>();
  workload->family = WorkloadFamily::AuthenticFullTer;
  workload->n = n;
  workload->seed = seed;
  workload->id = "authentic_n" + std::to_string(n) + "_s" + hex(seed);
  const auto start = Clock::now();

  // Preflight on the finite parameters alone, derived exactly as solve_full_ter
  // derives them (modulus width = n): the residual microproblem only exists when
  // the quotient is at least as wide as the donor's projection.
  const auto parameters = ter::derive_full_ter_parameters(ShapeFamily::FiniteSanity, n, n);
  if (parameters.status != Status::Completed) {
    workload->status = WorkloadStatus::NotApplicable;
    workload->status_detail = "finite Full TER parameters unavailable at n=" +
                              std::to_string(n) + ": " + parameters.message;
    return workload;
  }
  workload->r1 = parameters.value.r1;
  workload->quotient_bits = n > parameters.value.r1 ? n - parameters.value.r1 : 0U;
  if (workload->quotient_bits < mr8::kProjectionBits) {
    workload->status = WorkloadStatus::NotApplicable;
    workload->status_detail =
        "quotient width Q=" + std::to_string(workload->quotient_bits) +
        " is below the 25-bit projection (6 split bits + 19 residual bits); the 19-bit "
        "residual microproblem does not exist at this n";
    return workload;
  }

  workload->data = mr8::build_dataset(n, seed, /*run_production_anchor=*/true);
  if (!workload->data->ok) {
    const std::string& message = workload->data->message;
    const std::string limit =
        std::to_string(limits::benchmark::kFullTerMemoryBytes / (1024ULL * 1024ULL)) + " MiB";
    if (message.rfind("root capture did not complete", 0U) != 0U) {
      workload->status = WorkloadStatus::Failed;
      workload->status_detail = message;
      workload->construction_seconds = seconds_since(start);
      return workload;
    }
    // The donor discards the solver's status, so the identical solve is
    // repeated to learn it.  Only a solver-reported ResourceLimit is reported
    // as one; the budget is never raised and the solve is never retried with
    // different settings.
    const CaptureDiagnosis diagnosis = diagnose_capture(n, seed);
    workload->status = diagnosis.status == Status::ResourceLimit ? WorkloadStatus::ResourceLimit
                                                                 : WorkloadStatus::Failed;
    workload->status_detail =
        std::string("root capture did not complete; solver status=") +
        solver_status_name(diagnosis.status) + " (declared memory limit " + limit +
        ", solver peak working bytes " + std::to_string(diagnosis.peak_working_bytes) +
        (diagnosis.message.empty() ? "" : ": " + diagnosis.message) + ")";
    workload->construction_seconds = seconds_since(start);
    return workload;
  }
  if (!workload->data->production_anchor_available) {
    workload->status = WorkloadStatus::Failed;
    workload->status_detail =
        "production low-25 anchor unavailable: " + workload->data->production_anchor_status;
    return workload;
  }
  workload->r1 = workload->data->r1;
  workload->quotient_bits = workload->data->quotient_bits;
  fill_sizes(*workload);
  if (with_authority) {
    workload->authority = tt::build_authority_tables(n, seed);
    if (!workload->authority.ok) {
      workload->status = WorkloadStatus::Failed;
      workload->status_detail = "authority tables failed: " + workload->authority.message;
      return workload;
    }
    if (workload->authority.tau_low32 != workload->data->tau_low32) {
      workload->status = WorkloadStatus::Failed;
      workload->status_detail = "authority capture disagrees with the donor on tau";
      return workload;
    }
  }
  workload->fingerprint = fingerprint(*workload->data);
  workload->construction_seconds = seconds_since(start);
  workload->status = WorkloadStatus::Completed;
  return workload;
}

ResidualProfile measure_profile(const mr8::Dataset& data, const std::uint32_t n,
                                const std::uint64_t seed) {
  ResidualProfile profile;
  profile.source_n = n;
  profile.source_seed = seed;
  const std::array<const std::array<std::vector<mr8::Posting>, kFamilies>*, 3> lists{
      &data.q_family, &data.x_family, &data.y_family};
  for (std::uint32_t list = 0U; list < 3U; ++list) {
    for (std::uint32_t family = 0U; family < kFamilies; ++family) {
      const std::vector<mr8::Posting>& postings = (*lists[list])[family];
      profile.family_sizes[list][family] = static_cast<std::uint32_t>(postings.size());
      std::size_t index = 0U;
      while (index < postings.size()) {
        std::size_t run = index + 1U;
        while (run < postings.size() && postings[run].value == postings[index].value) {
          ++run;
        }
        const std::size_t multiplicity = std::min<std::size_t>(run - index,
                                                               kMaxProfileMultiplicity);
        ++profile.multiplicity[list][multiplicity];
        ++profile.value_buckets[list][postings[index].value >> kProfileBucketBits];
        index = run;
      }
    }
  }
  profile.ok = true;
  return profile;
}

std::unique_ptr<Workload> generate_uniform(const double scale, const std::uint64_t seed) {
  auto workload = std::make_unique<Workload>();
  workload->family = WorkloadFamily::SyntheticUniform;
  workload->seed = seed;
  workload->scale = scale;
  workload->id = "uniform_x" + scale_label(scale) + "_s" + hex(seed);
  const auto start = Clock::now();
  workload->data = std::make_unique<mr8::Dataset>();
  mr8::Dataset& data = *workload->data;

  const std::array<double, 3> means{kReferenceQMean, kReferenceXMean, kReferenceYMean};
  const std::array<std::array<std::vector<mr8::Posting>, kFamilies>*, 3> lists{
      &data.q_family, &data.x_family, &data.y_family};
  for (std::uint32_t list = 0U; list < 3U; ++list) {
    SplitMix64 rng{stream_seed(workload->family, scale, seed, list)};
    const auto total = static_cast<std::uint64_t>(std::llround(scale * means[list] * kFamilies));
    EntryId id = 0U;
    for (std::uint32_t family = 0U; family < kFamilies; ++family) {
      std::vector<mr8::Posting>& postings = (*lists[list])[family];
      const std::uint32_t size = even_share(total, family);
      postings.reserve(size);
      for (std::uint32_t index = 0U; index < size; ++index) {
        postings.push_back(mr8::Posting{
            static_cast<std::uint32_t>(rng.below(mr8::kResidualUniverse)), id++});
      }
      sort_family(postings);
    }
  }
  finish_synthetic_dataset(data);
  fill_sizes(*workload);
  workload->fingerprint = fingerprint(data);
  workload->construction_seconds = seconds_since(start);
  workload->status = WorkloadStatus::Completed;
  return workload;
}

std::unique_ptr<Workload> generate_profile_emulation(const ResidualProfile& profile,
                                                     const double scale,
                                                     const std::uint64_t seed) {
  auto workload = std::make_unique<Workload>();
  workload->family = WorkloadFamily::ResidualProfileEmulation;
  workload->seed = seed;
  workload->scale = scale;
  workload->id = "profile_x" + scale_label(scale) + "_s" + hex(seed);
  if (!profile.ok) {
    workload->status = WorkloadStatus::Failed;
    workload->status_detail = "no measured residual profile";
    return workload;
  }
  const auto start = Clock::now();
  workload->data = std::make_unique<mr8::Dataset>();
  mr8::Dataset& data = *workload->data;

  const std::array<std::array<std::vector<mr8::Posting>, kFamilies>*, 3> lists{
      &data.q_family, &data.x_family, &data.y_family};
  for (std::uint32_t list = 0U; list < 3U; ++list) {
    SplitMix64 rng{stream_seed(workload->family, scale, seed, list)};

    // Cumulative tables for inverse-CDF sampling of the measured histograms.
    std::vector<std::uint64_t> bucket_cdf(kProfileBuckets);
    std::uint64_t running = 0U;
    for (std::uint32_t bucket = 0U; bucket < kProfileBuckets; ++bucket) {
      running += profile.value_buckets[list][bucket];
      bucket_cdf[bucket] = running;
    }
    std::vector<std::uint64_t> multiplicity_cdf(kMaxProfileMultiplicity + 1U);
    std::uint64_t multiplicity_total = 0U;
    for (std::uint32_t k = 0U; k <= kMaxProfileMultiplicity; ++k) {
      multiplicity_total += profile.multiplicity[list][k];
      multiplicity_cdf[k] = multiplicity_total;
    }
    if (running == 0U || multiplicity_total == 0U) {
      workload->status = WorkloadStatus::Failed;
      workload->status_detail = "empty measured histogram";
      return workload;
    }

    EntryId id = 0U;
    for (std::uint32_t family = 0U; family < kFamilies; ++family) {
      std::vector<mr8::Posting>& postings = (*lists[list])[family];
      const auto size = static_cast<std::uint32_t>(
          std::llround(scale * static_cast<double>(profile.family_sizes[list][family])));
      postings.reserve(size);
      while (postings.size() < size) {
        const std::uint64_t bucket_draw = rng.below(running);
        const auto bucket = static_cast<std::uint32_t>(
            std::upper_bound(bucket_cdf.begin(), bucket_cdf.end(), bucket_draw) -
            bucket_cdf.begin());
        const auto value = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(bucket) << kProfileBucketBits) |
            rng.below(std::uint64_t{1} << kProfileBucketBits));
        const std::uint64_t multiplicity_draw = rng.below(multiplicity_total);
        const auto multiplicity = static_cast<std::uint32_t>(
            std::upper_bound(multiplicity_cdf.begin(), multiplicity_cdf.end(),
                             multiplicity_draw) -
            multiplicity_cdf.begin());
        for (std::uint32_t copy = 0U; copy < multiplicity && postings.size() < size; ++copy) {
          postings.push_back(mr8::Posting{value, id++});
        }
      }
      sort_family(postings);
    }
  }
  finish_synthetic_dataset(data);
  fill_sizes(*workload);
  workload->fingerprint = fingerprint(data);
  workload->construction_seconds = seconds_since(start);
  workload->status = WorkloadStatus::Completed;
  return workload;
}

ter::Instance authentic_instance(const std::uint32_t n, const std::uint64_t seed) {
  return benchmark_instance(n, seed);
}

ter::FullTerConfig authentic_config(const std::uint64_t seed) {
  FullTerConfig config;
  config.family = ShapeFamily::FiniteSanity;
  config.execution_mode = TerExecutionMode::FullTer;
  config.target_mode = TerTargetMode::Random;
  config.seed = seed;
  config.attempt_limit = limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = limits::benchmark::kFullTerMemoryBytes;
  return config;
}

std::unique_ptr<Workload> load_snapshot_workload(const std::string& path,
                                                 const bool with_authority) {
  namespace rs = ter::root_snapshot;
  auto workload = std::make_unique<Workload>();
  workload->family = WorkloadFamily::AuthenticFullTer;
  const auto start = Clock::now();
  rs::Snapshot snapshot;
  std::string error;
  if (!rs::load(path, snapshot, error)) {
    workload->id = "snapshot_unreadable";
    workload->status = WorkloadStatus::Failed;
    workload->status_detail = "snapshot " + path + ": " + error;
    return workload;
  }
  const rs::Header& header = snapshot.header;
  const std::uint32_t n = header.n;
  workload->n = n;
  workload->seed = header.seed;
  workload->id = "authentic_n" + std::to_string(n) + "_s" + hex(header.seed) +
                 (header.attempt == 0U ? std::string() : "_a" + std::to_string(header.attempt)) +
                 "_snapshot";
  workload->r1 = header.r1;
  workload->quotient_bits = n > header.r1 ? n - header.r1 : 0U;
  if (workload->quotient_bits < mr8::kProjectionBits) {
    workload->status = WorkloadStatus::NotApplicable;
    workload->status_detail = "quotient width Q=" + std::to_string(workload->quotient_bits) +
                              " is below the 25-bit projection";
    return workload;
  }
  for (std::size_t list = 0U; list < 3U; ++list) {
    if (!root_partition_bench::capture_list_size_fits_entry_id(header.entries[list])) {
      workload->status = WorkloadStatus::Failed;
      workload->status_detail = "snapshot list cannot fit the EntryId domain";
      return workload;
    }
  }

  workload->data = std::make_unique<mr8::Dataset>();
  mr8::Dataset& data = *workload->data;
  data.n = n;
  data.seed = header.seed;
  data.r1 = header.r1;
  data.quotient_bits = workload->quotient_bits;
  data.a_size = header.entries[0];
  data.b_size = header.entries[1];
  data.c_size = header.entries[2];
  data.pair_universe = data.a_size * data.b_size;

  // tau, from the donor's three independent child-target summations.
  const auto to_width = [n](const std::span<const std::uint32_t> words) {
    std::vector<std::uint32_t> out(words.begin(), words.end());
    out.resize((n + 31U) / 32U, 0U);
    if ((n % 32U) != 0U) {
      out.back() &= (std::uint32_t{1} << (n % 32U)) - 1U;
    }
    return out;
  };
  std::array<std::vector<std::uint32_t>, 3> child_targets;
  for (std::size_t list = 0U; list < 3U; ++list) {
    child_targets[list] = to_width(snapshot.child_target(list));
  }
  const auto sum32 = mr8::sum_children_mod_2n_limb32(child_targets, n);
  const auto sum16 = mr8::sum_children_mod_2n_limb16(child_targets, n);
  data.tau_derivation_verified =
      sum32 == sum16 && mr8::sum_children_residue_check(child_targets, sum32, n);
  if (!data.tau_derivation_verified) {
    workload->status = WorkloadStatus::Failed;
    workload->status_detail = "tau child-target summation failed independent verification";
    return workload;
  }
  const std::vector<std::uint32_t> root_target = to_width(snapshot.root_target());
  data.tau_low32 =
      root_partition_bench::quotient_low32_from_words(root_target, sum32, n, header.r1);
  data.tau_low25 = data.tau_low32 & mr8::kProjectionMask;
  data.production_anchor_status =
      "not available: the snapshot path never materializes a FullTerRootCapture";

  // Per-entry low-32 quotient from the donor routine; EntryId = record index.
  std::array<std::vector<std::uint32_t>, 3> quotients;
  for (std::size_t list = 0U; list < 3U; ++list) {
    quotients[list].resize(static_cast<std::size_t>(header.entries[list]));
    for (std::uint64_t index = 0U; index < header.entries[list]; ++index) {
      quotients[list][index] = root_partition_bench::quotient_low32_from_words(
          snapshot.residue(list, index), snapshot.child_target(list), n, header.r1);
    }
  }
  // Full root quotient keys (A64-CHQR).  The quotient is ((residue - target)
  // mod 2^n) >> r1 over its full Q-bit width; C is transformed to d = tau - q_C.
  // Each low-32 prefix must equal the donor routine's value, entry for entry.
  const std::uint32_t full_bits = workload->quotient_bits;
  if (n <= 128U && full_bits <= 69U) {
    workload->full_quotient_bits = full_bits;
    const std::uint32_t low32_bits = std::min<std::uint32_t>(32U, full_bits);
    const std::uint32_t low32_mask =
        low32_bits == 32U ? ~std::uint32_t{0} : (std::uint32_t{1} << low32_bits) - 1U;
    const Wide tau = shifted_difference(root_target, sum32, n, header.r1, full_bits);
    if ((static_cast<std::uint32_t>(tau.lo) & low32_mask) != data.tau_low32) {
      workload->status = WorkloadStatus::Failed;
      workload->status_detail = "full tau disagrees with the donor's low-32 tau";
      return workload;
    }
    for (std::size_t list = 0U; list < 3U; ++list) {
      const std::uint64_t size = header.entries[list];
      workload->key_lo[list].resize(static_cast<std::size_t>(size));
      workload->key_hi[list].resize(static_cast<std::size_t>(size));
      for (std::uint64_t index = 0U; index < size; ++index) {
        Wide key = shifted_difference(snapshot.residue(list, index), snapshot.child_target(list),
                                      n, header.r1, full_bits);
        if ((static_cast<std::uint32_t>(key.lo) & low32_mask) != quotients[list][index]) {
          workload->status = WorkloadStatus::Failed;
          workload->status_detail = "full quotient disagrees with the donor's low-32 quotient";
          return workload;
        }
        if (list == 2U) {
          key = mask_bits(subtract(tau, key), full_bits);
        }
        workload->key_lo[list][index] = key.lo;
        workload->key_hi[list][index] = static_cast<std::uint8_t>(key.hi);
      }
    }
  }

  // Low-25 projection and the 6/19 split: the donor build_dataset's rule.
  for (std::uint64_t index = 0U; index < header.entries[0]; ++index) {
    const std::uint32_t q = quotients[0][index] & mr8::kProjectionMask;
    data.q_family[q & (mr8::kSplitRadix - 1U)].push_back(
        mr8::Posting{q >> mr8::kSplitBits, static_cast<EntryId>(index)});
  }
  for (std::uint64_t index = 0U; index < header.entries[1]; ++index) {
    const std::uint32_t q = quotients[1][index] & mr8::kProjectionMask;
    data.x_family[q & (mr8::kSplitRadix - 1U)].push_back(
        mr8::Posting{q >> mr8::kSplitBits, static_cast<EntryId>(index)});
  }
  for (std::uint64_t index = 0U; index < header.entries[2]; ++index) {
    const std::uint32_t q = quotients[2][index] & mr8::kProjectionMask;
    const std::uint32_t d = (data.tau_low25 - q) & mr8::kProjectionMask;
    data.y_family[d & (mr8::kSplitRadix - 1U)].push_back(
        mr8::Posting{d >> mr8::kSplitBits, static_cast<EntryId>(index)});
  }
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    sort_family(data.q_family[family]);
    sort_family(data.x_family[family]);
    sort_family(data.y_family[family]);
  }
  finish_synthetic_dataset(data);
  fill_sizes(*workload);

  if (with_authority) {
    tt::AuthorityTables& tables = workload->authority;
    tables.n = n;
    tables.r1 = header.r1;
    tables.quotient_bits = workload->quotient_bits;
    tables.tau_low32 = data.tau_low32;
    tables.residue_words = header.words32;
    for (std::size_t list = 0U; list < 3U; ++list) {
      const std::uint64_t size = header.entries[list];
      tables.list_sizes[list] = size;
      tables.quotient[list] = std::move(quotients[list]);
      tables.residue[list].resize(static_cast<std::size_t>(size * header.words32));
      tables.digits[list].resize(static_cast<std::size_t>(size * n));
      for (std::uint64_t index = 0U; index < size; ++index) {
        const auto residue = snapshot.residue(list, index);
        std::copy(residue.begin(), residue.end(),
                  tables.residue[list].begin() +
                      static_cast<std::ptrdiff_t>(index * header.words32));
        std::int8_t* const digits = tables.digits[list].data() + index * n;
        for (std::uint32_t coordinate = 0U; coordinate < n; ++coordinate) {
          digits[coordinate] = static_cast<std::int8_t>(snapshot.digit(list, index, coordinate));
        }
      }
      tables.bytes += tables.quotient[list].size() * sizeof(std::uint32_t) +
                      tables.residue[list].size() * sizeof(std::uint32_t) +
                      tables.digits[list].size();
    }
    tables.root_target = root_target;
    const auto shape = build_shape(ShapeFamily::FiniteSanity, n);
    tables.shape_available = shape.status == Status::Completed;
    if (tables.shape_available) {
      tables.expected_parent = shape.shape.parent;
    }
    tables.ok = true;
  }
  workload->fingerprint = fingerprint(data);
  workload->construction_seconds = seconds_since(start);
  workload->status = WorkloadStatus::Completed;
  return workload;
}

}  // namespace ter::root_generalization
