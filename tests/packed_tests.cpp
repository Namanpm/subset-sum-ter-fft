#include "ter/generator.hpp"
#include "ter/merge.hpp"
#include "ter/packed.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ter::SnapshotConfig packed_config(std::uint64_t seed) {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.n = 36;
  config.b2 = 6;
  config.b1 = 10;
  config.seed = seed;
  config.requested_list_size = 8;
  config.mode = ter::GeneratorMode::PlantedMitm;
  config.plant_valid_triple = true;
  config.half_pool_size = 96;
  config.max_half_pool_size = 384;
  config.attempt_limit = 500'000;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  return config;
}

void packed_vectors_round_trip_and_mask_high_bits() {
  const auto built = ter::build_snapshot(packed_config(0x1234U));
  expect(built.status == ter::Status::Completed, "packed fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  const auto packed = ter::pack_snapshot(built.snapshot);
  expect(packed.status == ter::Status::Completed, "reference snapshot packs");
  if (packed.status != ter::Status::Completed) {
    return;
  }
  expect(sizeof(ter::TerEntry) == 12U, "hot TerEntry is exactly 12 bytes");
  const std::uint64_t valid_mask = (std::uint64_t{1} << 36U) - 1U;
  for (std::size_t role = 0; role < 3U; ++role) {
    const auto& source = built.snapshot.lists[role];
    const auto& hot = packed.snapshot.lists[role];
    for (std::size_t id = 0; id < source.entries.size(); ++id) {
      expect(ter::unpack_entry(hot, static_cast<ter::EntryId>(id), 36) ==
                 source.entries[id].digits,
             "packed entry round-trips to the exact reference vector");
      const std::size_t offset = hot.entries[id].word_offset;
      expect(((hot.positive_words[offset] | hot.negative_words[offset]) & ~valid_mask) == 0U,
             "unused high bits are zero");
    }
  }

  auto corrupted = packed.snapshot;
  corrupted.lists[0].positive_words[0] |= 1U;
  corrupted.lists[0].negative_words[0] |= 1U;
  expect(ter::audit_packed_snapshot(corrupted).status == ter::Status::InvariantViolation,
         "overlapping positive and negative masks are rejected");

  corrupted = packed.snapshot;
  corrupted.lists[0].role = ter::ListRole::B;
  expect(ter::audit_packed_snapshot(corrupted).status == ter::Status::InvariantViolation,
         "packed child role corruption is rejected");

  corrupted = packed.snapshot;
  corrupted.lists[2].target_m2 ^= 1U;
  expect(ter::audit_packed_snapshot(corrupted).status == ter::Status::InvariantViolation,
         "packed child target corruption is rejected");
  expect(ter::run_pair_baseline_packed(corrupted, ter::OutputPolicy::Correctness).status ==
             ter::Status::InvariantViolation,
         "packed merge public API rejects corrupted metadata");
}

void packed_sum_logic_matches_all_ternary_truth_table_rows() {
  constexpr std::array<std::int8_t, 3> digits{-1, 0, 1};
  for (const auto a : digits) {
    for (const auto b : digits) {
      for (const auto c : digits) {
        const auto mask = [](std::int8_t digit, std::int8_t wanted) {
          return digit == wanted ? std::uint64_t{1} : std::uint64_t{0};
        };
        const auto packed = ter::add_packed_word(mask(a, 1),
                                                 mask(a, -1),
                                                 mask(b, 1),
                                                 mask(b, -1),
                                                 mask(c, 1),
                                                 mask(c, -1));
        const int scalar = static_cast<int>(a) + static_cast<int>(b) +
                           static_cast<int>(c);
        const bool scalar_valid = scalar >= -1 && scalar <= 1;
        expect(((packed.invalid & 1U) == 0U) == scalar_valid,
               "packed structural validity matches scalar addition");
        if (scalar_valid) {
          expect(((packed.positive & 1U) != 0U) == (scalar == 1),
                 "packed positive result matches scalar addition");
          expect(((packed.negative & 1U) != 0U) == (scalar == -1),
                 "packed negative result matches scalar addition");
        }
      }
    }
  }
}

void packed_baseline_matches_scalar_across_deterministic_seeds() {
  for (std::uint64_t seed = 1; seed <= 20U; ++seed) {
    const auto built = ter::build_snapshot(packed_config(0x900d0000U + seed));
    expect(built.status == ter::Status::Completed, "differential snapshot builds");
    if (built.status != ter::Status::Completed) {
      continue;
    }
    const auto packed = ter::pack_snapshot(built.snapshot);
    expect(packed.status == ter::Status::Completed, "differential snapshot packs");
    if (packed.status != ter::Status::Completed) {
      continue;
    }
    const auto scalar =
        ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Correctness);
    const auto fast =
        ter::run_pair_baseline_packed(packed.snapshot, ter::OutputPolicy::Correctness);
    const auto raw =
        ter::run_pair_baseline_packed(packed.snapshot, ter::OutputPolicy::Benchmark);
    expect(fast.status == ter::Status::Completed, "packed baseline completes");
    expect(scalar.parents == fast.parents,
           "packed baseline emits the exact scalar canonical parent set");
    expect(scalar.valid_provenance == fast.valid_provenance,
           "packed baseline preserves scalar raw provenance multiplicity");
    expect(scalar.telemetry.candidate_triples == fast.telemetry.candidate_triples,
           "packed and scalar baselines recover the same modular candidates");
    expect(scalar.telemetry.raw_valid_triples == fast.telemetry.raw_valid_triples,
           "packed and scalar validators accept the same raw triples");
    expect(raw.parents == fast.parents,
           "raw benchmark instrumentation preserves the exact parent set");
    expect(raw.telemetry.candidate_loop_ns > 0U,
           "benchmark telemetry times the complete recovery and validation loop");
    expect(raw.telemetry.validation_ns == 0U && raw.telemetry.recovery_ns == 0U,
           "raw benchmark mode disables per-candidate clock instrumentation");
  }
}

}  // namespace

int main() {
  packed_vectors_round_trip_and_mask_high_bits();
  packed_sum_logic_matches_all_ternary_truth_table_rows();
  packed_baseline_matches_scalar_across_deterministic_seeds();

  if (failures != 0) {
    std::cerr << failures << " packed assertion(s) failed\n";
    return 1;
  }
  std::cout << "packed tests passed\n";
  return 0;
}
