#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include "ter/types.hpp"

namespace ter {

struct TerPartitionEntry {
  QuotientResidue low_k;
  EntryId entry_id;
};
static_assert(sizeof(TerPartitionEntry) == 8);
static_assert(std::is_trivially_copyable_v<TerPartitionEntry>);

enum class PartitionBackend : std::uint8_t {
  CurrentScatter, BlockInPlace, Region2Path, Adaptive,
};

struct PartitionSpec {
  std::uint8_t shift = 0;
  std::uint8_t bits = 8;
  std::uint32_t threads = 1;
  PartitionBackend backend = PartitionBackend::CurrentScatter;
  std::uint64_t scratch_cap_bytes = 0;
  std::uint32_t block_bytes = 2048;
};

struct PartitionStats {
  PartitionBackend requested = PartitionBackend::CurrentScatter;
  PartitionBackend effective = PartitionBackend::CurrentScatter;
  std::uint64_t histogram_ns = 0, prefix_ns = 0, scatter_ns = 0;
  std::uint64_t local_classification_ns = 0, block_permute_ns = 0, cleanup_ns = 0;
  std::uint64_t records_classified = 0, records_written = 0;
  std::uint64_t blocks_flushed = 0, blocks_permuted = 0, partial_records_repaired = 0;
  std::uint64_t histogram_bytes = 0, offset_bytes = 0, record_scratch_bytes = 0;
  std::uint64_t block_buffer_bytes = 0, scratch_peak_bytes = 0;
  std::uint64_t logical_bytes_read = 0, logical_bytes_written = 0;
};

class PartitionWorkspace {
 public:
  PartitionWorkspace();
  PartitionWorkspace(const PartitionWorkspace&) = delete;
  PartitionWorkspace& operator=(const PartitionWorkspace&) = delete;
  PartitionWorkspace(PartitionWorkspace&&) noexcept;
  PartitionWorkspace& operator=(PartitionWorkspace&&) noexcept;
  ~PartitionWorkspace();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend Status partition_chunk(std::span<TerPartitionEntry>, const PartitionSpec&,
                                PartitionWorkspace&, std::span<std::uint64_t>,
                                PartitionStats&);
};

[[nodiscard]] Status partition_chunk(std::span<TerPartitionEntry> entries,
                                     const PartitionSpec& spec,
                                     PartitionWorkspace& workspace,
                                     std::span<std::uint64_t> bucket_offsets,
                                     PartitionStats& stats);

}  // namespace ter
