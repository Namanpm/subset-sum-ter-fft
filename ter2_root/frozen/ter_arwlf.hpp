#pragma once

// ARWLF: Adaptive Radix Wide-Leaf Filter for the TER^2 root (L1 -> L0).
//
// Benchmark surface only; production solve_full_ter semantics are untouched.
//
// Root equation per (s, t) microproblem (the donor low-6 split): with residual
// keys r (W = F - 6 bits, F = min(Q, 64)),
//     r_q + r_x + kappa == r_y (mod 2^W),  high Q - F bits checked with carry.
//
// Terminal leaves.  Each family is partitioned ONCE by residual bits [0, 13)
// into 8192 leaves.  Q leaf i and X leaf j reach exactly target leaf
//     k = (i + j + kappa) mod 8192,  low-13 carry c = (i + j + kappa) >> 13.
// For a fixed k this carry is 0 for every i <= k - kappa and 1 above, so the
// Q rows of one target leaf form two contiguous ranges with a constant carry.
// There is no second radix level.
//
// Wide leaf filter.  For every (q, x) of a leaf triple (Q_i, X_j, Y_k),
//     s = (mid_q + mid_x + c) mod 2^mid_bits,  mid = residual bits [13, 45),
// is exactly bits [13, 45) of the residual sum.  s is tested against Y_k's
// per-byte occupancy tables (four 256-bit tables, one per byte of mid): a
// lane is rejected only when some byte of s occurs in no entry of Y_k, so no
// true solution is ever rejected; false positives go to the exact resolver.
// The tables summarize Y alone, never Q_i + X_j.
//
// AVX2 sweep.  One flat loop over every Q row of the microproblem per target
// leaf; each row tests its whole partner X leaf as one 16 x 16-bit vector
// (bytes 0 and 1 of s via pshufb nibble tables).  X leaves are stored densely
// with a precomputed live-lane mask; leaves with more than 16 entries get
// their extra 16-lane blocks in a separate overflow pass.  Bytes 2 and 3 are
// tested only for the rare lanes that pass bytes 0 and 1.  The scalar sweep
// (per-leaf-pair packets) is the reference: both produce the same survivors.
//
// Exact resolver.  A surviving (q, x) is compared with every y of Y_k on the
// declared mid bits (counted: projected hits at k = 19 + check_bits), then on
// all W residual bits, then on the high Q - 64 bits, and only then reaches the
// RootSink's low-32 / residue / structural authority chain.  EntryId
// multiplicity is preserved: nothing is deduplicated.
//
// Dispatcher (regret-bounded).  Candidates: QUEUE_DENSE (the unchanged donor
// backend, inside its capacity), ARWLF (W >= 29 and at most one packet per 16
// logical pairs: dense leaves) and K2_WIDE (always).  Every candidate is probed
// on the same square block of up to 8 x 8 selected microproblems, reduced to a
// budget sized to its cost (K2_WIDE 2^21 logical pairs, ARWLF 2^26); builds
// are timed apart from sweeps, and the smallest extrapolated time wins.  A chosen ARWLF run carries a runtime guard
// at the runner-up's break-even ns per logical pair: if its measured prefix is
// slower, it aborts and the runner-up runs from scratch.

#include "ter_a64_chqr.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ter::arwlf {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace a64 = ter::a64;

inline constexpr std::uint32_t kLeafBits = 13U;
inline constexpr std::uint32_t kLeaves = 1U << kLeafBits;
inline constexpr std::uint32_t kPacket = 16U;  // lanes per row / physical packet side
inline constexpr std::uint32_t kChunks = 4U;   // 8-bit chunks of the 32-bit mid

// Y leaf summary: word[c][w] bit b set <=> some entry's mid byte c == 32 w + b.
// Read as 32 bytes, byte v >> 3 bit v & 7 is value v: the pshufb layout.
struct alignas(32) LeafTables {
  std::uint32_t word[kChunks][8];
};

struct Survivor {  // absolute leaf-array positions of q and x, and the low-13 carry
  std::uint32_t qa;
  std::uint32_t xa;
  std::uint32_t carry;
};

// X leaf j as seen by the AVX2 sweep: first entry in the dense arrays, and
// bit 2i set for each live lane i < 16 (the pair-of-bytes movemask layout).
struct LaneSlot {
  std::uint32_t offset;
  std::uint32_t valid;
};

// Everything one AVX2 target-leaf sweep reads; all arrays are in leaf order.
struct RowSweep {
  const std::uint16_t* q_leaf = nullptr;    // leaf digit of every Q row
  const std::uint32_t* q_dup[2] = {};       // ((mid + c) & 0xFFFF) * 0x10001, c = 0, 1
  const std::uint32_t* q_offset = nullptr;  // kLeaves + 1
  const std::uint32_t* q_mid = nullptr;     // full mids, survivor path only
  std::uint32_t q_count = 0;
  const std::uint16_t* x_lo = nullptr;      // mid & 0xFFFF, readable 16 past the end
  const LaneSlot* x_slot = nullptr;         // kLeaves
  const std::uint32_t* x_offset = nullptr;  // kLeaves + 1
  const std::uint32_t* x_mid = nullptr;     // full mids, survivor path only
  const std::uint32_t* x_overflow = nullptr;  // leaves with more than 16 entries
  std::uint32_t x_overflow_count = 0;
  const LeafTables* tables = nullptr;       // Y leaf k
  std::uint64_t* pending = nullptr;         // scratch, >= max(q_count, 1) entries
  std::uint32_t k = 0;
  std::uint32_t kappa = 0;
  std::uint32_t chunks = 0;                 // >= 2
  std::uint32_t mid_mask = 0;               // covers bits 0..15
};

namespace avx2 {
// Appends every surviving lane of target leaf k to `queue` (order: carry-0
// rows, carry-1 rows, then overflow blocks) and returns the rows tested.
std::uint64_t sweep_rows(const RowSweep& sweep, std::vector<Survivor>& queue);
[[nodiscard]] bool translation_unit_is_avx2() noexcept;
}  // namespace avx2

// Reference packet filter: lane (qi, xi) of q[0..nq) x x[0..nx) (nq, nx <= 16)
// survives iff every chunk byte c < chunks of s = (q + x + carry) & mid_mask is
// present in its table.  Survivors are appended as qi * 16 + xi.
[[nodiscard]] std::uint32_t wide_leaf_filter_scalar(const std::uint32_t* q, std::uint32_t nq,
                                                    const std::uint32_t* x, std::uint32_t nx,
                                                    std::uint32_t carry, const LeafTables& tables,
                                                    std::uint32_t chunks, std::uint32_t mid_mask,
                                                    std::uint16_t* survivors) noexcept;

// ---------------------------------------------------------------------------
// Backend.
// ---------------------------------------------------------------------------
enum class Status : std::uint8_t { Completed, Aborted, NotApplicable };

[[nodiscard]] const char* status_label(Status status) noexcept;

struct Config {
  std::uint32_t check_bits = 0;  // 0: all mid bits.  Otherwise the exact mid compare
                                 // (and the chunk tables used) cover only these bits.
  bool use_avx2 = true;
  bool account = false;          // exact lane / packet / leaf-triple counts (untimed use)
  std::uint32_t work_divisor = 1;  // > 1: only the first ceil(|Y nonempty| / d) target
                                   // leaves of each microproblem (a probe, not a result)
  // Guards, judged once, at the end of the first 1/64 of a microproblem's
  // target leaves by which >= 2^20 logical pairs are covered; sweep time only.
  double guard_ns_per_pair = 0.0;      // > 0: abort if the prefix is slower per logical pair
  double guard_survivor_rate = 0.0;    // > 0: abort if prefix survivors per logical pair exceed this
};

struct Telemetry {
  std::uint64_t logical_pairs = 0;     // sum of |Q_s| * |X_t| over microproblems run
  std::uint64_t rows = 0;              // AVX2: row vectors tested (main + overflow)
  std::uint64_t main_rows = 0;         // AVX2: target leaves run * |Q_s|
  std::uint64_t filter_survivors = 0;  // lanes passing every chunk table
  std::uint64_t projected_hits = 0;    // (q, x, y) equal on quotient bits [0, 19 + check_bits)
  std::uint64_t fast_hits = 0;         // ... on all F low bits
  std::uint64_t full_hits = 0;         // ... and on the high bits: emitted
  std::uint64_t build_ns = 0;          // leaf / lane / row / table construction
  std::uint64_t index_bytes = 0;
  // Config::account only.
  std::uint64_t leaf_triples = 0;      // (k, i) with nonempty Y_k, Q_i, X_j
  std::uint64_t packets = 0;           // physical 16 x 16 packets of those leaf pairs
  std::uint64_t lanes = 0;             // valid (q, x) lanes of those leaf pairs
  std::uint32_t mid_bits = 0;
  std::uint32_t check_bits = 0;
  std::uint32_t chunks = 0;
  bool avx2 = false;
  double guard_prefix_ns_per_pair = 0.0;  // measured when a guard is set
};

[[nodiscard]] Status run_arwlf(const mr8::Dataset& data, const rg::RootIndex& index,
                               const a64::FamilyKeys& keys, const Config& config,
                               rg::RootSink& sink, Telemetry* telemetry);

// Physical 16 x 16 packets of the selected microproblems: a microproblem
// (s, t) costs P_Q[s] * P_X[t], P_L[f] = sum over leaves of ceil(|leaf| / 16).
[[nodiscard]] double selected_packets(const a64::FamilyKeys& keys, const rg::RootIndex& index);

// ---------------------------------------------------------------------------
// Dispatcher.
// ---------------------------------------------------------------------------
enum class Backend : std::uint8_t { QueueDense = 0, Arwlf = 1, K2Wide = 2 };

[[nodiscard]] const char* backend_label(Backend backend) noexcept;

struct Dispatch {
  Backend chosen = Backend::K2Wide;
  Backend ran = Backend::K2Wide;  // differs from chosen only after a guard abort
  std::array<bool, 3> applicable{};
  std::array<double, 3> predicted_ms{};
  double arwlf_packets_per_pair = 0.0;  // ARWLF candidate iff <= 1/16
  double probe_ms = 0.0;
  std::uint32_t probe_microproblems = 0;
  std::uint32_t k2_probe_divisor = 1;
  std::uint32_t arwlf_probe_divisor = 1;
  bool guard_aborted = false;
};

// Probes, chooses, runs the chosen backend into `sink` (reset first), and on a
// guard abort resets the sink and runs the runner-up.  `tables` may be null
// (QUEUE_DENSE then inapplicable).  The probes narrow `index`'s selection in
// place and restore it before returning; nothing else in `index` changes.
Dispatch dispatch_and_run(const mr8::Dataset& data, rg::RootIndex& index,
                          const a64::FamilyKeys& keys, rg::RadixTables* tables,
                          rg::RootSink& sink);

}  // namespace ter::arwlf
