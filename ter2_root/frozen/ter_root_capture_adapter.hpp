#pragma once

#include "ter/full_ter.hpp"
#include "ter/root_partition.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ter::root_partition_bench {

struct CaptureRecordsResult {
  Status status = Status::InvalidConfiguration;
  std::vector<TerPartitionEntry> records;
  std::string message;
};

[[nodiscard]] std::uint32_t quotient_low32_from_words(
    std::span<const std::uint32_t> residue_words_le,
    std::span<const std::uint32_t> child_target_words_le,
    std::uint32_t n,
    std::uint32_t r1) noexcept;

[[nodiscard]] bool capture_list_size_fits_entry_id(std::uint64_t size) noexcept;

[[nodiscard]] CaptureRecordsResult capture_list_to_records(
    const FullTerRootCapture& capture,
    std::size_t list_index);

}  // namespace ter::root_partition_bench
