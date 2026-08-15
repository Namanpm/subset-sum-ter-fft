#pragma once

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
  std::uint32_t n = 36;
  std::uint8_t b2 = 6;
  std::uint8_t b1 = 10;
  std::uint64_t seed = 1;
  std::uint32_t requested_list_size = 1;
  GeneratorMode mode = GeneratorMode::PlantedMitm;
  bool plant_valid_triple = true;
  std::uint32_t half_pool_size = 64;
  std::uint32_t max_half_pool_size = 1'024;
  std::uint64_t attempt_limit = 1'000'000;
  std::uint64_t memory_limit_bytes = 256U * 1024U * 1024U;
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
