#pragma once

// TER^2 root-queue generalization experiment: workloads.
//
// SCOPE.  Benchmark/test surface only.  Owns exactly three things:
//
//   AUTHENTIC_FULLTER           a genuine Full TER root, captured through the
//                               unmodified MicroReport8 donor dataset builder,
//                               plus the unmodified authority tables for the
//                               full-Q / residue / structural checks.
//   SYNTHETIC_UNIFORM           64 x 3 residual families drawn uniformly from
//                               Z_{2^19} (SplitMix64), sized at a declared
//                               multiple of the n=72 reference means.
//   RESIDUAL_PROFILE_EMULATION  families whose per-family sizes, value-bucket
//                               histogram and value-multiplicity histogram are
//                               taken from a measured authentic root and scaled.
//                               It is NOT an authentic Full TER root.
//
// Every workload is built once, outside every timed region, and is then read
// immutably by every backend.  No backend ever generates or rewrites a workload.
//
// A synthetic workload has no parent TER instance, so it has no full-Q,
// original-residue or structural authority.  Its exact output ends at expanded
// residual triples; that difference is declared in every report, never hidden.

#include "ter_microreport8_dataset.hpp"
#include "ter_translated_tag_dataset.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ter::root_generalization {

namespace mr8 = ter::microreport8;
namespace tt = ter::translated_tag;

inline constexpr std::uint32_t kFamilies = mr8::kSplitRadix;  // 64

// n=72 reference means per residual family (|A|/64, |B|/64, |C|/64): the 1.0x
// density point of the controlled sweep.
inline constexpr double kReferenceQMean = 19109.0 / 64.0;  // 298.58
inline constexpr double kReferenceXMean = 32496.0 / 64.0;  // 507.75
inline constexpr double kReferenceYMean = 33505.0 / 64.0;  // 523.52

// Value-bucket resolution of the profile: 512 buckets of 1024 residues.
inline constexpr std::uint32_t kProfileBucketBits = 10U;
inline constexpr std::uint32_t kProfileBuckets = mr8::kResidualUniverse >> kProfileBucketBits;
inline constexpr std::uint32_t kMaxProfileMultiplicity = 64U;

enum class WorkloadFamily : std::uint8_t {
  AuthenticFullTer,
  SyntheticUniform,
  ResidualProfileEmulation,
};

[[nodiscard]] const char* family_label(WorkloadFamily family) noexcept;

// Completed is the only status that may be benchmarked.  ResourceLimit and
// NotApplicable are results, reported as such; they are never an empty cell.
enum class WorkloadStatus : std::uint8_t { Completed, ResourceLimit, NotApplicable, Failed };

[[nodiscard]] const char* status_label(WorkloadStatus status) noexcept;

// Measured from one authentic root.  List index 0 = Q (from A), 1 = X (from B),
// 2 = Y (from C).
struct ResidualProfile {
  bool ok = false;
  std::uint32_t source_n = 0;
  std::uint64_t source_seed = 0;
  std::array<std::array<std::uint32_t, kFamilies>, 3> family_sizes{};
  // [k] = number of distinct values that carry exactly k postings (k >= 1);
  // index kMaxProfileMultiplicity pools every k >= kMaxProfileMultiplicity.
  std::array<std::array<std::uint64_t, kMaxProfileMultiplicity + 1U>, 3> multiplicity{};
  // Distinct values per 1024-residue bucket, pooled over the 64 families.
  std::array<std::array<std::uint64_t, kProfileBuckets>, 3> value_buckets{};
};

struct Workload {
  WorkloadFamily family = WorkloadFamily::SyntheticUniform;
  WorkloadStatus status = WorkloadStatus::Failed;
  std::string status_detail;
  std::string id;

  std::uint32_t n = 0;  // authentic only
  std::uint64_t seed = 0;
  double scale = 1.0;  // synthetic density multiple; 1.0 for authentic

  std::uint64_t a_size = 0;
  std::uint64_t b_size = 0;
  std::uint64_t c_size = 0;
  std::uint64_t pair_universe = 0;  // P0 = |A| * |B|
  std::uint32_t quotient_bits = 0;  // authentic only
  std::uint32_t r1 = 0;             // authentic only
  std::uint32_t residual_bits = mr8::kResidualBits;

  // Families plus the Y exact/coarse bitmaps and their view.  Owned on the heap
  // so the view's pointers stay valid.  For synthetic workloads only the
  // families, targets and view are populated.
  std::unique_ptr<mr8::Dataset> data;
  tt::AuthorityTables authority;  // ok == false for synthetic workloads

  double construction_seconds = 0.0;  // never inside a timed region
  std::uint64_t fingerprint = 0;      // determinism evidence over every posting

  // Full root quotient keys, EntryId-indexed: list 0 -> q_A, 1 -> q_B,
  // 2 -> d = tau - q_C (all mod 2^Q, Q = n - r1).  key_lo holds bits
  // [0, min(Q, 64)), key_hi bits [64, Q).  Populated only for snapshot
  // workloads with Q <= 69; every low-32 prefix is cross-checked against the
  // donor quotient routine at load.
  std::uint32_t full_quotient_bits = 0;
  std::array<std::vector<std::uint64_t>, 3> key_lo;
  std::array<std::vector<std::uint8_t>, 3> key_hi;
};

// Captures a genuine root with the donor builder under the repository's frozen
// benchmark memory limit.  A capture that exceeds the limit is ResourceLimit; a
// root whose quotient width is below the donor's 25-bit projection is
// NotApplicable.  Neither is ever retried with a larger budget.
[[nodiscard]] std::unique_ptr<Workload> capture_authentic(std::uint32_t n, std::uint64_t seed,
                                                          bool with_authority);

[[nodiscard]] ResidualProfile measure_profile(const mr8::Dataset& data, std::uint32_t n,
                                              std::uint64_t seed);

[[nodiscard]] std::unique_ptr<Workload> generate_uniform(double scale, std::uint64_t seed);

[[nodiscard]] std::unique_ptr<Workload> generate_profile_emulation(const ResidualProfile& profile,
                                                                   double scale,
                                                                   std::uint64_t seed);

// Order-sensitive hash over every (family, value, id) of all three lists.
[[nodiscard]] std::uint64_t fingerprint(const mr8::Dataset& data) noexcept;

// The authentic benchmark instance for (n, seed): the verbatim fixture
// transplant shared by the donor dataset and authority builders.
[[nodiscard]] ter::Instance authentic_instance(std::uint32_t n, std::uint64_t seed);

// The Full TER configuration every authentic capture in this experiment uses.
[[nodiscard]] ter::FullTerConfig authentic_config(std::uint64_t seed);

// Builds an AUTHENTIC_FULLTER workload from a packed root snapshot (see
// ter_root_snapshot.hpp) instead of an in-process capture.  The per-entry
// quotients, tau and the authority tables come from the same donor routines
// the in-process path uses; only the source of the three lists differs.
[[nodiscard]] std::unique_ptr<Workload> load_snapshot_workload(const std::string& path,
                                                               bool with_authority);

}  // namespace ter::root_generalization
