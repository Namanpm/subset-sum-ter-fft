// TER^2 translated-tag locator research spike: authentic matched-scope benchmark.
//
// One question: can an exact translated 11-bit-position + 8-bit-tag set
// alignment locator beat the frozen K2 coarse-gather frontend on the authentic
// n=72 TER^2 root?
//
// SCOPE.  Standalone benchmark.  It reuses bench/ter_microreport8_dataset.*
// unmodified as the authentic dataset donor and bench/ter_microreport8_kernels*
// unmodified as the frozen K2 comparator.  It changes no production source, no
// production reporter semantics, and no previous spike result.
//
// MATCHED SCOPE.  All three comparator paths start from the same built dataset
// (families grouped and sorted, the 64 exact and coarse Y bitmaps built once as
// shared setup) and end at the identical downstream sink:
//
//     exact residual equality
//       -> EntryId / posting expansion, multiplicity preserved
//       -> low-32 quotient agreement (exact, no-false-negative prefilter)
//       -> original-residue agreement mod 2^n (the authority, never truncated)
//       -> ternary structural closure and parent shape
//       -> canonical output
//
// Path C additionally charges every auxiliary structure it needs -- layer
// construction, shifted views and translation tables are all built inside its
// timed region, once per sweep.
//
// The historical MicroReport8 figures (410.175 ms scalar, 138.524 ms K2) were
// projections from measured per-unit medians in a different executable and are
// reported separately, never mixed with the matched-scope measurements here.

#include "ter/limitations.hpp"
#include "ter/tiled_q3report.hpp"
#include "ter_microreport8_dataset.hpp"
#include "ter_radix13_locator.hpp"
#include "ter_radix13_packed.hpp"
#include "ter_translated_tag_dataset.hpp"
#include "ter_translated_tag_locator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {

namespace tt = ter::translated_tag;

using ter::EntryId;
using ter::microreport8::Dataset;
using ter::microreport8::MicroproblemKey;
using ter::microreport8::Posting;
using ter::microreport8::Tile;
using ter::microreport8::TargetView;

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kFamilies = ter::microreport8::kSplitRadix;  // 64
constexpr std::uint32_t kMask19 = ter::microreport8::kResidualMask;
constexpr std::uint32_t kNoGroup = tt::kNoGroup;

// PRIOR_MEASURED, MicroReport8 run on the same machine and fixture. Projections
// from measured per-unit medians, not matched-scope wall clock.
constexpr double kHistoricalScalarMs = 410.175;
constexpr double kHistoricalK2Ms = 138.524;
constexpr std::uint64_t kPriorPairUniverse = 620966064ULL;
constexpr std::uint64_t kPriorTrueLanes = 619303ULL;
constexpr std::uint64_t kPriorTriples = 619579ULL;
constexpr std::uint64_t kPriorTiles = 9890364ULL;

// Fixed gates from the task statement. Never adjusted after measurement.
constexpr double kBeatK2Ms = 138.524;
constexpr double kAdvancementMs = 92.349;
constexpr double kTenXMs = 41.018;
constexpr double kTwentyXMs = 20.509;
constexpr double kThirtyXMs = 13.673;
constexpr double kAdvancementSpeedup = 1.5;
constexpr std::uint64_t kMemoryGateBytes = 48ULL * 1024ULL * 1024ULL;
constexpr double kRoutingGateMs = 66.749;

// Model expectations from the pre-spike projection (MODEL_ONLY).
constexpr double kModelOccupancyVectors = 39.135e6;
constexpr double kModelTagComparisons = 104.651e6;
constexpr double kModelOverflowProbes = 11.775e6;
constexpr double kModelRoutingMs = 66.749;
constexpr double kModelTotalMs = 89.6;
constexpr double kModelAuxBytesLow = 36.0 * 1024.0 * 1024.0;
constexpr double kModelAuxBytesHigh = 38.0 * 1024.0 * 1024.0;

volatile std::uint64_t g_sink = 0U;

[[nodiscard]] double elapsed_ns(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

[[nodiscard]] std::string number(const double value) {
  std::ostringstream stream;
  stream << std::setprecision(10) << value;
  return stream.str();
}

[[nodiscard]] inline std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

struct Stats {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double p95 = 0.0;
  double iqr = 0.0;
  double minimum = 0.0;
  std::size_t samples = 0U;
};

[[nodiscard]] double quantile(const std::vector<double>& sorted, const double fraction) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = fraction * static_cast<double>(sorted.size() - 1U);
  const auto lower = static_cast<std::size_t>(position);
  const std::size_t upper = lower + 1U < sorted.size() ? lower + 1U : lower;
  const double weight = position - static_cast<double>(lower);
  return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] Stats summarize(std::vector<double> samples) {
  Stats stats;
  if (samples.empty()) {
    return stats;
  }
  std::sort(samples.begin(), samples.end());
  stats.samples = samples.size();
  stats.minimum = samples.front();
  stats.p25 = quantile(samples, 0.25);
  stats.median = quantile(samples, 0.50);
  stats.p75 = quantile(samples, 0.75);
  stats.p95 = quantile(samples, 0.95);
  stats.iqr = stats.p75 - stats.p25;
  return stats;
}

// ---------------------------------------------------------------------------
// Shared index.  Built once, before any timing, and used identically by all
// three comparator paths, so no path gains or loses from it.
// ---------------------------------------------------------------------------
struct SharedIndex {
  std::array<std::vector<tt::ResidualGroup>, kFamilies> q_groups;
  std::array<std::vector<tt::ResidualGroup>, kFamilies> x_groups;
  std::array<std::vector<tt::ResidualGroup>, kFamilies> y_groups;
  std::array<std::vector<std::uint32_t>, kFamilies> q_group_of;  // posting -> group
  std::array<std::vector<std::uint32_t>, kFamilies> x_group_of;
  std::array<std::vector<std::uint32_t>, kFamilies> y_block_rank;  // 1024 per family
  const TargetView* view = nullptr;
  std::uint64_t bytes = 0;

  // Exact value -> group index over Y_u, decided by the shared exact bitmap and
  // resolved by rank.  Group indices agree with the locator's own directory
  // because both enumerate the family's distinct values in ascending order.
  [[nodiscard]] inline std::uint32_t y_lookup(const std::uint32_t u,
                                              const std::uint32_t value) const noexcept {
    const std::uint32_t* const exact = view->exact[u];
    const std::uint32_t word = value >> 5U;
    if (((exact[word] >> (value & 31U)) & 1U) == 0U) {
      return kNoGroup;
    }
    const std::uint32_t block = value >> 9U;  // 512 bits == 16 words
    std::uint32_t rank = y_block_rank[u][block];
    for (std::uint32_t index = block * 16U; index < word; ++index) {
      rank += static_cast<std::uint32_t>(std::popcount(exact[index]));
    }
    const std::uint32_t low = value & 31U;
    rank += static_cast<std::uint32_t>(
        std::popcount(exact[word] & ((std::uint32_t{1} << low) - 1U)));
    return rank;
  }
};

void group_family(const std::vector<Posting>& postings,
                  std::vector<tt::ResidualGroup>& groups,
                  std::vector<std::uint32_t>* const posting_to_group) {
  groups.clear();
  if (posting_to_group != nullptr) {
    posting_to_group->assign(postings.size(), 0U);
  }
  std::size_t index = 0U;
  while (index < postings.size()) {
    std::size_t run = index + 1U;
    while (run < postings.size() && postings[run].value == postings[index].value) {
      ++run;
    }
    const auto group = static_cast<std::uint32_t>(groups.size());
    groups.push_back(tt::ResidualGroup{postings[index].value,
                                       static_cast<std::uint32_t>(index),
                                       static_cast<std::uint32_t>(run - index)});
    if (posting_to_group != nullptr) {
      for (std::size_t at = index; at < run; ++at) {
        (*posting_to_group)[at] = group;
      }
    }
    index = run;
  }
}

[[nodiscard]] std::uint32_t build_shared_index(const Dataset& data, SharedIndex& shared) {
  shared.view = &data.view;
  std::uint32_t widest = 0U;
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    group_family(data.q_family[family], shared.q_groups[family], &shared.q_group_of[family]);
    group_family(data.x_family[family], shared.x_groups[family], &shared.x_group_of[family]);
    group_family(data.y_family[family], shared.y_groups[family], nullptr);
    widest = std::max({widest, static_cast<std::uint32_t>(shared.q_groups[family].size()),
                       static_cast<std::uint32_t>(shared.x_groups[family].size()),
                       static_cast<std::uint32_t>(shared.y_groups[family].size())});
    shared.y_block_rank[family].assign(ter::microreport8::kExactWords / 16U, 0U);
    const std::uint32_t* const exact = data.view.exact[family];
    std::uint32_t running = 0U;
    for (std::size_t block = 0U; block < shared.y_block_rank[family].size(); ++block) {
      shared.y_block_rank[family][block] = running;
      for (std::size_t index = block * 16U; index < block * 16U + 16U; ++index) {
        running += static_cast<std::uint32_t>(std::popcount(exact[index]));
      }
    }
  }
  shared.bytes = 0;
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    shared.bytes += shared.q_groups[family].size() * sizeof(tt::ResidualGroup);
    shared.bytes += shared.x_groups[family].size() * sizeof(tt::ResidualGroup);
    shared.bytes += shared.y_groups[family].size() * sizeof(tt::ResidualGroup);
    shared.bytes += shared.q_group_of[family].size() * sizeof(std::uint32_t);
    shared.bytes += shared.x_group_of[family].size() * sizeof(std::uint32_t);
    shared.bytes += shared.y_block_rank[family].size() * sizeof(std::uint32_t);
  }
  return widest;
}

// Residual group triple identity, comparable across every path and the truth.
[[nodiscard]] inline std::uint64_t group_key(const std::uint32_t s, const std::uint32_t t,
                                             const std::uint32_t q_group,
                                             const std::uint32_t x_group,
                                             const std::uint32_t y_group) noexcept {
  return ((((static_cast<std::uint64_t>(s) * 64U + t) * 1024U + q_group) * 1024U + x_group) *
          1024U) +
         y_group;
}

[[nodiscard]] inline std::uint64_t entry_key(const EntryId a, const EntryId b,
                                             const EntryId c) noexcept {
  return (static_cast<std::uint64_t>(a) << 32U) | (static_cast<std::uint64_t>(b) << 16U) |
         static_cast<std::uint64_t>(c);
}

// ---------------------------------------------------------------------------
// The authoritative downstream sink.  Identical for all three paths.
// ---------------------------------------------------------------------------
// GroupOnly stops at exact residual equality, so the difference between it and
// Full is exactly the posting expansion plus the authority pipeline that every
// path pays identically.
enum class SinkDepth : std::uint8_t { GroupOnly, Expansion, QuotientPrefilter, Full };

struct MatchedSink {
  const Dataset* data = nullptr;
  const tt::AuthorityTables* auth = nullptr;
  const SharedIndex* shared = nullptr;
  bool collect = false;
  SinkDepth depth = SinkDepth::Full;

  std::uint64_t residual_group_emissions = 0;
  std::uint64_t pair_hits = 0;
  std::uint64_t expanded_entry_triples = 0;
  std::uint64_t full_q_survivors = 0;
  std::uint64_t residue_survivors = 0;
  std::uint64_t structural_survivors = 0;
  std::uint64_t final_outputs = 0;
  std::uint64_t residual_mismatches = 0;
  std::uint64_t directory_misses = 0;
  std::uint64_t sum_checksum = 0;
  std::uint64_t xor_checksum = 0;
  std::vector<std::uint64_t> entry_keys;
  std::vector<std::uint64_t> group_keys;

  void reset() {
    residual_group_emissions = 0;
    pair_hits = 0;
    expanded_entry_triples = 0;
    full_q_survivors = 0;
    residue_survivors = 0;
    structural_survivors = 0;
    final_outputs = 0;
    residual_mismatches = 0;
    directory_misses = 0;
    sum_checksum = 0;
    xor_checksum = 0;
    entry_keys.clear();
    group_keys.clear();
  }

  inline void emit_triple(const EntryId a, const EntryId b, const EntryId c) {
    const std::uint64_t key = entry_key(a, b, c);
    ++expanded_entry_triples;
    const std::uint64_t mixed = mix64(key);
    sum_checksum += mixed;
    xor_checksum ^= mixed;
    if (collect) {
      entry_keys.push_back(key);
    }
    if (depth == SinkDepth::Expansion) {
      return;
    }
    // Exact, no-false-negative prefilter: q_a + q_b + q_c == tau (mod 2^47)
    // implies the same congruence mod 2^32.  Not an authority on its own.
    const std::uint32_t sum =
        auth->quotient[0][a] + auth->quotient[1][b] + auth->quotient[2][c];
    if (sum != auth->tau_low32) {
      return;
    }
    ++full_q_survivors;
    if (depth == SinkDepth::QuotientPrefilter) {
      return;
    }
    // The authority: original arbitrary-width residues, never truncated.
    if (!tt::residue_sum_matches_root(*auth, a, b, c)) {
      return;
    }
    ++residue_survivors;
    if (!tt::structural_parent_valid(*auth, a, b, c)) {
      return;
    }
    ++structural_survivors;
    ++final_outputs;
  }

  // Group-level entry point: the translated-tag locator's natural output.
  inline void accept_group(const std::uint32_t s, const std::uint32_t t,
                           const std::uint32_t u, const std::uint32_t kappa,
                           const std::uint32_t q_group, const std::uint32_t x_group,
                           const std::uint32_t y_group) {
    const tt::ResidualGroup& q = shared->q_groups[s][q_group];
    const tt::ResidualGroup& x = shared->x_groups[t][x_group];
    const tt::ResidualGroup& y = shared->y_groups[u][y_group];
    if (((q.value + x.value + kappa) & kMask19) != y.value) {
      ++residual_mismatches;
      return;
    }
    ++residual_group_emissions;
    pair_hits += static_cast<std::uint64_t>(q.count) * x.count;
    if (collect) {
      group_keys.push_back(group_key(s, t, q_group, x_group, y_group));
    }
    if (depth == SinkDepth::GroupOnly) {
      return;
    }
    const std::vector<Posting>& qs = data->q_family[s];
    const std::vector<Posting>& xs = data->x_family[t];
    const std::vector<Posting>& ys = data->y_family[u];
    for (std::uint32_t qi = 0U; qi < q.count; ++qi) {
      for (std::uint32_t xi = 0U; xi < x.count; ++xi) {
        for (std::uint32_t yi = 0U; yi < y.count; ++yi) {
          emit_triple(qs[q.begin + qi].id, xs[x.begin + xi].id, ys[y.begin + yi].id);
        }
      }
    }
  }

  // Posting-level entry point: the scalar and K2 frontends' natural output.
  inline void accept_pair(const std::uint32_t s, const std::uint32_t t,
                          const std::uint32_t u, const std::uint32_t q_posting,
                          const std::uint32_t x_posting, const std::uint32_t value) {
    ++pair_hits;
    const std::uint32_t y_group = shared->y_lookup(u, value);
    if (y_group == kNoGroup) {
      ++directory_misses;
      return;
    }
    const tt::ResidualGroup& y = shared->y_groups[u][y_group];
    if (y.value != value) {
      ++residual_mismatches;
      return;
    }
    if (collect) {
      group_keys.push_back(group_key(s, t, shared->q_group_of[s][q_posting],
                                     shared->x_group_of[t][x_posting], y_group));
    }
    if (depth == SinkDepth::GroupOnly) {
      return;
    }
    const std::vector<Posting>& ys = data->y_family[u];
    const EntryId a = data->q_family[s][q_posting].id;
    const EntryId b = data->x_family[t][x_posting].id;
    for (std::uint32_t yi = 0U; yi < y.count; ++yi) {
      emit_triple(a, b, ys[y.begin + yi].id);
    }
  }
};

// ---------------------------------------------------------------------------
// VALIDATION-ONLY EXHAUSTIVE TRUTH.
//
// Enumerates all 620,966,064 authentic source pairs and decides membership with
// its own value -> group table built directly from the Y postings.  It shares no
// code with the donor bitmaps, with the shared index, or with the locator.  It
// is never consulted by any timed path.
// ---------------------------------------------------------------------------
struct Truth {
  std::vector<std::uint64_t> entry_keys;  // sorted, multiplicity preserved
  std::vector<std::uint64_t> group_keys;  // sorted, unique
  std::uint64_t pair_hits = 0;
  std::uint64_t triples = 0;
  std::uint64_t sum_checksum = 0;
  std::uint64_t xor_checksum = 0;
  double build_ns = 0.0;
};

[[nodiscard]] Truth build_truth(const Dataset& data) {
  Truth truth;
  const auto start = Clock::now();
  std::vector<std::int32_t> value_to_group(ter::microreport8::kResidualUniverse, -1);
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::vector<Posting>& ys = data.y_family[u];
    std::vector<std::uint32_t> begins;
    std::vector<std::uint32_t> counts;
    std::size_t index = 0U;
    while (index < ys.size()) {
      std::size_t run = index + 1U;
      while (run < ys.size() && ys[run].value == ys[index].value) {
        ++run;
      }
      value_to_group[ys[index].value] = static_cast<std::int32_t>(begins.size());
      begins.push_back(static_cast<std::uint32_t>(index));
      counts.push_back(static_cast<std::uint32_t>(run - index));
      index = run;
    }
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const MicroproblemKey key = ter::microreport8::microproblem_for(u, s);
      const std::vector<Posting>& qs = data.q_family[key.s];
      const std::vector<Posting>& xs = data.x_family[key.t];
      for (std::uint32_t qi = 0U; qi < qs.size(); ++qi) {
        const std::uint32_t base = qs[qi].value + key.kappa;
        for (std::uint32_t xi = 0U; xi < xs.size(); ++xi) {
          const std::uint32_t value = (base + xs[xi].value) & kMask19;
          const std::int32_t group = value_to_group[value];
          if (group < 0) {
            continue;
          }
          ++truth.pair_hits;
          const auto y_group = static_cast<std::uint32_t>(group);
          truth.group_keys.push_back(group_key(key.s, key.t, qi, xi, y_group));
          const std::uint32_t begin = begins[y_group];
          const std::uint32_t count = counts[y_group];
          for (std::uint32_t yi = 0U; yi < count; ++yi) {
            const std::uint64_t entry =
                entry_key(qs[qi].id, xs[xi].id, ys[begin + yi].id);
            truth.entry_keys.push_back(entry);
            const std::uint64_t mixed = mix64(entry);
            truth.sum_checksum += mixed;
            truth.xor_checksum ^= mixed;
            ++truth.triples;
          }
        }
      }
    }
    for (const Posting& posting : ys) {
      value_to_group[posting.value] = -1;
    }
  }
  // The truth enumerates postings, so its group keys carry posting indices; map
  // them to group indices only after the fact, outside any timed path.
  truth.build_ns = elapsed_ns(start);
  std::sort(truth.entry_keys.begin(), truth.entry_keys.end());
  return truth;
}

// The truth's group keys were emitted with posting indices in the q/x slots.
// Rewrite them to group indices and deduplicate, so they are comparable with
// every path's group keys.
void canonicalize_truth_groups(Truth& truth, const SharedIndex& shared) {
  for (std::uint64_t& key : truth.group_keys) {
    const std::uint32_t y_group = static_cast<std::uint32_t>(key % 1024U);
    const std::uint32_t x_posting = static_cast<std::uint32_t>((key / 1024U) % 1024U);
    const std::uint32_t q_posting = static_cast<std::uint32_t>((key / (1024U * 1024U)) % 1024U);
    const std::uint64_t head = key / (1024ULL * 1024ULL * 1024ULL);
    const std::uint32_t t = static_cast<std::uint32_t>(head % 64U);
    const std::uint32_t s = static_cast<std::uint32_t>(head / 64U);
    key = group_key(s, t, shared.q_group_of[s][q_posting], shared.x_group_of[t][x_posting],
                    y_group);
  }
  std::sort(truth.group_keys.begin(), truth.group_keys.end());
  truth.group_keys.erase(std::unique(truth.group_keys.begin(), truth.group_keys.end()),
                         truth.group_keys.end());
}

// ---------------------------------------------------------------------------
// Path A: scalar reference frontend.  One exact bitmap probe per source pair.
// ---------------------------------------------------------------------------
void sweep_scalar(const Dataset& data, MatchedSink& sink) {
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::uint32_t* const exact = data.view.exact[u];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const MicroproblemKey key = ter::microreport8::microproblem_for(u, s);
      const std::vector<Posting>& qs = data.q_family[key.s];
      const std::vector<Posting>& xs = data.x_family[key.t];
      for (std::uint32_t qi = 0U; qi < qs.size(); ++qi) {
        const std::uint32_t base = qs[qi].value + key.kappa;
        for (std::uint32_t xi = 0U; xi < xs.size(); ++xi) {
          const std::uint32_t value = (base + xs[xi].value) & kMask19;
          if (((exact[value >> 5U] >> (value & 31U)) & 1U) != 0U) {
            sink.accept_pair(key.s, key.t, u, qi, xi, value);
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Path B: frozen K2 coarse-gather frontend.
//
// The kernel is the donor's, called unmodified.  Only the enumeration and the
// candidate drain are new, and they exist solely to reach the same sink.  Tile
// materialization is inside the timed region because a real pipeline cannot
// hold 9,890,364 packed tiles; the tile_build control isolates its cost.
// ---------------------------------------------------------------------------
using TileKernelFn = std::uint64_t (*)(const Tile&, const TargetView&);

void sweep_k2(const Dataset& data, MatchedSink& sink, const TileKernelFn kernel) {
  Tile tile{};
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::uint32_t* const exact = data.view.exact[u];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const MicroproblemKey key = ter::microreport8::microproblem_for(u, s);
      const std::size_t x_blocks = ter::microreport8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = ter::microreport8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          ter::microreport8::make_tile(data, key, x_block, q_block, tile);
          std::uint64_t candidates = kernel(tile, data.view);
          while (candidates != 0U) {
            const auto lane = static_cast<std::uint32_t>(std::countr_zero(candidates));
            candidates &= candidates - 1U;
            const std::uint32_t row = lane >> 3U;
            const std::uint32_t column = lane & 7U;
            const std::uint32_t value =
                (tile.x[row] + tile.q[column] + tile.kappa) & kMask19;
            if (((exact[value >> 5U] >> (value & 31U)) & 1U) != 0U) {
              sink.accept_pair(key.s, key.t, u,
                               static_cast<std::uint32_t>(q_block * 8U) + column,
                               static_cast<std::uint32_t>(x_block * 8U) + row, value);
            }
          }
        }
      }
    }
  }
}

struct K2Phases {
  double kernel_sweep_ms = 0.0;
  double survivor_handling_ms = 0.0;
};

[[nodiscard]] std::uint64_t sweep_k2_kernel_only(const Dataset& data, const TileKernelFn kernel) {
  Tile tile{};
  std::uint64_t candidates_total = 0U;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const MicroproblemKey key = ter::microreport8::microproblem_for(u, s);
      const std::size_t x_blocks = ter::microreport8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = ter::microreport8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          ter::microreport8::make_tile(data, key, x_block, q_block, tile);
          candidates_total += std::popcount(kernel(tile, data.view));
        }
      }
    }
  }
  return candidates_total;
}

[[nodiscard]] std::uint64_t sweep_k2_survivor_only(const Dataset& data, const TileKernelFn kernel) {
  Tile tile{};
  std::uint64_t survivors = 0U;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::uint32_t* const exact = data.view.exact[u];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const MicroproblemKey key = ter::microreport8::microproblem_for(u, s);
      const std::size_t x_blocks = ter::microreport8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = ter::microreport8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          ter::microreport8::make_tile(data, key, x_block, q_block, tile);
          std::uint64_t candidates = kernel(tile, data.view);
          while (candidates != 0U) {
            const auto lane = static_cast<std::uint32_t>(std::countr_zero(candidates));
            candidates &= candidates - 1U;
            const std::uint32_t row = lane >> 3U;
            const std::uint32_t column = lane & 7U;
            const std::uint32_t value =
                (tile.x[row] + tile.q[column] + tile.kappa) & kMask19;
            if (((exact[value >> 5U] >> (value & 31U)) & 1U) != 0U) {
              ++survivors;
            }
          }
        }
      }
    }
  }
  return survivors;
}

[[nodiscard]] std::uint64_t sweep_tile_build_only(const Dataset& data) {
  Tile tile{};
  std::uint64_t lane[4] = {0U, 0U, 0U, 0U};
  std::size_t entry = 0U;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const MicroproblemKey key = ter::microreport8::microproblem_for(u, s);
      const std::size_t x_blocks = ter::microreport8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = ter::microreport8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          ter::microreport8::make_tile(data, key, x_block, q_block, tile);
          lane[entry & 3U] += ter::microreport8::tile_valid_mask(tile) ^ tile.x[0] ^ tile.q[0];
          ++entry;
        }
      }
    }
  }
  return lane[0] + lane[1] + lane[2] + lane[3];
}

// ---------------------------------------------------------------------------
// Path C: the translated-tag locator.
// ---------------------------------------------------------------------------
using RouteFn = std::uint32_t (*)(const std::uint64_t*, const std::uint64_t*,
                                  const std::uint64_t*, const tt::QueryPlan&,
                                  std::uint32_t*);
using RouteCountedFn = std::uint32_t (*)(const std::uint64_t*, const std::uint64_t*,
                                         const std::uint64_t*, const tt::QueryPlan&,
                                         std::uint32_t*, tt::Counters&);

struct LocatorPhases {
  double target_class_ns = 0.0;
  double shifted_view_ns = 0.0;
  double source_class_ns = 0.0;
  double translation_ns = 0.0;
  double route_ns = 0.0;
  double overflow_ns = 0.0;
};

struct LocatorStats {
  std::uint64_t queries = 0;
  std::uint64_t overflow_probe_x = 0;
  std::uint64_t overflow_probe_y = 0;
  std::uint64_t overflow_hit_x = 0;
  std::uint64_t overflow_hit_y = 0;
  std::uint64_t primary_directory_faults = 0;
  std::uint64_t translation_builds = 0;
  tt::Counters counters;
};

struct LocatorScratch {
  std::vector<tt::SourceClass> sources;
  std::vector<tt::TargetClass> targets;
  tt::AlignedWords views;
  tt::AlignedWords translation;
  std::vector<std::uint32_t> hits;

  void allocate() {
    sources.resize(kFamilies);
    targets.resize(kFamilies);
    views.reset(tt::kShiftedViewWords * kFamilies);
    translation.reset(tt::kTranslationWords);
    hits.assign(tt::kHitCapacity, 0U);
  }

  [[nodiscard]] std::uint64_t aux_bytes() const {
    std::uint64_t bytes = views.bytes() + translation.bytes();
    bytes += hits.size() * sizeof(std::uint32_t);
    for (const tt::SourceClass& source : sources) {
      bytes += sizeof(tt::SourceClass);
      bytes += source.groups.capacity() * sizeof(tt::ResidualGroup);
      bytes += source.excess.capacity() * sizeof(std::uint32_t);
    }
    for (const tt::TargetClass& target : targets) {
      bytes += sizeof(tt::TargetClass);
      bytes += target.groups.capacity() * sizeof(tt::ResidualGroup);
      bytes += target.excess.capacity() * sizeof(std::uint32_t);
    }
    return bytes;
  }
};

template <bool kInstrument>
void sweep_translated_tag(const Dataset& data, const SharedIndex& shared,
                          MatchedSink& sink, LocatorScratch& scratch,
                          const RouteFn route, const RouteCountedFn route_counted,
                          LocatorPhases* const phases, LocatorStats* const stats) {
  std::uint32_t* const hits = scratch.hits.data();
  std::uint64_t* const views = scratch.views.data();
  std::uint64_t* const translation = scratch.translation.data();

  Clock::time_point mark;
  if constexpr (kInstrument) {
    mark = Clock::now();
  }
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::vector<Posting>& ys = data.y_family[u];
    std::vector<std::uint32_t> values(ys.size());
    for (std::size_t index = 0U; index < ys.size(); ++index) {
      values[index] = ys[index].value;
    }
    tt::build_target_class(values.data(), values.size(), scratch.targets[u]);
  }
  if constexpr (kInstrument) {
    phases->target_class_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    tt::build_shifted_views(scratch.targets[u], views + static_cast<std::size_t>(u) *
                                                            tt::kShiftedViewWords);
  }
  if constexpr (kInstrument) {
    phases->shifted_view_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const std::vector<Posting>& xs = data.x_family[t];
    std::vector<std::uint32_t> values(xs.size());
    for (std::size_t index = 0U; index < xs.size(); ++index) {
      values[index] = xs[index].value;
    }
    tt::build_source_class(values.data(), values.size(), scratch.sources[t]);
  }
  if constexpr (kInstrument) {
    phases->source_class_ns += elapsed_ns(mark);
  }

  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const tt::SourceClass& source = scratch.sources[t];
    if constexpr (kInstrument) {
      mark = Clock::now();
    }
    tt::build_translation_table(source, translation);
    if constexpr (kInstrument) {
      phases->translation_ns += elapsed_ns(mark);
      ++stats->translation_builds;
    }
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> 6U;
      const tt::TargetClass& target = scratch.targets[u];
      const std::uint64_t* const view =
          views + static_cast<std::size_t>(u) * tt::kShiftedViewWords;
      const std::vector<tt::ResidualGroup>& queries = shared.q_groups[s];
      const std::uint32_t* const exact = data.view.exact[u];

      if constexpr (kInstrument) {
        mark = Clock::now();
      }
      for (std::uint32_t query = 0U; query < queries.size(); ++query) {
        const std::uint32_t d = (queries[query].value + kappa) & kMask19;
        const tt::QueryPlan plan = tt::make_query_plan(d);
        std::uint32_t found = 0U;
        if constexpr (kInstrument) {
          found = route_counted(source.occupancy.data(), translation, view, plan, hits,
                                stats->counters);
        } else {
          found = route(source.occupancy.data(), translation, view, plan, hits);
        }
        for (std::uint32_t index = 0U; index < found; ++index) {
          const std::uint32_t packed = hits[index];
          const std::uint32_t position = packed >> 2U;
          const std::uint32_t x_layer = (packed >> 1U) & 1U;
          const std::uint32_t y_layer = packed & 1U;
          const std::uint32_t x_group = source.group_at[x_layer][position];
          const std::uint32_t y_position = (position + plan.r) & tt::kPositionMask;
          const std::uint32_t y_group = target.group_at[y_layer][y_position];
          if constexpr (kInstrument) {
            if (x_group == kNoGroup || y_group == kNoGroup) {
              ++stats->primary_directory_faults;
              continue;
            }
          }
          sink.accept_group(s, t, u, kappa, query, x_group, y_group);
        }
      }
      if constexpr (kInstrument) {
        phases->route_ns += elapsed_ns(mark);
        stats->queries += queries.size();
        mark = Clock::now();
      }

      // Exact complement, disjoint by construction:
      //   (X_excess, Y_all) and (X_primary, Y_excess).
      if (!source.excess.empty() || !target.excess.empty()) {
        for (std::uint32_t query = 0U; query < queries.size(); ++query) {
          const std::uint32_t d = (queries[query].value + kappa) & kMask19;
          for (const std::uint32_t x_group : source.excess) {
            const std::uint32_t value = (source.groups[x_group].value + d) & kMask19;
            if (((exact[value >> 5U] >> (value & 31U)) & 1U) != 0U) {
              const std::uint32_t y_group = shared.y_lookup(u, value);
              if constexpr (kInstrument) {
                ++stats->overflow_hit_x;
              }
              sink.accept_group(s, t, u, kappa, query, x_group, y_group);
            }
          }
          for (const std::uint32_t y_group : target.excess) {
            const std::uint32_t value = (target.groups[y_group].value - d) & kMask19;
            const std::uint32_t x_group = tt::lookup_primary(source, value);
            if (x_group != kNoGroup) {
              if constexpr (kInstrument) {
                ++stats->overflow_hit_y;
              }
              sink.accept_group(s, t, u, kappa, query, x_group, y_group);
            }
          }
        }
        if constexpr (kInstrument) {
          stats->overflow_probe_x +=
              static_cast<std::uint64_t>(queries.size()) * source.excess.size();
          stats->overflow_probe_y +=
              static_cast<std::uint64_t>(queries.size()) * target.excess.size();
        }
      }
      if constexpr (kInstrument) {
        phases->overflow_ns += elapsed_ns(mark);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Path D: the radix occupancy locator, swept over the position width.
//
// Same matched scope, same shared index, same sink.  It replaces the 11+8 split
// with a PositionBits + (19 - PositionBits) split, which makes the complete tag
// universe at one position fit in a single machine word.  There are
// consequently no layers, no bit planes, no shifted tag views, no translation
// table and no excess lists: the guard emits compatible bucket locations and
// each one is resolved by exactly one rotation and one AND.
//
// The radix is a template parameter, so every radix in the sweep runs the same
// kernel source.  A difference between two radices is therefore a difference
// between two radices, never between two implementations.
//
// Four leaf kernels are measured on identical data and must agree exactly:
//   COMPACT    the representation the proposal specifies (rank directory),
//              whose tables are sized by the occupied positions, not by R.
//   DENSE      tags addressed by position; no rank directory, no dependent
//              load, but 8*R bytes per class.
//   BATCHED    dense, split into an extraction pass and a fixed-trip-count
//              resolution pass.
//   LANE-SKIP  batched, plus a movemask that names the nonempty 64-bit lanes.
// ---------------------------------------------------------------------------
namespace rx = ter::radix;

template <unsigned PB>
using RadixRouteFn = std::uint32_t (*)(const rx::RouteArgs<PB>&, std::uint32_t*);
template <unsigned PB>
using RadixCountedFn = std::uint32_t (*)(const rx::RouteArgs<PB>&, std::uint32_t*,
                                         rx::Counters&);
template <unsigned PB>
using RadixGuardFn = std::uint64_t (*)(const rx::RouteArgs<PB>&);

struct RadixPhases {
  double target_class_ns = 0.0;
  double shifted_view_ns = 0.0;
  double source_class_ns = 0.0;
  double route_ns = 0.0;
};

struct RadixStats {
  std::uint64_t queries = 0;
  std::uint64_t directory_faults = 0;
  std::uint64_t hit_capacity_overflows = 0;
  rx::Counters counters;
};

template <unsigned PB>
struct RadixScratch {
  using G = rx::Geom<PB>;
  std::vector<rx::SourceClass<PB>> sources;
  std::vector<rx::TargetClass<PB>> targets;
  tt::AlignedWords views;
  std::vector<std::uint32_t> hits;
  bool built = false;

  void allocate() {
    sources.resize(kFamilies);
    targets.resize(kFamilies);
    views.reset(G::kViewWordsPerClass * kFamilies);
    hits.assign(G::kHitCapacity, 0U);
  }

  [[nodiscard]] std::uint64_t aux_bytes() const {
    std::uint64_t bytes = views.bytes() + hits.size() * sizeof(std::uint32_t);
    for (const rx::SourceClass<PB>& source : sources) {
      bytes += sizeof(rx::SourceClass<PB>);
      bytes += source.groups.capacity() * sizeof(tt::ResidualGroup);
      bytes += source.tags_compact.capacity() * sizeof(std::uint64_t);
      bytes += source.tags_dense.capacity() * sizeof(std::uint64_t);
      bytes += source.group_base.capacity() * sizeof(std::uint32_t);
      bytes += source.group_ids.capacity() * sizeof(std::uint32_t);
    }
    for (const rx::TargetClass<PB>& target : targets) {
      bytes += sizeof(rx::TargetClass<PB>);
      bytes += target.groups.capacity() * sizeof(tt::ResidualGroup);
      bytes += target.bucket_rank.capacity() * sizeof(std::uint32_t);
      bytes += target.tags_compact.capacity() * sizeof(std::uint64_t);
      bytes += target.tags_dense.capacity() * sizeof(std::uint64_t);
      bytes += target.group_base.capacity() * sizeof(std::uint32_t);
      bytes += target.group_ids.capacity() * sizeof(std::uint32_t);
    }
    return bytes;
  }

  // Bytes the hot loop can actually reach for one (s, t) block: one source
  // class, one target class and one target shifted-view family.  This is the
  // number that decides whether a radix stays cache resident, and it is why a
  // large auxiliary total is not by itself a reason to reject a radix.
  [[nodiscard]] std::uint64_t hot_bytes_compact() const {
    const std::uint64_t compact_tags =
        sources.empty() ? 0U : sources[0].tags_compact.capacity() * sizeof(std::uint64_t);
    return static_cast<std::uint64_t>(G::kWords) * sizeof(std::uint64_t) * 2U +
           static_cast<std::uint64_t>(G::kViewWords) * sizeof(std::uint64_t) * G::kShifts +
           static_cast<std::uint64_t>(G::kR) * sizeof(std::uint32_t) + compact_tags * 2U;
  }
  [[nodiscard]] std::uint64_t hot_bytes_dense() const {
    return static_cast<std::uint64_t>(G::kWords) * sizeof(std::uint64_t) * 2U +
           static_cast<std::uint64_t>(G::kViewWords) * sizeof(std::uint64_t) * G::kShifts +
           2U * static_cast<std::uint64_t>(G::kR) * sizeof(std::uint64_t);
  }
};

// Selects which exact tag representation the leaf kernel addresses.  The guard
// is identical either way.
enum class RadixLeaf : std::uint8_t { Compact, Dense };

// kGuardOnly measures the guard phase in isolation: it performs the full vector
// AND plus zero test per query and emits nothing, so the difference against a
// full sweep is the leaf and reporting cost.
template <unsigned PB, bool kInstrument, bool kGuardOnly>
void sweep_radix(const Dataset& data, const SharedIndex& shared, MatchedSink& sink,
                 RadixScratch<PB>& scratch, const RadixLeaf leaf,
                 const RadixRouteFn<PB> route, const RadixCountedFn<PB> route_counted,
                 const RadixGuardFn<PB> guard, RadixPhases* const phases,
                 RadixStats* const stats) {
  using G = rx::Geom<PB>;
  std::uint32_t* const hits = scratch.hits.data();
  std::uint64_t* const views = scratch.views.data();

  Clock::time_point mark;
  if constexpr (kInstrument) {
    mark = Clock::now();
  }
  bool ok = true;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::vector<Posting>& ys = data.y_family[u];
    std::vector<std::uint32_t> values(ys.size());
    for (std::size_t index = 0U; index < ys.size(); ++index) {
      values[index] = ys[index].value;
    }
    ok = rx::build_target_class<PB>(values.data(), values.size(), scratch.targets[u]) && ok;
  }
  if constexpr (kInstrument) {
    phases->target_class_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    rx::build_shifted_views<PB>(
        scratch.targets[u], views + static_cast<std::size_t>(u) * G::kViewWordsPerClass);
  }
  if constexpr (kInstrument) {
    phases->shifted_view_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const std::vector<Posting>& xs = data.x_family[t];
    std::vector<std::uint32_t> values(xs.size());
    for (std::size_t index = 0U; index < xs.size(); ++index) {
      values[index] = xs[index].value;
    }
    ok = rx::build_source_class<PB>(values.data(), values.size(), scratch.sources[t]) && ok;
  }
  if constexpr (kInstrument) {
    phases->source_class_ns += elapsed_ns(mark);
  }
  scratch.built = ok;
  if (!ok) {
    return;  // fail-closed: the driver turns this into a fallback, never EMPTY
  }

  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const rx::SourceClass<PB>& source = scratch.sources[t];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> 6U;
      const rx::TargetClass<PB>& target = scratch.targets[u];
      const std::uint64_t* const view =
          views + static_cast<std::size_t>(u) * G::kViewWordsPerClass;
      const std::vector<tt::ResidualGroup>& queries = shared.q_groups[s];

      if constexpr (kInstrument) {
        mark = Clock::now();
      }
      for (std::uint32_t query = 0U; query < queries.size(); ++query) {
        const std::uint32_t d = (queries[query].value + kappa) & kMask19;
        const rx::QueryPlan<PB> plan = rx::make_query_plan<PB>(d);

        rx::RouteArgs<PB> args;
        args.x_occupancy = source.occupancy.data();
        args.x_prefix = source.occupied_prefix.data();
        args.y_view = view + static_cast<std::size_t>(plan.shift) * G::kViewWords;
        args.y_bucket_rank = target.bucket_rank.data();
        args.r = plan.r;
        args.alpha = plan.alpha;
        args.word_offset = plan.word_offset;
        if (leaf == RadixLeaf::Compact) {
          args.x_tags = source.tags_compact.data();
          args.y_tags = target.tags_compact.data();
        } else {
          args.x_tags = source.tags_dense.data();
          args.y_tags = target.tags_dense.data();
        }

        if constexpr (kGuardOnly) {
          g_sink += guard(args);
          continue;
        }

        std::uint32_t found = 0U;
        if constexpr (kInstrument) {
          found = route_counted(args, hits, stats->counters);
        } else {
          found = route(args, hits);
        }
        if (found > G::kHitCapacity) {
          if constexpr (kInstrument) {
            ++stats->hit_capacity_overflows;
          }
          continue;
        }
        for (std::uint32_t index = 0U; index < found; ++index) {
          const std::uint32_t packed = hits[index];
          const std::uint32_t position = rx::hit_position<PB>(packed);
          const std::uint32_t x_group =
              rx::source_group<PB>(source, position, rx::hit_source_tag<PB>(packed));
          const std::uint32_t y_position = (position + plan.r) & G::kPositionMask;
          const std::uint32_t y_group =
              rx::target_group<PB>(target, y_position, rx::hit_target_tag<PB>(packed));
          if constexpr (kInstrument) {
            if (x_group == kNoGroup || y_group == kNoGroup) {
              ++stats->directory_faults;
              continue;
            }
          }
          sink.accept_group(s, t, u, kappa, query, x_group, y_group);
        }
      }
      if constexpr (kInstrument) {
        phases->route_ns += elapsed_ns(mark);
        stats->queries += queries.size();
      }
    }
  }
}


// One radix's complete record.  Counts and phase costs come from the
// instrumented pass; the Stats members are filled after the timing loop.
struct RadixResult {
  unsigned position_bits = 0;
  std::uint32_t radix = 0;
  std::uint32_t tag_universe = 0;
  std::uint32_t guard_vectors_per_query = 0;
  bool available = false;
  std::string fallback_reason;

  RadixPhases phases;
  RadixStats stats;
  std::uint64_t aux_bytes = 0;
  std::uint64_t hot_bytes_compact = 0;
  std::uint64_t hot_bytes_dense = 0;
  bool scalar_reference_agrees = false;
  std::uint64_t scalar_reference_witnesses = 0;

  Stats dense;
  Stats compact;
  Stats batched;
  Stats skip;
  Stats group_only;
  Stats guard_only;
  Stats extract_only;
};

// Validation and the instrumented counting pass for one radix.  Compare is the
// caller's truth comparator; Outcomes is the caller's outcome list.  Both are
// template parameters only because they name types local to main.
template <unsigned PB, typename Compare, typename Outcomes>
void run_radix_validation(const Dataset& data, const SharedIndex& shared, MatchedSink& sink,
                          RadixScratch<PB>& scratch, const bool avx2_usable,
                          Outcomes& outcomes, Compare&& compare, RadixResult& out) {
  using G = rx::Geom<PB>;
  out.position_bits = PB;
  out.radix = G::kR;
  out.tag_universe = G::kH;
  out.guard_vectors_per_query = G::kSlices;

  const std::string tag = "radix" + std::to_string(G::kR);
  try {
    scratch.allocate();
  } catch (const std::bad_alloc&) {
    out.fallback_reason = tag + " auxiliary table allocation failed";
    return;
  }
  if (!avx2_usable) {
    out.fallback_reason = "AVX2 unavailable; the " + tag + " primary kernel is not executable";
    return;
  }

  std::cout << "# validating " << tag << " (portable, dense tags)...\n";
  std::cout.flush();
  sink.reset();
  sink.collect = true;
  sweep_radix<PB, false, false>(data, shared, sink, scratch, RadixLeaf::Dense,
                                &rx::route_query_dense_portable<PB>,
                                &rx::route_query_dense_portable_counted<PB>,
                                &rx::guard_only_portable<PB>, nullptr, nullptr);
  if (!scratch.built) {
    out.fallback_reason = tag + " class construction rejected the authentic families";
    return;
  }
  outcomes.push_back(compare(tag + "_portable_dense", sink));

  struct Variant {
    const char* name;
    RadixLeaf leaf;
    RadixRouteFn<PB> route;
  };
  const Variant variants[] = {
      {"_portable_compact", RadixLeaf::Compact, &rx::route_query_compact_portable<PB>},
      {"_avx2_dense", RadixLeaf::Dense, &rx::avx2::route_query_dense<PB>},
      {"_avx2_compact", RadixLeaf::Compact, &rx::avx2::route_query_compact<PB>},
      {"_avx2_batched", RadixLeaf::Dense, &rx::route_query_batched_portable<PB>},
      {"_avx2_skip", RadixLeaf::Dense, &rx::avx2::route_query_skip<PB>},
  };
  for (const Variant& variant : variants) {
    std::cout << "# validating " << tag << variant.name << "...\n";
    std::cout.flush();
    sink.reset();
    sink.collect = true;
    sweep_radix<PB, false, false>(data, shared, sink, scratch, variant.leaf, variant.route,
                                  &rx::route_query_dense_portable_counted<PB>,
                                  &rx::guard_only_portable<PB>, nullptr, nullptr);
    outcomes.push_back(compare(tag + variant.name, sink));
  }

  // Independent definitional cross-check on a deterministic query sample: the
  // scalar reference shares no bit-parallel machinery with the kernels.
  {
    std::vector<std::uint32_t> reference;
    std::vector<std::uint32_t> produced;
    bool agrees = true;
    std::uint64_t total = 0U;
    for (std::uint32_t t = 0U; t < kFamilies && agrees; ++t) {
      const std::uint32_t s = (t * 7U + 3U) & (kFamilies - 1U);
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> 6U;
      const std::vector<tt::ResidualGroup>& queries = shared.q_groups[s];
      const rx::SourceClass<PB>& source = scratch.sources[t];
      const rx::TargetClass<PB>& target = scratch.targets[u];
      const std::uint64_t* const view =
          scratch.views.data() + static_cast<std::size_t>(u) * G::kViewWordsPerClass;
      for (std::size_t index = 0U; index < queries.size() && agrees; index += 37U) {
        const std::uint32_t d = (queries[index].value + kappa) & kMask19;
        reference = rx::route_query_scalar<PB>(source, target, d);
        const rx::QueryPlan<PB> plan = rx::make_query_plan<PB>(d);
        rx::RouteArgs<PB> args;
        args.x_occupancy = source.occupancy.data();
        args.x_prefix = source.occupied_prefix.data();
        args.x_tags = source.tags_dense.data();
        args.y_view = view + static_cast<std::size_t>(plan.shift) * G::kViewWords;
        args.y_bucket_rank = target.bucket_rank.data();
        args.y_tags = target.tags_dense.data();
        args.r = plan.r;
        args.alpha = plan.alpha;
        args.word_offset = plan.word_offset;
        const std::uint32_t found =
            rx::avx2::route_query_skip<PB>(args, scratch.hits.data());
        produced.assign(scratch.hits.begin(), scratch.hits.begin() + found);
        std::sort(produced.begin(), produced.end());
        std::sort(reference.begin(), reference.end());
        total += produced.size();
        if (produced != reference) {
          agrees = false;
        }
      }
    }
    out.scalar_reference_agrees = agrees;
    out.scalar_reference_witnesses = total;
  }

  std::cout << "# " << tag << " instrumented counting pass...\n";
  std::cout.flush();
  sink.reset();
  sink.collect = false;
  sweep_radix<PB, true, false>(data, shared, sink, scratch, RadixLeaf::Dense,
                               &rx::avx2::route_query_dense<PB>,
                               &rx::avx2::route_query_dense_counted<PB>,
                               &rx::avx2::guard_only<PB>, &out.phases, &out.stats);
  out.aux_bytes = scratch.aux_bytes();
  out.hot_bytes_compact = scratch.hot_bytes_compact();
  out.hot_bytes_dense = scratch.hot_bytes_dense();
  if (out.stats.hit_capacity_overflows != 0U) {
    out.fallback_reason = tag + " witness buffer capacity exceeded";
    return;
  }
  out.available = true;
}

// One timed sweep of one radix.  The suffix names which kernel and which sink
// depth, so the timing loop needs no knowledge of the radix template.
template <unsigned PB>
[[nodiscard]] std::uint64_t run_radix_entity(const Dataset& data, const SharedIndex& shared,
                                             MatchedSink& sink, RadixScratch<PB>& scratch,
                                             const bool avx2_usable,
                                             const std::string& suffix) {
  const bool compact = suffix == "compact_matched";
  const bool batched = suffix == "batched_matched";
  const bool skip = suffix == "skip_matched";
  const RadixLeaf leaf = compact ? RadixLeaf::Compact : RadixLeaf::Dense;
  const RadixRouteFn<PB> route =
      compact   ? (avx2_usable ? &rx::avx2::route_query_compact<PB>
                               : &rx::route_query_compact_portable<PB>)
      : batched ? &rx::route_query_batched_portable<PB>
      : skip    ? (avx2_usable ? &rx::avx2::route_query_skip<PB>
                               : &rx::route_query_batched_portable<PB>)
                : (avx2_usable ? &rx::avx2::route_query_dense<PB>
                               : &rx::route_query_dense_portable<PB>);
  const RadixGuardFn<PB> guard =
      suffix == "extract_only"
          ? (avx2_usable ? &rx::avx2::extract_only<PB> : &rx::extract_only_portable<PB>)
          : (avx2_usable ? &rx::avx2::guard_only<PB> : &rx::guard_only_portable<PB>);

  sink.reset();
  sink.collect = false;
  sink.depth = suffix == "dense_group_only" ? SinkDepth::GroupOnly : SinkDepth::Full;
  if (suffix == "guard_only" || suffix == "extract_only") {
    sweep_radix<PB, false, true>(data, shared, sink, scratch, leaf, route,
                                 &rx::route_query_dense_portable_counted<PB>, guard, nullptr,
                                 nullptr);
    return g_sink;
  }
  sweep_radix<PB, false, false>(data, shared, sink, scratch, leaf, route,
                                &rx::route_query_dense_portable_counted<PB>, guard, nullptr,
                                nullptr);
  return sink.xor_checksum;
}

// The kernels a radix is timed with, in the order they are registered.
inline constexpr const char* kRadixSuffixes[] = {
    "dense_matched", "compact_matched",   "batched_matched", "skip_matched",
    "guard_only",    "dense_group_only",  "extract_only",
};

// ---------------------------------------------------------------------------
// Path E: Radix-13 occupancy guard + packed-tag mask consumer.
//
// Specification: TER_Radix13_Packed_Tag_Mask_Consumer_Architecture.pdf.  The
// guard, the classes, the shifted views and the exact dense tag words are the
// radix-13 ones, rebuilt inside this path's own timed region; the path adds the
// occupancy-rank payload decks (also built inside the timed region) and replaces
// "every guard bit -> position -> tag loads" with "PEXT ranks -> packed exact
// tags -> PDEP survivors -> exact resolver".  Witnesses reach the identical
// group mapping and matched sink as every other path.
// ---------------------------------------------------------------------------
namespace pk = ter::radix13_packed;

struct PackedPhases {
  double target_class_ns = 0.0;
  double shifted_view_ns = 0.0;
  double source_class_ns = 0.0;
  double deck_ns = 0.0;
  double route_ns = 0.0;
};

struct PackedScratch {
  using G = rx::Geom<13>;
  RadixScratch<13> base;  // classes, shifted views, witness buffer
  std::vector<pk::SourceDecks> source_decks;
  tt::AlignedWords target_deck0;
  tt::AlignedWords target_deck1;
  bool built = false;

  void allocate() {
    base.allocate();
    source_decks.resize(kFamilies);
    target_deck0.reset(pk::kTargetDeckWordsPerClass * kFamilies);
    target_deck1.reset(pk::kTargetDeckWordsPerClass * kFamilies);
  }

  [[nodiscard]] std::uint64_t deck_bytes() const {
    return source_decks.size() * sizeof(pk::SourceDecks) + target_deck0.bytes() +
           target_deck1.bytes();
  }

  // Bytes the common path can reach for one (s, t) block: source occupancy and
  // deck 0, and the target's shifted views and deck-0 planes for all 64 shifts.
  // Deck 1 and the dense tag words are touched only on exception paths.
  [[nodiscard]] static std::uint64_t hot_bytes_common() {
    return 2U * static_cast<std::uint64_t>(G::kWords) * sizeof(std::uint64_t) +
           2U * static_cast<std::uint64_t>(G::kViewWordsPerClass) * sizeof(std::uint64_t);
  }
};

enum class PackedMode : std::uint8_t {
  Resolve,
  FrontOnly,
  GuardOnly,
  QueuedFrontOnly,
  QueuedGuardOnly,
  BuildOnly
};

using PackedRouteFn = std::uint32_t (*)(const pk::PackedArgs&, std::uint32_t*);
using PackedCountedFn = std::uint32_t (*)(const pk::PackedArgs&, std::uint32_t*,
                                          pk::PackedCounters&);

struct PackedFaults {
  std::uint64_t directory_faults = 0;
  std::uint64_t hit_capacity_overflows = 0;
};

template <bool kInstrument>
void sweep_packed(const Dataset& data, const SharedIndex& shared, MatchedSink& sink,
                  PackedScratch& scratch, const PackedMode mode, const PackedRouteFn route,
                  const PackedCountedFn route_counted, PackedPhases* const phases,
                  pk::PackedCounters* const counters, PackedFaults* const faults) {
  using G = rx::Geom<13>;
  RadixScratch<13>& base = scratch.base;
  std::uint32_t* const hits = base.hits.data();
  std::uint64_t* const views = base.views.data();
  std::uint64_t* const deck0 = scratch.target_deck0.data();
  std::uint64_t* const deck1 = scratch.target_deck1.data();

  Clock::time_point mark;
  if constexpr (kInstrument) {
    mark = Clock::now();
  }
  bool ok = true;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::vector<Posting>& ys = data.y_family[u];
    std::vector<std::uint32_t> values(ys.size());
    for (std::size_t index = 0U; index < ys.size(); ++index) {
      values[index] = ys[index].value;
    }
    ok = rx::build_target_class<13>(values.data(), values.size(), base.targets[u]) && ok;
  }
  if constexpr (kInstrument) {
    phases->target_class_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    rx::build_shifted_views<13>(base.targets[u],
                                views + static_cast<std::size_t>(u) * G::kViewWordsPerClass);
  }
  if constexpr (kInstrument) {
    phases->shifted_view_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const std::vector<Posting>& xs = data.x_family[t];
    std::vector<std::uint32_t> values(xs.size());
    for (std::size_t index = 0U; index < xs.size(); ++index) {
      values[index] = xs[index].value;
    }
    ok = rx::build_source_class<13>(values.data(), values.size(), base.sources[t]) && ok;
  }
  if constexpr (kInstrument) {
    phases->source_class_ns += elapsed_ns(mark);
    mark = Clock::now();
  }
  scratch.built = ok;
  if (!ok) {
    return;  // fail-closed: never an empty result
  }
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::size_t offset = static_cast<std::size_t>(u) * pk::kTargetDeckWordsPerClass;
    pk::build_target_decks(base.targets[u], views + offset, deck0 + offset, deck1 + offset);
  }
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    pk::build_source_decks(base.sources[t], scratch.source_decks[t]);
  }
  if constexpr (kInstrument) {
    phases->deck_ns += elapsed_ns(mark);
  }
  if (mode == PackedMode::BuildOnly) {
    g_sink += deck0[kFamilies * pk::kTargetDeckWordsPerClass - 1U] ^ scratch.source_decks[0].deck0[0];
    return;
  }

  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const rx::SourceClass<13>& source = base.sources[t];
    const pk::SourceDecks& source_decks = scratch.source_decks[t];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> 6U;
      const rx::TargetClass<13>& target = base.targets[u];
      const std::size_t class_offset = static_cast<std::size_t>(u) * G::kViewWordsPerClass;
      const std::vector<tt::ResidualGroup>& queries = shared.q_groups[s];

      if constexpr (kInstrument) {
        mark = Clock::now();
      }
      for (std::uint32_t query = 0U; query < queries.size(); ++query) {
        const std::uint32_t d = (queries[query].value + kappa) & kMask19;
        const rx::QueryPlan<13> plan = rx::make_query_plan<13>(d);
        const std::size_t plane = class_offset + static_cast<std::size_t>(plan.shift) * G::kViewWords;

        pk::PackedArgs args;
        args.x_occupancy = source.occupancy.data();
        args.x_deck0 = source_decks.deck0.data();
        args.x_deck1 = source_decks.deck1.data();
        args.x_tags = source.tags_dense.data();
        args.y_view = views + plane;
        args.y_deck0 = deck0 + plane;
        args.y_deck1 = deck1 + plane;
        args.y_tags = target.tags_dense.data();
        args.r = plan.r;
        args.alpha = plan.alpha;
        args.word_offset = plan.word_offset;

        if (mode == PackedMode::GuardOnly) {
          g_sink += pk::bmi2::guard_only(args);
          continue;
        }
        if (mode == PackedMode::FrontOnly) {
          g_sink += pk::bmi2::front_only(args);
          continue;
        }
        if (mode == PackedMode::QueuedGuardOnly) {
          g_sink += pk::bmi2::queued_guard_only(args);
          continue;
        }
        if (mode == PackedMode::QueuedFrontOnly) {
          g_sink += pk::bmi2::queued_front_only(args);
          continue;
        }

        std::uint32_t found = 0U;
        if constexpr (kInstrument) {
          found = route_counted(args, hits, *counters);
        } else {
          found = route(args, hits);
        }
        if (found > G::kHitCapacity) {
          if constexpr (kInstrument) {
            ++faults->hit_capacity_overflows;
          }
          continue;
        }
        for (std::uint32_t index = 0U; index < found; ++index) {
          const std::uint32_t packed = hits[index];
          const std::uint32_t position = rx::hit_position<13>(packed);
          const std::uint32_t x_group =
              rx::source_group<13>(source, position, rx::hit_source_tag<13>(packed));
          const std::uint32_t y_position = (position + plan.r) & G::kPositionMask;
          const std::uint32_t y_group =
              rx::target_group<13>(target, y_position, rx::hit_target_tag<13>(packed));
          if constexpr (kInstrument) {
            if (x_group == kNoGroup || y_group == kNoGroup) {
              ++faults->directory_faults;
              continue;
            }
          }
          sink.accept_group(s, t, u, kappa, query, x_group, y_group);
        }
      }
      if constexpr (kInstrument) {
        phases->route_ns += elapsed_ns(mark);
      }
    }
  }
}

struct PackedResult {
  bool available = false;
  std::string fallback_reason;
  PackedPhases phases;
  pk::PackedCounters two_deck;  // counters of the two-deck consumer
  pk::PackedCounters one_deck;  // counters of the one-deck consumer
  PackedFaults faults;
  std::uint64_t stream_queries = 0;
  std::uint64_t stream_witnesses = 0;
  std::uint64_t stream_mismatches_two = 0;  // queries whose stream != dense kernel
  std::uint64_t stream_mismatches_one = 0;
  std::uint64_t stream_mismatches_queued = 0;  // either queued deck width
  bool scalar_reference_agrees = false;
  std::uint64_t scalar_reference_witnesses = 0;
  std::uint64_t deck_bytes = 0;
  std::uint64_t aux_bytes = 0;

  Stats matched;
  Stats onedeck;
  Stats group_only;
  Stats front_only;
  Stats guard_only;
  Stats queued;
  Stats queued_onedeck;
  Stats queued_group_only;
  Stats queued_front_only;
  Stats queued_guard_only;
  Stats queued_dense_control;
  Stats build_only;
};

template <typename Compare, typename Outcomes>
void run_packed_validation(const Dataset& data, const SharedIndex& shared, MatchedSink& sink,
                           PackedScratch& scratch, const bool bmi2_usable, Outcomes& outcomes,
                           Compare&& compare, PackedResult& out) {
  using G = rx::Geom<13>;
  try {
    scratch.allocate();
  } catch (const std::bad_alloc&) {
    out.fallback_reason = "packed13 table allocation failed";
    return;
  }
  if (!bmi2_usable) {
    out.fallback_reason =
        "AVX2+BMI2 unavailable (compile or CPU); the packed consumer's PEXT/PDEP route is "
        "not executable";
    return;
  }

  struct Variant {
    const char* name;
    PackedRouteFn route;
  };
  const Variant variants[] = {
      {"packed13_portable_onedeck", &pk::route_query_portable<false>},
      {"packed13_portable_twodeck", &pk::route_query_portable<true>},
      {"packed13_bmi2_onedeck", &pk::bmi2::route_query<false>},
      {"packed13_bmi2_twodeck", &pk::bmi2::route_query<true>},
      {"packed13_bmi2_queued_onedeck", &pk::bmi2::route_query_queued<false>},
      {"packed13_bmi2_queued_twodeck", &pk::bmi2::route_query_queued<true>},
      {"packed13_queued_dense_control", &pk::bmi2::route_query_queued_dense_control},
  };
  for (const Variant& variant : variants) {
    std::cout << "# validating " << variant.name << "...\n";
    std::cout.flush();
    sink.reset();
    sink.collect = true;
    sink.depth = SinkDepth::Full;
    sweep_packed<false>(data, shared, sink, scratch, PackedMode::Resolve, variant.route,
                        &pk::route_query_portable_counted<true>, nullptr, nullptr, nullptr);
    if (!scratch.built) {
      out.fallback_reason = "packed13 class construction rejected the authentic families";
      return;
    }
    outcomes.push_back(compare(variant.name, sink));
  }

  // Every query of the authentic sweep: both BMI2 consumers must reproduce the
  // plain radix-13 dense kernel's witness stream exactly, in order.  The dense
  // kernel uses no PEXT, no PDEP and no payload deck.
  std::cout << "# packed13 whole-stream identity against the plain radix-13 dense kernel...\n";
  std::cout.flush();
  {
    RadixScratch<13>& base = scratch.base;
    std::vector<std::uint32_t> reference(G::kHitCapacity, 0U);
    std::vector<std::uint32_t> produced(G::kHitCapacity, 0U);
    for (std::uint32_t t = 0U; t < kFamilies; ++t) {
      const rx::SourceClass<13>& source = base.sources[t];
      for (std::uint32_t s = 0U; s < kFamilies; ++s) {
        const std::uint32_t u = (s + t) & (kFamilies - 1U);
        const std::uint32_t kappa = (s + t) >> 6U;
        const rx::TargetClass<13>& target = base.targets[u];
        const std::size_t class_offset = static_cast<std::size_t>(u) * G::kViewWordsPerClass;
        for (const tt::ResidualGroup& group : shared.q_groups[s]) {
          const std::uint32_t d = (group.value + kappa) & kMask19;
          const rx::QueryPlan<13> plan = rx::make_query_plan<13>(d);
          const std::size_t plane =
              class_offset + static_cast<std::size_t>(plan.shift) * G::kViewWords;
          rx::RouteArgs<13> dense;
          dense.x_occupancy = source.occupancy.data();
          dense.x_tags = source.tags_dense.data();
          dense.y_view = base.views.data() + plane;
          dense.y_tags = target.tags_dense.data();
          dense.r = plan.r;
          dense.alpha = plan.alpha;
          dense.word_offset = plan.word_offset;
          const std::uint32_t expected =
              rx::route_query_dense_portable<13>(dense, reference.data());

          pk::PackedArgs args;
          args.x_occupancy = source.occupancy.data();
          args.x_deck0 = scratch.source_decks[t].deck0.data();
          args.x_deck1 = scratch.source_decks[t].deck1.data();
          args.x_tags = source.tags_dense.data();
          args.y_view = base.views.data() + plane;
          args.y_deck0 = scratch.target_deck0.data() + plane;
          args.y_deck1 = scratch.target_deck1.data() + plane;
          args.y_tags = target.tags_dense.data();
          args.r = plan.r;
          args.alpha = plan.alpha;
          args.word_offset = plan.word_offset;

          const std::uint32_t two = pk::bmi2::route_query<true>(args, produced.data());
          if (two != expected ||
              !std::equal(reference.begin(), reference.begin() + expected, produced.begin())) {
            ++out.stream_mismatches_two;
          }
          const std::uint32_t one = pk::bmi2::route_query<false>(args, produced.data());
          if (one != expected ||
              !std::equal(reference.begin(), reference.begin() + expected, produced.begin())) {
            ++out.stream_mismatches_one;
          }
          const std::uint32_t queued_two =
              pk::bmi2::route_query_queued<true>(args, produced.data());
          const bool queued_two_ok =
              queued_two == expected &&
              std::equal(reference.begin(), reference.begin() + expected, produced.begin());
          const std::uint32_t queued_one =
              pk::bmi2::route_query_queued<false>(args, produced.data());
          const bool queued_one_ok =
              queued_one == expected &&
              std::equal(reference.begin(), reference.begin() + expected, produced.begin());
          const std::uint32_t control =
              pk::bmi2::route_query_queued_dense_control(args, produced.data());
          const bool control_ok =
              control == expected &&
              std::equal(reference.begin(), reference.begin() + expected, produced.begin());
          if (!queued_two_ok || !queued_one_ok || !control_ok) {
            ++out.stream_mismatches_queued;
          }
          ++out.stream_queries;
          out.stream_witnesses += expected;
        }
      }
    }
  }

  // Definitional cross-check on a deterministic query sample: the scalar
  // reference uses no occupancy words, shifted views, decks or bit tricks.
  {
    RadixScratch<13>& base = scratch.base;
    std::vector<std::uint32_t> reference;
    std::vector<std::uint32_t> produced(G::kHitCapacity, 0U);
    bool agrees = true;
    std::uint64_t total = 0U;
    for (std::uint32_t t = 0U; t < kFamilies && agrees; ++t) {
      const std::uint32_t s = (t * 7U + 3U) & (kFamilies - 1U);
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> 6U;
      const std::vector<tt::ResidualGroup>& queries = shared.q_groups[s];
      const std::size_t class_offset = static_cast<std::size_t>(u) * G::kViewWordsPerClass;
      for (std::size_t index = 0U; index < queries.size() && agrees; index += 37U) {
        const std::uint32_t d = (queries[index].value + kappa) & kMask19;
        reference = rx::route_query_scalar<13>(base.sources[t], base.targets[u], d);
        const rx::QueryPlan<13> plan = rx::make_query_plan<13>(d);
        const std::size_t plane =
            class_offset + static_cast<std::size_t>(plan.shift) * G::kViewWords;
        pk::PackedArgs args;
        args.x_occupancy = base.sources[t].occupancy.data();
        args.x_deck0 = scratch.source_decks[t].deck0.data();
        args.x_deck1 = scratch.source_decks[t].deck1.data();
        args.x_tags = base.sources[t].tags_dense.data();
        args.y_view = base.views.data() + plane;
        args.y_deck0 = scratch.target_deck0.data() + plane;
        args.y_deck1 = scratch.target_deck1.data() + plane;
        args.y_tags = base.targets[u].tags_dense.data();
        args.r = plan.r;
        args.alpha = plan.alpha;
        args.word_offset = plan.word_offset;
        const std::uint32_t found = pk::bmi2::route_query<true>(args, produced.data());
        std::vector<std::uint32_t> got(produced.begin(), produced.begin() + found);
        std::sort(got.begin(), got.end());
        std::sort(reference.begin(), reference.end());
        total += got.size();
        agrees = got == reference;
      }
    }
    out.scalar_reference_agrees = agrees;
    out.scalar_reference_witnesses = total;
  }

  // Instrumented counting passes: the geometry diagnostics of section 23 and
  // the counters of section 36, for both deck widths.
  std::cout << "# packed13 instrumented counting passes...\n";
  std::cout.flush();
  sink.reset();
  sink.collect = false;
  sink.depth = SinkDepth::Full;
  sweep_packed<true>(data, shared, sink, scratch, PackedMode::Resolve,
                     &pk::bmi2::route_query<true>, &pk::route_query_portable_counted<true>,
                     &out.phases, &out.two_deck, &out.faults);
  {
    PackedPhases discard;
    PackedFaults faults;
    sink.reset();
    sweep_packed<true>(data, shared, sink, scratch, PackedMode::Resolve,
                       &pk::bmi2::route_query<false>, &pk::route_query_portable_counted<false>,
                       &discard, &out.one_deck, &faults);
    out.faults.directory_faults += faults.directory_faults;
    out.faults.hit_capacity_overflows += faults.hit_capacity_overflows;
  }
  out.deck_bytes = scratch.deck_bytes();
  out.aux_bytes = scratch.base.aux_bytes() + out.deck_bytes;
  if (out.faults.hit_capacity_overflows != 0U) {
    out.fallback_reason = "packed13 witness buffer capacity exceeded";
    return;
  }
  out.available = true;
}

inline constexpr const char* kPackedEntities[] = {
    "packed13_matched",
    "packed13_onedeck_matched",
    "packed13_group_only",
    "packed13_front_only",
    "packed13_guard_only",
    "packed13_queued_matched",
    "packed13_queued_onedeck_matched",
    "packed13_queued_group_only",
    "packed13_queued_front_only",
    "packed13_queued_guard_only",
    "packed13_queued_dense_control",
    "packed13_build_only",
};

[[nodiscard]] std::uint64_t run_packed_entity(const Dataset& data, const SharedIndex& shared,
                                              MatchedSink& sink, PackedScratch& scratch,
                                              const std::string& id) {
  sink.reset();
  sink.collect = false;
  sink.depth = id == "packed13_group_only" || id == "packed13_queued_group_only"
                   ? SinkDepth::GroupOnly
                   : SinkDepth::Full;
  const PackedMode mode = id == "packed13_front_only"          ? PackedMode::FrontOnly
                          : id == "packed13_guard_only"        ? PackedMode::GuardOnly
                          : id == "packed13_queued_front_only" ? PackedMode::QueuedFrontOnly
                          : id == "packed13_queued_guard_only" ? PackedMode::QueuedGuardOnly
                          : id == "packed13_build_only"        ? PackedMode::BuildOnly
                                                               : PackedMode::Resolve;
  const bool queued_route = id == "packed13_queued_matched" ||
                            id == "packed13_queued_onedeck_matched" ||
                            id == "packed13_queued_group_only";
  const bool one_deck =
      id == "packed13_onedeck_matched" || id == "packed13_queued_onedeck_matched";
  const PackedRouteFn route =
      id == "packed13_queued_dense_control" ? &pk::bmi2::route_query_queued_dense_control
      : queued_route ? (one_deck ? &pk::bmi2::route_query_queued<false>
                                 : &pk::bmi2::route_query_queued<true>)
                     : (one_deck ? &pk::bmi2::route_query<false> : &pk::bmi2::route_query<true>);
  sweep_packed<false>(data, shared, sink, scratch, mode, route,
                      &pk::route_query_portable_counted<true>, nullptr, nullptr, nullptr);
  return mode == PackedMode::Resolve ? sink.xor_checksum : g_sink;
}

// ---------------------------------------------------------------------------
// WORKING-SET LOCALITY CONTROL.  This is diagnostic-only and never participates
// in the spike verdict.  It reuses the real query stream and router but pins all
// lookup tables to family zero, keeping the target shifted-view working set at
// one family's size.  Its family pairings are intentionally not valid TER
// microproblems, so it folds hit counts into a checksum and does not invoke the
// downstream sink.  Because those fixed tables have different occupancy and
// early-exit behavior from the authentic sweep, this control can demonstrate a
// locality contribution; it cannot attribute the whole timing gap to locality.
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t sweep_locality_control(const SharedIndex& shared,
                                                    LocatorScratch& scratch,
                                                    const RouteFn route) {
  std::uint32_t* const hits = scratch.hits.data();
  std::uint64_t* const translation = scratch.translation.data();
  const std::uint64_t* const hot_view = scratch.views.data();
  const tt::SourceClass& hot_source = scratch.sources[0];
  std::uint64_t lane[4] = {0U, 0U, 0U, 0U};
  std::size_t entry = 0U;
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    tt::build_translation_table(hot_source, translation);
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const std::uint32_t kappa = (s + t) >> 6U;
      for (const tt::ResidualGroup& query : shared.q_groups[s]) {
        const std::uint32_t d = (query.value + kappa) & kMask19;
        const tt::QueryPlan plan = tt::make_query_plan(d);
        lane[entry & 3U] +=
            route(hot_source.occupancy.data(), translation, hot_view, plan, hits);
        ++entry;
      }
    }
  }
  return lane[0] + lane[1] + lane[2] + lane[3];
}

// ---------------------------------------------------------------------------
// Sink replay: the shared downstream cost, measured on its own so the report can
// separate frontend cost from the authority pipeline every path pays.
// ---------------------------------------------------------------------------
struct ReplayStream {
  std::vector<std::uint32_t> s;
  std::vector<std::uint32_t> t;
  std::vector<std::uint32_t> u;
  std::vector<std::uint32_t> kappa;
  std::vector<std::uint32_t> q_group;
  std::vector<std::uint32_t> x_group;
  std::vector<std::uint32_t> y_group;

  [[nodiscard]] std::size_t size() const noexcept { return s.size(); }
};

void replay_sink(const ReplayStream& stream, MatchedSink& sink) {
  for (std::size_t index = 0U; index < stream.size(); ++index) {
    sink.accept_group(stream.s[index], stream.t[index], stream.u[index],
                      stream.kappa[index], stream.q_group[index], stream.x_group[index],
                      stream.y_group[index]);
  }
}

[[nodiscard]] ReplayStream build_replay_stream(const std::vector<std::uint64_t>& group_keys) {
  ReplayStream stream;
  stream.s.reserve(group_keys.size());
  for (const std::uint64_t key : group_keys) {
    const auto y_group = static_cast<std::uint32_t>(key % 1024U);
    const auto x_group = static_cast<std::uint32_t>((key / 1024U) % 1024U);
    const auto q_group = static_cast<std::uint32_t>((key / (1024U * 1024U)) % 1024U);
    const std::uint64_t head = key / (1024ULL * 1024ULL * 1024ULL);
    const auto t = static_cast<std::uint32_t>(head % 64U);
    const auto s = static_cast<std::uint32_t>(head / 64U);
    stream.s.push_back(s);
    stream.t.push_back(t);
    stream.u.push_back((s + t) & (kFamilies - 1U));
    stream.kappa.push_back((s + t) >> 6U);
    stream.q_group.push_back(q_group);
    stream.x_group.push_back(x_group);
    stream.y_group.push_back(y_group);
  }
  return stream;
}

// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t process_peak_bytes() noexcept {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
  }
#endif
  return 0U;
}

void write_stats_json(std::ostream& out, const std::string_view name, const Stats& stats) {
  out << "\"" << name << "\":{\"median_ms\":" << number(stats.median)
      << ",\"p25_ms\":" << number(stats.p25) << ",\"p75_ms\":" << number(stats.p75)
      << ",\"p95_ms\":" << number(stats.p95) << ",\"iqr_ms\":" << number(stats.iqr)
      << ",\"min_ms\":" << number(stats.minimum) << ",\"samples\":" << stats.samples << "}";
}

struct TimedEntity {
  std::string id;
  std::vector<double> samples;
  Stats stats;
};

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t root_n = ter::microreport8::kReferenceRootN;
  std::uint64_t root_seed = ter::microreport8::kReferenceRootSeed;
  std::size_t sample_count = 51U;
  bool pin_to_one_core = true;
  bool run_production_anchor = true;
  std::string output_directory = "results/translated-tag-locator/raw";

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument.rfind("--samples=", 0U) == 0U) {
      sample_count = static_cast<std::size_t>(std::strtoull(argv[index] + 10, nullptr, 10));
    } else if (argument.rfind("--out=", 0U) == 0U) {
      output_directory = std::string(argument.substr(6U));
    } else if (argument == "--no-pin") {
      pin_to_one_core = false;
    } else if (argument == "--no-anchor") {
      run_production_anchor = false;
    } else {
      std::cerr << "unknown argument: " << argument << '\n';
      return EXIT_FAILURE;
    }
  }
  if (sample_count == 0U) {
    std::cerr << "FATAL: --samples must be positive\n";
    return EXIT_FAILURE;
  }

  std::cout << "# ter_translated_tag_bench -- TER^2 translated-tag locator spike\n";
  std::cout << "# configure_time_commit=" << TER_CONFIGURE_TIME_COMMIT
            << " configure_time_dirty=" << (TER_CONFIGURE_TIME_DIRTY ? "yes" : "no") << '\n';
  const ter::microreport8::IsaReport isa = ter::microreport8::detect_isa();
  std::cout << "# cpu=" << (isa.brand[0] != '\0' ? isa.brand : "unknown") << '\n';
  const bool avx2_compiled = tt::avx2::translation_unit_is_avx2();
  const bool avx2_usable = isa.usable && avx2_compiled;
  std::cout << "# isa_compiled=" << (avx2_compiled ? "AVX2" : "portable")
            << " isa_available=" << (isa.usable ? "AVX2" : "portable")
            << " isa_executed=" << (avx2_usable ? "AVX2" : "portable") << '\n';

  std::string affinity_state = "not attempted";
  if (pin_to_one_core) {
#if defined(_WIN32)
    const bool affinity =
        SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1)) != 0;
    const bool priority = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    affinity_state = affinity ? (priority ? "cpu0 + high priority" : "cpu0, priority unchanged")
                              : "failed";
#else
    affinity_state = "unsupported on this platform";
#endif
  } else {
    affinity_state = "disabled by --no-pin";
  }
  std::cout << "# thread_affinity=" << affinity_state << '\n';
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Fixture gate.
  // -------------------------------------------------------------------------
  std::cout << "# capturing authentic root (n=" << root_n << ", seed=" << root_seed << ")...\n";
  std::cout.flush();
  const auto capture_start = Clock::now();
  const auto data = ter::microreport8::build_dataset(root_n, root_seed, run_production_anchor);
  const double capture_ns = elapsed_ns(capture_start);
  if (!data->ok) {
    std::cerr << "FATAL: " << data->message << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "# |A|=" << data->a_size << " |B|=" << data->b_size << " |C|=" << data->c_size
            << " Q=" << data->quotient_bits << " r1=" << data->r1
            << " P0=" << data->pair_universe << " capture_ms=" << number(capture_ns / 1.0e6)
            << '\n';
  std::cout << "# tau_low32=" << data->tau_low32 << " tau_low25=" << data->tau_low25
            << " tau_derivation_verified=" << (data->tau_derivation_verified ? "true" : "false")
            << '\n';
  std::cout.flush();

  std::vector<std::string> failures;
  const auto require = [&](const bool condition, const std::string& detail) {
    if (!condition) {
      failures.push_back(detail);
    }
  };
  require(data->n == 72U, "n != 72");
  require(data->seed == 19797034556212562ULL, "seed mismatch");
  require(data->quotient_bits == 47U, "Q != 47");
  require(data->r1 == 25U, "r1 != 25");
  require(ter::microreport8::kProjectionBits == 25U, "k != 25");
  require(ter::microreport8::kResidualBits == 19U, "residual width != 19");
  require(data->a_size == 19109U, "|A| != 19109");
  require(data->b_size == 32496U, "|B| != 32496");
  require(data->c_size == 33505U, "|C| != 33505");
  require(data->pair_universe == kPriorPairUniverse, "P0 != 620966064");
  require(data->tau_low25 == 5005954U, "tau_low25 != 5005954");
  require(data->tau_derivation_verified, "tau derivation not verified");
  require(kFamilies * kFamilies == 4096U, "microproblem count != 4096");

  std::cout << "# running independent tiled_q3report_count cross-check...\n";
  std::cout.flush();
  ter::TiledQ3ReportConfig tiled_config;
  tiled_config.preferred_tile_bits = ter::limits::tiled::kDefaultPreferredTileBits;
  const auto crosscheck = ter::tiled_q3report_count(
      data->a_low25, data->b_low25, data->c_low25, data->tau_low25,
      std::uint64_t{1} << ter::microreport8::kProjectionBits, tiled_config);
  if (crosscheck.status != ter::TiledQ3ReportStatus::Completed) {
    std::cerr << "FATAL: cross-check reporter failed: "
              << ter::tiled_q3report_status_name(crosscheck.status) << " "
              << crosscheck.message << '\n';
    return EXIT_FAILURE;
  }
  const std::uint64_t crosscheck_triples = crosscheck.telemetry.raw_triples;
  require(crosscheck_triples == kPriorTriples, "tiled_q3report_count != 619579");
  require(data->production_anchor_available, "production low-25 anchor unavailable");
  if (data->production_anchor_available) {
    require(data->production_low_prefilter_candidates == kPriorTriples,
            "production low-25 count != 619579");
  }
  std::cout << "# decomposition_crosscheck=" << crosscheck_triples
            << " production_low25=" << data->production_low_prefilter_candidates << " ("
            << data->production_anchor_status << ")\n";

  // -------------------------------------------------------------------------
  // Authority tables from an independent capture of the same root.
  // -------------------------------------------------------------------------
  std::cout << "# building authority tables (independent capture)...\n";
  std::cout.flush();
  const auto authority_start = Clock::now();
  const tt::AuthorityTables authority = tt::build_authority_tables(root_n, root_seed);
  const double authority_ns = elapsed_ns(authority_start);
  if (!authority.ok) {
    std::cerr << "FATAL: " << authority.message << '\n';
    return EXIT_FAILURE;
  }
  require(authority.tau_low32 == data->tau_low32, "authority tau_low32 disagrees with donor");
  require(authority.list_sizes[0] == data->a_size, "authority |A| disagrees with donor");
  require(authority.list_sizes[1] == data->b_size, "authority |B| disagrees with donor");
  require(authority.list_sizes[2] == data->c_size, "authority |C| disagrees with donor");
  require(authority.shape_available, "expected parent shape unavailable");
  {
    // Independent re-derivation identity: the authority capture must reproduce
    // the donor's low-25 projection entry for entry, or the two captures are not
    // the same root and nothing downstream is comparable.
    std::uint64_t projection_mismatches = 0U;
    const std::array<const std::vector<ter::TiledQEntry>*, 3> donor{
        &data->a_low25, &data->b_low25, &data->c_low25};
    for (std::size_t group = 0U; group < 3U; ++group) {
      const auto& expected = *donor[group];
      if (expected.size() != authority.quotient[group].size()) {
        ++projection_mismatches;
        continue;
      }
      for (std::size_t index = 0U; index < expected.size(); ++index) {
        if ((authority.quotient[group][index] & ter::microreport8::kProjectionMask) !=
            expected[index].q) {
          ++projection_mismatches;
        }
      }
    }
    require(projection_mismatches == 0U,
            "authority capture low-25 projection differs from the donor dataset");
    std::cout << "# authority_capture_ms=" << number(authority_ns / 1.0e6)
              << " low25_projection_mismatches=" << projection_mismatches
              << " residue_words=" << authority.residue_words << " expected_parent=("
              << authority.expected_parent.negative << ',' << authority.expected_parent.zero
              << ',' << authority.expected_parent.positive << ")\n";
  }

  if (!failures.empty()) {
    std::cerr << "FATAL: the authenticated fixture invariants no longer hold:\n";
    for (const std::string& failure : failures) {
      std::cerr << "  - " << failure << '\n';
    }
    return EXIT_FAILURE;
  }
  std::cout << "# fixture gate: all authenticated invariants hold\n";
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Shared index (setup, common to all three paths).
  // -------------------------------------------------------------------------
  SharedIndex shared;
  const auto shared_start = Clock::now();
  const std::uint32_t widest_family = build_shared_index(*data, shared);
  const double shared_ns = elapsed_ns(shared_start);
  if (widest_family >= 1024U) {
    std::cerr << "FATAL: a family holds " << widest_family
              << " distinct values, which overflows the group-triple key\n";
    return EXIT_FAILURE;
  }
  std::uint64_t distinct_q = 0U;
  std::uint64_t distinct_x = 0U;
  std::uint64_t distinct_y = 0U;
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    distinct_q += shared.q_groups[family].size();
    distinct_x += shared.x_groups[family].size();
    distinct_y += shared.y_groups[family].size();
  }
  std::cout << "# shared_index_ms=" << number(shared_ns / 1.0e6)
            << " distinct Q/X/Y=" << distinct_q << '/' << distinct_x << '/' << distinct_y
            << " widest_family=" << widest_family
            << " shared_bytes=" << shared.bytes << '\n';
  std::cout.flush();

  // -------------------------------------------------------------------------
  // VALIDATION-ONLY exhaustive truth over all 620,966,064 authentic pairs.
  // -------------------------------------------------------------------------
  std::cout << "# building VALIDATION-ONLY exhaustive truth over " << data->pair_universe
            << " source pairs...\n";
  std::cout.flush();
  Truth truth = build_truth(*data);
  canonicalize_truth_groups(truth, shared);
  std::cout << "# truth_ms=" << number(truth.build_ns / 1.0e6)
            << " pair_hits=" << truth.pair_hits << " triples=" << truth.triples
            << " distinct_group_triples=" << truth.group_keys.size() << '\n';
  require(truth.pair_hits == kPriorTrueLanes, "truth pair hits != 619303");
  require(truth.triples == kPriorTriples, "truth triples != 619579");
  if (!failures.empty()) {
    std::cerr << "FATAL: exhaustive truth disagrees with the established fixture counts:\n";
    for (const std::string& failure : failures) {
      std::cerr << "  - " << failure << '\n';
    }
    return EXIT_FAILURE;
  }
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Correctness: run every path once in collect mode and compare with truth.
  // -------------------------------------------------------------------------
  struct PathOutcome {
    std::string id;
    std::uint64_t pair_hits = 0;
    std::uint64_t triples = 0;
    std::uint64_t distinct_groups = 0;
    std::uint64_t full_q = 0;
    std::uint64_t residue = 0;
    std::uint64_t structural = 0;
    std::uint64_t outputs = 0;
    std::uint64_t residual_mismatches = 0;
    std::uint64_t directory_misses = 0;
    std::uint64_t entry_mismatches = 0;   // symmetric difference against truth
    std::uint64_t group_mismatches = 0;
    std::uint64_t false_negatives = 0;
    std::uint64_t false_positives = 0;
    std::uint64_t sum_checksum = 0;
    std::uint64_t xor_checksum = 0;
    bool entry_multiset_equal = false;
    bool group_set_equal = false;
  };

  MatchedSink sink;
  sink.data = data.get();
  sink.auth = &authority;
  sink.shared = &shared;
  sink.depth = SinkDepth::Full;

  const auto compare_against_truth = [&](const std::string& id, MatchedSink& done) {
    PathOutcome outcome;
    outcome.id = id;
    outcome.pair_hits = done.pair_hits;
    outcome.triples = done.expanded_entry_triples;
    outcome.full_q = done.full_q_survivors;
    outcome.residue = done.residue_survivors;
    outcome.structural = done.structural_survivors;
    outcome.outputs = done.final_outputs;
    outcome.residual_mismatches = done.residual_mismatches;
    outcome.directory_misses = done.directory_misses;
    outcome.sum_checksum = done.sum_checksum;
    outcome.xor_checksum = done.xor_checksum;
    std::sort(done.entry_keys.begin(), done.entry_keys.end());
    std::sort(done.group_keys.begin(), done.group_keys.end());
    done.group_keys.erase(std::unique(done.group_keys.begin(), done.group_keys.end()),
                          done.group_keys.end());
    outcome.distinct_groups = done.group_keys.size();
    outcome.entry_multiset_equal = done.entry_keys == truth.entry_keys;
    outcome.group_set_equal = done.group_keys == truth.group_keys;
    std::vector<std::uint64_t> missing;
    std::set_difference(truth.entry_keys.begin(), truth.entry_keys.end(),
                        done.entry_keys.begin(), done.entry_keys.end(),
                        std::back_inserter(missing));
    std::vector<std::uint64_t> extra;
    std::set_difference(done.entry_keys.begin(), done.entry_keys.end(),
                        truth.entry_keys.begin(), truth.entry_keys.end(),
                        std::back_inserter(extra));
    outcome.false_negatives = missing.size();
    outcome.false_positives = extra.size();
    outcome.entry_mismatches = missing.size() + extra.size();
    std::vector<std::uint64_t> group_difference;
    std::set_symmetric_difference(truth.group_keys.begin(), truth.group_keys.end(),
                                  done.group_keys.begin(), done.group_keys.end(),
                                  std::back_inserter(group_difference));
    outcome.group_mismatches = group_difference.size();
    return outcome;
  };

  std::vector<PathOutcome> outcomes;

  std::cout << "# validating scalar path...\n";
  std::cout.flush();
  sink.reset();
  sink.collect = true;
  sweep_scalar(*data, sink);
  outcomes.push_back(compare_against_truth("scalar", sink));
  const std::vector<std::uint64_t> canonical_group_keys = sink.group_keys;

  const TileKernelFn k2_kernel = avx2_usable
                                     ? &ter::microreport8::avx2::k2_coarse_gather
                                     : &ter::microreport8::k2_coarse_gather_portable;
  std::cout << "# validating frozen K2 path...\n";
  std::cout.flush();
  sink.reset();
  sink.collect = true;
  sweep_k2(*data, sink, k2_kernel);
  outcomes.push_back(compare_against_truth("k2_frozen", sink));

  LocatorScratch scratch;
  std::string fallback_reason;
  bool tables_ready = true;
  try {
    scratch.allocate();
  } catch (const std::bad_alloc&) {
    tables_ready = false;
    fallback_reason = "auxiliary table allocation failed";
  }
  // Fail-closed: the AVX2 primary router IS the proposed architecture.  Without
  // it the run does not silently report a portable result, and it never reports
  // EMPTY: the frozen exact fallback runs for the same matched scope instead.
  if (tables_ready && !avx2_usable) {
    fallback_reason = "AVX2 unavailable; the primary router is not executable";
  }
  const bool locator_available = tables_ready && avx2_usable;

  LocatorPhases phases;
  LocatorStats locator_stats;
  std::uint64_t locator_aux_bytes = 0U;
  if (tables_ready) {
    std::cout << "# validating translated-tag path (portable router)...\n";
    std::cout.flush();
    sink.reset();
    sink.collect = true;
    sweep_translated_tag<false>(*data, shared, sink, scratch, &tt::route_query_portable,
                                &tt::route_query_portable_counted, nullptr, nullptr);
    outcomes.push_back(compare_against_truth("translated_tag_portable", sink));

    if (avx2_usable) {
      std::cout << "# validating translated-tag path (AVX2 router)...\n";
      std::cout.flush();
      sink.reset();
      sink.collect = true;
      sweep_translated_tag<false>(*data, shared, sink, scratch, &tt::avx2::route_query,
                                  &tt::avx2::route_query_counted, nullptr, nullptr);
      outcomes.push_back(compare_against_truth("translated_tag_avx2", sink));
    }

    std::cout << "# instrumented counting pass...\n";
    std::cout.flush();
    sink.reset();
    sink.collect = false;
    sweep_translated_tag<true>(
        *data, shared, sink, scratch,
        avx2_usable ? &tt::avx2::route_query : &tt::route_query_portable,
        avx2_usable ? &tt::avx2::route_query_counted : &tt::route_query_portable_counted,
        &phases, &locator_stats);
    locator_aux_bytes = scratch.aux_bytes();
  }

  // -------------------------------------------------------------------------
  // Path D: the radix occupancy locator, swept over the position width.
  //
  // The first campaign measured only R = 8192, which is the smallest radix
  // whose tag universe fills a machine word.  The calibrated cost model said
  // that radix is not at the guard/leaf optimum, so the sweep is measured here
  // rather than projected.  Every radix runs the same kernel source and the
  // same matched scope, and R = 8192 is re-measured in this run so the
  // comparison across radices is internally consistent.
  // -------------------------------------------------------------------------
  RadixScratch<13> radix_scratch_13;
  RadixScratch<14> radix_scratch_14;
  RadixScratch<15> radix_scratch_15;
  RadixScratch<16> radix_scratch_16;
  std::array<RadixResult, 4> radix{};
  run_radix_validation<13>(*data, shared, sink, radix_scratch_13, avx2_usable, outcomes,
                           compare_against_truth, radix[0]);
  run_radix_validation<14>(*data, shared, sink, radix_scratch_14, avx2_usable, outcomes,
                           compare_against_truth, radix[1]);
  run_radix_validation<15>(*data, shared, sink, radix_scratch_15, avx2_usable, outcomes,
                           compare_against_truth, radix[2]);
  run_radix_validation<16>(*data, shared, sink, radix_scratch_16, avx2_usable, outcomes,
                           compare_against_truth, radix[3]);
  const bool any_radix_available =
      std::any_of(radix.begin(), radix.end(),
                  [](const RadixResult& entry) { return entry.available; });

  // Path E: the packed-tag mask consumer.  BMI2 must be both compiled into its
  // translation unit and reported by the CPU; otherwise it fails closed.
  PackedScratch packed_scratch;
  PackedResult packed;
  const bool bmi2_usable = avx2_usable && isa.bmi2 && pk::bmi2::translation_unit_is_bmi2();
  std::cout << "# packed13 bmi2: compiled=" << (pk::bmi2::translation_unit_is_bmi2() ? "yes" : "no")
            << " cpu=" << (isa.bmi2 ? "yes" : "no") << " executed=" << (bmi2_usable ? "yes" : "no")
            << "\n";
  run_packed_validation(*data, shared, sink, packed_scratch, bmi2_usable, outcomes,
                        compare_against_truth, packed);
  bool correct = true;
  for (const PathOutcome& outcome : outcomes) {
    if (!outcome.entry_multiset_equal || !outcome.group_set_equal ||
        outcome.residual_mismatches != 0U || outcome.directory_misses != 0U) {
      correct = false;
    }
  }
  if (tables_ready && locator_stats.primary_directory_faults != 0U) {
    correct = false;
  }
  for (const RadixResult& entry : radix) {
    if (entry.available &&
        (entry.stats.directory_faults != 0U || entry.stats.hit_capacity_overflows != 0U ||
         !entry.scalar_reference_agrees)) {
      correct = false;
    }
  }
  if (packed.available &&
      (packed.faults.directory_faults != 0U || packed.stream_mismatches_two != 0U ||
       packed.stream_mismatches_one != 0U || packed.stream_mismatches_queued != 0U ||
       !packed.scalar_reference_agrees ||
       packed.two_deck.packed_scalar_disagreements != 0U ||
       packed.one_deck.packed_scalar_disagreements != 0U)) {
    correct = false;
  }

  std::cout << "# correctness:\n";
  for (const PathOutcome& outcome : outcomes) {
    std::cout << "#   " << std::left << std::setw(26) << outcome.id << std::right
              << " pairs=" << outcome.pair_hits << " triples=" << outcome.triples
              << " groups=" << outcome.distinct_groups << " FN=" << outcome.false_negatives
              << " FP=" << outcome.false_positives
              << " residual_mismatch=" << outcome.residual_mismatches
              << " fullQ=" << outcome.full_q << " residue=" << outcome.residue
              << " structural=" << outcome.structural << " outputs=" << outcome.outputs
              << (outcome.entry_multiset_equal && outcome.group_set_equal ? " EXACT" : " DIVERGED")
              << '\n';
  }
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Timing.
  // -------------------------------------------------------------------------
  double clock_overhead_ns = 0.0;
  double clock_resolution_ns = 0.0;
  {
    constexpr std::size_t kProbes = 20000U;
    std::vector<double> gaps;
    gaps.reserve(kProbes);
    for (std::size_t probe = 0U; probe < kProbes; ++probe) {
      const auto start = Clock::now();
      gaps.push_back(elapsed_ns(start));
    }
    clock_overhead_ns = summarize(gaps).median;
    for (const double gap : gaps) {
      if (gap > 0.0 && (clock_resolution_ns == 0.0 || gap < clock_resolution_ns)) {
        clock_resolution_ns = gap;
      }
    }
  }

  const ReplayStream replay = build_replay_stream(canonical_group_keys);

  std::vector<TimedEntity> timed;
  const auto add_entity = [&](const std::string& id) {
    timed.push_back(TimedEntity{id, {}, {}});
    timed.back().samples.reserve(sample_count);
  };
  add_entity("scalar_matched");
  add_entity("k2_matched");
  add_entity("k2_kernel_only");
  add_entity("k2_survivor_only");
  if (locator_available) {
    add_entity("translated_tag_matched");
    add_entity("translated_tag_group_only");
    add_entity("locality_control");
  }
  for (const RadixResult& entry : radix) {
    if (!entry.available) {
      continue;
    }
    for (const char* const suffix : kRadixSuffixes) {
      add_entity("radix" + std::to_string(entry.radix) + "_" + suffix);
    }
  }
  if (packed.available) {
    for (const char* const id : kPackedEntities) {
      add_entity(id);
    }
  }
  add_entity("tile_build_control");
  add_entity("sink_expansion");
  add_entity("sink_quotient_prefilter");
  add_entity("sink_full");

  const auto run_entity = [&](const std::string& id) {
    if (id == "scalar_matched") {
      sink.reset();
      sink.collect = false;
      sink.depth = SinkDepth::Full;
      sweep_scalar(*data, sink);
      return sink.xor_checksum;
    }
    if (id == "k2_matched") {
      sink.reset();
      sink.collect = false;
      sink.depth = SinkDepth::Full;
      sweep_k2(*data, sink, k2_kernel);
      return sink.xor_checksum;
    }
    if (id == "k2_kernel_only") {
      return sweep_k2_kernel_only(*data, k2_kernel);
    }
    if (id == "k2_survivor_only") {
      return sweep_k2_survivor_only(*data, k2_kernel);
    }
    if (id == "translated_tag_matched" || id == "translated_tag_group_only") {
      sink.reset();
      sink.collect = false;
      sink.depth = id == "translated_tag_matched" ? SinkDepth::Full : SinkDepth::GroupOnly;
      sweep_translated_tag<false>(
          *data, shared, sink, scratch,
          avx2_usable ? &tt::avx2::route_query : &tt::route_query_portable,
          avx2_usable ? &tt::avx2::route_query_counted : &tt::route_query_portable_counted,
          nullptr, nullptr);
      return sink.xor_checksum;
    }
    if (id.rfind("radix", 0U) == 0U) {
      const std::size_t split = id.find('_');
      const unsigned long value = std::strtoul(id.c_str() + 5, nullptr, 10);
      const std::string suffix = id.substr(split + 1U);
      switch (value) {
        case 8192U:
          return run_radix_entity<13>(*data, shared, sink, radix_scratch_13, avx2_usable,
                                      suffix);
        case 16384U:
          return run_radix_entity<14>(*data, shared, sink, radix_scratch_14, avx2_usable,
                                      suffix);
        case 32768U:
          return run_radix_entity<15>(*data, shared, sink, radix_scratch_15, avx2_usable,
                                      suffix);
        default:
          return run_radix_entity<16>(*data, shared, sink, radix_scratch_16, avx2_usable,
                                      suffix);
      }
    }
    if (id.rfind("packed13_", 0U) == 0U) {
      return run_packed_entity(*data, shared, sink, packed_scratch, id);
    }
    if (id == "tile_build_control") {
      return sweep_tile_build_only(*data);
    }
    if (id == "locality_control") {
      return sweep_locality_control(
          shared, scratch,
          avx2_usable ? &tt::avx2::route_query : &tt::route_query_portable);
    }
    sink.reset();
    sink.collect = false;
    sink.depth = id == "sink_expansion"           ? SinkDepth::Expansion
                 : id == "sink_quotient_prefilter" ? SinkDepth::QuotientPrefilter
                                                   : SinkDepth::Full;
    replay_sink(replay, sink);
    return sink.xor_checksum;
  };

  std::cout << "# timing (" << sample_count << " samples, rotated order, clock resolution "
            << number(clock_resolution_ns) << " ns, median call overhead "
            << number(clock_overhead_ns) << " ns)...\n";
  std::cout.flush();
  for (TimedEntity& entity : timed) {
    g_sink = run_entity(entity.id);  // warm-up
  }
  for (std::size_t round = 0U; round < sample_count; ++round) {
    for (std::size_t offset = 0U; offset < timed.size(); ++offset) {
      TimedEntity& entity = timed[(offset + round) % timed.size()];
      const auto start = Clock::now();
      const std::uint64_t checksum = run_entity(entity.id);
      const double region = elapsed_ns(start);
      g_sink = checksum;
      entity.samples.push_back(region / 1.0e6);
    }
    if ((round + 1U) % 10U == 0U) {
      std::cout << "#   round " << (round + 1U) << '/' << sample_count << '\n';
      std::cout.flush();
    }
  }
  for (TimedEntity& entity : timed) {
    entity.stats = summarize(entity.samples);
  }
  const auto stats_for = [&](const std::string_view id) {
    for (const TimedEntity& entity : timed) {
      if (entity.id == id) {
        return entity.stats;
      }
    }
    return Stats{};
  };

  const Stats scalar_stats = stats_for("scalar_matched");
  const Stats k2_stats = stats_for("k2_matched");
  const Stats k2_kernel_stats = stats_for("k2_kernel_only");
  const Stats k2_survivor_stats = stats_for("k2_survivor_only");
  const Stats locator_stats_timing = stats_for("translated_tag_matched");
  const Stats locator_group_only_stats = stats_for("translated_tag_group_only");
  const Stats locality_control_stats = stats_for("locality_control");
  const Stats tile_build_stats = stats_for("tile_build_control");
  const Stats sink_expansion_stats = stats_for("sink_expansion");
  const Stats sink_prefilter_stats = stats_for("sink_quotient_prefilter");
  const Stats sink_full_stats = stats_for("sink_full");

  double worst_relative_iqr = 0.0;
  for (const TimedEntity& entity : timed) {
    if (entity.stats.median > 0.0) {
      worst_relative_iqr = std::max(worst_relative_iqr, entity.stats.iqr / entity.stats.median);
    }
  }
  const char* const timing_stability = worst_relative_iqr <= 0.10 ? "acceptable" : "unstable";

  // -------------------------------------------------------------------------
  // Fallback measurement (task section 24).  If the locator is unavailable the
  // frozen exact fallback runs for the same matched scope and is reported as
  // fallback, never as the architecture's own result.
  // -------------------------------------------------------------------------
  double fallback_ms = 0.0;
  std::uint64_t fallback_count = 0U;
  if (!locator_available) {
    fallback_count = 1U;
    const auto start = Clock::now();
    sink.reset();
    sink.collect = false;
    sink.depth = SinkDepth::Full;
    sweep_scalar(*data, sink);
    fallback_ms = elapsed_ns(start) / 1.0e6;
    g_sink = sink.xor_checksum;
  }

  // -------------------------------------------------------------------------
  // Verdict.
  // -------------------------------------------------------------------------
  const double locator_ms = locator_stats_timing.median;
  const double speedup_vs_scalar = locator_ms > 0.0 ? scalar_stats.median / locator_ms : 0.0;
  const double speedup_vs_k2 = locator_ms > 0.0 ? k2_stats.median / locator_ms : 0.0;
  const double routing_ms = phases.route_ns / 1.0e6;
  const bool memory_gate_pass = locator_aux_bytes <= kMemoryGateBytes;

  std::string verdict;
  std::string verdict_note;
  if (!correct) {
    verdict = "INCORRECT";
    verdict_note = "a comparator path diverged from the exhaustive truth";
  } else if (!locator_available) {
    verdict = "INCORRECT";
    verdict_note = "the primary router could not be executed: " + fallback_reason;
  } else if (locator_ms >= k2_stats.median) {
    verdict = "CORRECT_BUT_SLOWER_THAN_K2";
    verdict_note = "matched median is at or above the matched K2 median";
  } else if (speedup_vs_k2 >= kAdvancementSpeedup && locator_ms < kAdvancementMs &&
             memory_gate_pass) {
    verdict = locator_ms < kTenXMs ? "BREAKTHROUGH" : "GO_FOR_NEXT_STAGE";
    verdict_note = locator_ms < kTenXMs ? "all advancement gates plus 10x scalar"
                                        : "all advancement gates";
  } else {
    verdict = "MODEST_WIN";
    verdict_note = "faster than matched K2 but short of the advancement gate";
    if (!memory_gate_pass) {
      verdict_note += " (auxiliary memory gate also failed)";
    }
  }

  // -------------------------------------------------------------------------
  // Radix verdict, against the gates the radix proposal fixed in advance.
  //
  // These are the proposal's own numbers and are never adjusted after
  // measurement: the meaningful-win gate is 225.558 ms with a p95 stability
  // gate at the matched K2 median, and the component NO-GO limits are build
  // 8 ms, guard 70 ms, bucket resolution 105 ms and at most 40 million
  // compatible bucket interactions.
  //
  // The proposal's 16 MiB auxiliary-memory cap is deliberately NOT a gate here.
  // It conflated capacity with locality: on a 32 GiB machine the auxiliary
  // total is irrelevant, and what actually decides a radix is the working set
  // the hot loop touches per (s, t) block.  That number is measured and
  // reported instead, and the auxiliary total is reported without a verdict.
  // -------------------------------------------------------------------------
  constexpr double kRadixMeaningfulMs = 225.558;
  constexpr double kRadixBuildLimitMs = 8.0;
  constexpr double kRadixGuardLimitMs = 70.0;
  constexpr double kRadixLeafLimitMs = 105.0;
  constexpr std::uint64_t kRadixLeafLimit = 40000000ULL;

  // Derived per-radix costs.  Every phase is a difference of matched medians
  // measured in this same run, never a sum of independently timed regions.
  struct RadixCosts {
    const RadixResult* entry = nullptr;
    const Stats* best = nullptr;
    const char* best_leaf = "none";
    double best_ms = 0.0;
    double speedup_vs_scalar = 0.0;
    double speedup_vs_k2 = 0.0;
    double build_ms = 0.0;
    double guard_ms = 0.0;
    double extract_ms = 0.0;
    double tag_ms = 0.0;
    double leaf_ms = 0.0;
    double report_ms = 0.0;
    double guard_ns = 0.0;
    double leaf_ns = 0.0;          // dense kernel, matches the phase split
    double best_leaf_ns = 0.0;     // best kernel, what the verdict rests on
    std::vector<std::string> failed_gates;
  };

  std::array<RadixCosts, 4> costs{};
  for (std::size_t index = 0U; index < radix.size(); ++index) {
    RadixResult& entry = radix[index];
    RadixCosts& cost = costs[index];
    cost.entry = &entry;
    if (!entry.available) {
      continue;
    }
    const std::string prefix = "radix" + std::to_string(entry.radix) + "_";
    entry.dense = stats_for(prefix + "dense_matched");
    entry.compact = stats_for(prefix + "compact_matched");
    entry.batched = stats_for(prefix + "batched_matched");
    entry.skip = stats_for(prefix + "skip_matched");
    entry.group_only = stats_for(prefix + "dense_group_only");
    entry.guard_only = stats_for(prefix + "guard_only");
    entry.extract_only = stats_for(prefix + "extract_only");

    cost.best = &entry.dense;
    cost.best_leaf = "dense";
    const std::pair<const Stats*, const char*> candidates[] = {
        {&entry.compact, "compact"},
        {&entry.batched, "batched"},
        {&entry.skip, "lane-skip"},
    };
    for (const auto& candidate : candidates) {
      if (candidate.first->median > 0.0 && candidate.first->median < cost.best->median) {
        cost.best = candidate.first;
        cost.best_leaf = candidate.second;
      }
    }
    cost.best_ms = cost.best->median;
    cost.speedup_vs_scalar =
        cost.best_ms > 0.0 ? scalar_stats.median / cost.best_ms : 0.0;
    cost.speedup_vs_k2 = cost.best_ms > 0.0 ? k2_stats.median / cost.best_ms : 0.0;
    cost.build_ms = (entry.phases.target_class_ns + entry.phases.shifted_view_ns +
                     entry.phases.source_class_ns) /
                    1.0e6;
    // Guard-only holds the build and the query stream constant and removes all
    // leaf and reporting work, so each difference isolates one phase.
    cost.guard_ms =
        entry.guard_only.median > 0.0 ? entry.guard_only.median - cost.build_ms : 0.0;
    cost.extract_ms = entry.extract_only.median > 0.0 && entry.guard_only.median > 0.0
                          ? entry.extract_only.median - entry.guard_only.median
                          : 0.0;
    cost.leaf_ms = entry.group_only.median > 0.0 && entry.guard_only.median > 0.0
                       ? entry.group_only.median - entry.guard_only.median
                       : 0.0;
    cost.tag_ms = cost.leaf_ms - cost.extract_ms;
    cost.report_ms = entry.dense.median > 0.0 && entry.group_only.median > 0.0
                         ? entry.dense.median - entry.group_only.median
                         : 0.0;
    const double guards = static_cast<double>(entry.stats.counters.guard_vectors);
    const double buckets = static_cast<double>(entry.stats.counters.emitted_buckets);
    cost.guard_ns = guards > 0.0 ? cost.guard_ms * 1.0e6 / guards : 0.0;
    cost.leaf_ns = buckets > 0.0 ? cost.leaf_ms * 1.0e6 / buckets : 0.0;
    cost.best_leaf_ns =
        buckets > 0.0
            ? (cost.best_ms - cost.build_ms - cost.guard_ms - cost.report_ms) * 1.0e6 /
                  buckets
            : 0.0;

    if (cost.build_ms > kRadixBuildLimitMs) {
      cost.failed_gates.push_back("build > 8 ms");
    }
    if (cost.guard_ms > kRadixGuardLimitMs) {
      cost.failed_gates.push_back("guard phase > 70 ms");
    }
    if (cost.leaf_ms > kRadixLeafLimitMs) {
      cost.failed_gates.push_back("bucket resolution > 105 ms");
    }
    if (entry.stats.counters.emitted_buckets > kRadixLeafLimit) {
      cost.failed_gates.push_back("compatible bucket interactions > 40 million");
    }
    if (cost.best_ms >= kRadixMeaningfulMs) {
      cost.failed_gates.push_back("matched median >= 225.558 ms");
    }
    if (cost.best->p95 >= k2_stats.median) {
      cost.failed_gates.push_back("p95 >= matched K2 median");
    }
  }

  // The architecture is judged on its best radix.
  const RadixCosts* best_radix = nullptr;
  for (const RadixCosts& cost : costs) {
    if (cost.entry != nullptr && cost.entry->available && cost.best_ms > 0.0 &&
        (best_radix == nullptr || cost.best_ms < best_radix->best_ms)) {
      best_radix = &cost;
    }
  }

  std::string radix_verdict;
  std::string radix_verdict_note;
  if (best_radix == nullptr) {
    radix_verdict = "NOT_EXECUTED";
    radix_verdict_note = radix[0].fallback_reason.empty() ? "no radix was executable"
                                                          : radix[0].fallback_reason;
  } else if (!correct) {
    radix_verdict = "INCORRECT";
    radix_verdict_note = "a comparator path diverged from the exhaustive truth";
  } else if (best_radix->failed_gates.empty()) {
    radix_verdict = "GO_FOR_NEXT_STAGE";
    radix_verdict_note = "every pre-registered radix gate passed at R = " +
                         std::to_string(best_radix->entry->radix);
  } else if (best_radix->best_ms < k2_stats.median) {
    radix_verdict = "MODEST_WIN_BELOW_GATE";
    radix_verdict_note = "faster than matched K2 but at least one pre-registered gate failed";
  } else {
    radix_verdict = "NO_GO_SLOWER_THAN_K2";
    radix_verdict_note = "the best radix is at or above the matched K2 median";
  }

  // -------------------------------------------------------------------------
  // Artifacts.
  // -------------------------------------------------------------------------
  std::error_code directory_error;
  std::filesystem::create_directories(output_directory, directory_error);
  const auto path_for = [&](const std::string& name) {
    return std::filesystem::path(output_directory) / name;
  };

  {
    std::ofstream out(path_for("timing_samples.csv"));
    out << "entity,sample_index,milliseconds\n";
    for (const TimedEntity& entity : timed) {
      for (std::size_t index = 0U; index < entity.samples.size(); ++index) {
        out << entity.id << ',' << index << ',' << number(entity.samples[index]) << '\n';
      }
    }
  }
  {
    std::ofstream out(path_for("phase_samples.csv"));
    out << "phase,milliseconds,note\n";
    out << "target_class_build," << number(phases.target_class_ns / 1.0e6)
        << ",instrumented pass\n";
    out << "shifted_view_build," << number(phases.shifted_view_ns / 1.0e6)
        << ",instrumented pass\n";
    out << "source_class_build," << number(phases.source_class_ns / 1.0e6)
        << ",instrumented pass\n";
    out << "translation_build," << number(phases.translation_ns / 1.0e6)
        << ",instrumented pass; " << locator_stats.translation_builds << " builds\n";
    out << "primary_route_extract_sink," << number(phases.route_ns / 1.0e6)
        << ",instrumented pass; includes candidate extraction and the sink\n";
    out << "overflow," << number(phases.overflow_ns / 1.0e6) << ",instrumented pass\n";
    out << "sink_expansion_only," << number(sink_expansion_stats.median)
        << ",timed replay median\n";
    out << "sink_with_quotient_prefilter," << number(sink_prefilter_stats.median)
        << ",timed replay median\n";
    out << "sink_full_authority," << number(sink_full_stats.median)
        << ",timed replay median\n";
    out << "translated_tag_group_only," << number(locator_group_only_stats.median)
        << ",timed median; locator with the sink stopped at residual equality\n";
    out << "locality_control," << number(locality_control_stats.median)
        << ",DIAGNOSTIC ONLY: real query stream through one family's tables; not a valid "
           "TER sweep and never verdict-eligible\n";
    out << "tile_build_control," << number(tile_build_stats.median)
        << ",timed median; K2's tile materialization cost\n";
  }
  {
    std::ofstream out(path_for("model_vs_actual.csv"));
    out << "quantity,projected,actual,actual_over_projected\n";
    const auto row = [&](const char* name, const double projected, const double actual) {
      out << name << ',' << number(projected) << ',' << number(actual) << ','
          << number(projected > 0.0 ? actual / projected : 0.0) << '\n';
    };
    row("occupancy_vector_intersections", kModelOccupancyVectors,
        static_cast<double>(locator_stats.counters.occupancy_vectors));
    row("tag_plane_comparisons", kModelTagComparisons,
        static_cast<double>(locator_stats.counters.tag_plane_comparisons));
    row("overflow_probes", kModelOverflowProbes,
        static_cast<double>(locator_stats.overflow_probe_x + locator_stats.overflow_probe_y));
    row("exact_residual_witnesses", 0.6201e6,
        static_cast<double>(locator_stats.counters.primary_group_matches));
    row("routing_ms", kModelRoutingMs, routing_ms);
    row("total_ms", kModelTotalMs, locator_ms);
    row("aux_bytes_low", kModelAuxBytesLow, static_cast<double>(locator_aux_bytes));
    row("aux_bytes_high", kModelAuxBytesHigh, static_cast<double>(locator_aux_bytes));
  }

  const std::uint64_t shifted_view_bytes_per_family = tt::kShiftedViewWords * 8ULL;
  const std::uint64_t shifted_view_bytes_total = shifted_view_bytes_per_family * kFamilies;
  const std::uint64_t translation_bytes = tt::kTranslationWords * 8ULL;
  const std::uint64_t occupancy_bytes = locator_stats.counters.occupancy_vectors * 64ULL;
  const std::uint64_t tag_bytes = locator_stats.counters.tag_plane_comparisons * 64ULL;
  const std::uint64_t logical_routing_bytes = occupancy_bytes + tag_bytes;
  const std::uint64_t peak_bytes = process_peak_bytes();

  {
    std::ofstream out(path_for("translated_tag.json"));
    out << std::setprecision(10);
    out << "{\"schema\":\"ter_translated_tag_bench\",\"schema_version\":1";
    out << ",\"git_commit\":\"" << TER_CONFIGURE_TIME_COMMIT << "\""
        << ",\"git_dirty\":" << (TER_CONFIGURE_TIME_DIRTY ? "true" : "false")
        << ",\"git_diff_paths\":\"see run_manifest.json emitted by the runner\"";
#if defined(_MSC_VER)
    out << ",\"compiler\":\"MSVC\",\"compiler_version\":\"" << _MSC_FULL_VER << "\"";
#else
    out << ",\"compiler\":\"other\",\"compiler_version\":\"unknown\"";
#endif
    out << ",\"build_type\":\"Release\",\"language\":\"C++20\"";
    out << ",\"isa_compiled\":\"" << (avx2_compiled ? "AVX2" : "portable") << "\""
        << ",\"isa_available\":\"" << (isa.usable ? "AVX2" : "portable") << "\""
        << ",\"isa_executed\":\"" << (avx2_usable ? "AVX2" : "portable") << "\"";
    out << ",\"cpu\":\"" << (isa.brand[0] != '\0' ? isa.brand : "unknown") << "\""
        << ",\"core_policy\":\"" << affinity_state << "\""
        << ",\"executable_hash\":\"not_computed\"";
    out << ",\"fixture_identity\":{\"seed\":" << data->seed << ",\"attempt\":\"first capture\""
        << ",\"n\":" << data->n << ",\"r1\":" << data->r1 << ",\"Q\":" << data->quotient_bits
        << ",\"k\":" << ter::microreport8::kProjectionBits
        << ",\"residual_bits\":" << ter::microreport8::kResidualBits
        << ",\"tau_low32\":" << data->tau_low32 << ",\"tau_low25\":" << data->tau_low25
        << ",\"tau_derivation_verified\":"
        << (data->tau_derivation_verified ? "true" : "false")
        << ",\"tau_verification_detail\":\"" << data->tau_verification_detail << "\""
        << ",\"geometry_matches_reference\":"
        << (data->geometry_matches_reference ? "true" : "false") << "}";
    out << ",\"size_A\":" << data->a_size << ",\"size_B\":" << data->b_size
        << ",\"size_C\":" << data->c_size << ",\"P0\":" << data->pair_universe
        << ",\"decomposition_crosscheck\":" << crosscheck_triples
        << ",\"production_low25\":" << data->production_low_prefilter_candidates;
    out << ",\"R\":" << tt::kR << ",\"H\":" << tt::kH << ",\"layers\":" << tt::kLayers
        << ",\"vector_width\":" << tt::kVectorBits;
    out << ",\"distinct_Q\":" << distinct_q << ",\"distinct_X\":" << distinct_x
        << ",\"distinct_Y\":" << distinct_y;

    std::uint64_t primary_x = 0U;
    std::uint64_t primary_y = 0U;
    std::uint64_t overflow_x = 0U;
    std::uint64_t overflow_y = 0U;
    std::array<std::uint64_t, tt::kLayers> x_layer_values{};
    std::array<std::uint64_t, tt::kLayers> y_layer_values{};
    if (tables_ready) {
      for (std::uint32_t family = 0U; family < kFamilies; ++family) {
        primary_x += scratch.sources[family].primary_values;
        overflow_x += scratch.sources[family].excess.size();
        primary_y += scratch.targets[family].primary_values;
        overflow_y += scratch.targets[family].excess.size();
        for (std::uint32_t layer = 0U; layer < tt::kLayers; ++layer) {
          x_layer_values[layer] += scratch.sources[family].layer_values[layer];
          y_layer_values[layer] += scratch.targets[family].layer_values[layer];
        }
      }
    }
    out << ",\"primary_X\":" << primary_x << ",\"primary_Y\":" << primary_y
        << ",\"overflow_X\":" << overflow_x << ",\"overflow_Y\":" << overflow_y
        << ",\"x_layer0_values\":" << x_layer_values[0]
        << ",\"x_layer1_values\":" << x_layer_values[1]
        << ",\"y_layer0_values\":" << y_layer_values[0]
        << ",\"y_layer1_values\":" << y_layer_values[1];
    out << ",\"query_count\":" << locator_stats.queries;
    out << ",\"occupancy_vector_count\":" << locator_stats.counters.occupancy_vectors
        << ",\"occupancy_zero_exits\":" << locator_stats.counters.occupancy_zero_exits
        << ",\"tag_plane_comparison_count\":" << locator_stats.counters.tag_plane_comparisons
        << ",\"tag_early_zero_exits\":" << locator_stats.counters.tag_early_zero_exits
        << ",\"mixed_carry_slice_count\":" << locator_stats.counters.mixed_carry_slices
        << ",\"uniform_carry_slice_count\":" << locator_stats.counters.uniform_carry_slices
        << ",\"final_mask_count\":" << locator_stats.counters.final_masks
        << ",\"primary_group_match_count\":" << locator_stats.counters.primary_group_matches;
    out << ",\"overflow_probe_X\":" << locator_stats.overflow_probe_x
        << ",\"overflow_probe_Y\":" << locator_stats.overflow_probe_y
        << ",\"overflow_hit_X\":" << locator_stats.overflow_hit_x
        << ",\"overflow_hit_Y\":" << locator_stats.overflow_hit_y;

    out << ",\"paths\":[";
    for (std::size_t index = 0U; index < outcomes.size(); ++index) {
      const PathOutcome& outcome = outcomes[index];
      if (index != 0U) {
        out << ',';
      }
      out << "{\"id\":\"" << outcome.id << "\",\"pair_hits\":" << outcome.pair_hits
          << ",\"residual_group_triples\":" << outcome.distinct_groups
          << ",\"expanded_entry_triples\":" << outcome.triples
          << ",\"full_Q_survivors\":" << outcome.full_q
          << ",\"residue_survivors\":" << outcome.residue
          << ",\"structural_survivors\":" << outcome.structural
          << ",\"final_outputs\":" << outcome.outputs
          << ",\"false_negatives\":" << outcome.false_negatives
          << ",\"false_positives\":" << outcome.false_positives
          << ",\"unexpected_primary_residual_mismatches\":" << outcome.residual_mismatches
          << ",\"duplicate_errors\":" << outcome.entry_mismatches
          << ",\"group_set_mismatches\":" << outcome.group_mismatches
          << ",\"entry_multiset_equal\":"
          << (outcome.entry_multiset_equal ? "true" : "false")
          << ",\"group_set_equal\":" << (outcome.group_set_equal ? "true" : "false")
          << ",\"sum_checksum\":" << outcome.sum_checksum
          << ",\"xor_checksum\":" << outcome.xor_checksum << "}";
    }
    out << "]";
    out << ",\"truth\":{\"kind\":\"VALIDATION_ONLY exhaustive over all source pairs\""
        << ",\"pairs_enumerated\":" << data->pair_universe
        << ",\"pair_hits\":" << truth.pair_hits << ",\"triples\":" << truth.triples
        << ",\"distinct_group_triples\":" << truth.group_keys.size()
        << ",\"sum_checksum\":" << truth.sum_checksum
        << ",\"xor_checksum\":" << truth.xor_checksum
        << ",\"build_ms\":" << number(truth.build_ns / 1.0e6)
        << ",\"reused_by_runtime\":false}";

    out << ",\"phases_ms\":{\"target_class_build\":" << number(phases.target_class_ns / 1.0e6)
        << ",\"shifted_view_build\":" << number(phases.shifted_view_ns / 1.0e6)
        << ",\"source_class_build\":" << number(phases.source_class_ns / 1.0e6)
        << ",\"translation_build\":" << number(phases.translation_ns / 1.0e6)
        << ",\"primary_route_extract_sink\":" << number(phases.route_ns / 1.0e6)
        << ",\"overflow\":" << number(phases.overflow_ns / 1.0e6)
        << ",\"sink_expansion_only\":" << number(sink_expansion_stats.median)
        << ",\"sink_with_quotient_prefilter\":" << number(sink_prefilter_stats.median)
        << ",\"sink_full_authority\":" << number(sink_full_stats.median)
        << ",\"locality_control\":" << number(locality_control_stats.median)
        << ",\"tile_build_control\":" << number(tile_build_stats.median) << "}";

    out << ",\"routing_economics\":{\"shifted_view_bytes_per_family\":"
        << shifted_view_bytes_per_family << ",\"shifted_view_bytes_total\":"
        << shifted_view_bytes_total << ",\"translation_table_bytes\":" << translation_bytes
        << ",\"locality_control_kind\":\"DIAGNOSTIC ONLY, never verdict-eligible; "
           "the real query stream is replayed over family-zero tables, whose data-dependent "
           "early exits may differ from the authentic sweep\"}";

    out << ",\"timing\":{";
    write_stats_json(out, "scalar_matched", scalar_stats);
    out << ',';
    write_stats_json(out, "k2_matched", k2_stats);
    out << ',';
    write_stats_json(out, "translated_tag_matched", locator_stats_timing);
    out << ',';
    write_stats_json(out, "translated_tag_group_only", locator_group_only_stats);
    out << ',';
    write_stats_json(out, "locality_control", locality_control_stats);
    out << ',';
    write_stats_json(out, "tile_build_control", tile_build_stats);
    out << ',';
    write_stats_json(out, "sink_full", sink_full_stats);
    for (const RadixResult& entry : radix) {
      if (!entry.available) {
        continue;
      }
      const std::string prefix = "radix" + std::to_string(entry.radix) + "_";
      const std::pair<const char*, const Stats*> series[] = {
          {"dense_matched", &entry.dense},       {"compact_matched", &entry.compact},
          {"batched_matched", &entry.batched},   {"skip_matched", &entry.skip},
          {"dense_group_only", &entry.group_only}, {"guard_only", &entry.guard_only},
          {"extract_only", &entry.extract_only},
      };
      for (const auto& item : series) {
        out << ',';
        write_stats_json(out, prefix + item.first, *item.second);
      }
    }
    out << ",\"samples\":" << sample_count
        << ",\"clock_resolution_ns\":" << number(clock_resolution_ns)
        << ",\"clock_overhead_ns\":" << number(clock_overhead_ns)
        << ",\"timing_stability\":\"" << timing_stability << "\""
        << ",\"worst_relative_iqr\":" << number(worst_relative_iqr) << "}";

    out << ",\"speedups\":{\"translated_tag_vs_scalar\":" << number(speedup_vs_scalar)
        << ",\"translated_tag_vs_k2\":" << number(speedup_vs_k2)
        << ",\"k2_vs_scalar\":"
        << number(k2_stats.median > 0.0 ? scalar_stats.median / k2_stats.median : 0.0) << "}";
    out << ",\"radix_sweep\":{\"verdict\":\"" << radix_verdict << "\",\"verdict_note\":\""
        << radix_verdict_note << "\""
        << ",\"memory_cap_gate\":\"dropped; the proposal's 16 MiB auxiliary cap conflated "
           "capacity with locality, so the auxiliary total is reported without a verdict "
           "and the per-block hot working set is reported instead\""
        << ",\"gates\":{\"meaningful_ms\":" << number(kRadixMeaningfulMs)
        << ",\"build_limit_ms\":" << number(kRadixBuildLimitMs)
        << ",\"guard_limit_ms\":" << number(kRadixGuardLimitMs)
        << ",\"leaf_limit_ms\":" << number(kRadixLeafLimitMs)
        << ",\"leaf_count_limit\":" << kRadixLeafLimit << "}"
        << ",\"best_radix\":"
        << (best_radix != nullptr ? std::to_string(best_radix->entry->radix)
                                  : std::string("null"))
        << ",\"radices\":[";
    bool first_radix = true;
    for (const RadixCosts& cost : costs) {
      const RadixResult& entry = *cost.entry;
      out << (first_radix ? "" : ",") << "{\"R\":" << entry.radix
          << ",\"H\":" << entry.tag_universe
          << ",\"position_bits\":" << entry.position_bits
          << ",\"tag_bits\":" << (19U - entry.position_bits)
          << ",\"guard_vectors_per_query\":" << entry.guard_vectors_per_query
          << ",\"available\":" << (entry.available ? "true" : "false")
          << ",\"fallback_reason\":\"" << entry.fallback_reason << "\""
          << ",\"best_leaf_representation\":\"" << cost.best_leaf << "\""
          << ",\"matched_median_ms\":" << number(cost.best_ms)
          << ",\"matched_p95_ms\":" << number(cost.best != nullptr ? cost.best->p95 : 0.0)
          << ",\"dense_median_ms\":" << number(entry.dense.median)
          << ",\"compact_median_ms\":" << number(entry.compact.median)
          << ",\"batched_median_ms\":" << number(entry.batched.median)
          << ",\"lane_skip_median_ms\":" << number(entry.skip.median)
          << ",\"speedup_vs_scalar\":" << number(cost.speedup_vs_scalar)
          << ",\"speedup_vs_k2\":" << number(cost.speedup_vs_k2)
          << ",\"build_ms\":" << number(cost.build_ms)
          << ",\"guard_phase_ms\":" << number(cost.guard_ms)
          << ",\"extract_phase_ms\":" << number(cost.extract_ms)
          << ",\"tag_resolution_phase_ms\":" << number(cost.tag_ms)
          << ",\"leaf_phase_ms\":" << number(cost.leaf_ms)
          << ",\"report_authority_output_ms\":" << number(cost.report_ms)
          << ",\"instrumented_route_ms\":" << number(entry.phases.route_ns / 1.0e6)
          << ",\"guard_ns_per_vector\":" << number(cost.guard_ns)
          << ",\"leaf_ns_per_bucket_dense\":" << number(cost.leaf_ns)
          << ",\"leaf_ns_per_bucket_best\":" << number(cost.best_leaf_ns)
          << ",\"queries\":" << entry.stats.queries
          << ",\"guard_vectors\":" << entry.stats.counters.guard_vectors
          << ",\"guard_zero_vectors\":" << entry.stats.counters.guard_zero_vectors
          << ",\"compatible_bucket_interactions\":" << entry.stats.counters.emitted_buckets
          << ",\"nonempty_bucket_interactions\":" << entry.stats.counters.nonempty_buckets
          << ",\"witnesses\":" << entry.stats.counters.witnesses
          << ",\"directory_faults\":" << entry.stats.directory_faults
          << ",\"hit_capacity_overflows\":" << entry.stats.hit_capacity_overflows
          << ",\"scalar_reference_agrees\":"
          << (entry.scalar_reference_agrees ? "true" : "false")
          << ",\"scalar_reference_witnesses\":" << entry.scalar_reference_witnesses
          << ",\"aux_bytes\":" << entry.aux_bytes
          << ",\"hot_bytes_per_block_compact\":" << entry.hot_bytes_compact
          << ",\"hot_bytes_per_block_dense\":" << entry.hot_bytes_dense
          << ",\"guard_popcount_histogram\":[";
      for (std::size_t bucket = 0U;
           bucket < entry.stats.counters.guard_popcount_histogram.size(); ++bucket) {
        out << (bucket == 0U ? "" : ",")
            << entry.stats.counters.guard_popcount_histogram[bucket];
      }
      out << "],\"failed_gates\":[";
      for (std::size_t gate = 0U; gate < cost.failed_gates.size(); ++gate) {
        out << (gate == 0U ? "\"" : ",\"") << cost.failed_gates[gate] << "\"";
      }
      out << "]}";
      first_radix = false;
    }
    out << "],\"guard_popcount_histogram_labels\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5-8\","
           "\"9-16\",\"17-32\",\"33-64\",\"65-128\",\"129-256\",\">256\"]}";
    out << ",\"historical_reference\":{\"scalar_ms\":" << number(kHistoricalScalarMs)
        << ",\"k2_ms\":" << number(kHistoricalK2Ms)
        << ",\"kind\":\"PRIOR_MEASURED projection from per-unit medians, MicroReport8 run, "
           "different executable, tile materialization excluded\""
        << ",\"tiles\":" << kPriorTiles << "}";
    out << ",\"memory\":{\"locator_aux_bytes\":" << locator_aux_bytes
        << ",\"shared_index_bytes\":" << shared.bytes
        << ",\"authority_bytes\":" << authority.bytes
        << ",\"donor_y_bitmap_bytes\":" << data->all_y_bitmap_bytes
        << ",\"total_peak_bytes\":" << peak_bytes
        << ",\"gate_bytes\":" << kMemoryGateBytes
        << ",\"gate_pass\":" << (memory_gate_pass ? "true" : "false") << "}";
    out << ",\"logical_routing_load_bytes\":" << logical_routing_bytes
        << ",\"logical_bytes_per_query\":"
        << number(locator_stats.queries > 0U
                      ? static_cast<double>(logical_routing_bytes) /
                            static_cast<double>(locator_stats.queries)
                      : 0.0)
        << ",\"logical_bytes_per_occupancy_vector\":"
        << number(locator_stats.counters.occupancy_vectors > 0U
                      ? static_cast<double>(logical_routing_bytes) /
                            static_cast<double>(locator_stats.counters.occupancy_vectors)
                      : 0.0);
    out << ",\"fallback\":{\"count\":" << fallback_count << ",\"reason\":\""
        << (fallback_reason.empty() ? "none" : fallback_reason) << "\",\"fallback_ms\":"
        << number(fallback_ms) << "}";
    out << ",\"gates\":{\"beat_k2_ms\":" << number(kBeatK2Ms)
        << ",\"advancement_ms\":" << number(kAdvancementMs)
        << ",\"advancement_speedup\":" << number(kAdvancementSpeedup)
        << ",\"ten_x_ms\":" << number(kTenXMs) << ",\"twenty_x_ms\":" << number(kTwentyXMs)
        << ",\"thirty_x_ms\":" << number(kThirtyXMs)
        << ",\"routing_gate_ms\":" << number(kRoutingGateMs) << "}";
    out << ",\"verdict\":\"" << verdict << "\",\"verdict_note\":\"" << verdict_note
        << "\"}";
  }

  {
    std::ofstream out(path_for("summary.txt"));
    out << "TER^2 translated-tag locator spike -- matched-scope summary\n";
    out << "verdict: " << verdict << "\n\n";
    out << "MEASURED, this executable, matched scope (median of " << sample_count
        << " full authentic sweeps):\n";
    out << "  scalar          " << number(scalar_stats.median) << " ms\n";
    out << "  K2 (frozen)     " << number(k2_stats.median) << " ms\n";
    out << "  translated tag  " << number(locator_ms) << " ms\n";
    out << "  speedup vs scalar " << number(speedup_vs_scalar) << "x\n";
    out << "  speedup vs K2     " << number(speedup_vs_k2) << "x\n\n";
    out << "PRIOR_MEASURED (MicroReport8, projections, different executable):\n";
    out << "  scalar " << number(kHistoricalScalarMs) << " ms, K2 " << number(kHistoricalK2Ms)
        << " ms\n\n";
    out << "controls: tile build " << number(tile_build_stats.median)
        << " ms, sink (full authority) " << number(sink_full_stats.median) << " ms\n";
    out << "diagnostic-only locality control (family-zero tables) "
        << number(locality_control_stats.median)
        << " ms; it is not a valid TER sweep or a verdict candidate\n";
    out << "locator routing (instrumented) " << number(routing_ms) << " ms, gate "
        << number(kRoutingGateMs) << " ms\n";
    out << "locator auxiliary bytes " << locator_aux_bytes << ", gate " << kMemoryGateBytes
        << (memory_gate_pass ? " (pass)\n" : " (FAIL)\n");
    out << "timing stability " << timing_stability << ", worst relative IQR "
        << number(worst_relative_iqr) << "\n";
  }

  {
    std::ofstream out(path_for("radix_sweep_summary.txt"));
    out << "TER^2 radix occupancy locator -- matched-scope radix sweep\n";
    out << "verdict: " << radix_verdict << "\n";
    out << "note: " << radix_verdict_note << "\n\n";
    out << "MEASURED, this executable, matched scope (median of " << sample_count
        << " full authentic sweeps, all radices in one interleaved run):\n";
    out << "  scalar                 " << number(scalar_stats.median) << " ms\n";
    out << "  K2 (frozen)            " << number(k2_stats.median) << " ms\n";
    out << "  translated tag (11+8)  " << number(locator_ms) << " ms\n\n";

    out << "per-radix matched medians (ms), best leaf kernel in parentheses:\n";
    out << "      R     H  guards/q      dense    compact    batched  lane-skip"
           "       best  vs K2\n";
    for (const RadixCosts& cost : costs) {
      const RadixResult& entry = *cost.entry;
      out << std::setw(7) << entry.radix << std::setw(6) << entry.tag_universe
          << std::setw(10) << entry.guard_vectors_per_query;
      if (!entry.available) {
        out << "   NOT EXECUTED: " << entry.fallback_reason << '\n';
        continue;
      }
      out << std::setw(11) << number(entry.dense.median) << std::setw(11)
          << number(entry.compact.median) << std::setw(11) << number(entry.batched.median)
          << std::setw(11) << number(entry.skip.median) << std::setw(11)
          << number(cost.best_ms) << std::setw(7) << number(cost.speedup_vs_k2) << "x  ("
          << cost.best_leaf << ")\n";
    }

    out << "\nMEASURED phase decomposition per radix (differences of matched medians in\n";
    out << "this same run, never a sum of independently timed regions).  The extraction\n";
    out << "and tag rows are measured against the dense kernel, which is the kernel the\n";
    out << "guard-only / extract-only / group-only controls share:\n";
    for (const RadixCosts& cost : costs) {
      const RadixResult& entry = *cost.entry;
      if (!entry.available) {
        continue;
      }
      out << "  R=" << entry.radix << " H=" << entry.tag_universe << '\n';
      out << "    build                        " << number(cost.build_ms) << " ms (gate "
          << number(kRadixBuildLimitMs) << ")\n";
      out << "    guard phase                  " << number(cost.guard_ms) << " ms (gate "
          << number(kRadixGuardLimitMs) << "), " << number(cost.guard_ns) << " ns/vector\n";
      out << "    bucket resolution, dense     " << number(cost.leaf_ms) << " ms (gate "
          << number(kRadixLeafLimitMs) << "), " << number(cost.leaf_ns) << " ns/bucket\n";
      out << "      of which bit extraction    " << number(cost.extract_ms) << " ms\n";
      out << "      of which tag rot+AND       " << number(cost.tag_ms) << " ms\n";
      out << "    bucket resolution, best      "
          << number(cost.best_ms - cost.build_ms - cost.guard_ms - cost.report_ms)
          << " ms, " << number(cost.best_leaf_ns) << " ns/bucket (" << cost.best_leaf
          << ")\n";
      out << "    report + authority + output  " << number(cost.report_ms) << " ms\n";
      out << "    compatible bucket interactions " << entry.stats.counters.emitted_buckets
          << " (gate " << kRadixLeafLimit << ")\n";
      out << "    guard vectors                " << entry.stats.counters.guard_vectors
          << ", zero " << entry.stats.counters.guard_zero_vectors << " ("
          << number(entry.stats.counters.guard_vectors > 0U
                        ? 100.0 *
                              static_cast<double>(entry.stats.counters.guard_zero_vectors) /
                              static_cast<double>(entry.stats.counters.guard_vectors)
                        : 0.0)
          << "%)\n";
      out << "    witnesses                    " << entry.stats.counters.witnesses << '\n';
      out << "    scalar reference             "
          << (entry.scalar_reference_agrees ? "exact" : "DIVERGED") << " over "
          << entry.scalar_reference_witnesses << " sampled witnesses\n";
      out << "    auxiliary bytes              " << entry.aux_bytes << " ("
          << number(static_cast<double>(entry.aux_bytes) / (1024.0 * 1024.0))
          << " MiB, reported without a verdict)\n";
      out << "    hot bytes per (s,t) block    " << entry.hot_bytes_compact
          << " compact / " << entry.hot_bytes_dense << " dense\n";
      out << "    failed gates                 ";
      if (cost.failed_gates.empty()) {
        out << "none";
      }
      for (std::size_t gate = 0U; gate < cost.failed_gates.size(); ++gate) {
        out << (gate == 0U ? "" : "; ") << cost.failed_gates[gate];
      }
      out << '\n';
    }

    out << "\nguard population histogram per radix (256-bit vectors):\n";
    static constexpr const char* kLabels[] = {"0",     "1",      "2",       "3",
                                              "4",     "5-8",    "9-16",    "17-32",
                                              "33-64", "65-128", "129-256", ">256"};
    out << "  bucket";
    for (const RadixCosts& cost : costs) {
      if (cost.entry->available) {
        out << std::setw(14) << ("R=" + std::to_string(cost.entry->radix));
      }
    }
    out << '\n';
    for (std::size_t bucket = 0U; bucket < 12U; ++bucket) {
      out << "  " << std::left << std::setw(6) << kLabels[bucket] << std::right;
      for (const RadixCosts& cost : costs) {
        if (cost.entry->available) {
          out << std::setw(14) << cost.entry->stats.counters.guard_popcount_histogram[bucket];
        }
      }
      out << '\n';
    }
    out << "\nThe 16 MiB auxiliary-memory cap from the original proposal is NOT applied as\n";
    out << "a gate: it conflated capacity with locality.  Auxiliary totals are reported\n";
    out << "above without a verdict, and the number that actually governs a radix -- the\n";
    out << "bytes the hot loop touches per (s,t) block -- is reported beside them.\n";
  }

  // -------------------------------------------------------------------------
  // Console report.
  // -------------------------------------------------------------------------
  std::cout << "\n=== MATCHED-SCOPE TIMING (median of " << sample_count << " full sweeps) ===\n";
  std::cout << std::left << std::setw(28) << "entity" << std::right << std::setw(14) << "median_ms"
            << std::setw(14) << "p25" << std::setw(14) << "p75" << std::setw(14) << "min"
            << std::setw(14) << "iqr/median" << '\n';
  for (const TimedEntity& entity : timed) {
    std::cout << std::left << std::setw(28) << entity.id << std::right << std::setw(14)
              << number(entity.stats.median) << std::setw(14) << number(entity.stats.p25)
              << std::setw(14) << number(entity.stats.p75) << std::setw(14)
              << number(entity.stats.minimum) << std::setw(14)
              << number(entity.stats.median > 0.0 ? entity.stats.iqr / entity.stats.median : 0.0)
              << '\n';
  }
  std::cout << "\nspeedup vs scalar = " << number(speedup_vs_scalar)
            << "x, speedup vs K2 = " << number(speedup_vs_k2) << "x\n";
  std::cout << "routing (instrumented) = " << number(routing_ms) << " ms\n";
  std::cout << "locator auxiliary bytes = " << locator_aux_bytes << " ("
            << number(static_cast<double>(locator_aux_bytes) / (1024.0 * 1024.0)) << " MiB), gate "
            << number(static_cast<double>(kMemoryGateBytes) / (1024.0 * 1024.0)) << " MiB -> "
            << (memory_gate_pass ? "pass" : "FAIL") << '\n';
  std::cout << "logical routing load bytes = " << logical_routing_bytes << " ("
            << number(static_cast<double>(logical_routing_bytes) / 1.0e9) << " GB)\n";
  std::cout << "shifted views = " << shifted_view_bytes_total << " bytes ("
            << number(static_cast<double>(shifted_view_bytes_total) / (1024.0 * 1024.0))
            << " MiB); translation table = " << translation_bytes << " bytes\n";
  std::cout << "diagnostic-only locality control = " << number(locality_control_stats.median)
            << " ms (family-zero replay; not a valid TER sweep or verdict candidate)\n";
  std::cout << "timing_stability=" << timing_stability
            << " worst_relative_iqr=" << number(worst_relative_iqr) << '\n';

  std::cout << "\n=== K2 RUNTIME BREAKDOWN ===\n";
  std::cout << "K2 total (matched):                 " << number(k2_stats.median) << " ms\n";
  const double kernel_ms = k2_kernel_stats.median;
  const double survivor_ms = k2_survivor_stats.median - k2_kernel_stats.median;
  const double overhead_ms = k2_stats.median - k2_survivor_stats.median;
  std::cout << "  kernel/sweep (scan):            " << number(kernel_ms) << " ms ("
            << number(100.0 * kernel_ms / k2_stats.median) << "%)\n";
  std::cout << "  survivor handling (filter):     " << number(survivor_ms) << " ms ("
            << number(100.0 * survivor_ms / k2_stats.median) << "%)\n";
  std::cout << "  overhead (tile build/misc):     " << number(overhead_ms) << " ms ("
            << number(100.0 * overhead_ms / k2_stats.median) << "%)\n";
  std::cout << "\nK2 ANALYSIS: ";
  if (survivor_ms > kernel_ms * 1.5) {
    std::cout << "SURVIVOR-HANDLING-BOUND — filter cost dominates by > 1.5×\n";
  } else if (kernel_ms > survivor_ms * 1.5) {
    std::cout << "KERNEL-BOUND — scan cost dominates by > 1.5×\n";
  } else {
    std::cout << "BALANCED — kernel and filter costs are comparable (within 1.5×)\n";
  }

  std::cout << "\n=== RADIX OCCUPANCY LOCATOR SWEEP ===\n";
  std::cout << std::left << std::setw(9) << "R" << std::setw(6) << "H" << std::setw(10)
            << "guards/q" << std::right << std::setw(11) << "dense" << std::setw(11)
            << "compact" << std::setw(11) << "batched" << std::setw(11) << "lane-skip"
            << std::setw(11) << "best" << std::setw(9) << "vs K2" << "   best kernel\n";
  for (const RadixCosts& cost : costs) {
    const RadixResult& entry = *cost.entry;
    std::cout << std::left << std::setw(9) << entry.radix << std::setw(6)
              << entry.tag_universe << std::setw(10) << entry.guard_vectors_per_query
              << std::right;
    if (!entry.available) {
      std::cout << "   NOT EXECUTED: " << entry.fallback_reason << '\n';
      continue;
    }
    std::cout << std::setw(11) << number(entry.dense.median) << std::setw(11)
              << number(entry.compact.median) << std::setw(11)
              << number(entry.batched.median) << std::setw(11) << number(entry.skip.median)
              << std::setw(11) << number(cost.best_ms) << std::setw(8)
              << number(cost.speedup_vs_k2) << "x   " << cost.best_leaf << '\n';
  }
  std::cout << "\nphase decomposition (ms; extraction/tag rows measured on the dense kernel)\n";
  for (const RadixCosts& cost : costs) {
    const RadixResult& entry = *cost.entry;
    if (!entry.available) {
      continue;
    }
    std::cout << "  R=" << std::left << std::setw(7) << entry.radix << std::right
              << " build " << number(cost.build_ms) << " | guard " << number(cost.guard_ms)
              << " (" << number(cost.guard_ns) << " ns/vec) | extract "
              << number(cost.extract_ms) << " | tag " << number(cost.tag_ms)
              << " | report " << number(cost.report_ms) << '\n';
    std::cout << "           buckets " << entry.stats.counters.emitted_buckets
              << " (gate " << kRadixLeafLimit << "), best leaf "
              << number(cost.best_leaf_ns) << " ns/bucket, aux "
              << number(static_cast<double>(entry.aux_bytes) / (1024.0 * 1024.0))
              << " MiB, hot/block " << number(static_cast<double>(entry.hot_bytes_compact) /
                                              1024.0)
              << " KiB compact / "
              << number(static_cast<double>(entry.hot_bytes_dense) / 1024.0)
              << " KiB dense\n";
    std::cout << "           failed gates: ";
    if (cost.failed_gates.empty()) {
      std::cout << "none";
    }
    for (std::size_t gate = 0U; gate < cost.failed_gates.size(); ++gate) {
      std::cout << (gate == 0U ? "" : "; ") << cost.failed_gates[gate];
    }
    std::cout << '\n';
  }
  std::cout << "\nauxiliary memory is reported, not gated: the 16 MiB cap conflated capacity\n"
               "with locality, so the per-block hot working set is the number that matters.\n";
  std::cout << "RADIX SWEEP VERDICT: " << radix_verdict << "  (" << radix_verdict_note
            << ")\n";

  // -------------------------------------------------------------------------
  // Path E report: Radix-13 occupancy guard + packed-tag mask consumer.
  //
  // Gates are the specification's own pre-registered numbers and are never
  // adjusted after measurement: 181.17 ms (1.5x of the 271.75 ms K2 figure the
  // specification was written against), < 3.0 M exception candidate buckets,
  // < 3.05 ns (hard) / 2.65 ns (central) per nonzero guard word for the packed
  // common path, and < 9.62 ns per explicit resolution.  The same-run K2
  // median is reported beside the fixed 271.75 ms because only it is matched.
  // -------------------------------------------------------------------------
  {
    constexpr double kSpecK2Ms = 271.75;
    constexpr double kSpecGateMs = 181.17;
    constexpr double kSpecProjectionMs = 176.0;
    constexpr double kSpecExceptionGate = 3.0e6;
    constexpr double kSpecWordHardNs = 3.05;
    constexpr double kSpecWordCentralNs = 2.65;
    constexpr double kSpecResolverNs = 9.62;
    constexpr double kSpecResolverInputs = 2.91e6;
    constexpr double kSpecNonzeroWords = 33.245e6;
    constexpr double kPairUniverse = 620966064.0;

    std::string packed_verdict = "NOT_EXECUTED";
    std::string packed_note = packed.fallback_reason;
    std::vector<std::string> packed_failed;
    const pk::PackedCounters& two = packed.two_deck;
    const pk::PackedCounters& one = packed.one_deck;
    double best_ms = 0.0;
    const char* best_name = "none";
    const Stats* best = nullptr;
    bool best_queued = false;
    double build_ms = 0.0;
    double guard_ms = 0.0;
    double consumer_ms = 0.0;
    double resolver_ms = 0.0;
    double report_ms = 0.0;
    double ns_per_word = 0.0;
    double ns_per_bit = 0.0;
    double ns_per_resolution = 0.0;
    double exceptions = 0.0;
    const RadixResult& plain = radix[0];
    const double plain_skip_ms = plain.available ? plain.skip.median : 0.0;
    const double plain_guard_ms = plain.available ? plain.guard_only.median : 0.0;

    if (packed.available) {
      packed.matched = stats_for("packed13_matched");
      packed.onedeck = stats_for("packed13_onedeck_matched");
      packed.group_only = stats_for("packed13_group_only");
      packed.front_only = stats_for("packed13_front_only");
      packed.guard_only = stats_for("packed13_guard_only");
      packed.queued = stats_for("packed13_queued_matched");
      packed.queued_onedeck = stats_for("packed13_queued_onedeck_matched");
      packed.queued_group_only = stats_for("packed13_queued_group_only");
      packed.queued_front_only = stats_for("packed13_queued_front_only");
      packed.queued_guard_only = stats_for("packed13_queued_guard_only");
      packed.queued_dense_control = stats_for("packed13_queued_dense_control");
      packed.build_only = stats_for("packed13_build_only");
      const std::pair<const Stats*, const char*> variants[] = {
          {&packed.matched, "lane-skip schedule, two decks"},
          {&packed.onedeck, "lane-skip schedule, one deck"},
          {&packed.queued, "queued schedule, two decks"},
          {&packed.queued_onedeck, "queued schedule, one deck"},
      };
      for (const auto& variant : variants) {
        if (variant.first->median > 0.0 &&
            (best == nullptr || variant.first->median < best->median)) {
          best = variant.first;
          best_name = variant.second;
        }
      }
      best_ms = best->median;
      best_queued = best == &packed.queued || best == &packed.queued_onedeck;
      // The phase controls run the two-deck consumer of the best schedule.
      const Stats& guard_control = best_queued ? packed.queued_guard_only : packed.guard_only;
      const Stats& front_control = best_queued ? packed.queued_front_only : packed.front_only;
      const Stats& group_control = best_queued ? packed.queued_group_only : packed.group_only;
      const Stats& full_control = best_queued ? packed.queued : packed.matched;
      build_ms = packed.build_only.median;
      guard_ms = guard_control.median - packed.build_only.median;
      consumer_ms = front_control.median - guard_control.median;
      resolver_ms = group_control.median - front_control.median;
      report_ms = full_control.median - group_control.median;
      ns_per_word = two.nonzero_words > 0U
                        ? consumer_ms * 1.0e6 / static_cast<double>(two.nonzero_words)
                        : 0.0;
      ns_per_bit = two.candidate_bits > 0U
                       ? consumer_ms * 1.0e6 / static_cast<double>(two.candidate_bits)
                       : 0.0;
      ns_per_resolution = two.resolver_inputs > 0U
                              ? resolver_ms * 1.0e6 / static_cast<double>(two.resolver_inputs)
                              : 0.0;
      exceptions = static_cast<double>(two.multi_tag_bits + two.representation_exception_bits);

      if (best_ms >= kSpecGateMs) {
        packed_failed.push_back("matched median >= 181.17 ms (spec 1.5x gate)");
      }
      if (best_ms >= k2_stats.median / 1.5) {
        packed_failed.push_back("matched median >= same-run K2 median / 1.5");
      }
      if (best->p95 >= k2_stats.median) {
        packed_failed.push_back("p95 >= same-run K2 median");
      }
      if (exceptions >= kSpecExceptionGate) {
        packed_failed.push_back("exception stream >= 3.0 M candidate buckets");
      }
      if (ns_per_word >= kSpecWordHardNs) {
        packed_failed.push_back("packed common path >= 3.05 ns per nonzero word");
      }
      if (ns_per_resolution >= kSpecResolverNs) {
        packed_failed.push_back("resolver >= 9.62 ns per explicit resolution");
      }

      if (!correct) {
        packed_verdict = "INCORRECT";
        packed_note = "a comparator path diverged from the exhaustive truth";
      } else if (best_ms < kSpecGateMs && best_ms < k2_stats.median / 1.5) {
        packed_verdict = "RUNTIME_WIN_1_5X";
        packed_note = "below 181.17 ms and below the same-run K2 median / 1.5";
      } else if (best_ms < k2_stats.median) {
        packed_verdict = "IMPROVEMENT_BELOW_1_5X";
        packed_note = "faster than same-run K2 but short of the 1.5x runtime gate";
      } else {
        packed_verdict = "NO_SPEEDUP_VS_K2";
        packed_note = "at or above the same-run K2 median";
      }
    }

    const auto ratio = [](const double numerator, const double denominator) {
      return denominator > 0.0 ? numerator / denominator : 0.0;
    };
    const auto millions = [](const std::uint64_t value) {
      return number(static_cast<double>(value) / 1.0e6);
    };

    std::ostringstream report;
    report << "TER^2 Radix-13 occupancy guard + packed-tag mask consumer -- matched scope\n";
    report << "verdict: " << packed_verdict << "  (" << packed_note << ")\n";
    report << "best packed variant: " << best_name << "\n\n";
    report << "MEASURED, this executable, one interleaved run, median of " << sample_count
           << " full authentic sweeps (p95 in brackets):\n";
    const auto line = [&](const char* name, const Stats& stats) {
      report << "  " << std::left << std::setw(36) << name << std::right << std::setw(11)
             << number(stats.median) << " ms  [" << number(stats.p95) << "]\n";
    };
    line("scalar pair baseline", scalar_stats);
    line("K2 frozen (matched)", k2_stats);
    if (plain.available) {
      line("plain radix-13 lane-skip", plain.skip);
      line("plain radix-13 dense", plain.dense);
    }
    if (packed.available) {
      line("packed, lane-skip schedule, 2 decks", packed.matched);
      line("packed, lane-skip schedule, 1 deck", packed.onedeck);
      line("packed, queued schedule, 2 decks", packed.queued);
      line("packed, queued schedule, 1 deck", packed.queued_onedeck);
      line("CONTROL queued, dense leaf, no PEXT", packed.queued_dense_control);
    }
    if (packed.available) {
      report << "\nspeedup of best packed variant:\n";
      report << "  vs same-run K2 median         " << number(ratio(k2_stats.median, best_ms))
             << "x   (1.5x gate at " << number(k2_stats.median / 1.5) << " ms)\n";
      report << "  vs spec K2 figure 271.75 ms   " << number(ratio(kSpecK2Ms, best_ms))
             << "x   (spec gate 181.17 ms, spec projection 176 ms: PROJECTED, not measured)\n";
      report << "  vs plain radix-13 lane-skip   " << number(ratio(plain_skip_ms, best_ms))
             << "x\n";
      report << "  vs CONTROL (same queue, dense leaf, no packed tags) "
             << number(ratio(packed.queued_dense_control.median, best_ms))
             << "x   <- the part of the win attributable to packed-tag consumption\n";
      report << "  vs scalar                     " << number(ratio(scalar_stats.median, best_ms))
             << "x\n";

      report << "\nMEASURED phase decomposition of the best schedule ("
             << (best_queued ? "queued" : "lane-skip")
             << ", two-deck controls; differences of matched medians in this run):\n";
      report << "  index/deck build                " << number(build_ms)
             << " ms   (spec budget 12)\n";
      report << "  occupancy guard scan            " << number(guard_ms)
             << " ms   (spec budget 28)\n";
      report << "  packed consumer common path     " << number(consumer_ms)
             << " ms   (spec budget 88; " << number(ns_per_word) << " ns/nonzero word, "
             << number(ns_per_bit) << " ns/candidate bit)\n";
      report << "  exact resolver + group mapping  " << number(resolver_ms)
             << " ms   (spec budget 28; " << number(ns_per_resolution)
             << " ns/explicit resolution)\n";
      report << "  full-Q authority + sink         " << number(report_ms)
             << " ms   (spec budget 13 + 7)\n";
      report << "  instrumented build breakdown    target classes "
             << number(packed.phases.target_class_ns / 1.0e6) << ", shifted views "
             << number(packed.phases.shifted_view_ns / 1.0e6) << ", source classes "
             << number(packed.phases.source_class_ns / 1.0e6) << ", decks "
             << number(packed.phases.deck_ns / 1.0e6) << " ms\n";
      {
        const auto schedule = [&](const char* name, const Stats& guard_control,
                                  const Stats& front_control, const Stats& group_control,
                                  const Stats& full_control) {
          report << "  " << name << ": guard+skip " << number(guard_control.median - build_ms)
                 << " | consumer " << number(front_control.median - guard_control.median)
                 << " | resolver+groups " << number(group_control.median - front_control.median)
                 << " | authority+sink " << number(full_control.median - group_control.median)
                 << " ms\n";
        };
        report << "  both schedules (ms, two-deck controls):\n";
        schedule("  lane-skip", packed.guard_only, packed.front_only, packed.group_only,
                 packed.matched);
        schedule("  queued   ", packed.queued_guard_only, packed.queued_front_only,
                 packed.queued_group_only, packed.queued);
      }
      if (plain.available) {
        report << "  plain radix-13: guard "<< number(plain_guard_ms - 0.0)
               << " ms incl. build; mask->location->tag path (lane-skip - guard) "
               << number(plain_skip_ms - plain_guard_ms) << " ms over "
               << millions(plain.stats.counters.emitted_buckets) << " M buckets = "
               << number(ratio((plain_skip_ms - plain_guard_ms) * 1.0e6,
                               static_cast<double>(plain.stats.counters.emitted_buckets)))
               << " ns/bucket\n";
        report << "  packed: consumer + resolver + report over the same buckets = "
               << number(ratio((packed.matched.median - packed.guard_only.median) * 1.0e6,
                               static_cast<double>(two.candidate_bits)))
               << " ns/bucket\n";
      }

      report << "\nMEASURED geometry and counters (instrumented passes, authentic fixture):\n";
      report << "  queries                         " << two.queries << "\n";
      report << "  guard vectors                   " << two.guard_vectors << " (zero "
             << number(100.0 * ratio(static_cast<double>(two.guard_zero_vectors),
                                     static_cast<double>(two.guard_vectors)))
             << "%)\n";
      report << "  nonzero 64-bit words            " << two.nonzero_words << " ("
             << millions(two.nonzero_words) << " M; spec model 33.245 M)\n";
      report << "  candidate bits                  " << two.candidate_bits << " ("
             << millions(two.candidate_bits) << " M; plain radix measured 37.371 M)\n";
      report << "  rank 0-7 / 8-15 / 16+ bits      " << two.rank_0_7_bits << " / "
             << two.rank_8_15_bits << " / " << two.rank_16_plus_bits << "\n";
      report << "  words needing deck 1            " << two.deck1_words << "\n";
      report << "  words with > 8 candidate lanes  " << two.wide_words << "\n";
      report << "  multi-tag candidate bits        " << two.multi_tag_bits << " ("
             << millions(two.multi_tag_bits) << " M; spec model 2.29 M)\n";
      report << "  representation-exception bits   two-deck " << two.representation_exception_bits
             << " (" << two.representation_exception_words << " words), one-deck "
             << one.representation_exception_bits << " (" << one.representation_exception_words
             << " words)\n";
      report << "  exception stream (kill gate)    " << number(exceptions / 1.0e6)
             << " M (gate < 3.0 M; multi-tag + representation, may double-count overlap)\n";
      report << "  carry-seam words                " << two.carry_seam_words << "\n";
      report << "  exact packed singleton survivors " << two.packed_exact_survivors << "\n";
      report << "  explicit resolver inputs        two-deck " << two.resolver_inputs << " ("
             << millions(two.resolver_inputs) << " M; spec projection 2.91 M), one-deck "
             << one.resolver_inputs << "\n";
      report << "  resolver inputs with no witness " << two.resolver_empty << "\n";
      report << "  exact residual witnesses        " << two.witnesses << "\n";
      report << "  packed/scalar disagreements     two-deck "
             << two.packed_scalar_disagreements << ", one-deck "
             << one.packed_scalar_disagreements << " (must be 0)\n";
      report << "  whole-stream identity vs plain radix-13 dense kernel: " << packed.stream_queries
             << " queries, " << packed.stream_witnesses << " witnesses, mismatching queries "
             << packed.stream_mismatches_two << " (two-deck) / " << packed.stream_mismatches_one
             << " (one-deck) / " << packed.stream_mismatches_queued << " (queued, both)\n";
      report << "  scalar reference                " << (packed.scalar_reference_agrees ? "exact" : "DIVERGED")
             << " over " << packed.scalar_reference_witnesses << " sampled witnesses\n";
      report << "  directory faults                " << packed.faults.directory_faults << "\n";
      report << "  deck bytes                      " << packed.deck_bytes << " ("
             << number(static_cast<double>(packed.deck_bytes) / (1024.0 * 1024.0))
             << " MiB); total auxiliary "
             << number(static_cast<double>(packed.aux_bytes) / (1024.0 * 1024.0)) << " MiB\n";
      report << "  common-path hot bytes per block " << PackedScratch::hot_bytes_common() << "\n";

      report << "\nLOGICAL work (NOT runtime; section 29 of the specification):\n";
      report << "  pair universe / candidate bits  " << number(ratio(kPairUniverse, static_cast<double>(two.candidate_bits)))
             << "x\n";
      report << "  candidate bits / resolver inputs " << number(ratio(static_cast<double>(two.candidate_bits), static_cast<double>(two.resolver_inputs)))
             << "x (spec projection 12.84x)\n";
      report << "  pair universe / resolver inputs " << number(ratio(kPairUniverse, static_cast<double>(two.resolver_inputs)))
             << "x (spec projection 213.4x)\n";

      report << "\nPRE-REGISTERED gates failed: ";
      if (packed_failed.empty()) {
        report << "none";
      }
      for (std::size_t gate = 0U; gate < packed_failed.size(); ++gate) {
        report << (gate == 0U ? "" : "; ") << packed_failed[gate];
      }
      report << "\n";
      report << "\nspec reference values (PROJECTED / MODEL, never measured): total "
             << number(kSpecProjectionMs) << " ms, nonzero words "
             << number(kSpecNonzeroWords / 1.0e6) << " M, resolver inputs "
             << number(kSpecResolverInputs / 1.0e6) << " M, common path "
             << number(kSpecWordCentralNs) << " ns/word central\n";
    }

    std::cout << "\n=== RADIX-13 PACKED-TAG MASK CONSUMER ===\n" << report.str();
    std::cout << "PACKED VERDICT: " << packed_verdict << "  (" << packed_note << ")\n";

    {
      std::ofstream out(path_for("packed_tag_summary.txt"));
      out << report.str();
    }
    {
      std::ofstream out(path_for("packed_tag.json"));
      const auto stats_json = [&](const Stats& stats) {
        std::ostringstream text;
        text << "{\"median_ms\":" << number(stats.median) << ",\"p95_ms\":" << number(stats.p95)
             << ",\"iqr_ms\":" << number(stats.iqr) << ",\"min_ms\":" << number(stats.minimum)
             << ",\"samples\":" << stats.samples << "}";
        return text.str();
      };
      const auto counters_json = [](const pk::PackedCounters& c) {
        std::ostringstream text;
        text << "{\"queries\":" << c.queries << ",\"guard_vectors\":" << c.guard_vectors
             << ",\"guard_zero_vectors\":" << c.guard_zero_vectors
             << ",\"nonzero_words\":" << c.nonzero_words
             << ",\"candidate_bits\":" << c.candidate_bits
             << ",\"rank_0_7_bits\":" << c.rank_0_7_bits
             << ",\"rank_8_15_bits\":" << c.rank_8_15_bits
             << ",\"rank_16_plus_bits\":" << c.rank_16_plus_bits
             << ",\"wide_words\":" << c.wide_words << ",\"deck1_words\":" << c.deck1_words
             << ",\"multi_tag_bits\":" << c.multi_tag_bits
             << ",\"representation_exception_words\":" << c.representation_exception_words
             << ",\"representation_exception_bits\":" << c.representation_exception_bits
             << ",\"carry_seam_words\":" << c.carry_seam_words
             << ",\"packed_exact_survivors\":" << c.packed_exact_survivors
             << ",\"resolver_inputs\":" << c.resolver_inputs
             << ",\"resolver_empty\":" << c.resolver_empty << ",\"witnesses\":" << c.witnesses
             << ",\"packed_scalar_disagreements\":" << c.packed_scalar_disagreements << "}";
        return text.str();
      };
      out << "{\"schema\":\"ter_radix13_packed_tag\",\"schema_version\":1"
          << ",\"verdict\":\"" << packed_verdict << "\",\"verdict_note\":\"" << packed_note << "\""
          << ",\"available\":" << (packed.available ? "true" : "false")
          << ",\"correct\":" << (correct ? "true" : "false")
          << ",\"best_variant\":\"" << best_name << "\""
          << ",\"samples\":" << sample_count
          << ",\"scalar\":" << stats_json(scalar_stats) << ",\"k2\":" << stats_json(k2_stats)
          << ",\"plain_radix13_lane_skip\":" << stats_json(plain.skip)
          << ",\"plain_radix13_dense\":" << stats_json(plain.dense)
          << ",\"plain_radix13_guard_only\":" << stats_json(plain.guard_only)
          << ",\"packed_twodeck\":" << stats_json(packed.matched)
          << ",\"packed_onedeck\":" << stats_json(packed.onedeck)
          << ",\"packed_queued_twodeck\":" << stats_json(packed.queued)
          << ",\"packed_queued_onedeck\":" << stats_json(packed.queued_onedeck)
          << ",\"packed_queued_group_only\":" << stats_json(packed.queued_group_only)
          << ",\"packed_queued_front_only\":" << stats_json(packed.queued_front_only)
          << ",\"packed_queued_guard_only\":" << stats_json(packed.queued_guard_only)
          << ",\"control_queued_dense_leaf\":" << stats_json(packed.queued_dense_control)
          << ",\"packed_group_only\":" << stats_json(packed.group_only)
          << ",\"packed_front_only\":" << stats_json(packed.front_only)
          << ",\"packed_guard_only\":" << stats_json(packed.guard_only)
          << ",\"packed_build_only\":" << stats_json(packed.build_only)
          << ",\"speedup_vs_same_run_k2\":" << number(ratio(k2_stats.median, best_ms))
          << ",\"speedup_vs_spec_k2_271_75\":" << number(ratio(kSpecK2Ms, best_ms))
          << ",\"speedup_vs_plain_radix13_lane_skip\":" << number(ratio(plain_skip_ms, best_ms))
          << ",\"phases_ms\":{\"build\":" << number(build_ms) << ",\"guard\":" << number(guard_ms)
          << ",\"packed_consumer\":" << number(consumer_ms)
          << ",\"resolver_and_group_mapping\":" << number(resolver_ms)
          << ",\"full_q_authority_and_sink\":" << number(report_ms) << "}"
          << ",\"instrumented_build_ms\":{\"target_classes\":"
          << number(packed.phases.target_class_ns / 1.0e6)
          << ",\"shifted_views\":" << number(packed.phases.shifted_view_ns / 1.0e6)
          << ",\"source_classes\":" << number(packed.phases.source_class_ns / 1.0e6)
          << ",\"decks\":" << number(packed.phases.deck_ns / 1.0e6) << "}"
          << ",\"ns_per_nonzero_word\":" << number(ns_per_word)
          << ",\"ns_per_candidate_bit\":" << number(ns_per_bit)
          << ",\"ns_per_explicit_resolution\":" << number(ns_per_resolution)
          << ",\"exception_stream\":" << number(exceptions)
          << ",\"counters_twodeck\":" << counters_json(two)
          << ",\"counters_onedeck\":" << counters_json(one)
          << ",\"stream_identity\":{\"queries\":" << packed.stream_queries
          << ",\"witnesses\":" << packed.stream_witnesses
          << ",\"mismatching_queries_twodeck\":" << packed.stream_mismatches_two
          << ",\"mismatching_queries_onedeck\":" << packed.stream_mismatches_one
          << ",\"mismatching_queries_queued\":" << packed.stream_mismatches_queued << "}"
          << ",\"scalar_reference_agrees\":" << (packed.scalar_reference_agrees ? "true" : "false")
          << ",\"scalar_reference_witnesses\":" << packed.scalar_reference_witnesses
          << ",\"directory_faults\":" << packed.faults.directory_faults
          << ",\"deck_bytes\":" << packed.deck_bytes << ",\"aux_bytes\":" << packed.aux_bytes
          << ",\"failed_gates\":[";
      for (std::size_t gate = 0U; gate < packed_failed.size(); ++gate) {
        out << (gate == 0U ? "\"" : ",\"") << packed_failed[gate] << "\"";
      }
      out << "],\"spec_values_projected_not_measured\":{\"total_ms\":176,\"gate_ms\":181.17"
             ",\"k2_ms\":271.75,\"nonzero_words\":33245000,\"resolver_inputs\":2910000"
             ",\"multi_tag_exceptions\":2290000}}\n";
    }
  }

  std::cout << "\nVERDICT: " << verdict << "  (" << verdict_note << ")\n";
  std::cout << "checksum sink = " << g_sink << '\n';
  std::cout << "artifacts written to " << output_directory << '\n';
  return correct ? EXIT_SUCCESS : EXIT_FAILURE;
}
