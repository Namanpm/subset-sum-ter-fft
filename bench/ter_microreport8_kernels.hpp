#pragma once

// TER^2 MicroReport8 research spike: leaf-reporter kernels and truth oracle.
//
// SCOPE.  This header is benchmark/test surface only.  Nothing here is linked
// into ter_t0, into src/full_ter.cpp, or into any production reporter.  It
// exists to answer one question: can a genuinely packed, zero-false-negative
// 8x8 reporter process authentic n=72 TER^2 residual tiles cheaply enough to
// justify building an architecture around it?
//
// PROBLEM.  After the exact low-25 TER^2 decomposition (see
// ter_microreport8_dataset.hpp) each microproblem is
//
//     x + q + kappa  ==  y   (mod 2^19),   x in X_t, q in Q_s, y in Y_u.
//
// A tile is at most 8 x-values by 8 q-values.  Lane (i, j) occupies bit 8i+j.
//
// CONTRACT.  Every reporter returns a 64-bit candidate mask M satisfying
//
//     (1)  T & ~M == 0        zero false negatives (T is the exact truth mask)
//     (2)  M & ~V == 0        no reporting on padding lanes (V is the valid mask)
//
// False positives are legal and are charged explicitly by the probe.
//
// ISA.  The packed kernels are AVX2 and live in their own translation unit
// (ter_microreport8_kernels_avx2.cpp) compiled with the AVX2 option.  This
// header, the oracle, the portable fallbacks and the dispatch stay in ordinary
// portable compilation so the rest of the executable cannot silently acquire
// AVX2 instruction selection.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ter::microreport8 {

// Residual geometry fixed by the k=25 research projection: 25 = 6 + 19.
inline constexpr std::uint32_t kSplitBits = 6U;
inline constexpr std::uint32_t kSplitRadix = 1U << kSplitBits;  // 64 families
inline constexpr std::uint32_t kResidualBits = 19U;
inline constexpr std::uint32_t kResidualUniverse = 1U << kResidualBits;  // 524288
inline constexpr std::uint32_t kResidualMask = kResidualUniverse - 1U;

// Exact membership bitmap: 2^19 bits held as 32-bit words so an AVX2 gather can
// address it directly.  16384 words = 64 KiB per active Y_u.
inline constexpr std::size_t kExactWords = kResidualUniverse / 32U;
// Coarse (block-of-8) superset bitmap: 2^16 bits = 2048 words = 8 KiB.
inline constexpr std::uint32_t kCoarseShift = 3U;
inline constexpr std::size_t kCoarseWords = (kResidualUniverse >> kCoarseShift) / 32U;

inline constexpr std::size_t kTileDim = 8U;
inline constexpr std::size_t kTileLanes = kTileDim * kTileDim;

// One 8x8 residual microproblem tile.  Padding lanes hold zero but are excluded
// by the valid mask; a reporter must never set them.
struct Tile {
  std::uint32_t x[kTileDim];  // residual quotients from B, ascending
  std::uint32_t q[kTileDim];  // residual quotients from A, ascending
  std::uint8_t x_count;       // 1..8
  std::uint8_t q_count;       // 1..8
  std::uint8_t kappa;         // 0 or 1
  std::uint8_t u;             // which Y family this tile reports against
};
static_assert(sizeof(Tile) == 68U);

// Byte i has its low bit set for each of the first i rows, so a row mask times
// this value replicates the row across the valid rows with no inter-byte carry.
inline constexpr std::array<std::uint64_t, 9> kRowSpread{
    0x0000000000000000ULL, 0x0000000000000001ULL, 0x0000000000000101ULL,
    0x0000000000010101ULL, 0x0000000001010101ULL, 0x0000000101010101ULL,
    0x0000010101010101ULL, 0x0001010101010101ULL, 0x0101010101010101ULL,
};

[[nodiscard]] inline std::uint64_t valid_mask(const std::uint32_t x_count,
                                              const std::uint32_t q_count) noexcept {
  const std::uint64_t row = (std::uint64_t{1} << q_count) - 1U;
  return row * kRowSpread[x_count];
}

[[nodiscard]] inline std::uint64_t tile_valid_mask(const Tile& tile) noexcept {
  return valid_mask(tile.x_count, tile.q_count);
}

// Target representations for one Y_u family.  All are built once, before any
// timing, and retained for the whole run (see the probe's bitmap build report).
struct TargetSet {
  std::vector<std::uint32_t> exact;   // 64 KiB, exact membership over 2^19
  std::vector<std::uint32_t> coarse;  // 8 KiB, block-of-8 superset
  std::array<std::uint8_t, 16> sig07{};   // byte-signature, high nibbles 0..7
  std::array<std::uint8_t, 16> sig815{};  // byte-signature, high nibbles 8..15
  std::vector<std::uint32_t> values;      // postings, multiplicity preserved
  std::vector<std::uint32_t> distinct;    // distinct residual values, ascending
};

// Flat pointer view handed to the kernels so a hot loop performs one indexed
// load rather than walking std::vector indirections.
struct TargetView {
  const std::uint32_t* exact[kSplitRadix]{};
  const std::uint32_t* coarse[kSplitRadix]{};
  const std::uint8_t* sig07[kSplitRadix]{};
  const std::uint8_t* sig815[kSplitRadix]{};
};

[[nodiscard]] inline bool exact_contains(const std::uint32_t* const exact,
                                         const std::uint32_t value) noexcept {
  return ((exact[value >> 5U] >> (value & 31U)) & 1U) != 0U;
}

// ---------------------------------------------------------------------------
// Truth oracle.  Deliberately obvious: two nested loops, one exact membership
// test per lane.  Auditability beats speed here; this is never a candidate.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint64_t scalar_report8(const Tile& tile,
                                                  const TargetView& view) noexcept {
  const std::uint32_t* const exact = view.exact[tile.u];
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
    for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
      const std::uint32_t sum = (tile.x[i] + tile.q[j] + tile.kappa) & kResidualMask;
      if (exact_contains(exact, sum)) {
        mask |= std::uint64_t{1} << (8U * i + j);
      }
    }
  }
  return mask;
}

// Independent brute-force validator.  It shares no code with scalar_report8:
// it never touches a bitmap and decides membership by linear search over the
// raw posting list, so a defect in bitmap construction cannot hide in both.
[[nodiscard]] inline std::uint64_t brute_report8(const Tile& tile,
                                                 const std::uint32_t* const values,
                                                 const std::size_t value_count) noexcept {
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
    for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
      const std::uint64_t wide =
          static_cast<std::uint64_t>(tile.x[i]) + tile.q[j] + tile.kappa;
      const std::uint32_t sum = static_cast<std::uint32_t>(wide % kResidualUniverse);
      for (std::size_t index = 0U; index < value_count; ++index) {
        if (values[index] == sum) {
          mask |= std::uint64_t{1} << (8U * i + j);
          break;
        }
      }
    }
  }
  return mask;
}

// ---------------------------------------------------------------------------
// Portable reference implementations of the four kernels.
//
// These define each kernel's *semantics*.  The AVX2 translation unit must
// reproduce them bit-for-bit; the probe and the tests check that whenever AVX2
// is executable.  They are not the packed candidates and never bear a verdict.
// ---------------------------------------------------------------------------

// K0 semantics: materialize the 64 sums, emit a mask from a predicate that is
// true for every representable residual.  SUM-MATERIALIZATION FLOOR CONTROL.
[[nodiscard]] inline std::uint64_t k0_sum_floor_portable(const Tile& tile,
                                                         const TargetView&) noexcept {
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    for (std::uint32_t j = 0U; j < kTileDim; ++j) {
      const std::uint32_t sum = (tile.x[i] + tile.q[j] + tile.kappa) & kResidualMask;
      if (sum <= kResidualMask) {
        mask |= std::uint64_t{1} << (8U * i + j);
      }
    }
  }
  return mask & tile_valid_mask(tile);
}

// K1 semantics: exact membership in the 2^19-bit bitmap. Zero false positives.
[[nodiscard]] inline std::uint64_t k1_exact_gather_portable(const Tile& tile,
                                                            const TargetView& view) noexcept {
  const std::uint32_t* const exact = view.exact[tile.u];
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    for (std::uint32_t j = 0U; j < kTileDim; ++j) {
      const std::uint32_t sum = (tile.x[i] + tile.q[j] + tile.kappa) & kResidualMask;
      if (exact_contains(exact, sum)) {
        mask |= std::uint64_t{1} << (8U * i + j);
      }
    }
  }
  return mask & tile_valid_mask(tile);
}

// K3 semantics: 256-bucket byte signature over (value & 0xFF).  Superset,
// because sum == y (mod 2^19) implies sum == y (mod 256).
[[nodiscard]] inline bool signature_contains(const std::uint8_t* const sig07,
                                             const std::uint8_t* const sig815,
                                             const std::uint32_t byte_value) noexcept {
  const std::uint32_t low = byte_value & 15U;
  const std::uint32_t high = byte_value >> 4U;
  const std::uint8_t row = high < 8U ? sig07[low] : sig815[low];
  return ((row >> (high & 7U)) & 1U) != 0U;
}

[[nodiscard]] inline std::uint64_t k3_signature_shuffle_portable(
    const Tile& tile, const TargetView& view) noexcept {
  const std::uint8_t* const sig07 = view.sig07[tile.u];
  const std::uint8_t* const sig815 = view.sig815[tile.u];
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    for (std::uint32_t j = 0U; j < kTileDim; ++j) {
      const std::uint32_t byte_value = (tile.x[i] + tile.q[j] + tile.kappa) & 0xFFU;
      if (signature_contains(sig07, sig815, byte_value)) {
        mask |= std::uint64_t{1} << (8U * i + j);
      }
    }
  }
  return mask & tile_valid_mask(tile);
}

// K2 semantics: block-of-8 coarse bitmap over 2^16 blocks.  Superset.
[[nodiscard]] inline std::uint64_t k2_coarse_gather_portable(const Tile& tile,
                                                             const TargetView& view) noexcept {
  const std::uint32_t* const coarse = view.coarse[tile.u];
  std::uint64_t mask = 0U;
  for (std::uint32_t i = 0U; i < kTileDim; ++i) {
    for (std::uint32_t j = 0U; j < kTileDim; ++j) {
      const std::uint32_t sum = (tile.x[i] + tile.q[j] + tile.kappa) & kResidualMask;
      const std::uint32_t block = sum >> kCoarseShift;
      if (((coarse[block >> 5U] >> (block & 31U)) & 1U) != 0U) {
        mask |= std::uint64_t{1} << (8U * i + j);
      }
    }
  }
  return mask & tile_valid_mask(tile);
}

// ---------------------------------------------------------------------------
// Batched drivers.  A single call processes a whole batch, so the timed region
// pays one call boundary rather than one per tile.  Every driver folds its
// results into the same rotate-multiply checksum chain, which the probe prints,
// so no result can be eliminated as dead.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint64_t fold(const std::uint64_t accumulator,
                                        const std::uint64_t value) noexcept {
  const std::uint64_t mixed = accumulator ^ value;
  return ((mixed << 7U) | (mixed >> 57U)) * 0x9E3779B97F4A7C15ULL;
}

using TileKernel = std::uint64_t (*)(const Tile&, const TargetView&);
using BatchKernel = std::uint64_t (*)(const Tile*, std::size_t, const TargetView&,
                                      std::uint64_t);

// One exact residual pair probe: the scalar baseline work unit.  Defined here
// so the oracle, the baseline and the candidate verification all agree.
[[nodiscard]] inline bool pair_probe(const std::uint32_t x,
                                     const std::uint32_t q,
                                     const std::uint32_t kappa,
                                     const std::uint32_t* const exact) noexcept {
  return exact_contains(exact, (x + q + kappa) & kResidualMask);
}

// Declared in the portable kernels translation unit (the probe).
[[nodiscard]] std::uint64_t scalar_pair_batch(const Tile* tiles,
                                              std::size_t count,
                                              const TargetView& view,
                                              std::uint64_t checksum);
[[nodiscard]] std::uint64_t scalar_report8_batch(const Tile* tiles,
                                                 std::size_t count,
                                                 const TargetView& view,
                                                 std::uint64_t checksum);
[[nodiscard]] std::uint64_t k0_sum_floor_portable_batch(const Tile* tiles,
                                                        std::size_t count,
                                                        const TargetView& view,
                                                        std::uint64_t checksum);
[[nodiscard]] std::uint64_t k1_exact_gather_portable_batch(const Tile* tiles,
                                                           std::size_t count,
                                                           const TargetView& view,
                                                           std::uint64_t checksum);
[[nodiscard]] std::uint64_t k3_signature_shuffle_portable_batch(const Tile* tiles,
                                                                std::size_t count,
                                                                const TargetView& view,
                                                                std::uint64_t checksum);
[[nodiscard]] std::uint64_t k2_coarse_gather_portable_batch(const Tile* tiles,
                                                            std::size_t count,
                                                            const TargetView& view,
                                                            std::uint64_t checksum);

// One candidate lane awaiting exact verification.  Charged at C_verify.
struct CandidateLane {
  std::uint32_t x;
  std::uint32_t q;
  std::uint32_t meta;  // bit 0 = kappa, bits 1..6 = u
};

[[nodiscard]] std::uint64_t verify_candidate_batch(const CandidateLane* lanes,
                                                   std::size_t count,
                                                   const TargetView& view,
                                                   std::uint64_t checksum);

// ---------------------------------------------------------------------------
// AVX2 translation unit (ter_microreport8_kernels_avx2.cpp).
// ---------------------------------------------------------------------------
namespace avx2 {

[[nodiscard]] std::uint64_t k0_sum_floor(const Tile& tile, const TargetView& view);
[[nodiscard]] std::uint64_t k1_exact_gather(const Tile& tile, const TargetView& view);
[[nodiscard]] std::uint64_t k3_signature_shuffle(const Tile& tile, const TargetView& view);
[[nodiscard]] std::uint64_t k2_coarse_gather(const Tile& tile, const TargetView& view);

[[nodiscard]] std::uint64_t k0_sum_floor_batch(const Tile* tiles, std::size_t count,
                                               const TargetView& view, std::uint64_t checksum);
[[nodiscard]] std::uint64_t k1_exact_gather_batch(const Tile* tiles, std::size_t count,
                                                  const TargetView& view, std::uint64_t checksum);
[[nodiscard]] std::uint64_t k3_signature_shuffle_batch(const Tile* tiles, std::size_t count,
                                                       const TargetView& view,
                                                       std::uint64_t checksum);
[[nodiscard]] std::uint64_t k2_coarse_gather_batch(const Tile* tiles, std::size_t count,
                                                   const TargetView& view,
                                                   std::uint64_t checksum);

// True only when this translation unit was actually compiled for AVX2.  It is
// resolved in that translation unit, not by the caller's preprocessor state.
[[nodiscard]] bool translation_unit_is_avx2() noexcept;

}  // namespace avx2

// ---------------------------------------------------------------------------
// Runtime ISA eligibility.
//
// The AVX2 feature bit alone is not sufficient: an operating system that has
// not enabled XMM/YMM state through XCR0 will fault or silently corrupt state.
// Dispatch therefore requires OSXSAVE, AVX, XCR0[2:1] == 11b, and AVX2.
// ---------------------------------------------------------------------------
struct IsaReport {
  bool cpuid_available = false;
  bool osxsave = false;
  bool avx = false;
  bool ymm_state_enabled = false;
  bool avx2 = false;
  bool bmi2 = false;
  bool avx512f = false;
  bool usable = false;  // every prerequisite above satisfied
  char brand[49]{};
};

[[nodiscard]] IsaReport detect_isa() noexcept;

}  // namespace ter::microreport8
