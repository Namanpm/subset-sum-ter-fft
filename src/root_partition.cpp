#include "ter/root_partition.hpp"

#include "root_partition_internal.hpp"

#include <algorithm>
#include <cstddef>

namespace ter {

PartitionWorkspace::PartitionWorkspace() : impl_(std::make_unique<Impl>()) {}

PartitionWorkspace::PartitionWorkspace(PartitionWorkspace&&) noexcept = default;

PartitionWorkspace& PartitionWorkspace::operator=(PartitionWorkspace&&) noexcept = default;

PartitionWorkspace::~PartitionWorkspace() = default;

namespace {

[[nodiscard]] bool valid_block_bytes(const std::uint32_t block_bytes) noexcept {
  return block_bytes >= sizeof(TerPartitionEntry) &&
         (block_bytes & (block_bytes - 1U)) == 0U &&
         block_bytes % sizeof(TerPartitionEntry) == 0U;
}

[[nodiscard]] bool current_scatter_width_supported(const std::uint8_t bits) noexcept {
  return bits == 8U || bits == 12U || bits == 16U;
}

}  // namespace

Status partition_chunk(const std::span<TerPartitionEntry> entries,
                       const PartitionSpec& spec,
                       PartitionWorkspace& workspace,
                       const std::span<std::uint64_t> bucket_offsets,
                       PartitionStats& stats) {
  stats = {};
  stats.requested = spec.backend;

  if (spec.bits == 0U ||
      static_cast<std::uint32_t>(spec.shift) + static_cast<std::uint32_t>(spec.bits) > 32U ||
      spec.threads == 0U) {
    return Status::InvalidConfiguration;
  }

  const std::uint64_t bucket_count = std::uint64_t{1} << spec.bits;
  if (bucket_offsets.size() != bucket_count + 1U) {
    return Status::InvalidConfiguration;
  }

  switch (spec.backend) {
    case PartitionBackend::CurrentScatter:
      if (!current_scatter_width_supported(spec.bits)) {
        return Status::InvalidConfiguration;
      }
      stats.effective = PartitionBackend::CurrentScatter;
      break;
    case PartitionBackend::BlockInPlace:
      if (spec.bits != 8U) {
        return Status::NotApplicable;
      }
      if (!valid_block_bytes(spec.block_bytes)) {
        return Status::InvalidConfiguration;
      }
      stats.effective = PartitionBackend::BlockInPlace;
      break;
    case PartitionBackend::Region2Path:
    case PartitionBackend::Adaptive:
      return Status::NotApplicable;
    default:
      return Status::InvalidConfiguration;
  }

  if (entries.size() <= 1U) {
    const std::uint64_t returned_offset_bytes =
        static_cast<std::uint64_t>(bucket_offsets.size()) *
        sizeof(std::uint64_t);
    const Status admission =
        spec.backend == PartitionBackend::CurrentScatter
            ? workspace.impl_->admit_trivial_current_scatter(
                  spec.scratch_cap_bytes, returned_offset_bytes, stats)
            : workspace.impl_->admit_trivial_block_inplace(
                  spec.scratch_cap_bytes, returned_offset_bytes, stats);
    if (admission != Status::Completed) {
      return admission;
    }
    std::fill(bucket_offsets.begin(), bucket_offsets.end(), 0U);
    if (entries.empty()) {
      return Status::Completed;
    }

    const std::uint32_t mask = (std::uint32_t{1} << spec.bits) - 1U;
    const std::uint32_t bucket = (entries.front().low_k >> spec.shift) & mask;
    for (std::size_t offset = static_cast<std::size_t>(bucket) + 1U;
         offset < bucket_offsets.size(); ++offset) {
      bucket_offsets[offset] = 1U;
    }
    stats.records_classified = 1U;
    return Status::Completed;
  }

  if (spec.backend == PartitionBackend::CurrentScatter) {
    return workspace.impl_->partition_current_scatter(entries, spec, bucket_offsets, stats);
  }
  return workspace.impl_->partition_block_inplace(entries, spec, bucket_offsets, stats);
}

}  // namespace ter
