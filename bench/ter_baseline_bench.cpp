#include "ter/generator.hpp"
#include "ter/limitations.hpp"
#include "ter/merge.hpp"
#include "ter/packed.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

std::uint64_t median(std::vector<std::uint64_t> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2U];
}

std::uint64_t output_fingerprint(const ter::MergeOutput& output) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto& parent : output.parents) {
    for (const std::int8_t digit : parent.digits) {
      mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(digit) + 1));
    }
    mix(parent.residue_m1);
    mix(parent.multiplicity);
  }
  mix(output.telemetry.raw_valid_triples);
  return hash;
}

const char* workload_name(bool planted) {
  return planted ? "planted_mitm" : "natural_mitm";
}

std::uint64_t owned_peak_budget_bytes(const ter::PackedSnapshot& snapshot,
                                      const ter::MergeOutput& output) {
  const std::uint64_t audit_scratch_bytes =
      static_cast<std::uint64_t>(snapshot.spec.shape.n) * 2U +
      sizeof(ter::ReferenceTernaryVector) * 2U;
  std::uint64_t bytes =
      std::max(output.telemetry.working_set_budget_bytes, audit_scratch_bytes);
  bytes += snapshot.instance.values_le.size() * sizeof(std::uint64_t);
  bytes += snapshot.instance.residues_m1.size() * sizeof(std::uint64_t);
  bytes += snapshot.instance.planted_root.size() * sizeof(std::uint8_t);
  bytes += snapshot.instance.root_target_le.size() * sizeof(std::uint64_t);
  for (const auto& list : snapshot.lists) {
    bytes += list.entries.size() * sizeof(ter::TerEntry);
    bytes += list.positive_words.size() * sizeof(std::uint64_t);
    bytes += list.negative_words.size() * sizeof(std::uint64_t);
    bytes += list.residues_m1.size() * sizeof(std::uint64_t);
  }
  return bytes;
}

int run_case(std::uint32_t list_size,
             std::uint64_t seed,
             bool planted,
             std::uint32_t repetitions) {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::PaperRounded;
  config.n = ter::limits::paper_generator::kDefaultN;
  config.b2 = ter::limits::benchmark::kBaselineB2;
  config.b1 = ter::limits::benchmark::kBaselineB1;
  config.seed = seed;
  config.requested_list_size = list_size;
  config.mode = planted ? ter::GeneratorMode::PlantedMitm
                        : ter::GeneratorMode::MitmPerformance;
  config.plant_valid_triple = planted;
  config.half_pool_size = std::max<std::uint32_t>(
      ter::limits::benchmark::kBaselineMinimumHalfPool,
      list_size * ter::limits::benchmark::kBaselineHalfPoolMultiplier);
  config.max_half_pool_size = ter::limits::paper_generator::kDefaultHalfPoolSize;
  config.attempt_limit = ter::limits::paper_generator::kDefaultAttemptLimit;
  config.memory_limit_bytes = ter::limits::paper_generator::kDefaultMemoryBytes;

  const auto generated = ter::build_snapshot(config);
  if (generated.status != ter::Status::Completed) {
    std::cerr << "generation failed: list_size=" << list_size << " seed=" << seed
              << " workload=" << workload_name(planted)
              << " status=" << static_cast<int>(generated.status)
              << " message=" << generated.message << '\n';
    return 1;
  }
  const auto packed = ter::pack_snapshot(generated.snapshot);
  if (packed.status != ter::Status::Completed) {
    std::cerr << "packing failed: " << packed.message << '\n';
    return 1;
  }
  if (ter::audit_packed_snapshot(packed.snapshot).status != ter::Status::Completed) {
    std::cerr << "packed prevalidation failed\n";
    return 1;
  }

  const auto warmup =
      ter::run_pair_baseline_packed(packed.snapshot, ter::OutputPolicy::Benchmark);
  if (warmup.status != ter::Status::Completed) {
    std::cerr << "warm-up merge failed: " << warmup.message << '\n';
    return 1;
  }
  const std::uint64_t expected_fingerprint = output_fingerprint(warmup);

  std::vector<std::uint64_t> total_samples;
  std::vector<std::uint64_t> api_total_samples;
  std::vector<std::uint64_t> pair_samples;
  std::vector<std::uint64_t> candidate_loop_samples;
  std::vector<std::uint64_t> output_samples;
  total_samples.reserve(repetitions);
  api_total_samples.reserve(repetitions);
  pair_samples.reserve(repetitions);
  candidate_loop_samples.reserve(repetitions);
  output_samples.reserve(repetitions);
  ter::MergeOutput last;
  for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto api_start = std::chrono::steady_clock::now();
    auto measured =
        ter::run_pair_baseline_packed(packed.snapshot, ter::OutputPolicy::Benchmark);
    const auto api_elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - api_start)
            .count());
    if (measured.status != ter::Status::Completed ||
        output_fingerprint(measured) != expected_fingerprint ||
        measured.parents != warmup.parents) {
      std::cerr << "measured merge changed its exact output\n";
      return 1;
    }
    total_samples.push_back(measured.telemetry.total_ns);
    api_total_samples.push_back(api_elapsed);
    pair_samples.push_back(measured.telemetry.pair_build_ns);
    candidate_loop_samples.push_back(measured.telemetry.candidate_loop_ns);
    output_samples.push_back(measured.telemetry.output_ns);
    last = std::move(measured);
  }

  const auto& orientation = last.telemetry.orientation;
  std::cout << seed << ',' << workload_name(planted) << ',' << list_size << ','
            << packed.snapshot.lists[0].entries.size() << ','
            << packed.snapshot.lists[1].entries.size() << ','
            << packed.snapshot.lists[2].entries.size() << ','
            << static_cast<unsigned>(orientation.pair_roles[0])
            << static_cast<unsigned>(orientation.pair_roles[1]) << "->"
            << static_cast<unsigned>(orientation.lookup_role) << ','
            << last.telemetry.pair_combinations << ','
            << last.telemetry.candidate_triples << ','
            << last.telemetry.raw_valid_triples << ','
            << last.telemetry.unique_parents << ','
            << median(api_total_samples) << ','
            << median(pair_samples) << ',' << median(candidate_loop_samples) << ','
            << median(output_samples) << ','
            << median(total_samples) << ',' << sizeof(ter::TerEntry) << ','
            << packed.snapshot.lists[0].words_per_entry * 2U * sizeof(std::uint64_t) << ','
            << last.telemetry.pair_index_bytes << ','
            << owned_peak_budget_bytes(packed.snapshot, last) << ','
            << expected_fingerprint << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--quick") {
      quick = true;
    } else {
      std::cerr << "usage: ter_baseline_bench [--quick]\n";
      return 2;
    }
  }

#if defined(_MSC_VER)
  std::cout << "# compiler=MSVC-" << _MSC_VER;
#elif defined(__GNUC__)
  std::cout << "# compiler=GCC-" << __GNUC__ << '.' << __GNUC_MINOR__;
#else
  std::cout << "# compiler=unknown";
#endif
#if defined(NDEBUG)
  std::cout << " build=Release";
#else
  std::cout << " build=Debug";
#endif
#if defined(TER_NATIVE_OPT_ENABLED)
  std::cout << " native_opt=on";
#else
  std::cout << " native_opt=off";
#endif
#if defined(TER_IPO_ENABLED)
  std::cout << " lto=on";
#else
  std::cout << " lto=off";
#endif
  std::cout << " cxx=20 threads=1 n=360 shape=PAPER_ROUNDED b2=8 b1=18 Q=1024\n";
  std::cout << "seed,workload,requested_size,A,B,C,orientation,pairs,candidates,valid_raw,"
               "unique_parents,api_total_ns,pair_build_ns,recovery_and_validation_ns,"
               "output_ns,kernel_total_ns,"
               "ter_entry_bytes,packed_vector_bytes,pair_index_bytes,"
               "owned_peak_budget_bytes,"
               "fingerprint\n";

  const auto& full_sizes = ter::limits::benchmark::kBaselineFullListSizes;
  const auto& quick_sizes = ter::limits::benchmark::kBaselineQuickListSizes;
  const std::uint32_t seed_count = quick
                                       ? ter::limits::benchmark::kBaselineQuickSeedCount
                                       : ter::limits::benchmark::kBaselineFullSeedCount;
  const std::uint32_t repetitions = quick
                                        ? ter::limits::benchmark::kBaselineQuickTimedRepetitions
                                        : ter::limits::benchmark::kBaselineFullTimedRepetitions;
  const auto run_sizes = [&](const auto& sizes) {
    for (const std::uint32_t size : sizes) {
      for (std::uint32_t seed_index = 0; seed_index < seed_count; ++seed_index) {
        const std::uint64_t seed = ter::limits::benchmark::kBaselineSeedBase +
                                   static_cast<std::uint64_t>(size) *
                                       ter::limits::benchmark::kBaselineSeedSizeMultiplier +
                                   seed_index;
        for (const bool planted : {false, true}) {
          if (run_case(size, seed, planted, repetitions) != 0) {
            return false;
          }
        }
      }
    }
    return true;
  };
  const bool ok = quick ? run_sizes(quick_sizes) : run_sizes(full_sizes);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
