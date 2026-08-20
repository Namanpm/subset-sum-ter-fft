#include "ter/stress_generator.hpp"
#include "ter/limitations.hpp"
#include "ter/merge.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
  ter::TerStressConfig config;
  config.source.n = 36U;
  config.source.b2 = 6U;
  config.source.b1 = 10U;
  config.source.seed = 0x535452455353ULL;
  config.source.mode = ter::GeneratorMode::PlantedMitm;
  config.source.plant_valid_triple = true;
  config.source.half_pool_size = 512U;
  config.source.max_half_pool_size = 2048U;
  config.source.memory_limit_bytes = 256ULL * 1024ULL * 1024ULL;
  config.candidate_pool_size = 256U;
  config.requested_list_sizes = {32U, 64U, 96U};
  config.mode = ter::StressMode::Composite;
  config.bucket_profile = ter::BucketProfile::HotBuckets;
  config.hot_bucket_count = 1U;
  config.target_hot_mass_percent = 60U;
  config.hot_mass_tolerance_percent = 15U;
  config.prefilter_bits = 2U;
  config.solution_mode = ter::StressSolutionMode::SinglePlantedSolution;

  const auto generated = ter::build_stress_snapshot(config);
  if (generated.status != ter::Status::Completed) {
    std::cerr << "stress generation failed: " << generated.message << '\n';
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto tiled = ter::run_pair_tiled(generated.snapshot, ter::OutputPolicy::Benchmark);
  const auto end = std::chrono::steady_clock::now();
  if (tiled.status != ter::Status::Completed) {
    std::cerr << "tiled merge failed: " << tiled.message << '\n';
    return 1;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  const auto& buckets = generated.telemetry.buckets;
  std::cout << "fixture_family,seed,n,level,r1,r2,quotient_bits,A_size,B_size,C_size,"
               "bucket_profile,candidate_w,candidate_tile_bits,bucket_count,nonempty_buckets,"
               "hot_mass_percent,normalized_entropy,prediction_available,"
               "predicted_survivor_count,actual_survivor_count,auxiliary_memory_bytes,"
               "scan_ns,verification_ns,total_ns,outer_elapsed_ns,candidate_triples\n";
  std::cout << "ADVERSARIAL_STRESS," << config.source.seed << ',' << config.source.n << ','
            << "L2_TO_L1," << static_cast<unsigned>(config.source.b1) << ','
            << static_cast<unsigned>(config.source.b2) << ','
            << static_cast<unsigned>(generated.telemetry.quotient_bits) << ','
            << generated.telemetry.actual_list_sizes[0] << ','
            << generated.telemetry.actual_list_sizes[1] << ','
            << generated.telemetry.actual_list_sizes[2] << ','
            << "hot," << static_cast<unsigned>(config.prefilter_bits) << ','
            << ter::limits::tiled::kDefaultPreferredTileBits << ','
            << buckets.bucket_count << ',' << buckets.nonempty_bucket_count << ','
            << buckets.hot_mass_percent << ',' << buckets.normalized_entropy << ','
            << "0,NA," << tiled.telemetry.raw_valid_triples << ','
            << tiled.telemetry.pair_index_bytes << ',' << tiled.telemetry.candidate_loop_ns << ','
            << "NA," << tiled.telemetry.total_ns << ','
            << elapsed << ',' << tiled.telemetry.candidate_triples << '\n';
  return 0;
}
