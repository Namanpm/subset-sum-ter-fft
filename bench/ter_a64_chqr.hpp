#pragma once

// A64-CHQR: adaptive 64-bit carry-aware hierarchical quotient refinement.
//
// Specification: TER_n108_A64_CHQR_Compiler_God_Spec.pdf.  Benchmark surface
// only; production solve_full_ter semantics are untouched.
//
// Root equation over the full quotient width Q (= n - r1 <= 69):
//     q_A + q_B == d (mod 2^Q),   d = tau - q_C.
// Fast key: low F = min(Q, 64) bits; the low 6 bits select the (s, t, u)
// microproblem exactly as the 25-bit queue does, leaving a W = F - 6 bit
// residual.  Refinement consumes the residual low-digit first with the exact
// carry recurrence (spec 10.3); a branch is pruned only when Y lacks the one
// digit the exact incoming carry requires.  Every surviving (q, x, y) is then
// checked on all W residual bits and, when Q > 64, on the high Q - 64 bits
// using the carry out of the low-F addition (spec 9.2).  Output keeps EntryId
// multiplicity: nothing is deduplicated.
//
// Index (spec 12): per microproblem, each family is one flat array sorted by a
// digit-permuted key, so every trie node is a contiguous range and its
// children are consecutive runs.  Level 0 also gets a dense offset directory.
// No 2^58 or 2^64 structure exists anywhere.
//
// K2_WIDE is the matched control: the frozen K2 tile kernel and its exact
// 19-bit survivor test, followed by the same full-key check before the sink.
// It isolates what the trie buys over simply widening K2's final filter.

#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ter::a64 {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace tt = ter::translated_tag;

inline constexpr std::uint32_t kMaxLevels = 8U;

struct DigitProfile {
  std::uint32_t residual_bits = 0;              // refined residual bits: k - 6 <= W
  std::uint32_t levels = 0;
  std::array<std::uint32_t, kMaxLevels> widths{};  // low digit first
};

// Greedy digits over `residual_bits`, remainder last (58 -> 13,13,13,13,6).
[[nodiscard]] DigitProfile default_profile(std::uint32_t residual_bits, std::uint32_t digit_bits = 13U);

// Family-aligned full keys, built once per workload (shared input, untimed):
// for list L and family f, key[L][f][i] is the W-bit residual of posting i of
// the donor family and hi[L][f][i] its bits above 64.
struct FamilyKeys {
  std::uint32_t full_bits = 0;  // Q
  std::uint32_t fast_bits = 0;  // F
  std::uint32_t residual_bits = 0;  // W
  std::uint32_t hi_bits = 0;
  std::array<std::array<std::vector<std::uint64_t>, rg::kFamilies>, 3> key;
  std::array<std::array<std::vector<std::uint8_t>, rg::kFamilies>, 3> hi;
  bool ok = false;
};

[[nodiscard]] FamilyKeys build_family_keys(const rg::Workload& workload);

struct Telemetry {
  std::array<std::uint64_t, kMaxLevels + 1U> tasks{};  // node triples entered per level
  std::uint64_t digit_pairs = 0;     // (q run, x run) digit combinations examined
  std::uint64_t direct_pairs = 0;    // (q, x) entry pairs resolved directly at leaves
  std::uint64_t logical_pairs = 0;   // sum over microproblems of |Q_s| * |X_t|
  std::uint64_t projected_hits = 0;  // (q, x, y) equal on the 6 + refined bits (width k)
  std::uint64_t fast_hits = 0;       // ... equal on all F low bits
  std::uint64_t full_hits = 0;       // ... and on the high Q - F bits: emitted
  std::uint64_t index_bytes = 0;     // sorted family arrays built by this call
  std::array<std::uint64_t, 65> sweep{};  // K2_WIDE: [c] = 25-bit hits with exactly c
                                          // common low residual bits (c = W: all)
};

struct Config {
  DigitProfile profile;                    // refined width k = 6 + profile.residual_bits
  std::uint64_t direct_threshold = 128U;  // resolve directly when |Q| * |X| <= this
};

// Emitted triples go straight to the sink's exact authority chain.
void run_a64(const mr8::Dataset& data, const rg::RootIndex& index, const FamilyKeys& keys,
             const Config& config, rg::RootSink& sink, Telemetry* telemetry);

// sweep = true additionally fills Telemetry::sweep (the spec 11.2 width sweep).
// work_divisor > 1 runs only the first ceil(x_blocks / d) X blocks of each
// microproblem: a cost probe for a dispatcher, never a result.
void run_k2_wide(const mr8::Dataset& data, const rg::RootIndex& index, const FamilyKeys& keys,
                 rg::RootSink& sink, Telemetry* telemetry, bool sweep,
                 std::uint32_t work_divisor = 1U);

}  // namespace ter::a64
