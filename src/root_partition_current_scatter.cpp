#include "root_partition_internal.hpp"

#include "ter/limitations.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
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

namespace {

using Clock = std::chrono::steady_clock;

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

#ifdef _MSC_VER
#pragma warning(push)
// The explicit cache-line alignment required by the parallel contract
// necessarily introduces tail padding on MSVC.
#pragma warning(disable : 4324)
#endif
struct alignas(64) PartitionWorkerState {
  std::vector<std::uint64_t> histogram;
  std::vector<std::uint64_t> offsets;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Donor-derived persistent synchronous pool. A workspace owns one pool, and
// only the active partition call's input and disjoint scratch ranges are
// visible to its workers.
class PartitionWorkerPool {
 public:
  PartitionWorkerPool() = default;
  PartitionWorkerPool(const PartitionWorkerPool&) = delete;
  PartitionWorkerPool& operator=(const PartitionWorkerPool&) = delete;

  ~PartitionWorkerPool() { stop(); }

  void run(const std::size_t worker_count,
           const std::function<void(std::size_t)>& task) {
    ensure_workers(worker_count);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = &task;
      completed_ = 0U;
      ++generation_;
    }
    work_ready_.notify_all();

    std::unique_lock<std::mutex> lock(mutex_);
    work_done_.wait(lock, [this] { return completed_ == workers_.size(); });
    task_ = nullptr;
  }

  void prepare(const std::size_t worker_count) { ensure_workers(worker_count); }

  [[nodiscard]] std::size_t worker_capacity() const noexcept {
    return workers_.capacity();
  }

 private:
  void ensure_workers(const std::size_t worker_count) {
    if (worker_count == workers_.size()) {
      return;
    }
    stop();
    if (worker_count == 0U) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = false;
      generation_ = 0U;
      completed_ = 0U;
      task_ = nullptr;
    }
    workers_.reserve(worker_count);
    try {
      for (std::size_t worker = 0U; worker < worker_count; ++worker) {
        workers_.emplace_back([this, worker] { worker_loop(worker); });
      }
    } catch (...) {
      stop();
      throw;
    }
  }

  void stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (workers_.empty()) {
        return;
      }
      stopping_ = true;
      ++generation_;
    }
    work_ready_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  void worker_loop(const std::size_t worker) noexcept {
    std::uint64_t observed_generation = 0U;
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      work_ready_.wait(lock, [this, &observed_generation] {
        return stopping_ || generation_ != observed_generation;
      });
      if (stopping_) {
        return;
      }
      observed_generation = generation_;
      const std::function<void(std::size_t)>* const task = task_;
      lock.unlock();
      (*task)(worker);
      lock.lock();
      ++completed_;
      if (completed_ == workers_.size()) {
        work_done_.notify_one();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable work_done_;
  std::vector<std::thread> workers_;
  const std::function<void(std::size_t)>* task_ = nullptr;
  std::uint64_t generation_ = 0U;
  std::size_t completed_ = 0U;
  bool stopping_ = false;
};

}  // namespace

std::pair<std::size_t, std::size_t> partition_worker_stripe(
    const std::size_t record_count,
    const std::size_t worker_count,
    const std::size_t worker) noexcept {
  const std::size_t base = record_count / worker_count;
  const std::size_t remainder = record_count % worker_count;
  const std::size_t begin = worker * base + std::min(worker, remainder);
  const std::size_t end = begin + base + (worker < remainder ? 1U : 0U);
  return {begin, end};
}

struct ParallelCurrentScatterState {
  std::vector<PartitionWorkerState> workers;
  PartitionWorkerPool pool;
#ifdef TER_ROOT_PARTITION_TESTING
  CurrentScatterWorkerObservation observation;
#endif
};

void run_partition_workers(
    std::unique_ptr<ParallelCurrentScatterState>& state,
    const std::size_t worker_count,
    const std::function<void(std::size_t)>& task) {
  if (state == nullptr) {
    state = std::make_unique<ParallelCurrentScatterState>();
  }
  state->pool.run(worker_count, task);
}

void prepare_partition_workers(
    std::unique_ptr<ParallelCurrentScatterState>& state,
    const std::size_t worker_count) {
  if (state == nullptr) {
    state = std::make_unique<ParallelCurrentScatterState>();
  }
  state->pool.prepare(worker_count);
}

bool partition_worker_pool_scratch_bytes(
    const std::unique_ptr<ParallelCurrentScatterState>& state,
    const std::size_t requested_workers,
    std::uint64_t* const required_bytes,
    std::uint64_t* const live_bytes) noexcept {
  *required_bytes = 0U;
  *live_bytes = 0U;
  if (requested_workers > 1U) {
    std::uint64_t thread_bytes = 0U;
    if (!capacity_bytes(requested_workers, sizeof(std::thread), &thread_bytes) ||
        !limits::checked::checked_add(sizeof(ParallelCurrentScatterState),
                                      thread_bytes, required_bytes)) {
      return false;
    }
  }
  if (state == nullptr) {
    return true;
  }
  std::uint64_t thread_bytes = 0U;
  return capacity_bytes(state->pool.worker_capacity(), sizeof(std::thread),
                        &thread_bytes) &&
         limits::checked::checked_add(sizeof(ParallelCurrentScatterState),
                                      thread_bytes, live_bytes);
}

#ifdef TER_ROOT_PARTITION_TESTING
namespace {

std::mutex observation_mutex;
CurrentScatterWorkerObservation last_worker_observation;

}  // namespace

CurrentScatterWorkerObservation current_scatter_worker_observation() {
  std::lock_guard<std::mutex> lock(observation_mutex);
  return last_worker_observation;
}
#endif

namespace {

struct LiveScratchBytes {
  std::uint64_t record = 0U;
  std::uint64_t global_histogram = 0U;
  std::uint64_t global_cursor = 0U;
  std::uint64_t worker_histograms = 0U;
  std::uint64_t worker_offsets = 0U;
  std::uint64_t worker_state_vector = 0U;
  std::uint64_t worker_pool = 0U;
  std::uint64_t total = 0U;
};

}  // namespace

PartitionWorkspace::Impl::Impl() = default;
PartitionWorkspace::Impl::~Impl() = default;

void PartitionWorkspace::Impl::release_current_scatter_storage() noexcept {
  std::vector<TerPartitionEntry>().swap(record_scratch);
  std::vector<std::uint64_t>().swap(histogram);
  std::vector<std::uint64_t>().swap(cursor);
  if (parallel_current_scatter != nullptr) {
    std::vector<PartitionWorkerState>().swap(parallel_current_scatter->workers);
  }
}

Status PartitionWorkspace::Impl::admit_trivial_current_scatter(
    const std::uint64_t scratch_cap_bytes,
    const std::uint64_t returned_offset_bytes,
    PartitionStats& stats) {
  release_block_inplace_storage();
  std::uint64_t total = 0U;
  std::uint64_t record_bytes = 0U;
  std::uint64_t histogram_bytes = 0U;
  std::uint64_t offset_bytes = returned_offset_bytes;
  std::uint64_t unreported_bytes = 0U;
  const auto add_capacity = [&](const std::size_t capacity,
                                const std::size_t bytes,
                                std::uint64_t* const component) noexcept {
    std::uint64_t allocation = 0U;
    return capacity_bytes(capacity, bytes, &allocation) &&
           limits::checked::checked_add(*component, allocation, component) &&
           limits::checked::checked_add(total, allocation, &total);
  };
  if (!add_capacity(record_scratch.capacity(), sizeof(TerPartitionEntry),
                    &record_bytes) ||
      !add_capacity(histogram.capacity(), sizeof(std::uint64_t),
                    &histogram_bytes) ||
      !add_capacity(cursor.capacity(), sizeof(std::uint64_t), &offset_bytes)) {
    return Status::ResourceLimit;
  }
  if (parallel_current_scatter != nullptr) {
    if (!add_capacity(parallel_current_scatter->workers.capacity(),
                      sizeof(PartitionWorkerState), &unreported_bytes)) {
      return Status::ResourceLimit;
    }
    for (const PartitionWorkerState& worker :
         parallel_current_scatter->workers) {
      if (!add_capacity(worker.histogram.capacity(), sizeof(std::uint64_t),
                        &histogram_bytes) ||
          !add_capacity(worker.offsets.capacity(), sizeof(std::uint64_t),
                        &offset_bytes)) {
        return Status::ResourceLimit;
      }
    }
  }
  std::uint64_t unused_required = 0U;
  std::uint64_t pool_live = 0U;
  if (!partition_worker_pool_scratch_bytes(parallel_current_scatter, 0U,
                                           &unused_required, &pool_live) ||
      !limits::checked::checked_add(total, pool_live, &total)) {
    return Status::ResourceLimit;
  }
  if (scratch_cap_bytes != 0U && total > scratch_cap_bytes) {
    return Status::ResourceLimit;
  }
  stats.record_scratch_bytes = record_bytes;
  stats.histogram_bytes = histogram_bytes;
  stats.offset_bytes = offset_bytes;
  stats.scratch_peak_bytes = total;
  return Status::Completed;
}

Status PartitionWorkspace::Impl::partition_current_scatter(
    const std::span<TerPartitionEntry> entries,
    const PartitionSpec& spec,
    const std::span<std::uint64_t> bucket_offsets,
    PartitionStats& stats) {
  // A backend switch transfers scratch ownership. This occurs before cap
  // admission and before caller records or returned offsets can be changed.
  release_block_inplace_storage();

  const std::uint64_t bucket_count = std::uint64_t{1} << spec.bits;
  const std::uint64_t worker_count = spec.threads;
  const bool use_parallel = worker_count > 1U;
  std::uint64_t record_bytes = 0U;
  std::uint64_t histogram_bytes = 0U;
  std::uint64_t cursor_bytes = 0U;
  std::uint64_t returned_offset_bytes = 0U;
  std::uint64_t worker_bucket_slots = 0U;
  std::uint64_t worker_histogram_bytes = 0U;
  std::uint64_t worker_offset_bytes = 0U;
  std::uint64_t worker_state_bytes = 0U;
  std::uint64_t worker_pool_required_bytes = 0U;
  std::uint64_t worker_pool_live_bytes = 0U;
  std::uint64_t required_scratch_bytes = 0U;
  std::uint64_t logical_bytes_read = 0U;
  std::uint64_t logical_bytes_written = 0U;
  if (!limits::checked::checked_multiply(entries.size(), sizeof(TerPartitionEntry),
                                         &record_bytes) ||
      !limits::checked::checked_multiply(bucket_count, sizeof(std::uint64_t),
                                         &histogram_bytes) ||
      !limits::checked::checked_multiply(bucket_count, sizeof(std::uint64_t),
                                         &cursor_bytes) ||
      !limits::checked::checked_multiply(bucket_count + 1U, sizeof(std::uint64_t),
                                         &returned_offset_bytes) ||
      !limits::checked::checked_add(record_bytes, histogram_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::checked_add(required_scratch_bytes, cursor_bytes,
                                    &required_scratch_bytes) ||
      !limits::checked::checked_multiply(record_bytes, 3U, &logical_bytes_read) ||
      !limits::checked::checked_multiply(record_bytes, 2U, &logical_bytes_written) ||
      !limits::checked::fits_size_t(bucket_count) ||
      !limits::checked::fits_size_t(entries.size())) {
    return Status::ResourceLimit;
  }
  if (use_parallel &&
      (!limits::checked::checked_multiply(bucket_count, worker_count,
                                          &worker_bucket_slots) ||
       !limits::checked::checked_multiply(worker_bucket_slots, sizeof(std::uint64_t),
                                          &worker_histogram_bytes) ||
       !limits::checked::checked_multiply(worker_bucket_slots, sizeof(std::uint64_t),
                                          &worker_offset_bytes) ||
       !limits::checked::checked_multiply(worker_count, sizeof(PartitionWorkerState),
                                          &worker_state_bytes) ||
       !limits::checked::checked_add(required_scratch_bytes, worker_histogram_bytes,
                                     &required_scratch_bytes) ||
       !limits::checked::checked_add(required_scratch_bytes, worker_offset_bytes,
                                     &required_scratch_bytes) ||
       !limits::checked::checked_add(required_scratch_bytes, worker_state_bytes,
                                     &required_scratch_bytes) ||
       !partition_worker_pool_scratch_bytes(
           parallel_current_scatter, static_cast<std::size_t>(worker_count),
           &worker_pool_required_bytes, &worker_pool_live_bytes) ||
       !limits::checked::checked_add(required_scratch_bytes,
                                     worker_pool_required_bytes,
                                     &required_scratch_bytes) ||
       !limits::checked::fits_size_t(worker_count))) {
    return Status::ResourceLimit;
  }

  // Scratch-cap scope is every live algorithm-owned vector allocation. This
  // includes the persistent pool object and retained std::thread handles, but
  // not implementation-defined OS thread stacks.
  const auto measure_live_scratch = [&](LiveScratchBytes* const live) noexcept {
    *live = {};
    if (!capacity_bytes(record_scratch.capacity(), sizeof(TerPartitionEntry),
                        &live->record) ||
        !capacity_bytes(histogram.capacity(), sizeof(std::uint64_t),
                        &live->global_histogram) ||
        !capacity_bytes(cursor.capacity(), sizeof(std::uint64_t),
                        &live->global_cursor)) {
      return false;
    }
    if (parallel_current_scatter != nullptr) {
      if (!capacity_bytes(parallel_current_scatter->workers.capacity(),
                          sizeof(PartitionWorkerState), &live->worker_state_vector)) {
        return false;
      }
      for (const PartitionWorkerState& worker : parallel_current_scatter->workers) {
        std::uint64_t bytes = 0U;
        if (!capacity_bytes(worker.histogram.capacity(), sizeof(std::uint64_t), &bytes) ||
            !limits::checked::checked_add(live->worker_histograms, bytes,
                                          &live->worker_histograms) ||
            !capacity_bytes(worker.offsets.capacity(), sizeof(std::uint64_t), &bytes) ||
            !limits::checked::checked_add(live->worker_offsets, bytes,
                                          &live->worker_offsets)) {
          return false;
        }
      }
    }
    std::uint64_t unused_required = 0U;
    if (!partition_worker_pool_scratch_bytes(parallel_current_scatter, 0U,
                                             &unused_required,
                                             &live->worker_pool)) {
      return false;
    }
    return limits::checked::checked_add(live->record, live->global_histogram,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->global_cursor,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->worker_histograms,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->worker_offsets,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->worker_state_vector,
                                        &live->total) &&
           limits::checked::checked_add(live->total, live->worker_pool,
                                        &live->total);
  };

  LiveScratchBytes live_scratch;
  if (!measure_live_scratch(&live_scratch) ||
      (spec.scratch_cap_bytes != 0U &&
       (required_scratch_bytes > spec.scratch_cap_bytes ||
        live_scratch.total > spec.scratch_cap_bytes))) {
    return Status::ResourceLimit;
  }

  try {
    record_scratch.resize(entries.size());
    histogram.resize(static_cast<std::size_t>(bucket_count));
    cursor.resize(static_cast<std::size_t>(bucket_count));
    if (use_parallel) {
      if (parallel_current_scatter == nullptr) {
        parallel_current_scatter = std::make_unique<ParallelCurrentScatterState>();
      }
      parallel_current_scatter->workers.resize(static_cast<std::size_t>(worker_count));
      for (PartitionWorkerState& worker : parallel_current_scatter->workers) {
        worker.histogram.resize(static_cast<std::size_t>(bucket_count));
        worker.offsets.resize(static_cast<std::size_t>(bucket_count));
      }
      prepare_partition_workers(parallel_current_scatter,
                                static_cast<std::size_t>(worker_count));
    }
  } catch (const std::bad_alloc&) {
    return Status::ResourceLimit;
  } catch (const std::length_error&) {
    return Status::ResourceLimit;
  } catch (const std::system_error&) {
    return Status::ResourceLimit;
  }

  std::uint64_t reported_histogram_bytes = 0U;
  std::uint64_t reported_offset_bytes = 0U;
  if (!measure_live_scratch(&live_scratch) ||
      !limits::checked::checked_add(live_scratch.global_histogram,
                                    live_scratch.worker_histograms,
                                    &reported_histogram_bytes) ||
      !limits::checked::checked_add(returned_offset_bytes, live_scratch.global_cursor,
                                    &reported_offset_bytes) ||
      !limits::checked::checked_add(reported_offset_bytes, live_scratch.worker_offsets,
                                    &reported_offset_bytes) ||
      (spec.scratch_cap_bytes != 0U && live_scratch.total > spec.scratch_cap_bytes)) {
    return Status::ResourceLimit;
  }

  const std::uint32_t mask = (std::uint32_t{1} << spec.bits) - 1U;
  if (!use_parallel) {
    const auto histogram_started = Clock::now();
    std::fill(histogram.begin(), histogram.end(), 0U);
    for (const TerPartitionEntry& entry : entries) {
      const std::uint32_t digit = (entry.low_k >> spec.shift) & mask;
      ++histogram[static_cast<std::size_t>(digit)];
    }
    stats.histogram_ns = elapsed_ns(histogram_started);

    const auto prefix_started = Clock::now();
    std::uint64_t prefix = 0U;
    for (std::size_t bucket = 0U; bucket < histogram.size(); ++bucket) {
      bucket_offsets[bucket] = prefix;
      prefix += histogram[bucket];
      cursor[bucket] = bucket_offsets[bucket];
    }
    bucket_offsets.back() = prefix;
    stats.prefix_ns = elapsed_ns(prefix_started);

    const auto scatter_started = Clock::now();
    // Adapted from the donor's serial radix histogram/prefix/scatter pass, but
    // movement always copies the complete TER record.
    for (const TerPartitionEntry& entry : entries) {
      const std::uint32_t digit = (entry.low_k >> spec.shift) & mask;
      record_scratch[static_cast<std::size_t>(cursor[static_cast<std::size_t>(digit)]++)] = entry;
    }
    std::copy(record_scratch.begin(), record_scratch.end(), entries.begin());
    stats.scatter_ns = elapsed_ns(scatter_started);
  } else {
    const std::size_t parallel_workers = static_cast<std::size_t>(worker_count);
    const std::size_t record_count = entries.size();
    try {
#ifdef TER_ROOT_PARTITION_TESTING
      parallel_current_scatter->observation.caller_thread = std::this_thread::get_id();
      parallel_current_scatter->observation.count_thread_ids.assign(parallel_workers, {});
      parallel_current_scatter->observation.scatter_thread_ids.assign(parallel_workers, {});
#endif
      const std::function<void(std::size_t)> count_task = [&](const std::size_t worker_index) {
        PartitionWorkerState& worker = parallel_current_scatter->workers[worker_index];
#ifdef TER_ROOT_PARTITION_TESTING
        parallel_current_scatter->observation.count_thread_ids[worker_index] =
            std::this_thread::get_id();
#endif
        std::fill(worker.histogram.begin(), worker.histogram.end(), 0U);
        const auto [begin, end] =
            partition_worker_stripe(record_count, parallel_workers, worker_index);
        for (std::size_t index = begin; index < end; ++index) {
          const std::uint32_t digit = (entries[index].low_k >> spec.shift) & mask;
          ++worker.histogram[static_cast<std::size_t>(digit)];
        }
      };
      const std::function<void(std::size_t)> scatter_task = [&](const std::size_t worker_index) {
        PartitionWorkerState& worker = parallel_current_scatter->workers[worker_index];
#ifdef TER_ROOT_PARTITION_TESTING
        parallel_current_scatter->observation.scatter_thread_ids[worker_index] =
            std::this_thread::get_id();
#endif
        const auto [begin, end] =
            partition_worker_stripe(record_count, parallel_workers, worker_index);
        for (std::size_t index = begin; index < end; ++index) {
          const TerPartitionEntry entry = entries[index];
          const std::uint32_t digit = (entry.low_k >> spec.shift) & mask;
          record_scratch[static_cast<std::size_t>(
              worker.offsets[static_cast<std::size_t>(digit)]++)] = entry;
        }
      };

      const auto histogram_started = Clock::now();
      parallel_current_scatter->pool.run(parallel_workers, count_task);
      stats.histogram_ns = elapsed_ns(histogram_started);

      const auto prefix_started = Clock::now();
      std::uint64_t global_offset = 0U;
      for (std::size_t bucket = 0U; bucket < histogram.size(); ++bucket) {
        cursor[bucket] = global_offset;
        histogram[bucket] = 0U;
        for (std::size_t worker_index = 0U; worker_index < parallel_workers;
             ++worker_index) {
          PartitionWorkerState& worker = parallel_current_scatter->workers[worker_index];
          worker.offsets[bucket] = global_offset;
          const std::uint64_t count = worker.histogram[bucket];
          histogram[bucket] += count;
          global_offset += count;
        }
      }
      stats.prefix_ns = elapsed_ns(prefix_started);
      if (global_offset != entries.size()) {
        return Status::InvariantViolation;
      }

      const auto scatter_started = Clock::now();
      parallel_current_scatter->pool.run(parallel_workers, scatter_task);
#ifdef TER_ROOT_PARTITION_TESTING
      {
        std::lock_guard<std::mutex> lock(observation_mutex);
        last_worker_observation = parallel_current_scatter->observation;
      }
#endif
      std::copy(cursor.begin(), cursor.end(), bucket_offsets.begin());
      bucket_offsets.back() = global_offset;
      std::copy(record_scratch.begin(), record_scratch.end(), entries.begin());
      stats.scatter_ns = elapsed_ns(scatter_started);
    } catch (const std::bad_alloc&) {
      return Status::ResourceLimit;
    } catch (const std::system_error&) {
      return Status::ResourceLimit;
    }
  }

  stats.records_classified = entries.size();
  stats.records_written = entries.size();
  stats.record_scratch_bytes = live_scratch.record;
  stats.histogram_bytes = reported_histogram_bytes;
  stats.offset_bytes = reported_offset_bytes;
  stats.scratch_peak_bytes = live_scratch.total;
  stats.logical_bytes_read = logical_bytes_read;
  stats.logical_bytes_written = logical_bytes_written;
  return Status::Completed;
}

}  // namespace ter
