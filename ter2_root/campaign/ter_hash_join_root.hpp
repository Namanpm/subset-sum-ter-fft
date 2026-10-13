#pragma once

#include "ter_a64_chqr.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ter::hash_join_root {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace a64 = ter::a64;

enum class Status : std::uint8_t { Completed, InvalidInput, ResourceLimit };

struct Config {
  // Auxiliary hash slots and duplicate-posting storage, excluding borrowed
  // inputs and caller-owned sink output. No allocation in the probe loop.
  std::size_t memory_limit_bytes = std::numeric_limits<std::size_t>::max();
};

struct Telemetry {
  std::uint64_t logical_pairs = 0;
  std::uint64_t low_hits = 0;   // W-bit posting matches before high-bit check
  std::uint64_t full_hits = 0;  // full-Q triples emitted, with multiplicity
  std::uint64_t tables_built = 0;
  std::size_t auxiliary_bytes = 0;
};

// Independent scalar full-Q hash join over exactly RootIndex.selected.
// Rebuild Y's table per nonempty cell; duplicates retain original EntryIds.
// Borrows aligned, immutable Dataset/FamilyKeys and the shared exact RootSink.
// Does not reset the sink: callers time reset + call, like existing backends.
// InvalidInput/ResourceLimit leave the sink unchanged; telemetry is cleared.
// Optional witness capacity remains the caller's responsibility (the frozen
// sink's emit_triple is noexcept). No output cap or reduced-work mode exists.
[[nodiscard]] Status run_hash_join(const mr8::Dataset& data, const rg::RootIndex& index,
                                   const a64::FamilyKeys& keys, const Config& config,
                                   rg::RootSink& sink, Telemetry* telemetry = nullptr);
[[nodiscard]] const char* status_label(Status status) noexcept;

}  // namespace ter::hash_join_root
