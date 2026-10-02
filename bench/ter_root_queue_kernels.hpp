#pragma once

// TER^2 root-queue generalization experiment: backends, exact sink, oracle.
//
// SCOPE.  Benchmark/test surface only.  This file owns wiring, not algorithms:
// every backend calls an existing, unmodified donor kernel.
//
//   K2           ter::microreport8::avx2::k2_coarse_gather (frozen), driven by
//                the same tile enumeration and exact survivor drain the matched
//                campaigns used.
//   LANE_SKIP    ter::radix::avx2::route_query_skip<13> (plain radix-13).
//   QUEUE_DENSE  ter::radix13_packed::bmi2::route_query_queued_dense_control:
//                branch-free per-query queue of nonzero 64-bit guard words, then
//                the plain radix-13 exact dense leaf.  This is the exact
//                function behind the 154.19 ms n=72 anchor; no PEXT, PDEP or
//                payload deck is on its path.
//
// Every backend reads one immutable workload and feeds one RootSink.  Radix
// backends build their classes and shifted views inside their own call, so the
// build is charged to them; the shared Y bitmaps and group index are built once
// per workload, outside every timed region, and are read by every backend.

#include "ter_radix13_locator.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_translated_tag_locator.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ter::root_generalization {

namespace rx = ter::radix;

inline constexpr std::uint32_t kNoGroup = rx::kNoGroup;
inline constexpr unsigned kPB = 13U;
using RadixGeom = rx::Geom<kPB>;

[[nodiscard]] inline std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

// ---------------------------------------------------------------------------
// Shared group index.  Transplanted from the translated-tag harness's
// SharedIndex (same grouping rule and the same bitmap-rank y lookup), plus the
// per-family posting offsets that make a posting globally addressable.
// ---------------------------------------------------------------------------
struct RootIndex {
  std::array<std::vector<tt::ResidualGroup>, kFamilies> q_groups;
  std::array<std::vector<tt::ResidualGroup>, kFamilies> x_groups;
  std::array<std::vector<tt::ResidualGroup>, kFamilies> y_groups;
  std::array<std::vector<std::uint32_t>, kFamilies> q_group_of;  // posting -> group
  std::array<std::vector<std::uint32_t>, kFamilies> x_group_of;
  std::array<std::vector<std::uint32_t>, kFamilies> y_block_rank;  // 1024 per family
  std::array<std::uint32_t, kFamilies> q_offset{};  // global posting index base
  std::array<std::uint32_t, kFamilies> x_offset{};
  std::uint64_t query_groups = 0;  // sum over s of distinct Q_s values, times 64
  // Microproblems (index s * 64 + t) every backend, the oracle and the
  // telemetry process.  build_root_index selects all 4096; a declared subsample
  // (select_microproblems) restricts every consumer to the same subset.
  std::array<std::uint8_t, kFamilies * kFamilies> selected{};
  std::uint32_t selected_count = 0;

  [[nodiscard]] bool active(const std::uint32_t s, const std::uint32_t t) const noexcept {
    return selected[s * kFamilies + t] != 0U;
  }
  const mr8::TargetView* view = nullptr;

  [[nodiscard]] std::uint32_t y_lookup(std::uint32_t u, std::uint32_t value) const noexcept;
};

void build_root_index(const mr8::Dataset& data, RootIndex& index);

// Declared, deterministic subsample: the first `count` microproblems of a fixed
// SplitMix64 shuffle of all 4096 (seed 0x5EED5EED).  count >= 4096 keeps all.
void select_microproblems(RootIndex& index, std::uint32_t count);

// ---------------------------------------------------------------------------
// Exact output.  Order-independent: counts plus multiset hashes over pairs,
// residual groups and expanded entry triples.  Keys are never packed into
// fewer bits than their domain, so no size of family or list can alias.
// ---------------------------------------------------------------------------
enum class SinkDepth : std::uint8_t { GroupOnly, Full };

struct OutputDigest {
  std::uint64_t pair_hits = 0;
  std::uint64_t group_emissions = 0;  // group-level backends and the oracle only
  std::uint64_t triples = 0;
  std::uint64_t full_q = 0;       // authentic only
  std::uint64_t residue = 0;      // authentic only
  std::uint64_t structural = 0;   // authentic only
  std::uint64_t residue_sum = 0;  // authentic only: multiset hash of residue-exact triples
  std::uint64_t pair_sum = 0;
  std::uint64_t group_sum = 0;    // group-level backends and the oracle only
  std::uint64_t triple_sum = 0;
  std::uint64_t triple_xor = 0;
  std::uint64_t triple_sum2 = 0;
  std::uint64_t residual_mismatches = 0;
  std::uint64_t directory_misses = 0;
};

struct RootSink {
  const mr8::Dataset* data = nullptr;
  const RootIndex* index = nullptr;
  const tt::AuthorityTables* authority = nullptr;  // null: no authority stage
  SinkDepth depth = SinkDepth::Full;

  // Optional exact pair-key collection, (global q posting << 32) | global x
  // posting.  Abandoned (and flagged) past `pair_key_limit` keys.
  std::vector<std::uint64_t>* pair_keys = nullptr;
  std::size_t pair_key_limit = 0;
  bool pair_keys_overflowed = false;

  // Optional exact final-authority output: every residue-exact (a, b, c), in
  // emission order.  Appended only on the residue-exact branch, never in a
  // filter loop.
  std::vector<std::array<EntryId, 3>>* witnesses = nullptr;

  OutputDigest digest;

  void reset() noexcept {
    digest = OutputDigest{};
    pair_keys_overflowed = false;
    if (pair_keys != nullptr) {
      pair_keys->clear();
    }
    if (witnesses != nullptr) {
      witnesses->clear();
    }
  }

  void emit_pair(std::uint32_t s, std::uint32_t t, std::uint32_t q_posting,
                 std::uint32_t x_posting, std::uint32_t u, const tt::ResidualGroup& y);
  void emit_triple(EntryId a, EntryId b, EntryId c) noexcept;

  // Posting-level entry point (K2).
  void accept_pair(std::uint32_t s, std::uint32_t t, std::uint32_t u, std::uint32_t q_posting,
                   std::uint32_t x_posting, std::uint32_t value);
  // Group-level entry point (radix backends and the oracle).
  void accept_group(std::uint32_t s, std::uint32_t t, std::uint32_t u, std::uint32_t kappa,
                    std::uint32_t q_group, std::uint32_t x_group, std::uint32_t y_group);
};

// ---------------------------------------------------------------------------
// Backends.
// ---------------------------------------------------------------------------
void run_k2(const mr8::Dataset& data, RootSink& sink);
// Phase controls: tiles + frozen kernel only; tiles + kernel + exact survivor
// test with no sink.  Differences of medians attribute K2's sweep and survivor
// costs, exactly as in the matched n=72 campaign.
[[nodiscard]] std::uint64_t run_k2_kernel_only(const mr8::Dataset& data, const RootIndex& index);
[[nodiscard]] std::uint64_t run_k2_survivor_only(const mr8::Dataset& data, const RootIndex& index);

struct RadixTables {
  std::vector<rx::SourceClass<kPB>> sources;
  std::vector<rx::TargetClass<kPB>> targets;
  tt::AlignedWords views;
  std::vector<std::uint32_t> hits;
  bool built = false;
  void allocate();
};

enum class RadixRoute : std::uint8_t { LaneSkip, QueueDense };
enum class RadixStop : std::uint8_t { Full, BuildOnly, GuardQueueOnly };

// Builds classes and views (charged to the caller's timing) then routes every
// query of every microproblem.  Returns a checksum for the partial stops.
std::uint64_t run_radix(const mr8::Dataset& data, const RootIndex& index, RadixTables& tables,
                        RadixRoute route, RadixStop stop, RootSink& sink);

// ---------------------------------------------------------------------------
// Untimed telemetry.
// ---------------------------------------------------------------------------
struct QueueTelemetry {
  std::uint64_t queries = 0;
  std::uint64_t guard_word_ops = 0;         // queries * R / 64
  std::uint64_t nonzero_guard_words = 0;    // == queue entries
  std::uint64_t candidate_bits = 0;         // E
  std::array<std::uint64_t, 65> popcount_histogram{};  // over nonzero words
  std::uint64_t k2_tiles = 0;
  std::uint64_t k2_coarse_candidates = 0;   // K2 lanes handed to the survivor test
  bool ok = false;
};

[[nodiscard]] QueueTelemetry count_telemetry(const mr8::Dataset& data, const RootIndex& index,
                                             RadixTables& tables);

// ---------------------------------------------------------------------------
// VALIDATION-ONLY exhaustive oracle.  Enumerates every distinct (q, x) value
// pair of every microproblem and decides membership with its own value -> group
// table built from the raw Y postings.  It shares no bitmap, tile, radix table
// or queue with any backend, and is never timed.
// ---------------------------------------------------------------------------
void run_oracle(const mr8::Dataset& data, const RootIndex& index, RootSink& sink);

}  // namespace ter::root_generalization
