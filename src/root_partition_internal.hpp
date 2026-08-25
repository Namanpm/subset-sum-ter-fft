#pragma once

#include "ter/root_partition.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#ifdef TER_ROOT_PARTITION_TESTING
#include <thread>
#endif
#include <cstdint>
#include <utility>
#include <vector>

namespace ter {

struct ParallelCurrentScatterState;

struct BucketBlockPointers {
  std::uint64_t write_block = 0U;
  std::uint64_t read_block = 0U;
  std::uint64_t end_block = 0U;
  std::mutex pointer_mutex;
  std::condition_variable pointer_ready;
  std::atomic<std::uint32_t> readers{0U};

  void reset(std::uint64_t begin, std::uint64_t end);
  [[nodiscard]] bool claim_read_block(std::uint64_t* block);
  void release_read_block();
  [[nodiscard]] bool claim_write_block(std::uint64_t* block);
  [[nodiscard]] bool is_reading() const noexcept;
  [[nodiscard]] bool complete();
};

struct ParallelBlockInPlaceState {
  std::array<BucketBlockPointers, 256U> pointers;
};

void run_partition_workers(
    std::unique_ptr<ParallelCurrentScatterState>& state,
    std::size_t worker_count,
    const std::function<void(std::size_t)>& task);
void prepare_partition_workers(
    std::unique_ptr<ParallelCurrentScatterState>& state,
    std::size_t worker_count);
[[nodiscard]] bool partition_worker_pool_scratch_bytes(
    const std::unique_ptr<ParallelCurrentScatterState>& state,
    std::size_t requested_workers, std::uint64_t* required_bytes,
    std::uint64_t* live_bytes) noexcept;
[[nodiscard]] std::pair<std::size_t, std::size_t> partition_worker_stripe(
    std::size_t item_count, std::size_t worker_count,
    std::size_t worker) noexcept;

#ifdef TER_ROOT_PARTITION_TESTING
struct CurrentScatterWorkerObservation {
  std::thread::id caller_thread;
  std::vector<std::thread::id> count_thread_ids;
  std::vector<std::thread::id> scatter_thread_ids;
};

[[nodiscard]] CurrentScatterWorkerObservation current_scatter_worker_observation();

struct BlockInPlaceWorkerObservation {
  std::thread::id caller_thread;
  std::vector<std::thread::id> classification_thread_ids;
  std::vector<std::thread::id> permutation_thread_ids;
};

[[nodiscard]] BlockInPlaceWorkerObservation block_inplace_worker_observation();
#endif

// Private workspace storage for the Stage-C partition backends.  Keeping this
// definition out of the public header preserves the frozen pImpl ABI.
struct PartitionWorkspace::Impl {
  Impl();
  ~Impl();

  std::vector<TerPartitionEntry> record_scratch;
  std::vector<std::uint64_t> histogram;
  std::vector<std::uint64_t> cursor;
  std::unique_ptr<ParallelCurrentScatterState> parallel_current_scatter;
  std::unique_ptr<ParallelBlockInPlaceState> parallel_block_inplace;

  // BlockInPlace keeps bounded record buffers plus one owner word per physical
  // input block. Cross-backend calls release the inactive backend's retained
  // vectors before applying a nonzero scratch cap.
  std::vector<TerPartitionEntry> block_records;
  std::vector<std::uint16_t> block_owners;
  std::vector<std::uint64_t> block_metadata;

  void release_current_scatter_storage() noexcept;
  void release_block_inplace_storage() noexcept;
  [[nodiscard]] Status admit_trivial_current_scatter(
      std::uint64_t scratch_cap_bytes, std::uint64_t returned_offset_bytes,
      PartitionStats& stats);
  [[nodiscard]] Status admit_trivial_block_inplace(
      std::uint64_t scratch_cap_bytes, std::uint64_t returned_offset_bytes,
      PartitionStats& stats);

  [[nodiscard]] Status partition_current_scatter(std::span<TerPartitionEntry> entries,
                                                  const PartitionSpec& spec,
                                                  std::span<std::uint64_t> bucket_offsets,
                                                  PartitionStats& stats);
  [[nodiscard]] Status partition_block_inplace(std::span<TerPartitionEntry> entries,
                                                const PartitionSpec& spec,
                                                std::span<std::uint64_t> bucket_offsets,
                                                PartitionStats& stats);
  [[nodiscard]] Status partition_block_inplace_parallel(
      std::span<TerPartitionEntry> entries, const PartitionSpec& spec,
      std::span<std::uint64_t> bucket_offsets, PartitionStats& stats);
};

}  // namespace ter
