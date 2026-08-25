/******************************************************************************
 * Serial and portable parallel radix8 BlockInPlace partition for TER records.
 *
 * Upstream: https://github.com/ips4o/ips2ra
 * Reference commit: ef3407632348f58a6ab75832a749cde99f3b9ad8
 * Derived concepts/files: partitioning.hpp, local_classification.hpp,
 * block_permutation.hpp, cleanup_margins.hpp, bucket_pointers.hpp,
 * config.hpp, memory/buffer helpers as actually used.
 * License: BSD-2-Clause; full text in third_party/licenses/ips2ra-BSD-2-Clause.txt
 ******************************************************************************/

#include "root_partition_internal.hpp"

#include "ter/limitations.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace ter {

#ifdef TER_ROOT_PARTITION_TESTING
namespace {

std::mutex block_observation_mutex;
BlockInPlaceWorkerObservation last_block_observation;

}  // namespace

BlockInPlaceWorkerObservation block_inplace_worker_observation() {
  std::lock_guard<std::mutex> lock(block_observation_mutex);
  return last_block_observation;
}
#endif

void BucketBlockPointers::reset(const std::uint64_t begin,
                                const std::uint64_t end) {
  std::lock_guard<std::mutex> lock(pointer_mutex);
  write_block = begin;
  read_block = end;
  end_block = end;
  readers.store(0U, std::memory_order_relaxed);
}

bool BucketBlockPointers::claim_read_block(std::uint64_t* const block) {
  std::lock_guard<std::mutex> lock(pointer_mutex);
  if (read_block <= write_block) {
    return false;
  }
  --read_block;
  readers.fetch_add(1U, std::memory_order_acq_rel);
  *block = read_block;
  return true;
}

void BucketBlockPointers::release_read_block() {
  std::lock_guard<std::mutex> lock(pointer_mutex);
  readers.fetch_sub(1U, std::memory_order_acq_rel);
  pointer_ready.notify_all();
}

bool BucketBlockPointers::claim_write_block(std::uint64_t* const block) {
  std::unique_lock<std::mutex> lock(pointer_mutex);
  pointer_ready.wait(lock, [this] {
    return write_block >= end_block || write_block < read_block ||
           !is_reading();
  });
  if (write_block >= end_block) {
    return false;
  }
  *block = write_block;
  ++write_block;
  return true;
}

bool BucketBlockPointers::is_reading() const noexcept {
  return readers.load(std::memory_order_acquire) != 0U;
}

bool BucketBlockPointers::complete() {
  std::lock_guard<std::mutex> lock(pointer_mutex);
  return write_block == end_block && !is_reading();
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kBuckets = 256U;
constexpr std::uint16_t kEmptyBlock = 256U;
constexpr std::uint16_t kFixedBlock = 257U;

constexpr std::size_t kCountsBase = 0U;
constexpr std::size_t kPartialSizesBase = kCountsBase + kBuckets;
constexpr std::size_t kWriteBlocksBase = kPartialSizesBase + kBuckets;
constexpr std::size_t kEndBlocksBase = kWriteBlocksBase + kBuckets;
constexpr std::size_t kOverflowSizesBase = kEndBlocksBase + kBuckets;
constexpr std::size_t kOffsetsBase = kOverflowSizesBase + kBuckets;
constexpr std::size_t kMetadataSlots = kOffsetsBase + kBuckets + 1U;

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point started) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

[[nodiscard]] bool capacity_bytes(const std::size_t capacity,
                                  const std::size_t element_bytes,
                                  std::uint64_t* const bytes) noexcept {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  return limits::checked::checked_multiply(static_cast<std::uint64_t>(capacity),
                                           static_cast<std::uint64_t>(element_bytes), bytes);
}

struct LiveBlockScratch {
  std::uint64_t records = 0U;
  std::uint64_t owners = 0U;
  std::uint64_t metadata = 0U;
  std::uint64_t parallel_state = 0U;
  std::uint64_t total = 0U;
};

[[nodiscard]] bool add_record_io(const std::uint64_t records,
                                 std::uint64_t* const counter) noexcept {
  return limits::checked::checked_add(*counter, records, counter);
}

}  // namespace

void PartitionWorkspace::Impl::release_block_inplace_storage() noexcept {
  std::vector<TerPartitionEntry>().swap(block_records);
  std::vector<std::uint16_t>().swap(block_owners);
  std::vector<std::uint64_t>().swap(block_metadata);
  parallel_block_inplace.reset();
}

Status PartitionWorkspace::Impl::admit_trivial_block_inplace(
    const std::uint64_t scratch_cap_bytes,
    const std::uint64_t returned_offset_bytes,
    PartitionStats& stats) {
  release_current_scatter_storage();
  LiveBlockScratch live;
  std::uint64_t unused_required = 0U;
  std::uint64_t pool_live = 0U;
  if (!capacity_bytes(block_records.capacity(), sizeof(TerPartitionEntry),
                      &live.records) ||
      !capacity_bytes(block_owners.capacity(), sizeof(std::uint16_t),
                      &live.owners) ||
      !capacity_bytes(block_metadata.capacity(), sizeof(std::uint64_t),
                      &live.metadata) ||
      !partition_worker_pool_scratch_bytes(parallel_current_scatter, 0U,
                                           &unused_required, &pool_live)) {
    return Status::ResourceLimit;
  }
  live.parallel_state = parallel_block_inplace == nullptr
                            ? 0U
                            : sizeof(ParallelBlockInPlaceState);
  if (!limits::checked::checked_add(live.records, live.owners, &live.total) ||
      !limits::checked::checked_add(live.total, live.metadata, &live.total) ||
      !limits::checked::checked_add(live.total, live.parallel_state,
                                    &live.total) ||
      !limits::checked::checked_add(live.total, pool_live, &live.total)) {
    return Status::ResourceLimit;
  }
  if (scratch_cap_bytes != 0U && live.total > scratch_cap_bytes) {
    return Status::ResourceLimit;
  }
  stats.block_buffer_bytes = live.records;
  stats.offset_bytes = returned_offset_bytes;
  stats.scratch_peak_bytes = live.total;
  return Status::Completed;
}

Status PartitionWorkspace::Impl::partition_block_inplace(
    const std::span<TerPartitionEntry> entries,
    const PartitionSpec& spec,
    const std::span<std::uint64_t> bucket_offsets,
    PartitionStats& stats) {
  const std::uint64_t records_per_block =
      static_cast<std::uint64_t>(spec.block_bytes / sizeof(TerPartitionEntry));
  if (records_per_block == 0U ||
      (records_per_block & (records_per_block - 1U)) != 0U) {
    return Status::InvalidConfiguration;
  }
  if (spec.threads > 1U) {
    return partition_block_inplace_parallel(entries, spec, bucket_offsets, stats);
  }

  // Ownership transition: CurrentScatter's N-record scratch, histograms, and
  // worker tables are inactive here and are released before cap admission.
  // The private pool is shared by both parallel backends and remains alive.
  release_current_scatter_storage();

  const std::uint64_t record_count = static_cast<std::uint64_t>(entries.size());
  const std::uint64_t physical_blocks = record_count / records_per_block;
  constexpr std::uint64_t kBufferedBlocks = 2U * kBuckets + 2U;
  std::uint64_t buffered_records = 0U;
  std::uint64_t required_record_bytes = 0U;
  std::uint64_t required_owner_bytes = 0U;
  std::uint64_t required_metadata_bytes = 0U;
  std::uint64_t required_scratch_bytes = 0U;
  if (!limits::checked::checked_multiply(kBufferedBlocks, records_per_block,
                                         &buffered_records) ||
      !limits::checked::checked_multiply(buffered_records,
                                         sizeof(TerPartitionEntry),
                                         &required_record_bytes) ||
      !limits::checked::checked_multiply(physical_blocks,
                                         sizeof(std::uint16_t),
                                         &required_owner_bytes) ||
      !limits::checked::checked_multiply(kMetadataSlots,
                                         sizeof(std::uint64_t),
                                         &required_metadata_bytes) ||
      !limits::checked::checked_add(required_record_bytes, required_owner_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::checked_add(required_scratch_bytes, required_metadata_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::fits_size_t(buffered_records) ||
      !limits::checked::fits_size_t(physical_blocks)) {
    return Status::ResourceLimit;
  }

  std::uint64_t serial_pool_required = 0U;
  std::uint64_t serial_pool_live = 0U;
  const auto measure_live_scratch = [&](LiveBlockScratch* const live) noexcept {
    *live = {};
    return capacity_bytes(block_records.capacity(), sizeof(TerPartitionEntry),
                          &live->records) &&
           capacity_bytes(block_owners.capacity(), sizeof(std::uint16_t),
                          &live->owners) &&
           capacity_bytes(block_metadata.capacity(), sizeof(std::uint64_t),
                          &live->metadata) &&
           (parallel_block_inplace == nullptr ||
            (live->parallel_state = sizeof(ParallelBlockInPlaceState), true)) &&
           limits::checked::checked_add(live->records, live->owners,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->metadata,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->parallel_state,
                                        &live->total) &&
           partition_worker_pool_scratch_bytes(
               parallel_current_scatter, 0U, &serial_pool_required,
               &serial_pool_live) &&
           limits::checked::checked_add(live->total, serial_pool_live,
                                        &live->total);
  };

  LiveBlockScratch live_scratch;
  if (!measure_live_scratch(&live_scratch) ||
      (spec.scratch_cap_bytes != 0U &&
       (required_scratch_bytes > spec.scratch_cap_bytes ||
        live_scratch.total > spec.scratch_cap_bytes))) {
    return Status::ResourceLimit;
  }

  try {
    block_records.resize(static_cast<std::size_t>(buffered_records));
    block_owners.resize(static_cast<std::size_t>(physical_blocks));
    block_metadata.resize(kMetadataSlots);
  } catch (const std::bad_alloc&) {
    return Status::ResourceLimit;
  } catch (const std::length_error&) {
    return Status::ResourceLimit;
  }

  if (!measure_live_scratch(&live_scratch) ||
      (spec.scratch_cap_bytes != 0U &&
       live_scratch.total > spec.scratch_cap_bytes)) {
    return Status::ResourceLimit;
  }

  const std::size_t block_size = static_cast<std::size_t>(records_per_block);
  TerPartitionEntry* const local_buffers = block_records.data();
  TerPartitionEntry* const overflow_buffers =
      local_buffers + kBuckets * block_size;
  TerPartitionEntry* const swap_buffers =
      overflow_buffers + kBuckets * block_size;
  std::uint64_t* const counts = block_metadata.data() + kCountsBase;
  std::uint64_t* const partial_sizes =
      block_metadata.data() + kPartialSizesBase;
  std::uint64_t* const write_blocks =
      block_metadata.data() + kWriteBlocksBase;
  std::uint64_t* const end_blocks = block_metadata.data() + kEndBlocksBase;
  std::uint64_t* const overflow_sizes =
      block_metadata.data() + kOverflowSizesBase;
  std::uint64_t* const private_offsets =
      block_metadata.data() + kOffsetsBase;

  std::fill(block_metadata.begin(), block_metadata.end(), 0U);
  std::fill(block_owners.begin(), block_owners.end(), kEmptyBlock);

  std::uint64_t logical_record_reads = 0U;
  std::uint64_t logical_record_writes = 0U;
  std::uint64_t flushed_blocks = 0U;

  const auto classification_started = Clock::now();
  for (std::size_t input_index = 0U; input_index < entries.size(); ++input_index) {
    // Copy the current record before a flush can overwrite its source slot.
    const TerPartitionEntry entry = entries[input_index];
    const std::size_t bucket =
        static_cast<std::size_t>((entry.low_k >> spec.shift) & 0xFFU);
    const std::size_t partial_size =
        static_cast<std::size_t>(partial_sizes[bucket]);
    local_buffers[bucket * block_size + partial_size] = entry;
    ++counts[bucket];
    ++partial_sizes[bucket];

    if (partial_sizes[bucket] == records_per_block) {
      if (flushed_blocks >= physical_blocks) {
        return Status::InvariantViolation;
      }
      const std::size_t destination =
          static_cast<std::size_t>(flushed_blocks * records_per_block);
      std::copy_n(local_buffers + bucket * block_size, block_size,
                  entries.begin() + static_cast<std::ptrdiff_t>(destination));
      block_owners[static_cast<std::size_t>(flushed_blocks)] =
          static_cast<std::uint16_t>(bucket);
      ++flushed_blocks;
      partial_sizes[bucket] = 0U;

      // Every flushed block represents B already classified records. Hence,
      // after processing i, flushed_blocks * B <= i + 1: this write ends no
      // later than the current record and cannot overwrite unread input.
      if (flushed_blocks * records_per_block >
          static_cast<std::uint64_t>(input_index) + 1U) {
        return Status::InvariantViolation;
      }
    }
  }
  stats.local_classification_ns = elapsed_ns(classification_started);
  stats.blocks_flushed = flushed_blocks;
  if (!add_record_io(record_count, &logical_record_reads) ||
      !add_record_io(record_count, &logical_record_writes) ||
      !add_record_io(flushed_blocks * records_per_block,
                     &logical_record_reads) ||
      !add_record_io(flushed_blocks * records_per_block,
                     &logical_record_writes)) {
    return Status::ResourceLimit;
  }

  private_offsets[0] = 0U;
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    if (!limits::checked::checked_add(private_offsets[bucket], counts[bucket],
                                      &private_offsets[bucket + 1U])) {
      return Status::ResourceLimit;
    }
  }
  if (private_offsets[kBuckets] != record_count) {
    return Status::InvariantViolation;
  }

  std::uint64_t interior_blocks = 0U;
  std::uint64_t overflow_blocks = 0U;
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    const std::uint64_t begin = private_offsets[bucket];
    const std::uint64_t end = private_offsets[bucket + 1U];
    const std::uint64_t first_block =
        begin / records_per_block + (begin % records_per_block == 0U ? 0U : 1U);
    const std::uint64_t last_block = end / records_per_block;
    const std::uint64_t bucket_interior_blocks =
        last_block >= first_block ? last_block - first_block : 0U;
    write_blocks[bucket] = first_block;
    end_blocks[bucket] = first_block + bucket_interior_blocks;
    const std::uint64_t bucket_full_blocks = counts[bucket] / records_per_block;
    if (bucket_interior_blocks > bucket_full_blocks ||
        bucket_full_blocks - bucket_interior_blocks > 1U ||
        !limits::checked::checked_add(interior_blocks, bucket_interior_blocks,
                                      &interior_blocks)) {
      return Status::InvariantViolation;
    }
    if (bucket_full_blocks != bucket_interior_blocks) {
      overflow_sizes[bucket] = records_per_block;
      ++overflow_blocks;
    }
  }
  if (interior_blocks + overflow_blocks != flushed_blocks) {
    return Status::InvariantViolation;
  }

  const auto permutation_started = Clock::now();

  // Remove the single boundary-overflow block (when present) for each bucket.
  // The remaining homogeneous blocks exactly match the fully aligned interior
  // slots, while overflow storage is bounded by one block per radix8 bucket.
  for (std::size_t source_block = 0U; source_block < block_owners.size();
       ++source_block) {
    const std::uint16_t owner = block_owners[source_block];
    if (owner >= kBuckets || overflow_sizes[owner] != records_per_block) {
      continue;
    }
    const std::size_t source = source_block * block_size;
    std::copy_n(entries.begin() + static_cast<std::ptrdiff_t>(source), block_size,
                overflow_buffers + static_cast<std::size_t>(owner) * block_size);
    block_owners[source_block] = kEmptyBlock;
    overflow_sizes[owner] += records_per_block;
    if (!add_record_io(records_per_block, &logical_record_reads) ||
        !add_record_io(records_per_block, &logical_record_writes)) {
      return Status::ResourceLimit;
    }
  }
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    if (overflow_sizes[bucket] != 0U &&
        overflow_sizes[bucket] != 2U * records_per_block) {
      return Status::InvariantViolation;
    }
    overflow_sizes[bucket] /= 2U;
  }

  std::uint64_t progress_steps = 0U;
  std::uint64_t progress_bound = 0U;
  if (!limits::checked::checked_multiply(flushed_blocks, 4U, &progress_bound) ||
      !limits::checked::checked_add(progress_bound, kBuckets, &progress_bound)) {
    return Status::ResourceLimit;
  }

  for (std::size_t source_block = 0U; source_block < block_owners.size();
       ++source_block) {
    std::uint16_t destination_bucket = block_owners[source_block];
    if (destination_bucket >= kBuckets) {
      continue;
    }

    std::copy_n(entries.begin() +
                    static_cast<std::ptrdiff_t>(source_block * block_size),
                block_size, swap_buffers);
    block_owners[source_block] = kEmptyBlock;
    if (!add_record_io(records_per_block, &logical_record_reads) ||
        !add_record_io(records_per_block, &logical_record_writes)) {
      return Status::ResourceLimit;
    }

    std::size_t current_swap = 0U;
    while (destination_bucket < kBuckets) {
      ++progress_steps;
      if (progress_steps > progress_bound) {
        return Status::InvariantViolation;
      }

      const std::size_t bucket = static_cast<std::size_t>(destination_bucket);
      const std::uint64_t destination_block = write_blocks[bucket];
      if (destination_block >= end_blocks[bucket] ||
          destination_block >= physical_blocks) {
        return Status::InvariantViolation;
      }
      ++write_blocks[bucket];

      const std::size_t destination_index =
          static_cast<std::size_t>(destination_block);
      const std::uint16_t displaced_bucket = block_owners[destination_index];
      if (displaced_bucket == kFixedBlock) {
        return Status::InvariantViolation;
      }
      const std::size_t destination = destination_index * block_size;
      const std::size_t writing_swap = current_swap;
      if (displaced_bucket < kBuckets) {
        const std::size_t next_swap = current_swap ^ 1U;
        std::copy_n(entries.begin() + static_cast<std::ptrdiff_t>(destination),
                    block_size, swap_buffers + next_swap * block_size);
        if (!add_record_io(records_per_block, &logical_record_reads) ||
            !add_record_io(records_per_block, &logical_record_writes)) {
          return Status::ResourceLimit;
        }
        destination_bucket = displaced_bucket;
        current_swap = next_swap;
      } else if (displaced_bucket == kEmptyBlock) {
        destination_bucket = kEmptyBlock;
      } else {
        return Status::InvariantViolation;
      }

      std::copy_n(swap_buffers + writing_swap * block_size, block_size,
                  entries.begin() + static_cast<std::ptrdiff_t>(destination));
      block_owners[destination_index] = kFixedBlock;
      ++stats.blocks_permuted;
      if (!add_record_io(records_per_block, &logical_record_reads) ||
          !add_record_io(records_per_block, &logical_record_writes)) {
        return Status::ResourceLimit;
      }
    }
  }

  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    if (write_blocks[bucket] != end_blocks[bucket]) {
      return Status::InvariantViolation;
    }
  }
  if (stats.blocks_permuted != interior_blocks) {
    return Status::InvariantViolation;
  }
  stats.block_permute_ns = elapsed_ns(permutation_started);

  const auto cleanup_started = Clock::now();
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    const std::uint64_t interior_begin =
        (private_offsets[bucket] / records_per_block +
         (private_offsets[bucket] % records_per_block == 0U ? 0U : 1U)) *
        records_per_block;
    const std::uint64_t interior_end =
        (private_offsets[bucket + 1U] / records_per_block) * records_per_block;
    std::uint64_t partial_read = 0U;
    std::uint64_t overflow_read = 0U;
    for (std::uint64_t destination = private_offsets[bucket];
         destination < private_offsets[bucket + 1U]; ++destination) {
      if (destination >= interior_begin && destination < interior_end) {
        continue;
      }

      TerPartitionEntry entry{};
      if (partial_read < partial_sizes[bucket]) {
        entry = local_buffers[bucket * block_size +
                              static_cast<std::size_t>(partial_read)];
        ++partial_read;
      } else if (overflow_read < overflow_sizes[bucket]) {
        entry = overflow_buffers[bucket * block_size +
                                 static_cast<std::size_t>(overflow_read)];
        ++overflow_read;
      } else {
        return Status::InvariantViolation;
      }
      entries[static_cast<std::size_t>(destination)] = entry;
      ++stats.partial_records_repaired;
      if (!add_record_io(1U, &logical_record_reads) ||
          !add_record_io(1U, &logical_record_writes)) {
        return Status::ResourceLimit;
      }
    }
    if (partial_read != partial_sizes[bucket] ||
        overflow_read != overflow_sizes[bucket]) {
      return Status::InvariantViolation;
    }
    partial_sizes[bucket] = 0U;
    overflow_sizes[bucket] = 0U;
  }
  stats.cleanup_ns = elapsed_ns(cleanup_started);

  // Completion postcondition: exact CSR digit membership and counts. Complete
  // records were copied through every buffer, so low_k and EntryId move as one
  // trivially-copyable unit; differential tests independently check the full
  // record multiset.
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    for (std::uint64_t index = private_offsets[bucket];
         index < private_offsets[bucket + 1U]; ++index) {
      const std::uint32_t digit =
          (entries[static_cast<std::size_t>(index)].low_k >> spec.shift) & 0xFFU;
      if (digit != bucket) {
        return Status::InvariantViolation;
      }
    }
  }
  if (!add_record_io(record_count, &logical_record_reads)) {
    return Status::ResourceLimit;
  }

  std::uint64_t logical_bytes_read = 0U;
  std::uint64_t logical_bytes_written = 0U;
  if (!limits::checked::checked_multiply(logical_record_reads,
                                         sizeof(TerPartitionEntry),
                                         &logical_bytes_read) ||
      !limits::checked::checked_multiply(logical_record_writes,
                                         sizeof(TerPartitionEntry),
                                         &logical_bytes_written)) {
    return Status::ResourceLimit;
  }

  std::copy_n(private_offsets, kBuckets + 1U, bucket_offsets.begin());

  stats.records_classified = record_count;
  stats.records_written = record_count;
  stats.block_buffer_bytes = live_scratch.records;
  stats.offset_bytes =
      static_cast<std::uint64_t>(bucket_offsets.size()) * sizeof(std::uint64_t);
  stats.scratch_peak_bytes = live_scratch.total;
  stats.logical_bytes_read = logical_bytes_read;
  stats.logical_bytes_written = logical_bytes_written;
  return Status::Completed;
}

Status PartitionWorkspace::Impl::partition_block_inplace_parallel(
    const std::span<TerPartitionEntry> entries,
    const PartitionSpec& spec,
    const std::span<std::uint64_t> bucket_offsets,
    PartitionStats& stats) {
  const std::uint64_t records_per_block =
      static_cast<std::uint64_t>(spec.block_bytes / sizeof(TerPartitionEntry));
  const std::uint64_t record_count = static_cast<std::uint64_t>(entries.size());
  const std::uint64_t physical_blocks = record_count / records_per_block;
  const std::uint64_t worker_count = spec.threads;
  std::uint64_t worker_bucket_slots = 0U;
  std::uint64_t local_buffer_blocks = 0U;
  std::uint64_t swap_blocks = 0U;
  std::uint64_t buffered_blocks = 0U;
  std::uint64_t buffered_records = 0U;
  std::uint64_t metadata_slots = 0U;
  std::uint64_t required_record_bytes = 0U;
  std::uint64_t required_owner_bytes = 0U;
  std::uint64_t required_metadata_bytes = 0U;
  std::uint64_t required_pool_bytes = 0U;
  std::uint64_t live_pool_bytes = 0U;
  std::uint64_t required_scratch_bytes = 0U;
  if (!limits::checked::checked_multiply(worker_count, kBuckets,
                                         &worker_bucket_slots) ||
      !limits::checked::checked_multiply(worker_count, kBuckets,
                                         &local_buffer_blocks) ||
      !limits::checked::checked_multiply(worker_count, 2U, &swap_blocks) ||
      !limits::checked::checked_add(local_buffer_blocks, kBuckets,
                                    &buffered_blocks) ||
      !limits::checked::checked_add(buffered_blocks, swap_blocks,
                                    &buffered_blocks) ||
      !limits::checked::checked_multiply(buffered_blocks, records_per_block,
                                         &buffered_records) ||
      !limits::checked::checked_multiply(buffered_records,
                                         sizeof(TerPartitionEntry),
                                         &required_record_bytes) ||
      !limits::checked::checked_multiply(physical_blocks,
                                         sizeof(std::uint16_t),
                                         &required_owner_bytes) ||
      !limits::checked::checked_multiply(worker_bucket_slots, 2U,
                                         &metadata_slots) ||
      !limits::checked::checked_add(metadata_slots, kBuckets + 1U,
                                    &metadata_slots) ||
      !limits::checked::checked_add(metadata_slots, 4U * kBuckets,
                                    &metadata_slots) ||
      !limits::checked::checked_multiply(metadata_slots, sizeof(std::uint64_t),
                                         &required_metadata_bytes) ||
      !limits::checked::checked_add(required_record_bytes, required_owner_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::checked_add(required_scratch_bytes,
                                    required_metadata_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::checked_add(required_scratch_bytes,
                                    sizeof(ParallelBlockInPlaceState),
                                    &required_scratch_bytes) ||
      !partition_worker_pool_scratch_bytes(
          parallel_current_scatter, static_cast<std::size_t>(worker_count),
          &required_pool_bytes, &live_pool_bytes) ||
      !limits::checked::checked_add(required_scratch_bytes,
                                    required_pool_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::fits_size_t(worker_count) ||
      !limits::checked::fits_size_t(worker_bucket_slots) ||
      !limits::checked::fits_size_t(buffered_records) ||
      !limits::checked::fits_size_t(physical_blocks) ||
      !limits::checked::fits_size_t(metadata_slots)) {
    return Status::ResourceLimit;
  }

  // The pool is shared private infrastructure; only CurrentScatter's inactive
  // record/histogram/worker tables are released at this ownership transition.
  release_current_scatter_storage();

  const auto measure_live_scratch = [&](LiveBlockScratch* const live) noexcept {
    *live = {};
    return capacity_bytes(block_records.capacity(), sizeof(TerPartitionEntry),
                          &live->records) &&
           capacity_bytes(block_owners.capacity(), sizeof(std::uint16_t),
                          &live->owners) &&
           capacity_bytes(block_metadata.capacity(), sizeof(std::uint64_t),
                          &live->metadata) &&
           (parallel_block_inplace == nullptr ||
            (live->parallel_state = sizeof(ParallelBlockInPlaceState), true)) &&
           limits::checked::checked_add(live->records, live->owners,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->metadata,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->parallel_state,
                                        &live->total) &&
           partition_worker_pool_scratch_bytes(
               parallel_current_scatter, 0U, &required_pool_bytes,
               &live_pool_bytes) &&
           limits::checked::checked_add(live->total, live_pool_bytes,
                                        &live->total);
  };

  LiveBlockScratch live_scratch;
  if (!measure_live_scratch(&live_scratch) ||
      (spec.scratch_cap_bytes != 0U &&
       (required_scratch_bytes > spec.scratch_cap_bytes ||
        live_scratch.total > spec.scratch_cap_bytes))) {
    return Status::ResourceLimit;
  }

#ifdef TER_ROOT_PARTITION_TESTING
  BlockInPlaceWorkerObservation observation;
#endif
  try {
    block_records.resize(static_cast<std::size_t>(buffered_records));
    block_owners.resize(static_cast<std::size_t>(physical_blocks));
    block_metadata.resize(static_cast<std::size_t>(metadata_slots));
    if (parallel_block_inplace == nullptr) {
      parallel_block_inplace = std::make_unique<ParallelBlockInPlaceState>();
    }
    prepare_partition_workers(parallel_current_scatter,
                              static_cast<std::size_t>(worker_count));
#ifdef TER_ROOT_PARTITION_TESTING
    observation.caller_thread = std::this_thread::get_id();
    observation.classification_thread_ids.assign(
        static_cast<std::size_t>(worker_count), {});
    observation.permutation_thread_ids.assign(
        static_cast<std::size_t>(worker_count), {});
#endif
  } catch (const std::bad_alloc&) {
    return Status::ResourceLimit;
  } catch (const std::length_error&) {
    return Status::ResourceLimit;
  } catch (const std::system_error&) {
    return Status::ResourceLimit;
  }

  if (!measure_live_scratch(&live_scratch) ||
      (spec.scratch_cap_bytes != 0U &&
       live_scratch.total > spec.scratch_cap_bytes)) {
    return Status::ResourceLimit;
  }

  const std::size_t workers = static_cast<std::size_t>(worker_count);
  const std::size_t block_size = static_cast<std::size_t>(records_per_block);
  const std::size_t worker_slots =
      static_cast<std::size_t>(worker_bucket_slots);
  TerPartitionEntry* const local_buffers = block_records.data();
  TerPartitionEntry* const overflow_buffers =
      local_buffers + static_cast<std::size_t>(local_buffer_blocks) * block_size;
  TerPartitionEntry* const swap_buffers =
      overflow_buffers + kBuckets * block_size;
  std::uint64_t* const worker_counts = block_metadata.data();
  std::uint64_t* const worker_partial_sizes = worker_counts + worker_slots;
  std::uint64_t* const private_offsets = worker_partial_sizes + worker_slots;
  std::uint64_t* const full_blocks = private_offsets + kBuckets + 1U;
  std::uint64_t* const target_begin = full_blocks + kBuckets;
  std::uint64_t* const target_end = target_begin + kBuckets;
  std::uint64_t* const overflow_sizes = target_end + kBuckets;

  std::fill(block_metadata.begin(), block_metadata.end(), 0U);
  std::fill(block_owners.begin(), block_owners.end(), kEmptyBlock);
  std::atomic<Status> phase_status{Status::Completed};
  const auto set_error = [&](const Status error) noexcept {
    Status expected = Status::Completed;
    phase_status.compare_exchange_strong(expected, error,
                                         std::memory_order_acq_rel);
  };

  const auto classification_started = Clock::now();
  const std::function<void(std::size_t)> classification_task =
      [&](const std::size_t worker) noexcept {
#ifdef TER_ROOT_PARTITION_TESTING
        observation.classification_thread_ids[worker] =
            std::this_thread::get_id();
#endif
        const auto [first_block, last_block] =
            partition_worker_stripe(static_cast<std::size_t>(physical_blocks),
                                    workers, worker);
        const std::size_t begin = first_block * block_size;
        std::size_t end = last_block * block_size;
        if (worker + 1U == workers) {
          end = entries.size();
        }
        std::uint64_t flushed = 0U;
        for (std::size_t input_index = begin; input_index < end; ++input_index) {
          const TerPartitionEntry entry = entries[input_index];
          const std::size_t bucket = static_cast<std::size_t>(
              (entry.low_k >> spec.shift) & 0xFFU);
          const std::size_t slot = worker * kBuckets + bucket;
          const std::size_t partial_size =
              static_cast<std::size_t>(worker_partial_sizes[slot]);
          local_buffers[slot * block_size + partial_size] = entry;
          ++worker_counts[slot];
          ++worker_partial_sizes[slot];
          if (worker_partial_sizes[slot] == records_per_block) {
            const std::uint64_t destination_block = first_block + flushed;
            if (destination_block >= last_block) {
              set_error(Status::InvariantViolation);
              return;
            }
            const std::size_t destination =
                static_cast<std::size_t>(destination_block) * block_size;
            std::copy_n(local_buffers + slot * block_size, block_size,
                        entries.begin() + static_cast<std::ptrdiff_t>(destination));
            block_owners[static_cast<std::size_t>(destination_block)] =
                static_cast<std::uint16_t>(bucket);
            ++flushed;
            worker_partial_sizes[slot] = 0U;
            if ((destination_block + 1U) * records_per_block >
                static_cast<std::uint64_t>(input_index) + 1U) {
              set_error(Status::InvariantViolation);
              return;
            }
          }
        }
      };
  try {
    run_partition_workers(parallel_current_scatter, workers,
                          classification_task);
  } catch (const std::bad_alloc&) {
    return Status::ResourceLimit;
  } catch (const std::system_error&) {
    return Status::ResourceLimit;
  }
  const std::uint64_t classification_ns = elapsed_ns(classification_started);
  if (phase_status.load(std::memory_order_acquire) != Status::Completed) {
    return phase_status.load(std::memory_order_relaxed);
  }

  private_offsets[0] = 0U;
  std::uint64_t flushed_blocks = 0U;
  std::uint64_t target_blocks = 0U;
  std::uint64_t overflow_blocks = 0U;
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    std::uint64_t count = 0U;
    std::uint64_t local_full_blocks = 0U;
    for (std::size_t worker = 0U; worker < workers; ++worker) {
      const std::uint64_t local_count =
          worker_counts[worker * kBuckets + bucket];
      if (!limits::checked::checked_add(count, local_count, &count) ||
          !limits::checked::checked_add(local_full_blocks,
                                        local_count / records_per_block,
                                        &local_full_blocks)) {
        return Status::ResourceLimit;
      }
    }
    if (!limits::checked::checked_add(private_offsets[bucket], count,
                                      &private_offsets[bucket + 1U]) ||
        !limits::checked::checked_add(flushed_blocks, local_full_blocks,
                                      &flushed_blocks)) {
      return Status::ResourceLimit;
    }
    full_blocks[bucket] = local_full_blocks;
  }
  if (private_offsets[kBuckets] != record_count) {
    return Status::InvariantViolation;
  }

  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    const std::uint64_t begin = private_offsets[bucket];
    const std::uint64_t end = private_offsets[bucket + 1U];
    const std::uint64_t first =
        begin / records_per_block +
        (begin % records_per_block == 0U ? 0U : 1U);
    const std::uint64_t last = end / records_per_block;
    const std::uint64_t interior = last >= first ? last - first : 0U;
    const std::uint64_t target = std::min(full_blocks[bucket], interior);
    if (full_blocks[bucket] - target > 1U ||
        !limits::checked::checked_add(target_blocks, target, &target_blocks)) {
      return Status::InvariantViolation;
    }
    target_begin[bucket] = first;
    target_end[bucket] = first + target;
    if (full_blocks[bucket] != target) {
      overflow_sizes[bucket] = records_per_block;
      ++overflow_blocks;
    }
    parallel_block_inplace->pointers[bucket].reset(target_begin[bucket],
                                                   target_end[bucket]);
  }
  if (target_blocks + overflow_blocks != flushed_blocks) {
    return Status::InvariantViolation;
  }

  std::uint64_t extracted_overflow = 0U;
  for (std::size_t source_block = 0U; source_block < block_owners.size();
       ++source_block) {
    const std::uint16_t owner = block_owners[source_block];
    if (owner >= kBuckets || overflow_sizes[owner] != records_per_block) {
      continue;
    }
    std::copy_n(entries.begin() +
                    static_cast<std::ptrdiff_t>(source_block * block_size),
                block_size,
                overflow_buffers + static_cast<std::size_t>(owner) * block_size);
    block_owners[source_block] = kEmptyBlock;
    overflow_sizes[owner] = 2U * records_per_block;
    ++extracted_overflow;
  }
  if (extracted_overflow != overflow_blocks) {
    return Status::InvariantViolation;
  }
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    overflow_sizes[bucket] /= 2U;
  }

  const auto target_bucket_for_block = [&](const std::uint64_t block) noexcept {
    const std::uint64_t position = block * records_per_block;
    const std::uint64_t* const upper =
        std::upper_bound(private_offsets, private_offsets + kBuckets + 1U,
                         position);
    if (upper == private_offsets) {
      return kBuckets;
    }
    const std::size_t bucket = static_cast<std::size_t>(upper - private_offsets - 1);
    return bucket < kBuckets && block >= target_begin[bucket] &&
                   block < target_end[bucket]
               ? bucket
               : kBuckets;
  };

  std::uint64_t progress_bound = 0U;
  if (!limits::checked::checked_multiply(flushed_blocks, 4U,
                                         &progress_bound) ||
      !limits::checked::checked_add(progress_bound, kBuckets,
                                    &progress_bound)) {
    return Status::ResourceLimit;
  }
  std::atomic<std::uint64_t> progress_steps{0U};
  std::atomic<std::uint64_t> blocks_permuted{0U};
  std::atomic<std::uint64_t> source_copies{0U};
  std::atomic<std::uint64_t> displaced_copies{0U};

  const auto permutation_started = Clock::now();
  const std::function<void(std::size_t)> permutation_task =
      [&](const std::size_t worker) noexcept {
#ifdef TER_ROOT_PARTITION_TESTING
        observation.permutation_thread_ids[worker] = std::this_thread::get_id();
#endif
        TerPartitionEntry* const worker_swaps =
            swap_buffers + worker * 2U * block_size;
        const auto route_block = [&](std::uint16_t destination_bucket) {
          std::size_t current_swap = 0U;
          while (destination_bucket < kBuckets &&
                 phase_status.load(std::memory_order_acquire) ==
                     Status::Completed) {
            const std::uint64_t step =
                progress_steps.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            if (step > progress_bound) {
              set_error(Status::InvariantViolation);
              return;
            }
            std::uint64_t destination_block = 0U;
            if (!parallel_block_inplace->pointers[destination_bucket]
                     .claim_write_block(&destination_block) ||
                destination_block >= physical_blocks) {
              set_error(Status::InvariantViolation);
              return;
            }
            const std::size_t destination =
                static_cast<std::size_t>(destination_block) * block_size;
            const std::uint16_t displaced_bucket =
                block_owners[static_cast<std::size_t>(destination_block)];
            const std::size_t writing_swap = current_swap;
            if (displaced_bucket < kBuckets) {
              const std::size_t next_swap = current_swap ^ 1U;
              std::copy_n(entries.begin() +
                              static_cast<std::ptrdiff_t>(destination),
                          block_size, worker_swaps + next_swap * block_size);
              displaced_copies.fetch_add(1U, std::memory_order_relaxed);
              current_swap = next_swap;
            } else if (displaced_bucket != kEmptyBlock) {
              set_error(Status::InvariantViolation);
              return;
            }
            std::copy_n(worker_swaps + writing_swap * block_size, block_size,
                        entries.begin() +
                            static_cast<std::ptrdiff_t>(destination));
            block_owners[static_cast<std::size_t>(destination_block)] =
                kFixedBlock;
            blocks_permuted.fetch_add(1U, std::memory_order_relaxed);
            destination_bucket = displaced_bucket;
          }
        };

        try {
          const auto [source_begin, source_end] = partition_worker_stripe(
              static_cast<std::size_t>(physical_blocks), workers, worker);
          for (std::size_t source_block = source_begin;
               source_block < source_end &&
               phase_status.load(std::memory_order_acquire) == Status::Completed;
               ++source_block) {
            std::uint16_t owner = block_owners[source_block];
            if (owner >= kBuckets ||
                target_bucket_for_block(source_block) < kBuckets) {
              continue;
            }
            std::copy_n(entries.begin() +
                            static_cast<std::ptrdiff_t>(source_block * block_size),
                        block_size, worker_swaps);
            block_owners[source_block] = kEmptyBlock;
            source_copies.fetch_add(1U, std::memory_order_relaxed);
            route_block(owner);
          }

          for (std::size_t bucket = worker;
               bucket < kBuckets &&
               phase_status.load(std::memory_order_acquire) == Status::Completed;
               bucket += workers) {
            std::uint64_t source_block = 0U;
            while (parallel_block_inplace->pointers[bucket].claim_read_block(
                       &source_block)) {
              const std::uint16_t owner =
                  block_owners[static_cast<std::size_t>(source_block)];
              if (owner < kBuckets) {
                std::copy_n(entries.begin() + static_cast<std::ptrdiff_t>(
                                                source_block * block_size),
                            block_size, worker_swaps);
                block_owners[static_cast<std::size_t>(source_block)] =
                    kEmptyBlock;
                source_copies.fetch_add(1U, std::memory_order_relaxed);
              } else if (owner != kEmptyBlock) {
                set_error(Status::InvariantViolation);
              }
              parallel_block_inplace->pointers[bucket].release_read_block();
              if (owner < kBuckets) {
                route_block(owner);
              }
              if (phase_status.load(std::memory_order_acquire) !=
                  Status::Completed) {
                break;
              }
            }
          }
        } catch (const std::system_error&) {
          set_error(Status::ResourceLimit);
        }
      };
  try {
    run_partition_workers(parallel_current_scatter, workers, permutation_task);
  } catch (const std::system_error&) {
    return Status::ResourceLimit;
  }
  const std::uint64_t permutation_ns = elapsed_ns(permutation_started);
  if (phase_status.load(std::memory_order_acquire) != Status::Completed) {
    return phase_status.load(std::memory_order_relaxed);
  }
  if (blocks_permuted.load(std::memory_order_relaxed) != target_blocks) {
    return Status::InvariantViolation;
  }
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    if (!parallel_block_inplace->pointers[bucket].complete()) {
      return Status::InvariantViolation;
    }
    for (std::uint64_t block = target_begin[bucket];
         block < target_end[bucket]; ++block) {
      if (block_owners[static_cast<std::size_t>(block)] != kFixedBlock) {
        return Status::InvariantViolation;
      }
    }
  }

  std::uint64_t repaired_records = 0U;
  const auto cleanup_started = Clock::now();
  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    std::size_t partial_worker = 0U;
    std::uint64_t partial_read = 0U;
    std::uint64_t overflow_read = 0U;
    const std::uint64_t fixed_begin = target_begin[bucket] * records_per_block;
    const std::uint64_t fixed_end = target_end[bucket] * records_per_block;
    for (std::uint64_t destination = private_offsets[bucket];
         destination < private_offsets[bucket + 1U]; ++destination) {
      if (destination >= fixed_begin && destination < fixed_end) {
        continue;
      }
      while (partial_worker < workers &&
             partial_read >=
                 worker_partial_sizes[partial_worker * kBuckets + bucket]) {
        ++partial_worker;
        partial_read = 0U;
      }
      TerPartitionEntry entry{};
      if (partial_worker < workers) {
        const std::size_t slot = partial_worker * kBuckets + bucket;
        entry = local_buffers[slot * block_size +
                              static_cast<std::size_t>(partial_read)];
        ++partial_read;
      } else if (overflow_read < overflow_sizes[bucket]) {
        entry = overflow_buffers[bucket * block_size +
                                 static_cast<std::size_t>(overflow_read)];
        ++overflow_read;
      } else {
        return Status::InvariantViolation;
      }
      entries[static_cast<std::size_t>(destination)] = entry;
      ++repaired_records;
    }
    while (partial_worker < workers &&
           partial_read >=
               worker_partial_sizes[partial_worker * kBuckets + bucket]) {
      ++partial_worker;
      partial_read = 0U;
    }
    if (partial_worker != workers || overflow_read != overflow_sizes[bucket]) {
      return Status::InvariantViolation;
    }
  }
  const std::uint64_t cleanup_ns = elapsed_ns(cleanup_started);
  if (repaired_records != record_count - target_blocks * records_per_block) {
    return Status::InvariantViolation;
  }

  for (std::size_t bucket = 0U; bucket < kBuckets; ++bucket) {
    for (std::uint64_t index = private_offsets[bucket];
         index < private_offsets[bucket + 1U]; ++index) {
      const std::uint32_t digit =
          (entries[static_cast<std::size_t>(index)].low_k >> spec.shift) & 0xFFU;
      if (digit != bucket) {
        return Status::InvariantViolation;
      }
    }
  }

  std::uint64_t logical_record_reads = record_count;
  std::uint64_t logical_record_writes = record_count;
  const auto add_block_io = [&](const std::uint64_t blocks) noexcept {
    std::uint64_t records = 0U;
    return limits::checked::checked_multiply(blocks, records_per_block,
                                             &records) &&
           add_record_io(records, &logical_record_reads) &&
           add_record_io(records, &logical_record_writes);
  };
  if (!add_block_io(flushed_blocks) || !add_block_io(overflow_blocks) ||
      !add_block_io(source_copies.load(std::memory_order_relaxed)) ||
      !add_block_io(displaced_copies.load(std::memory_order_relaxed)) ||
      !add_block_io(target_blocks) ||
      !add_record_io(repaired_records, &logical_record_reads) ||
      !add_record_io(repaired_records, &logical_record_writes) ||
      !add_record_io(record_count, &logical_record_reads)) {
    return Status::ResourceLimit;
  }
  std::uint64_t logical_bytes_read = 0U;
  std::uint64_t logical_bytes_written = 0U;
  if (!limits::checked::checked_multiply(logical_record_reads,
                                         sizeof(TerPartitionEntry),
                                         &logical_bytes_read) ||
      !limits::checked::checked_multiply(logical_record_writes,
                                         sizeof(TerPartitionEntry),
                                         &logical_bytes_written)) {
    return Status::ResourceLimit;
  }

  std::copy_n(private_offsets, kBuckets + 1U, bucket_offsets.begin());
#ifdef TER_ROOT_PARTITION_TESTING
  {
    std::lock_guard<std::mutex> lock(block_observation_mutex);
    last_block_observation = observation;
  }
#endif
  stats.local_classification_ns = classification_ns;
  stats.block_permute_ns = permutation_ns;
  stats.cleanup_ns = cleanup_ns;
  stats.records_classified = record_count;
  stats.records_written = record_count;
  stats.blocks_flushed = flushed_blocks;
  stats.blocks_permuted = target_blocks;
  stats.partial_records_repaired = repaired_records;
  stats.block_buffer_bytes = live_scratch.records;
  stats.offset_bytes =
      static_cast<std::uint64_t>(bucket_offsets.size()) * sizeof(std::uint64_t);
  stats.scratch_peak_bytes = live_scratch.total;
  stats.logical_bytes_read = logical_bytes_read;
  stats.logical_bytes_written = logical_bytes_written;
  return Status::Completed;
}

}  // namespace ter
