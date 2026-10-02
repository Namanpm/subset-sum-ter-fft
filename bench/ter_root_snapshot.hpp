#pragma once

// Packed root snapshot: the three genuine L1 root lists of one Full TER attempt,
// written once by a capture process and replayed read-only by a fresh benchmark
// process.
//
// Layout (little-endian, this platform's natural alignment, versioned):
//   Header                         fixed size, at offset 0, written last
//   list 0 records | list 1 | list 2   record_bytes each, EntryId = index
// Record: uint64 positive[W64] | uint64 negative[W64] | uint32 residue[W32],
// exactly the FullTerRootStream record (see ter/full_ter.hpp).
//
// Integrity: the header carries its own checksum and one FNV-1a-64 checksum per
// list.  A file is written as <path>.partial and renamed only after the header
// is final, so an interrupted or failed capture never leaves a file at <path>.

#include "ter/full_ter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ter::root_snapshot {

inline constexpr std::uint32_t kLayoutVersion = 1U;
inline constexpr std::uint32_t kMaxWords32 = 8U;  // n <= 256

struct Header {
  char magic[8];
  std::uint32_t layout_version;
  std::uint32_t header_bytes;
  std::uint32_t n;
  std::uint32_t r1;
  std::uint32_t r2;
  std::uint32_t words64;
  std::uint32_t words32;
  std::uint32_t record_bytes;
  std::uint32_t attempt;
  std::uint32_t reserved;
  std::uint64_t seed;          // the instance / config seed
  std::uint64_t attempt_seed;  // seed + attempt: the targets' seed
  std::uint64_t entries[3];
  std::uint64_t offset[3];
  std::uint64_t checksum[3];
  std::uint32_t child_target[3][kMaxWords32];
  std::uint32_t root_target[kMaxWords32];
  std::uint64_t level2_sizes[9];
  std::uint64_t memory_limit_bytes;
  std::uint64_t declared_peak_bytes;        // solver accounting model
  std::uint64_t process_peak_working_set;   // OS-measured, capture process
  std::uint64_t capture_elapsed_ns;
  char git_commit[48];
  std::uint64_t header_checksum;  // over the header with this field zeroed
};

// In-memory snapshot: header plus each list's raw records.
struct Snapshot {
  Header header{};
  std::array<std::vector<std::uint8_t>, 3> records;

  [[nodiscard]] const std::uint8_t* record(std::size_t list, std::uint64_t index) const noexcept {
    return records[list].data() + index * header.record_bytes;
  }
  [[nodiscard]] std::span<const std::uint32_t> residue(std::size_t list,
                                                       std::uint64_t index) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> child_target(std::size_t list) const noexcept {
    return {header.child_target[list], header.words32};
  }
  [[nodiscard]] std::span<const std::uint32_t> root_target() const noexcept {
    return {header.root_target, header.words32};
  }
  // Digit of one coordinate: +1, -1 or 0.  Overlapping masks are rejected at load.
  [[nodiscard]] int digit(std::size_t list, std::uint64_t index, std::uint32_t coordinate) const noexcept;
};

[[nodiscard]] std::uint64_t fnv1a(const std::uint8_t* bytes, std::size_t size,
                                  std::uint64_t state = 0xcbf29ce484222325ULL) noexcept;

// Streams one Full TER attempt straight into <path>, with a bounded buffer.
struct CaptureOutcome {
  FullTerRootStreamResult stream;
  bool written = false;        // final file present at <path>
  std::string io_error;        // non-empty when the file could not be written
  std::uint64_t file_bytes = 0;
};

[[nodiscard]] CaptureOutcome capture_to_file(const Instance& instance, const FullTerConfig& config,
                                             std::uint32_t attempt, const std::string& path,
                                             std::uint64_t buffer_bytes, const std::string& git_commit);

// Loads and fully validates a snapshot.  On failure returns false and a reason.
[[nodiscard]] bool load(const std::string& path, Snapshot& out, std::string& error);

}  // namespace ter::root_snapshot
