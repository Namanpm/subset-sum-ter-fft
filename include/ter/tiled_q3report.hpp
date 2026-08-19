#pragma once

// Exact cache-tiled Q3REPORT arithmetic primitive.
//
// This is the single implementation of the tiling math.  It locates every
// (x, y, z) whose quotient residues satisfy q_x + q_y + q_z == tau (mod Q)
// without ever building a Q-wide lookup table or an |X||Y| pair-record array.
//
// It is an arithmetic locator only.  It never validates ternary structure, never
// constructs parents, and never deduplicates: those remain the responsibility of
// the existing validators and finalizers.
//
// Construction.  Partition [0, Q) into P = Q/S buckets of width S.  For x in
// bucket i and y in bucket j, q_x + q_y lies in [(i+j)S, (i+j+2)S), a window
// that depends only on m = i + j.  Processing all bucket pairs sharing m keeps a
// single 2S-wide exact target structure hot in cache.  The lookup list is
// re-keyed by k_z = (tau - q_z) mod Q and each key stored twice, at k_z and
// k_z + Q, so that modular equality becomes ordinary integer equality against a
// pair sum in [0, 2Q).

#include "ter/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ter {

enum class TiledQ3ReportStatus : std::uint8_t {
  Completed,
  NotApplicable,
  InvalidConfiguration,
  ResourceLimit,
  Cancelled,
  ArithmeticSafetyFailure,
  InvariantViolation,
};

[[nodiscard]] const char* tiled_q3report_status_name(TiledQ3ReportStatus status) noexcept;

struct TiledQEntry {
  QuotientResidue q = 0;
  EntryId id = 0;
};

struct TiledQ3ReportConfig {
  // Deterministic policy: tile_bits = min(preferred_tile_bits, q_bits).  V1 does
  // not autotune; tests may pass explicit values to prove invariance.
  std::uint32_t preferred_tile_bits = 15U;
  // Zero means "no ceiling declared"; any nonzero value is enforced fail-closed
  // against the fixed auxiliary allocation before any scan begins.
  std::uint64_t memory_limit_bytes = 0;
};

struct TiledQ3ReportTelemetry {
  std::uint32_t tile_bits = 0;
  std::uint64_t tile_width = 0;    // S
  std::uint64_t window_width = 0;  // 2S
  std::uint64_t bucket_count = 0;  // P
  std::uint64_t window_count = 0;  // 2P - 1
  std::uint64_t logical_pairs = 0;
  std::uint64_t executed_pairs = 0;
  std::uint64_t raw_triples = 0;
  std::uint64_t nonempty_windows = 0;
  std::uint64_t doubled_lookup_records = 0;
  std::uint64_t auxiliary_peak_bytes = 0;
  std::uint64_t setup_ns = 0;
  std::uint64_t scan_ns = 0;
};

enum class TiledVisitDecision : std::uint8_t {
  Continue,
  ResourceLimit,
  Cancelled,
  InvariantViolation,
};

// Reports one arithmetic witness.  `x` and `y` are EntryIds from the two paired
// lists in the order they were supplied; `z` is an EntryId from the lookup list.
using TiledTripleVisitor = TiledVisitDecision (*)(void* context,
                                                  EntryId x,
                                                  EntryId y,
                                                  EntryId z);

struct TiledQ3ReportResult {
  TiledQ3ReportStatus status = TiledQ3ReportStatus::InvalidConfiguration;
  TiledQ3ReportTelemetry telemetry{};
  std::string message;
};

// Count-only kernel.  Specialized so the benchmark comparator pays no callback
// overhead.  `raw_triples` in the telemetry is the exact arithmetic witness
// count including multiplicity.
[[nodiscard]] TiledQ3ReportResult tiled_q3report_count(
    const std::vector<TiledQEntry>& pair_first,
    const std::vector<TiledQEntry>& pair_second,
    const std::vector<TiledQEntry>& lookup,
    std::uint64_t tau,
    std::uint64_t q,
    const TiledQ3ReportConfig& config);

// Witness kernel.  Every arithmetic witness is reported exactly once, in a
// single pass; duplicate lookup quotients remain distinct witnesses.  A visitor
// decision other than Continue stops the scan and is surfaced in the status.
[[nodiscard]] TiledQ3ReportResult tiled_q3report_report(
    const std::vector<TiledQEntry>& pair_first,
    const std::vector<TiledQEntry>& pair_second,
    const std::vector<TiledQEntry>& lookup,
    std::uint64_t tau,
    std::uint64_t q,
    const TiledQ3ReportConfig& config,
    TiledTripleVisitor visitor,
    void* context);

// Fixed auxiliary requirement of the two kernels, excluding anything a visitor
// chooses to accumulate.  Exposed so callers can budget before committing.
[[nodiscard]] bool tiled_q3report_auxiliary_bytes(std::uint64_t pair_first_size,
                                                  std::uint64_t pair_second_size,
                                                  std::uint64_t lookup_size,
                                                  std::uint64_t q,
                                                  std::uint32_t preferred_tile_bits,
                                                  bool witness_mode,
                                                  std::uint64_t* count_bytes,
                                                  std::uint64_t* report_bytes) noexcept;

// Deterministic tile policy, exposed for tests and telemetry.
[[nodiscard]] std::uint32_t tiled_q3report_tile_bits(std::uint64_t q,
                                                     std::uint32_t preferred_tile_bits) noexcept;

}  // namespace ter
