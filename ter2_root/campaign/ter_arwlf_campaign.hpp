#pragma once

// ARWLF regime campaign: geometry census, stratified microproblem selection
// and deterministic source / target thinning.  Benchmark surface only: nothing
// here runs a backend, changes a backend, or is ever timed.
//
// Census.  For microproblem (s, t) (u = (s + t) mod 64, kappa = (s + t) >> 6)
// every family is histogrammed by residual bits [0, 13) (ARWLF's terminal
// leaves).  Q leaf i and X leaf j reach target leaf k = (i + j + kappa) mod
// 8192, so the ARWLF leaf-pair geometry over the nonempty Y leaves K is
//     lanes   = sum_{k in K} sum_i |Q_i| |X_{k-i-kappa}|
//     packets = sum_{k in K} sum_i ceil(|Q_i|/16) ceil(|X_{k-i-kappa}|/16)
//     triples = #{(k, i) : k in K, Q_i and X_{k-i-kappa} nonempty}
//     rows    = |K| |Q| + sum_{k in K} sum_{j: |X_j| > 16} |Q_{k-j-kappa}| (ceil(|X_j|/16) - 1)
// exactly the Telemetry::lanes / packets / leaf_triples / rows of a counted
// AVX2 run (the campaign gates on that equality).  Every (i, j) reaches exactly
// one k, so each sum over all 8192 k is a product of family totals; the census
// sums over whichever of K or its complement is smaller and never enumerates a
// pair.
//
// Thinning.  An entry survives with probability p iff
//     H(root seed, microproblem, list tag, EntryId, thinning seed) < p 2^64,
// H a fixed SplitMix64 chain.  Deterministic, order-independent, and nested:
// for one thinning seed, the survivors at p are a subset of those at p' > p.
// Thinning removes postings only; EntryIds, keys and authority tables are
// untouched, so every exact check still runs on the authentic entries.

#include "ter_a64_chqr.hpp"
#include "ter_arwlf.hpp"
#include "ter_microreport8_dataset.hpp"
#include "ter_root_queue_kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ter::arwlf_campaign {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace a64 = ter::a64;

inline constexpr std::uint32_t kCells = rg::kFamilies * rg::kFamilies;  // 4096 microproblems

struct Geometry {
  std::uint32_t cell = 0;  // s * 64 + t
  std::uint32_t s = 0;
  std::uint32_t t = 0;
  std::uint32_t u = 0;
  std::uint32_t kappa = 0;
  std::uint64_t q_count = 0;
  std::uint64_t x_count = 0;
  std::uint64_t y_count = 0;
  std::uint64_t logical_pairs = 0;  // |Q_s| |X_t|
  std::uint32_t q_leaves = 0;       // nonempty 13-bit leaves
  std::uint32_t x_leaves = 0;
  std::uint32_t y_leaves = 0;
  std::uint32_t q_leaf_max = 0;
  std::uint32_t x_leaf_max = 0;
  std::uint32_t y_leaf_max = 0;
  std::uint64_t leaf_triples = 0;
  std::uint64_t packets = 0;  // physical 16 x 16 packets
  std::uint64_t lanes = 0;    // valid (q, x) lanes of those packets
  std::uint64_t rows = 0;     // AVX2 row vectors (main + overflow)
  std::uint64_t x_groups = 0;  // distinct residual values: QUEUE_DENSE's capacity unit
  std::uint64_t y_groups = 0;

  // L: logical lanes per physical packet (<= 256).
  [[nodiscard]] double lanes_per_packet() const noexcept {
    return packets == 0U ? 0.0 : static_cast<double>(lanes) / static_cast<double>(packets);
  }
  [[nodiscard]] double row_utilization() const noexcept {
    return rows == 0U ? 0.0 : static_cast<double>(lanes) / (16.0 * static_cast<double>(rows));
  }
};

// Leaf histogram of one family's residual keys.
struct LeafFamily {
  std::vector<std::uint32_t> count;     // kLeaves
  std::vector<std::uint32_t> nonempty;  // ascending leaf digits
  std::vector<std::uint32_t> empty;
  std::vector<std::uint32_t> overflow;  // leaves with > 16 entries
  std::uint64_t size = 0;
  std::uint64_t blocks = 0;        // sum ceil(|leaf| / 16)
  std::uint64_t extra_blocks = 0;  // sum over overflow leaves of ceil(|leaf| / 16) - 1
  std::uint32_t max = 0;
};

[[nodiscard]] LeafFamily leaf_family(const std::vector<std::uint64_t>& keys);

// Geometry of microproblem (s, t) from its three families; groups left zero.
[[nodiscard]] Geometry measure(const LeafFamily& q, const LeafFamily& x, const LeafFamily& y, std::uint32_t s,
                               std::uint32_t t);

// Per-family leaf histograms of one set of family keys.
class LeafCensus {
 public:
  explicit LeafCensus(const a64::FamilyKeys& keys);

  [[nodiscard]] Geometry measure(std::uint32_t s, std::uint32_t t) const;

 private:
  std::array<std::array<LeafFamily, rg::kFamilies>, 3> family_;
};

// Geometry of every microproblem, in cell order, with the index's group counts.
[[nodiscard]] std::vector<Geometry> census(const a64::FamilyKeys& keys, const rg::RootIndex& index);

// Sum of the geometry of every microproblem the index selects (cell = kCells).
[[nodiscard]] Geometry selected_geometry(const std::vector<Geometry>& census,
                                         const rg::RootIndex& index);

// Linear-interpolated quantile of an ascending vector (0 when empty).
[[nodiscard]] double quantile(const std::vector<double>& sorted, double fraction);

// Stratified sample.  Population: microproblems with packets > 0.  Axes:
// x = log2(logical pairs), y = lanes per packet, each cut at its own 25 / 50 /
// 75 % quantiles (bin b holds cut[b-1] <= v < cut[b]).  From every nonempty
// cell of the 4 x 4 grid, the member nearest the cell's median point, with
// each axis scaled by its population IQR (1 when zero); ties go to the lower
// cell id.  Ascending by cell, distinct.
struct Stratum {
  std::uint32_t cell = 0;
  std::uint32_t pair_bin = 0;
  std::uint32_t lane_bin = 0;
  std::uint32_t members = 0;
};

[[nodiscard]] std::vector<Stratum> select_stratified(const std::vector<Geometry>& census);

// ---------------------------------------------------------------------------
// Thinning.
// ---------------------------------------------------------------------------
inline constexpr std::uint64_t kTagQ = 0x514C495354ULL;  // "QLIST"
inline constexpr std::uint64_t kTagX = 0x584C495354ULL;
inline constexpr std::uint64_t kTagY = 0x594C495354ULL;

[[nodiscard]] std::uint64_t thinning_hash(std::uint64_t root_seed, std::uint32_t cell, std::uint64_t tag,
                                          std::uint64_t entry_id, std::uint64_t thinning_seed) noexcept;

// floor(p 2^64) for 0 <= p < 1 (exact: p 2^64 < 2^64 is a representable scaling).
[[nodiscard]] std::uint64_t keep_threshold(double p) noexcept;

// p >= 1 keeps every entry, p <= 0 (or NaN) none; otherwise hash < floor(p 2^64).
[[nodiscard]] bool keep(std::uint64_t hash, double p) noexcept;

// Retention probability that moves lanes per packet from `base` to about
// `target` under independent Q and X thinning: sqrt(target / base), clamped to
// [0.01, 1].  The observed L is always re-measured; this is only the start.
[[nodiscard]] double source_retention(double target, double base) noexcept;

struct Thinning {
  double p_qx = 1.0;  // Q_s and X_t, each independently
  double p_y = 1.0;   // Y_u
  std::uint64_t seed = 0;
};

// Geometry microproblem `cell` would have under `thinning`, computed from the
// surviving keys alone (keys family-aligned with `data`); nothing is modified.
[[nodiscard]] Geometry thinned_geometry(const a64::FamilyKeys& keys, const mr8::Dataset& data,
                                        std::uint64_t root_seed, std::uint32_t cell, const Thinning& thinning);

// Source retention for a target lanes-per-packet.  The sqrt rule is only the
// start: L falls slower than p^2 once leaves thin out.  Bisects p in [1e-4, 1]
// (48 steps) on the census L of the thinned cell, taking L as nondecreasing in
// p (nesting makes it so up to packet rounding), and returns the largest p
// found with L <= target.  At the floor (target 1) that is the largest fully
// singleton thinning.  A target at or above the authentic L keeps p = 1.
// Deterministic and untimed.
struct Retention {
  double sqrt_rule = 1.0;         // source_retention(target, authentic L)
  double p = 1.0;                 // used
  double lanes_per_packet = 0.0;  // census L at p
};

[[nodiscard]] Retention calibrate_source_retention(const a64::FamilyKeys& keys, const mr8::Dataset& data,
                                                   std::uint64_t root_seed, std::uint32_t cell, double target,
                                                   std::uint64_t seed);

// Applies a thinning to the families of one microproblem of `data` in place
// and restores them, entry for entry, when destroyed.  Y_u's target set and
// view are rebuilt for the thinned list and restored with it.  The caller
// rebuilds keys and index for the state it runs on.
class ThinnedMicroproblem {
 public:
  ThinnedMicroproblem(mr8::Dataset& data, std::uint64_t root_seed, std::uint32_t cell,
                      const Thinning& thinning);
  ~ThinnedMicroproblem();
  ThinnedMicroproblem(const ThinnedMicroproblem&) = delete;
  ThinnedMicroproblem& operator=(const ThinnedMicroproblem&) = delete;

 private:
  void point_view_at_target() noexcept;

  mr8::Dataset& data_;
  std::uint32_t s_;
  std::uint32_t t_;
  std::uint32_t u_;
  std::vector<mr8::Posting> q_;  // originals, valid when the list was thinned
  std::vector<mr8::Posting> x_;
  std::vector<mr8::Posting> y_;
  mr8::TargetSet y_target_;
  bool q_thinned_ = false;
  bool x_thinned_ = false;
  bool y_thinned_ = false;
};

// ---------------------------------------------------------------------------
// Planted targets (positive-output experiment).
//
// Every authentic n=108 campaign case had zero final outputs.  To measure the
// output path, M extra C entries are appended to microproblem `cell`'s target
// list Y_u, with EntryIds after the authentic ones.  Their authority data are
// constructed from the root equations, so they are NOT TER vectors and never
// solutions; the case label says so (AUTHENTIC_PLANTED_TARGETS).
//
//   Positive: for M distinct authentic pairs (a in Q_s, b in X_t), the entry c
//     has key d = q_a + q_b (mod 2^Q), low-32 quotient tau - d, residue
//     root - res_a - res_b (mod 2^n), and digits completing a + b + c to a
//     parent with the expected digit counts.  (a, b, c) therefore passes the
//     full-key check, the low-32 prefilter, residue equality and the structural
//     check: a known final output.  A pair whose parent digits cannot be
//     completed is skipped and counted.
//   Decoy: an entry with a random key in family u (consistent quotient, random
//     residue, zero digits): the same Y growth, almost surely no final output.
//
// Destruction removes every appended entry and restores Y_u, its target set
// and view exactly.  The caller rebuilds keys and index for the state it runs.
// ---------------------------------------------------------------------------
using Witness = std::array<EntryId, 3>;

enum class PlantKind : std::uint8_t { Positive, Decoy };

struct Plant {
  PlantKind kind = PlantKind::Positive;
  std::uint32_t count = 0;
  std::uint64_t seed = 0;
};

class PlantedTargets {
 public:
  // Throws std::invalid_argument when the workload lacks full keys or authority
  // tables, or the cell has no work; std::runtime_error when M distinct
  // completable pairs cannot be found within a bounded number of draws.
  PlantedTargets(rg::Workload& workload, std::uint32_t cell, const Plant& plant);
  ~PlantedTargets();
  PlantedTargets(const PlantedTargets&) = delete;
  PlantedTargets& operator=(const PlantedTargets&) = delete;

  [[nodiscard]] EntryId first_planted() const noexcept { return first_; }  // ids >= this are planted
  [[nodiscard]] const std::vector<Witness>& expected() const noexcept { return expected_; }  // sorted
  [[nodiscard]] std::uint64_t rejected_pairs() const noexcept { return rejected_; }

 private:
  void point_view_at_target() noexcept;

  rg::Workload& workload_;
  std::uint32_t u_;
  EntryId first_;
  std::size_t key_size_;
  std::size_t quotient_size_;
  std::size_t residue_size_;
  std::size_t digit_size_;
  std::uint64_t list_size_;
  std::vector<mr8::Posting> y_;
  mr8::TargetSet y_target_;
  std::vector<Witness> expected_;
  std::uint64_t rejected_ = 0;
};

// Matched-timing order of round r over `count` backends: the canonical order
// rotated left by r mod count, reversed on odd rounds.
[[nodiscard]] std::vector<std::size_t> round_order(std::size_t count, std::uint32_t round);

}  // namespace ter::arwlf_campaign
