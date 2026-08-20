#pragma once

#include "ter/limitations.hpp"
#include "ter/model.hpp"

#include <cstdint>
#include <string>

namespace ter {

enum class GeneratorMode : std::uint8_t {
  ExhaustiveReference,
  RejectionReference,
  MitmPerformance,
  PlantedMitm,
};

struct SnapshotConfig {
  ShapeFamily family = ShapeFamily::FiniteSanity;
  std::uint32_t n = limits::generator::kDefaultN;
  std::uint8_t b2 = limits::generator::kDefaultB2;
  std::uint8_t b1 = limits::generator::kDefaultB1;
  std::uint64_t seed = limits::generator::kDefaultSeed;
  std::uint32_t requested_list_size = limits::generator::kDefaultRequestedListSize;
  GeneratorMode mode = GeneratorMode::PlantedMitm;
  bool plant_valid_triple = true;
  std::uint32_t half_pool_size = limits::generator::kDefaultHalfPoolSize;
  std::uint32_t max_half_pool_size = limits::generator::kDefaultMaxHalfPoolSize;
  std::uint64_t attempt_limit = limits::generator::kDefaultAttemptLimit;
  std::uint64_t memory_limit_bytes = limits::generator::kDefaultMemoryBytes;
};

struct GenerationTelemetry {
  std::uint64_t attempts = 0;
  std::uint64_t accepted_entries = 0;
  std::uint64_t compatible_pairs = 0;
  std::uint64_t half_pool_entries = 0;
};

struct SnapshotResult {
  Status status = Status::InvalidConfiguration;
  InputSnapshot snapshot{};
  GenerationTelemetry telemetry{};
  std::string message;
};

[[nodiscard]] SnapshotResult build_snapshot(const SnapshotConfig& config);

}  // namespace ter
