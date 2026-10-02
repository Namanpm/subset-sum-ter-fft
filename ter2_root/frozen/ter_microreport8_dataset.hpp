#pragma once

// TER^2 MicroReport8 research spike: authentic n=72 dataset construction.
//
// Captures a genuine Full TER root through the existing FullTerRootCapture
// seam, converts it with the existing root-capture adapter, derives the
// quotient-domain target tau exactly as production does, projects onto the
// low-25 3SUM form and splits it into the 4096 residual microproblems.
//
// It never modifies production and never re-implements the quotient
// conversion: ter::root_partition_bench::capture_list_to_records is the single
// source of per-entry quotients, and the same donor routine derives tau.

#include "ter/full_ter.hpp"
#include "ter/root_partition.hpp"
#include "ter/tiled_q3report.hpp"
#include "ter_microreport8_kernels.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ter::microreport8 {

// Reference configuration of the repository's authentic root probe.
inline constexpr std::uint32_t kReferenceRootN = 72U;
inline constexpr std::uint64_t kReferenceRootSeed = 19797034556212562ULL;
inline constexpr std::uint32_t kReferenceQuotientBits = 47U;
inline constexpr std::uint64_t kReferenceASize = 19109U;
inline constexpr std::uint64_t kReferenceBSize = 32496U;
inline constexpr std::uint64_t kReferenceCSize = 33505U;

inline constexpr std::uint32_t kProjectionBits = 25U;
inline constexpr std::uint32_t kProjectionMask = (1U << kProjectionBits) - 1U;

struct FamilySummary {
  std::uint64_t postings = 0;
  std::uint32_t minimum = 0;
  std::uint32_t maximum = 0;
  double mean = 0.0;
  double median = 0.0;
  double p90 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  std::uint32_t empty_families = 0;
  std::uint64_t distinct_values = 0;    // summed over the 64 families
  std::uint64_t duplicate_values = 0;   // postings - distinct_values
  std::uint32_t max_multiplicity = 0;   // largest repeat count of one value
  std::array<std::uint32_t, kSplitRadix> sizes{};
};

struct Posting {
  std::uint32_t value = 0;  // residual quotient in [0, 2^19)
  EntryId id = 0;           // original captured list identity
};

struct Dataset {
  bool ok = false;
  std::string message;

  std::uint32_t n = 0;
  std::uint64_t seed = 0;
  std::uint32_t r1 = 0;
  std::uint32_t quotient_bits = 0;
  std::uint64_t a_size = 0;
  std::uint64_t b_size = 0;
  std::uint64_t c_size = 0;
  std::uint64_t pair_universe = 0;  // P0 = |A| * |B|
  bool geometry_matches_reference = false;

  std::uint32_t tau_low32 = 0;
  std::uint32_t tau_low25 = 0;
  bool tau_derivation_verified = false;
  std::string tau_verification_detail;

  // Low-25 projected values, in captured order, for the independent
  // tiled_q3report_count() cross-check.
  std::vector<TiledQEntry> a_low25;
  std::vector<TiledQEntry> b_low25;
  std::vector<TiledQEntry> c_low25;  // carries q_c, not d

  // Residual families, ascending by value; identities preserved alongside.
  std::array<std::vector<Posting>, kSplitRadix> q_family;  // from A, key s
  std::array<std::vector<Posting>, kSplitRadix> x_family;  // from B, key t
  std::array<std::vector<Posting>, kSplitRadix> y_family;  // from d, key u

  FamilySummary q_summary;
  FamilySummary x_summary;
  FamilySummary y_summary;

  std::array<TargetSet, kSplitRadix> targets;
  TargetView view{};
  std::uint64_t target_build_ns = 0;
  std::uint64_t per_active_y_bitmap_bytes = 0;
  std::uint64_t all_y_bitmap_bytes = 0;

  // Production anchor: low_prefilter_candidates from an unmodified
  // count_full_ter_root() run configured at exactly kProjectionBits.
  bool production_anchor_available = false;
  std::uint64_t production_low_prefilter_candidates = 0;
  std::uint64_t production_valid_root_count = 0;
  std::string production_anchor_status;
};

// Builds the whole authentic dataset. Returns a heap-owned object so the
// TargetView pointers stay valid without relying on move semantics.
[[nodiscard]] std::unique_ptr<Dataset> build_dataset(std::uint32_t root_n,
                                                     std::uint64_t root_seed,
                                                     bool run_production_anchor);

// Builds every target representation (exact bitmap, coarse bitmap, byte
// signature, posting list, distinct list) for one Y family. Single definition
// of these rules; the probe's synthetic section and the tests reuse it.
void build_target(const std::vector<Posting>& postings, TargetSet& target);

// Exposed for the test binary: the three independent computations of
// (cA + cB + cC) mod 2^n whose agreement gates tau_derivation_verified.
[[nodiscard]] std::vector<std::uint32_t> sum_children_mod_2n_limb32(
    const std::array<std::vector<std::uint32_t>, 3>& child_targets, std::uint32_t n);
[[nodiscard]] std::vector<std::uint32_t> sum_children_mod_2n_limb16(
    const std::array<std::vector<std::uint32_t>, 3>& child_targets, std::uint32_t n);
[[nodiscard]] bool sum_children_residue_check(
    const std::array<std::vector<std::uint32_t>, 3>& child_targets,
    const std::vector<std::uint32_t>& sum_words,
    std::uint32_t n);

// ---------------------------------------------------------------------------
// Tile enumeration.
//
// Order is grouped by u so a run of tiles reuses one active Y_u structure, then
// by s (which fixes t = (u - s) mod 64), then by X block, then by Q block.
// The union over all tiles of one (s, t) is exactly X_t x Q_s, so lane totals
// are independent of the within-family ordering.
// ---------------------------------------------------------------------------
struct MicroproblemKey {
  std::uint32_t s = 0;
  std::uint32_t t = 0;
  std::uint32_t u = 0;
  std::uint32_t kappa = 0;
};

[[nodiscard]] inline MicroproblemKey microproblem_for(const std::uint32_t u,
                                                      const std::uint32_t s) noexcept {
  const std::uint32_t t = (u - s) & (kSplitRadix - 1U);
  return MicroproblemKey{s, t, u, (s + t) >> kSplitBits};
}

[[nodiscard]] inline std::size_t block_count(const std::size_t family_size) noexcept {
  return (family_size + kTileDim - 1U) / kTileDim;
}

// Fills `tile` from the given family blocks. Padding lanes are zeroed.
inline void make_tile(const Dataset& data,
                      const MicroproblemKey& key,
                      const std::size_t x_block,
                      const std::size_t q_block,
                      Tile& tile) noexcept {
  const auto& xs = data.x_family[key.t];
  const auto& qs = data.q_family[key.s];
  const std::size_t x_begin = x_block * kTileDim;
  const std::size_t q_begin = q_block * kTileDim;
  const std::size_t x_take = xs.size() - x_begin < kTileDim ? xs.size() - x_begin : kTileDim;
  const std::size_t q_take = qs.size() - q_begin < kTileDim ? qs.size() - q_begin : kTileDim;
  for (std::size_t lane = 0U; lane < kTileDim; ++lane) {
    tile.x[lane] = lane < x_take ? xs[x_begin + lane].value : 0U;
    tile.q[lane] = lane < q_take ? qs[q_begin + lane].value : 0U;
  }
  tile.x_count = static_cast<std::uint8_t>(x_take);
  tile.q_count = static_cast<std::uint8_t>(q_take);
  tile.kappa = static_cast<std::uint8_t>(key.kappa);
  tile.u = static_cast<std::uint8_t>(key.u);
}

// Visits every authentic tile exactly once, in the grouped order described
// above. `visitor(const Tile&, const MicroproblemKey&)` returns void.
template <typename Visitor>
void for_each_tile(const Dataset& data, Visitor&& visitor) {
  Tile tile{};
  for (std::uint32_t u = 0U; u < kSplitRadix; ++u) {
    for (std::uint32_t s = 0U; s < kSplitRadix; ++s) {
      const MicroproblemKey key = microproblem_for(u, s);
      const std::size_t x_blocks = block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          make_tile(data, key, x_block, q_block, tile);
          visitor(tile, key);
        }
      }
    }
  }
}

}  // namespace ter::microreport8
