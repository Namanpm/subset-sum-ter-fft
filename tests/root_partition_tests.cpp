#include "ter/root_partition.hpp"
#include "ter_root_capture_adapter.hpp"
#ifdef TER_ROOT_PARTITION_TESTING
#include "root_partition_internal.hpp"
#endif

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <set>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void test_partition_entry_layout() {
  static_assert(sizeof(ter::TerPartitionEntry) == 8);
  static_assert(std::is_trivially_copyable_v<ter::TerPartitionEntry>);
}

void test_workspace_lifecycle() {
  ter::PartitionWorkspace first;
  ter::PartitionWorkspace second(std::move(first));
  ter::PartitionWorkspace third;
  third = std::move(second);
}

std::vector<std::uint64_t> offsets_for(const std::uint8_t bits) {
  return std::vector<std::uint64_t>((std::uint64_t{1} << bits) + 1U, 99U);
}

std::uint32_t independent_quotient_low32(
    const std::span<const std::uint32_t> residue,
    const std::span<const std::uint32_t> child_target,
    const std::uint32_t n,
    const std::uint32_t r1) {
  if (r1 >= n) {
    return 0U;
  }
  const std::size_t word_count = (static_cast<std::size_t>(n) + 31U) / 32U;
  std::vector<std::uint32_t> delta(word_count, 0U);
  std::uint64_t borrow = 0U;
  for (std::size_t word = 0U; word < word_count; ++word) {
    std::uint32_t residue_word = word < residue.size() ? residue[word] : 0U;
    std::uint32_t target_word = word < child_target.size() ? child_target[word] : 0U;
    if (word + 1U == word_count && (n % 32U) != 0U) {
      const std::uint32_t mask = (std::uint32_t{1} << (n % 32U)) - 1U;
      residue_word &= mask;
      target_word &= mask;
    }
    const std::uint64_t subtrahend = static_cast<std::uint64_t>(target_word) + borrow;
    delta[word] = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(residue_word) - subtrahend);
    borrow = static_cast<std::uint64_t>(residue_word) < subtrahend ? 1U : 0U;
  }
  if ((n % 32U) != 0U) {
    delta.back() &= (std::uint32_t{1} << (n % 32U)) - 1U;
  }

  std::uint32_t quotient = 0U;
  const std::uint32_t quotient_bits = std::min<std::uint32_t>(32U, n - r1);
  for (std::uint32_t bit = 0U; bit < quotient_bits; ++bit) {
    const std::uint32_t source_bit = r1 + bit;
    const std::uint32_t value =
        (delta[source_bit / 32U] >> (source_bit % 32U)) & 1U;
    quotient |= value << bit;
  }
  return quotient;
}

void test_captured_root_low32_projection() {
  struct Case {
    std::uint32_t n;
    std::uint32_t r1;
  };
  constexpr std::array<Case, 10> cases{{
      {17U, 0U}, {31U, 7U}, {32U, 8U}, {33U, 15U}, {63U, 16U},
      {64U, 25U}, {65U, 31U}, {72U, 32U}, {95U, 33U}, {97U, 63U},
  }};
  for (const auto [n, r1] : cases) {
    const std::size_t words = (static_cast<std::size_t>(n) + 31U) / 32U;
    std::vector<std::uint32_t> residue(words, 0U);
    std::vector<std::uint32_t> target(words, 0U);
    for (std::size_t word = 0U; word < words; ++word) {
      residue[word] = 0x13579BDFU ^ static_cast<std::uint32_t>(word * 0x10203041U);
      target[word] = 0xECA86420U ^ static_cast<std::uint32_t>(word * 0x55667789U);
    }
    // Dirty unused high bits prove that the n-bit modulus, not container width,
    // controls a non-word-aligned subtraction.
    residue.back() |= 0xF0000000U;
    target.back() |= 0xA0000000U;
    const auto expected = independent_quotient_low32(residue, target, n, r1);
    const auto actual = ter::root_partition_bench::quotient_low32_from_words(
        residue, target, n, r1);
    require(actual == expected,
            "captured-root projection matches independent arbitrary-width oracle");
  }

  const std::array<std::uint32_t, 3> residue{0U, 0U, 0U};
  const std::array<std::uint32_t, 3> target{1U, 0U, 0U};
  for (const std::uint32_t r1 : std::array<std::uint32_t, 3>{95U, 96U, 127U}) {
    require(ter::root_partition_bench::quotient_low32_from_words(
                residue, target, 95U, r1) == 0U,
            "captured-root projection handles shifts at or above n");
  }
}

void test_captured_root_record_conversion() {
  ter::FullTerRootCapture capture;
  capture.status = ter::Status::Completed;
  capture.parameters.n = 65U;
  capture.parameters.r1 = 33U;
  capture.children[0] = {
      ter::ReferenceTernaryVector(65U, 0),
      ter::ReferenceTernaryVector(65U, 1),
      ter::ReferenceTernaryVector(65U, -1),
  };
  capture.child_targets_words_le[0] = {0xFFFFFFFFU, 0x12345678U, 1U};
  capture.residues_words_le[0] = {
      {0U, 0U, 0U},
      {0xFFFFFFFFU, 0x12345678U, 1U},
      {0x01234567U, 0x89ABCDEFU, 0U},
  };
  const auto converted =
      ter::root_partition_bench::capture_list_to_records(capture, 0U);
  require(converted.status == ter::Status::Completed && converted.records.size() == 3U,
          "well-formed captured-root list converts exactly");
  for (std::size_t index = 0U; index < converted.records.size(); ++index) {
    require(converted.records[index].entry_id == static_cast<ter::EntryId>(index),
            "captured-root conversion preserves original list index as EntryId");
    require(converted.records[index].low_k == independent_quotient_low32(
                capture.residues_words_le[0][index], capture.child_targets_words_le[0],
                capture.parameters.n, capture.parameters.r1),
            "captured-root conversion matches independent quotient oracle");
  }

  require(ter::root_partition_bench::capture_list_size_fits_entry_id(
              std::numeric_limits<ter::EntryId>::max()) &&
              !ter::root_partition_bench::capture_list_size_fits_entry_id(
                  static_cast<std::uint64_t>(
                      std::numeric_limits<ter::EntryId>::max()) + 1U),
          "capture conversion rejects EntryId overflow before narrowing");

  auto malformed = capture;
  malformed.residues_words_le[0].pop_back();
  require(ter::root_partition_bench::capture_list_to_records(malformed, 0U).status ==
              ter::Status::InvalidConfiguration,
          "capture conversion rejects child/residue size mismatch");
  malformed = capture;
  malformed.residues_words_le[0][0][2] = 2U;
  require(ter::root_partition_bench::capture_list_to_records(malformed, 0U).status ==
              ter::Status::InvalidConfiguration,
          "capture conversion rejects bits above a partial n-bit top word");
  malformed = capture;
  malformed.status = ter::Status::AttemptLimitReached;
  require(ter::root_partition_bench::capture_list_to_records(malformed, 0U).status ==
              ter::Status::InvalidConfiguration,
          "capture conversion refuses a non-Completed capture status");
}

std::uint32_t bucket_for(const ter::TerPartitionEntry& entry,
                         const ter::PartitionSpec& spec) {
  const std::uint32_t mask = (std::uint32_t{1} << spec.bits) - 1U;
  return (entry.low_k >> spec.shift) & mask;
}

void require_partition_postconditions(const std::vector<ter::TerPartitionEntry>& before,
                                      const std::vector<ter::TerPartitionEntry>& entries,
                                      const ter::PartitionSpec& spec,
                                      const std::vector<std::uint64_t>& offsets) {
  require(offsets.front() == 0U, "CSR starts at zero");
  require(offsets.back() == entries.size(), "CSR ends at the record count");
  require(std::is_sorted(offsets.begin(), offsets.end()), "CSR offsets are monotone");

  std::vector<ter::EntryId> before_ids;
  std::vector<ter::EntryId> after_ids;
  std::vector<std::pair<ter::QuotientResidue, ter::EntryId>> before_records;
  std::vector<std::pair<ter::QuotientResidue, ter::EntryId>> after_records;
  before_ids.reserve(before.size());
  after_ids.reserve(entries.size());
  before_records.reserve(before.size());
  after_records.reserve(entries.size());
  for (const auto& entry : before) {
    before_ids.push_back(entry.entry_id);
    before_records.emplace_back(entry.low_k, entry.entry_id);
  }
  for (const auto& entry : entries) {
    after_ids.push_back(entry.entry_id);
    after_records.emplace_back(entry.low_k, entry.entry_id);
  }
  std::sort(before_ids.begin(), before_ids.end());
  std::sort(after_ids.begin(), after_ids.end());
  std::sort(before_records.begin(), before_records.end());
  std::sort(after_records.begin(), after_records.end());
  require(before_ids == after_ids, "partition preserves the EntryId multiset");
  require(before_records == after_records, "partition preserves full record pairs");

  for (std::size_t bucket = 0; bucket + 1U < offsets.size(); ++bucket) {
    for (std::uint64_t index = offsets[bucket]; index < offsets[bucket + 1U]; ++index) {
      require(bucket_for(entries[static_cast<std::size_t>(index)], spec) == bucket,
              "every CSR span contains only its recomputed bucket digit");
    }
  }
}

void require_exact_entries(const std::vector<ter::TerPartitionEntry>& left,
                           const std::vector<ter::TerPartitionEntry>& right,
                           const std::string_view message) {
  require(left.size() == right.size(), message);
  for (std::size_t index = 0; index < left.size(); ++index) {
    require(left[index].low_k == right[index].low_k &&
                left[index].entry_id == right[index].entry_id,
            message);
  }
}

void require_equal_bucket_id_sets(const std::vector<ter::TerPartitionEntry>& serial_entries,
                                  const std::vector<ter::TerPartitionEntry>& parallel_entries,
                                  const std::vector<std::uint64_t>& offsets) {
  for (std::size_t bucket = 0; bucket + 1U < offsets.size(); ++bucket) {
    std::vector<ter::EntryId> serial_ids;
    std::vector<ter::EntryId> parallel_ids;
    const std::size_t begin = static_cast<std::size_t>(offsets[bucket]);
    const std::size_t end = static_cast<std::size_t>(offsets[bucket + 1U]);
    serial_ids.reserve(end - begin);
    parallel_ids.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
      serial_ids.push_back(serial_entries[index].entry_id);
      parallel_ids.push_back(parallel_entries[index].entry_id);
    }
    std::sort(serial_ids.begin(), serial_ids.end());
    std::sort(parallel_ids.begin(), parallel_ids.end());
    require(serial_ids == parallel_ids,
            "parallel CurrentScatter preserves each bucket's EntryId set");
  }
}

void require_equal_bucket_record_sets(
    const std::vector<ter::TerPartitionEntry>& reference_entries,
    const std::vector<ter::TerPartitionEntry>& candidate_entries,
    const std::vector<std::uint64_t>& offsets) {
  for (std::size_t bucket = 0U; bucket + 1U < offsets.size(); ++bucket) {
    std::vector<std::pair<ter::QuotientResidue, ter::EntryId>> reference_records;
    std::vector<std::pair<ter::QuotientResidue, ter::EntryId>> candidate_records;
    const std::size_t begin = static_cast<std::size_t>(offsets[bucket]);
    const std::size_t end = static_cast<std::size_t>(offsets[bucket + 1U]);
    reference_records.reserve(end - begin);
    candidate_records.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
      reference_records.emplace_back(reference_entries[index].low_k,
                                     reference_entries[index].entry_id);
      candidate_records.emplace_back(candidate_entries[index].low_k,
                                     candidate_entries[index].entry_id);
    }
    std::sort(reference_records.begin(), reference_records.end());
    std::sort(candidate_records.begin(), candidate_records.end());
    require(reference_records == candidate_records,
            "BlockInPlace preserves each bucket's complete record multiset");
  }
}

void require_equal_logical_counters(const ter::PartitionStats& first,
                                    const ter::PartitionStats& second,
                                    const std::string_view message) {
  require(first.records_classified == second.records_classified &&
              first.records_written == second.records_written &&
              first.logical_bytes_read == second.logical_bytes_read &&
              first.logical_bytes_written == second.logical_bytes_written,
          message);
}

#ifdef TER_ROOT_PARTITION_TESTING
void require_parallel_worker_observation(
    const ter::CurrentScatterWorkerObservation& observation,
    const std::uint32_t requested_workers) {
  require(observation.count_thread_ids.size() == requested_workers &&
              observation.scatter_thread_ids.size() == requested_workers,
          "parallel observer records every requested worker in both phases");
  require(observation.caller_thread == std::this_thread::get_id(),
          "parallel observer records the calling test thread");
  std::set<std::thread::id> distinct_workers;
  for (std::size_t worker = 0U; worker < requested_workers; ++worker) {
    require(observation.count_thread_ids[worker] != observation.caller_thread,
            "count phase executes on a noncaller pool worker");
    require(observation.scatter_thread_ids[worker] != observation.caller_thread,
            "scatter phase executes on a noncaller pool worker");
    require(observation.count_thread_ids[worker] == observation.scatter_thread_ids[worker],
            "the same persistent worker executes both phases for its worker index");
    distinct_workers.insert(observation.count_thread_ids[worker]);
  }
  require(distinct_workers.size() == requested_workers,
          "requested worker indices execute on distinct pool threads");
}

void require_parallel_block_worker_observation(
    const ter::BlockInPlaceWorkerObservation& observation,
    const std::uint32_t requested_workers) {
  require(observation.classification_thread_ids.size() == requested_workers &&
              observation.permutation_thread_ids.size() == requested_workers,
          "parallel BlockInPlace observer records every requested worker phase");
  require(observation.caller_thread == std::this_thread::get_id(),
          "parallel BlockInPlace observer records the calling test thread");
  std::set<std::thread::id> classification_workers;
  std::set<std::thread::id> permutation_workers;
  for (std::size_t worker = 0U; worker < requested_workers; ++worker) {
    require(observation.classification_thread_ids[worker] != observation.caller_thread,
            "BlockInPlace classification executes on a noncaller pool worker");
    require(observation.permutation_thread_ids[worker] != observation.caller_thread,
            "BlockInPlace permutation executes on a noncaller pool worker");
    require(observation.classification_thread_ids[worker] ==
                observation.permutation_thread_ids[worker],
            "persistent BlockInPlace worker executes both phases for its index");
    classification_workers.insert(observation.classification_thread_ids[worker]);
    permutation_workers.insert(observation.permutation_thread_ids[worker]);
  }
  require(classification_workers.size() == requested_workers &&
              permutation_workers.size() == requested_workers,
          "parallel BlockInPlace phases execute on distinct requested workers");
}
#endif

std::vector<ter::TerPartitionEntry> deterministic_entries(const std::size_t count,
                                                          std::uint32_t state) {
  std::vector<ter::TerPartitionEntry> entries;
  entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    state = state * 1'664'525U + 1'013'904'223U;
    entries.push_back(
        {state ^ static_cast<std::uint32_t>(index * 2'654'435'761ULL),
         static_cast<ter::EntryId>(index)});
  }
  return entries;
}

void require_current_scatter(std::vector<ter::TerPartitionEntry> entries,
                             const ter::PartitionSpec& spec,
                             const std::string_view message) {
  ter::PartitionWorkspace workspace;
  ter::PartitionStats stats;
  const auto before = entries;
  auto offsets = offsets_for(spec.bits);
  require(ter::partition_chunk(entries, spec, workspace, offsets, stats) == ter::Status::Completed,
          message);
  require_partition_postconditions(before, entries, spec, offsets);
  if (entries.size() > 1U) {
    require(stats.records_classified == entries.size(), "telemetry classifies every record");
    require(stats.records_written == entries.size(), "telemetry writes every record");
    require(stats.histogram_bytes >=
                (std::uint64_t{1} << spec.bits) * sizeof(std::uint64_t),
            "telemetry reports live histogram capacity bytes");
    require(stats.offset_bytes >=
                offsets.size() * sizeof(std::uint64_t) +
                    (std::uint64_t{1} << spec.bits) * sizeof(std::uint64_t),
            "telemetry reports returned offsets and live cursor capacity");
    require(stats.record_scratch_bytes >= entries.size() * sizeof(ter::TerPartitionEntry),
            "telemetry reports record scratch capacity");
    require(stats.scratch_peak_bytes >= stats.record_scratch_bytes + stats.histogram_bytes,
            "telemetry reports scratch peak");
    require(stats.logical_bytes_read >= entries.size() * sizeof(ter::TerPartitionEntry),
            "telemetry reports logical reads");
    require(stats.logical_bytes_written >= entries.size() * sizeof(ter::TerPartitionEntry),
            "telemetry reports logical writes");
  }
}

void test_current_scatter_nontrivial_partitioning() {
  ter::PartitionSpec spec;

  require_current_scatter({}, spec, "empty CurrentScatter completes");
  require_current_scatter({{{0x1234U, 1U}}}, spec, "singleton CurrentScatter completes");

  std::vector<ter::TerPartitionEntry> one_bucket;
  for (std::uint32_t id = 0; id < 32U; ++id) {
    one_bucket.push_back({0x00ABCD07U + (id << 8U), id});
  }
  require_current_scatter(one_bucket, spec, "one-bucket CurrentScatter completes");

  std::vector<ter::TerPartitionEntry> one_per_bucket;
  for (std::uint32_t bucket = 0; bucket < 256U; ++bucket) {
    one_per_bucket.push_back({0xA5000000U | bucket, bucket});
  }
  require_current_scatter(one_per_bucket, spec, "one-per-bucket CurrentScatter completes");

  std::vector<ter::TerPartitionEntry> alternating;
  std::vector<ter::TerPartitionEntry> already_partitioned;
  std::vector<ter::TerPartitionEntry> reverse_partitioned;
  for (std::uint32_t id = 0; id < 512U; ++id) {
    alternating.push_back({(id & 1U) == 0U ? 0U : 255U, id});
    already_partitioned.push_back({id & 0xFFU, id});
    reverse_partitioned.push_back({255U - (id & 0xFFU), id});
  }
  require_current_scatter(alternating, spec, "alternating 0/255 CurrentScatter completes");
  require_current_scatter(already_partitioned, spec, "already-partitioned CurrentScatter completes");
  require_current_scatter(reverse_partitioned, spec, "reverse-partitioned CurrentScatter completes");

  std::vector<ter::TerPartitionEntry> random_entries;
  random_entries.reserve(10'000U);
  std::uint32_t state = 0xC0FFEE11U;
  for (std::uint32_t id = 0; id < 10'000U; ++id) {
    state = state * 1'664'525U + 1'013'904'223U;
    random_entries.push_back({state, id});
  }
  require_current_scatter(random_entries, spec, "random 10k CurrentScatter completes");

  spec.bits = 12U;
  spec.shift = 4U;
  require_current_scatter(random_entries, spec, "12-bit nonzero-shift CurrentScatter completes");
  spec.bits = 16U;
  spec.shift = 8U;
  require_current_scatter(random_entries, spec, "16-bit nonzero-shift CurrentScatter completes");
}

void test_current_scatter_cap_refusal_preserves_input() {
  const std::array<std::pair<std::uint32_t, std::uint64_t>, 2> cases{
      std::pair<std::uint32_t, std::uint64_t>{1U, 1U},
      // The second cap admits serial record/count/cursor storage, but not four
      // workers' required histogram and offset storage.
      std::pair<std::uint32_t, std::uint64_t>{4U, 5'000U}};
  for (const auto [threads, cap] : cases) {
    ter::PartitionWorkspace workspace;
    ter::PartitionSpec spec;
    spec.threads = threads;
    spec.scratch_cap_bytes = cap;
    std::vector<ter::TerPartitionEntry> entries{{{0U, 1U}, {255U, 2U}}};
    const auto before = entries;
    auto offsets = offsets_for(spec.bits);
    for (std::size_t index = 0; index < offsets.size(); ++index) {
      offsets[index] = static_cast<std::uint64_t>((index * 37U) % 101U);
    }
    const auto offsets_before = offsets;
    ter::PartitionStats stats;
    require(ter::partition_chunk(entries, spec, workspace, offsets, stats) ==
                ter::Status::ResourceLimit,
            "insufficient CurrentScatter scratch cap refuses");
    require_exact_entries(entries, before, "scratch-cap refusal leaves records unchanged");
    require(offsets == offsets_before, "scratch-cap refusal leaves returned offsets unchanged");
    require(stats.records_classified == 0U && stats.records_written == 0U,
            "scratch-cap refusal does not claim completed work");
  }
}

void test_current_scatter_retained_capacity_accounting() {
  constexpr std::uint64_t kLargeCap = 64ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t kWideBucketBytes = 65'536ULL * sizeof(std::uint64_t);
  const auto wide_fixture = deterministic_entries(100'000U, 0x51504F4FU);

  ter::PartitionWorkspace workspace;
  ter::PartitionSpec wide_spec;
  wide_spec.bits = 16U;
  wide_spec.shift = 8U;
  wide_spec.threads = 8U;
  // The frozen contract defines zero as unbounded.
  wide_spec.scratch_cap_bytes = 0U;
  auto wide_entries = wide_fixture;
  auto wide_offsets = offsets_for(wide_spec.bits);
  ter::PartitionStats wide_stats;
  require(ter::partition_chunk(wide_entries, wide_spec, workspace, wide_offsets,
                               wide_stats) == ter::Status::Completed,
          "wide parallel CurrentScatter establishes retained workspace capacity");

  {
    ter::PartitionSpec trivial_spec;
    trivial_spec.threads = 8U;
    trivial_spec.scratch_cap_bytes = 1U;
    std::vector<ter::TerPartitionEntry> empty;
    auto trivial_offsets = offsets_for(trivial_spec.bits);
    std::fill(trivial_offsets.begin(), trivial_offsets.end(), 0xC501U);
    const auto offsets_before = trivial_offsets;
    ter::PartitionStats trivial_stats;
    require(ter::partition_chunk(empty, trivial_spec, workspace, trivial_offsets,
                                 trivial_stats) == ter::Status::ResourceLimit,
            "trivial CurrentScatter refuses retained scratch above cap");
    require(trivial_offsets == offsets_before,
            "trivial CurrentScatter cap refusal preserves offsets");
    require(trivial_stats.records_classified == 0U &&
                trivial_stats.records_written == 0U,
            "trivial CurrentScatter refusal claims no completed work");

    trivial_spec.scratch_cap_bytes = kLargeCap;
    trivial_offsets = offsets_for(trivial_spec.bits);
    require(ter::partition_chunk(empty, trivial_spec, workspace, trivial_offsets,
                                 trivial_stats) == ter::Status::Completed,
            "trivial CurrentScatter admits retained scratch below cap");
    require(trivial_stats.scratch_peak_bytes > 0U &&
                trivial_stats.scratch_peak_bytes <=
                    trivial_spec.scratch_cap_bytes &&
                trivial_stats.record_scratch_bytes > 0U &&
                trivial_stats.histogram_bytes > 0U,
            "successful trivial CurrentScatter reports retained scratch");
  }

  ter::PartitionSpec narrow_serial_spec;
  narrow_serial_spec.threads = 1U;
  narrow_serial_spec.scratch_cap_bytes = 2'000'000U;
  std::vector<ter::TerPartitionEntry> narrow_entries{{{0x12U, 7U}, {0x34U, 9U}}};
  const auto narrow_before = narrow_entries;
  auto narrow_offsets = offsets_for(narrow_serial_spec.bits);
  for (std::size_t index = 0U; index < narrow_offsets.size(); ++index) {
    narrow_offsets[index] = static_cast<std::uint64_t>((index * 53U) % 127U);
  }
  const auto narrow_offsets_before = narrow_offsets;
  ter::PartitionStats narrow_stats;
  require(ter::partition_chunk(narrow_entries, narrow_serial_spec, workspace,
                               narrow_offsets, narrow_stats) == ter::Status::ResourceLimit,
          "narrow capped serial call refuses retained wide parallel scratch");
  require_exact_entries(narrow_entries, narrow_before,
                        "retained-cap refusal leaves narrow records unchanged");
  require(narrow_offsets == narrow_offsets_before,
          "retained-cap refusal leaves narrow sentinel offsets unchanged");
  require(narrow_stats.records_classified == 0U && narrow_stats.records_written == 0U,
          "retained-cap refusal claims no completed logical work");

  narrow_serial_spec.scratch_cap_bytes = kLargeCap;
  narrow_entries = narrow_before;
  narrow_offsets = offsets_for(narrow_serial_spec.bits);
  require(ter::partition_chunk(narrow_entries, narrow_serial_spec, workspace,
                               narrow_offsets, narrow_stats) == ter::Status::Completed,
          "larger cap admits narrow serial use of retained wide workspace");
  require_partition_postconditions(narrow_before, narrow_entries, narrow_serial_spec,
                                   narrow_offsets);
  const std::uint64_t retained_histogram_minimum = 9U * kWideBucketBytes;
  const std::uint64_t retained_offset_minimum =
      static_cast<std::uint64_t>(narrow_offsets.size()) * sizeof(std::uint64_t) +
      9U * kWideBucketBytes;
  require(narrow_stats.histogram_bytes >= retained_histogram_minimum,
          "serial telemetry includes retained global and worker histograms");
  require(narrow_stats.offset_bytes >= retained_offset_minimum,
          "serial telemetry includes retained global cursor and worker offsets");
  require(narrow_stats.scratch_peak_bytes >=
              narrow_stats.record_scratch_bytes + narrow_stats.histogram_bytes +
                  narrow_stats.offset_bytes -
                  static_cast<std::uint64_t>(narrow_offsets.size()) * sizeof(std::uint64_t),
          "serial scratch peak includes every reported live reusable buffer");

  ter::PartitionWorkspace parallel_workspace;
  ter::PartitionSpec parallel_spec;
  parallel_spec.bits = 16U;
  parallel_spec.shift = 8U;
  parallel_spec.threads = 2U;
  parallel_spec.scratch_cap_bytes = 0U;
  auto parallel_entries = deterministic_entries(10'000U, 0xC001D00DU);
  const auto parallel_before = parallel_entries;
  auto parallel_offsets = offsets_for(parallel_spec.bits);
  ter::PartitionStats parallel_stats;
  require(ter::partition_chunk(parallel_entries, parallel_spec, parallel_workspace,
                               parallel_offsets, parallel_stats) == ter::Status::Completed,
          "wide parallel capacity transition setup completes");

  parallel_spec.bits = 8U;
  parallel_spec.shift = 0U;
  parallel_spec.scratch_cap_bytes = kLargeCap;
  parallel_entries = parallel_before;
  parallel_offsets = offsets_for(parallel_spec.bits);
  require(ter::partition_chunk(parallel_entries, parallel_spec, parallel_workspace,
                               parallel_offsets, parallel_stats) == ter::Status::Completed,
          "narrow parallel call reuses wide global and worker capacities");
  require_partition_postconditions(parallel_before, parallel_entries, parallel_spec,
                                   parallel_offsets);
  require(parallel_stats.histogram_bytes >= 3U * kWideBucketBytes,
          "narrow parallel telemetry reports retained wide histogram capacities");
  require(parallel_stats.offset_bytes >=
              static_cast<std::uint64_t>(parallel_offsets.size()) * sizeof(std::uint64_t) +
                  3U * kWideBucketBytes,
          "narrow parallel telemetry reports retained wide offset capacities");
}

void test_parallel_current_scatter_matches_serial() {
  const auto fixture = deterministic_entries(100'000U, 0xA51CE55DU);
  for (const auto [bits, shift] :
       std::array<std::pair<std::uint8_t, std::uint8_t>, 3>{
           std::pair<std::uint8_t, std::uint8_t>{std::uint8_t{8}, std::uint8_t{0}},
           std::pair<std::uint8_t, std::uint8_t>{std::uint8_t{12}, std::uint8_t{4}},
           std::pair<std::uint8_t, std::uint8_t>{std::uint8_t{16}, std::uint8_t{8}}}) {
    ter::PartitionSpec serial_spec;
    serial_spec.bits = bits;
    serial_spec.shift = shift;
    auto serial_entries = fixture;
    auto serial_offsets = offsets_for(bits);
    ter::PartitionStats serial_stats;
    ter::PartitionWorkspace serial_workspace;
    require(ter::partition_chunk(serial_entries, serial_spec, serial_workspace,
                                 serial_offsets, serial_stats) == ter::Status::Completed,
            "serial CurrentScatter reference completes");
    require_partition_postconditions(fixture, serial_entries, serial_spec, serial_offsets);

    for (const std::uint32_t threads : std::array<std::uint32_t, 3>{2U, 4U, 8U}) {
      ter::PartitionSpec parallel_spec = serial_spec;
      parallel_spec.threads = threads;
      ter::PartitionWorkspace parallel_workspace;

      auto first_entries = fixture;
      auto first_offsets = offsets_for(bits);
      ter::PartitionStats first_stats;
      require(ter::partition_chunk(first_entries, parallel_spec, parallel_workspace,
                                   first_offsets, first_stats) == ter::Status::Completed,
              "parallel CurrentScatter completes");
#ifdef TER_ROOT_PARTITION_TESTING
      const auto first_observation = ter::current_scatter_worker_observation();
      require_parallel_worker_observation(first_observation, threads);
#endif
      require(first_offsets == serial_offsets,
              "parallel CurrentScatter returns the exact serial CSR offsets");
      require_equal_bucket_id_sets(serial_entries, first_entries, first_offsets);
      require_partition_postconditions(fixture, first_entries, parallel_spec, first_offsets);
      require(first_stats.records_classified == serial_stats.records_classified &&
                  first_stats.records_written == serial_stats.records_written &&
                  first_stats.logical_bytes_read == serial_stats.logical_bytes_read &&
                  first_stats.logical_bytes_written == serial_stats.logical_bytes_written,
              "serial and parallel CurrentScatter report the same logical work");
      require(first_stats.histogram_bytes > serial_stats.histogram_bytes &&
                  first_stats.offset_bytes > serial_stats.offset_bytes,
              "parallel CurrentScatter reports worker histogram and offset scratch");

      auto repeated_entries = fixture;
      auto repeated_offsets = offsets_for(bits);
      ter::PartitionStats repeated_stats;
      require(ter::partition_chunk(repeated_entries, parallel_spec, parallel_workspace,
                                   repeated_offsets, repeated_stats) == ter::Status::Completed,
              "repeated parallel CurrentScatter completes");
      require(repeated_offsets == first_offsets,
              "repeated parallel CurrentScatter returns deterministic offsets");
      require_exact_entries(repeated_entries, first_entries,
                            "repeated parallel CurrentScatter is deterministic");
      require_equal_logical_counters(first_stats, repeated_stats,
                                     "repeated parallel logical counters are exact");
#ifdef TER_ROOT_PARTITION_TESTING
      const auto repeated_observation = ter::current_scatter_worker_observation();
      require_parallel_worker_observation(repeated_observation, threads);
      require(repeated_observation.count_thread_ids == first_observation.count_thread_ids &&
                  repeated_observation.scatter_thread_ids ==
                      first_observation.scatter_thread_ids,
              "repeated same-workspace call reuses the persistent worker threads");
#endif
    }
  }
}

void test_parallel_current_scatter_race_stress() {
  constexpr std::size_t kFixtureSize = 10'000U;
  auto one_bucket = deterministic_entries(kFixtureSize, 0x0BADC0DEU);
  auto hot_bucket = deterministic_entries(kFixtureSize, 0x91E10DA5U);
  for (std::size_t index = 0; index < kFixtureSize; ++index) {
    one_bucket[index].low_k = (one_bucket[index].low_k & 0xFFFFFF00U) | 17U;
    hot_bucket[index].low_k =
        (hot_bucket[index].low_k & 0xFFFFFF00U) | (index == 0U ? 203U : 91U);
  }

  ter::PartitionSpec spec;
  for (const std::uint32_t threads : std::array<std::uint32_t, 4>{2U, 4U, 8U, 16U}) {
    spec.threads = threads;
    ter::PartitionWorkspace workspace;
    std::array<std::vector<ter::TerPartitionEntry>, 2> expected_entries;
    std::array<std::vector<std::uint64_t>, 2> expected_offsets;
    const std::array<const std::vector<ter::TerPartitionEntry>*, 2> fixtures{
        &one_bucket, &hot_bucket};

    for (std::size_t fixture_index = 0; fixture_index < fixtures.size(); ++fixture_index) {
      expected_entries[fixture_index] = *fixtures[fixture_index];
      expected_offsets[fixture_index] = offsets_for(spec.bits);
      ter::PartitionStats stats;
      require(ter::partition_chunk(expected_entries[fixture_index], spec, workspace,
                                   expected_offsets[fixture_index], stats) ==
                  ter::Status::Completed,
              "parallel CurrentScatter stress reference completes");
      require_partition_postconditions(*fixtures[fixture_index], expected_entries[fixture_index],
                                       spec, expected_offsets[fixture_index]);
    }

    for (std::uint32_t repetition = 1U; repetition < 100U; ++repetition) {
      for (std::size_t fixture_index = 0; fixture_index < fixtures.size(); ++fixture_index) {
        auto entries = *fixtures[fixture_index];
        auto offsets = offsets_for(spec.bits);
        ter::PartitionStats stats;
        require(ter::partition_chunk(entries, spec, workspace, offsets, stats) ==
                    ter::Status::Completed,
                "parallel CurrentScatter race stress completes");
        require(offsets == expected_offsets[fixture_index],
                "parallel CurrentScatter race stress preserves exact offsets");
        require_exact_entries(entries, expected_entries[fixture_index],
                              "parallel CurrentScatter race stress remains deterministic");
        require_partition_postconditions(*fixtures[fixture_index], entries, spec, offsets);
      }
    }
  }
}

void require_block_inplace_matches_current_scatter(
    const std::vector<ter::TerPartitionEntry>& fixture,
    const std::uint8_t shift,
    const std::uint32_t block_bytes) {
  ter::PartitionSpec reference_spec;
  reference_spec.shift = shift;
  ter::PartitionWorkspace reference_workspace;
  auto reference_entries = fixture;
  auto reference_offsets = offsets_for(reference_spec.bits);
  ter::PartitionStats reference_stats;
  require(ter::partition_chunk(reference_entries, reference_spec, reference_workspace,
                               reference_offsets, reference_stats) ==
              ter::Status::Completed,
          "CurrentScatter differential authority completes");

  ter::PartitionSpec candidate_spec = reference_spec;
  candidate_spec.backend = ter::PartitionBackend::BlockInPlace;
  candidate_spec.block_bytes = block_bytes;
  ter::PartitionWorkspace candidate_workspace;
  auto candidate_entries = fixture;
  auto candidate_offsets = offsets_for(candidate_spec.bits);
  ter::PartitionStats candidate_stats;
  require(ter::partition_chunk(candidate_entries, candidate_spec, candidate_workspace,
                               candidate_offsets, candidate_stats) ==
              ter::Status::Completed,
          "serial radix8 BlockInPlace differential candidate completes");

  require(candidate_offsets == reference_offsets,
          "BlockInPlace returns the exact CurrentScatter CSR offsets");
  require_equal_bucket_record_sets(reference_entries, candidate_entries,
                                   candidate_offsets);
  require_partition_postconditions(fixture, candidate_entries, candidate_spec,
                                   candidate_offsets);
  require(candidate_stats.requested == ter::PartitionBackend::BlockInPlace &&
              candidate_stats.effective == ter::PartitionBackend::BlockInPlace,
          "BlockInPlace telemetry reports the requested and effective backend");
  if (fixture.size() > 1U) {
    const std::uint64_t records_per_block =
        block_bytes / sizeof(ter::TerPartitionEntry);
    require(candidate_stats.records_classified == fixture.size() &&
                candidate_stats.records_written == fixture.size(),
            "BlockInPlace telemetry reports exact logical record counts");
    require(candidate_stats.record_scratch_bytes == 0U,
            "BlockInPlace does not report CurrentScatter record scratch");
    require(candidate_stats.block_buffer_bytes >=
                514U * records_per_block * sizeof(ter::TerPartitionEntry),
            "BlockInPlace reports partial, overflow, and swap block buffers");
    require(candidate_stats.scratch_peak_bytes >=
                candidate_stats.block_buffer_bytes,
            "BlockInPlace scratch peak includes block buffers");
    require(candidate_stats.logical_bytes_read >=
                fixture.size() * sizeof(ter::TerPartitionEntry) &&
                candidate_stats.logical_bytes_written >=
                    fixture.size() * sizeof(ter::TerPartitionEntry),
            "BlockInPlace reports actual logical record traffic");

    std::uint64_t expected_flushed_blocks = 0U;
    std::uint64_t expected_interior_blocks = 0U;
    for (std::size_t bucket = 0U; bucket + 1U < candidate_offsets.size(); ++bucket) {
      const std::uint64_t begin = candidate_offsets[bucket];
      const std::uint64_t end = candidate_offsets[bucket + 1U];
      expected_flushed_blocks += (end - begin) / records_per_block;
      const std::uint64_t first_block =
          begin / records_per_block + (begin % records_per_block == 0U ? 0U : 1U);
      const std::uint64_t last_block = end / records_per_block;
      if (last_block >= first_block) {
        expected_interior_blocks += last_block - first_block;
      }
    }
    require(candidate_stats.blocks_flushed == expected_flushed_blocks,
            "BlockInPlace reports every locally classified full block");
    require(candidate_stats.blocks_permuted == expected_interior_blocks,
            "BlockInPlace reports every aligned interior block placement");
    require(candidate_stats.partial_records_repaired ==
                fixture.size() - expected_interior_blocks * records_per_block,
            "BlockInPlace reports every exact margin repair");
  }
}

void test_serial_block_inplace_differential_matrix() {
  std::vector<std::vector<ter::TerPartitionEntry>> fixtures;
  for (const std::size_t count :
       std::array<std::size_t, 8>{0U, 1U, 255U, 256U, 257U, 511U, 512U, 513U}) {
    fixtures.push_back(deterministic_entries(count,
                                             0x9E3779B9U ^
                                                 static_cast<std::uint32_t>(count)));
  }

  fixtures.push_back(deterministic_entries(10'000U, 0xA11CE55DU));

  auto one_bucket = deterministic_entries(10'000U, 0x0BADC0DEU);
  auto hot_bucket = deterministic_entries(10'000U, 0x91E10DA5U);
  auto alternating = deterministic_entries(10'000U, 0xA17E2A7EU);
  auto sorted = deterministic_entries(10'000U, 0x50A7ED00U);
  auto reverse = deterministic_entries(10'000U, 0xAE7E25E0U);
  auto random_unique_ids = deterministic_entries(10'000U, 0xE17D1D5U);
  for (std::size_t index = 0U; index < one_bucket.size(); ++index) {
    constexpr std::uint32_t kClearTestDigits = 0xFFFF0000U;
    const std::uint32_t hot_digit = index == 0U ? 211U : 73U;
    const std::uint32_t alternating_digit = (index & 1U) == 0U ? 0U : 255U;
    one_bucket[index].low_k =
        (one_bucket[index].low_k & kClearTestDigits) | 37U | (37U << 8U);
    hot_bucket[index].low_k =
        (hot_bucket[index].low_k & kClearTestDigits) | hot_digit |
        (hot_digit << 8U);
    alternating[index].low_k =
        (alternating[index].low_k & kClearTestDigits) | alternating_digit |
        (alternating_digit << 8U);
    const std::uint32_t sorted_digit =
        static_cast<std::uint32_t>((index * 256U) / sorted.size());
    sorted[index].low_k =
        (sorted[index].low_k & kClearTestDigits) | sorted_digit |
        (sorted_digit << 8U);
    const std::uint32_t reverse_digit = 255U - sorted_digit;
    reverse[index].low_k =
        (reverse[index].low_k & kClearTestDigits) | reverse_digit |
        (reverse_digit << 8U);
    random_unique_ids[index].entry_id = static_cast<ter::EntryId>(
        static_cast<std::uint32_t>(index) * 2'654'435'761U + 1'013'904'223U);
  }
  fixtures.push_back(std::move(one_bucket));
  fixtures.push_back(std::move(hot_bucket));
  fixtures.push_back(std::move(alternating));
  fixtures.push_back(std::move(sorted));
  fixtures.push_back(std::move(reverse));
  fixtures.push_back(std::move(random_unique_ids));

  for (const auto& fixture : fixtures) {
    for (const std::uint8_t shift : std::array<std::uint8_t, 2>{0U, 8U}) {
      for (const std::uint32_t block_bytes :
           std::array<std::uint32_t, 2>{64U, 2'048U}) {
        require_block_inplace_matches_current_scatter(fixture, shift, block_bytes);
      }
    }
  }

  const auto chain_fixture = deterministic_entries(513U, 0xC4A1C4A1U);
  require_block_inplace_matches_current_scatter(chain_fixture, 0U, 8U);
}

std::array<std::vector<ter::TerPartitionEntry>, 4>
parallel_block_inplace_fixtures() {
  constexpr std::size_t kCount = 10'000U;
  std::array<std::vector<ter::TerPartitionEntry>, 4> fixtures{
      deterministic_entries(kCount, 0xB10C0001U),
      deterministic_entries(kCount, 0xB10C0002U),
      deterministic_entries(kCount, 0xB10C0003U),
      deterministic_entries(kCount, 0xB10C0004U)};
  for (std::size_t index = 0U; index < kCount; ++index) {
    fixtures[1][index].low_k =
        (fixtures[1][index].low_k & 0xFFFFFF00U) | 43U;
    fixtures[2][index].low_k =
        (fixtures[2][index].low_k & 0xFFFFFF00U) |
        ((index & 1U) == 0U ? 0U : 255U);
    fixtures[3][index].low_k =
        (fixtures[3][index].low_k & 0xFFFFFF00U) |
        (index == 0U ? 211U : 73U);
  }
  return fixtures;
}

void test_parallel_block_inplace_differential_and_stress() {
  const auto fixtures = parallel_block_inplace_fixtures();
  const auto boundary_fixture = deterministic_entries(10'003U, 0xB10CB00DU);
  std::array<std::vector<ter::TerPartitionEntry>, fixtures.size()> references;
  std::array<std::vector<std::uint64_t>, fixtures.size()> reference_offsets;
  ter::PartitionSpec reference_spec;
  for (std::size_t fixture = 0U; fixture < fixtures.size(); ++fixture) {
    references[fixture] = fixtures[fixture];
    reference_offsets[fixture] = offsets_for(reference_spec.bits);
    ter::PartitionWorkspace workspace;
    ter::PartitionStats stats;
    require(ter::partition_chunk(references[fixture], reference_spec, workspace,
                                 reference_offsets[fixture], stats) ==
                ter::Status::Completed,
            "parallel BlockInPlace CurrentScatter authority completes");
  }

  for (const std::uint32_t threads :
       std::array<std::uint32_t, 4>{2U, 4U, 8U, 16U}) {
    ter::PartitionSpec spec;
    spec.backend = ter::PartitionBackend::BlockInPlace;
    spec.threads = threads;
    spec.block_bytes = 64U;
    ter::PartitionWorkspace workspace;

    for (const std::uint8_t shift : std::array<std::uint8_t, 2>{0U, 8U}) {
      for (const std::uint32_t block_bytes :
           std::array<std::uint32_t, 2>{64U, 2'048U}) {
        ter::PartitionSpec boundary_reference_spec;
        boundary_reference_spec.shift = shift;
        auto boundary_reference = boundary_fixture;
        auto boundary_reference_offsets = offsets_for(boundary_reference_spec.bits);
        ter::PartitionWorkspace boundary_reference_workspace;
        ter::PartitionStats boundary_reference_stats;
        require(ter::partition_chunk(boundary_reference, boundary_reference_spec,
                                     boundary_reference_workspace,
                                     boundary_reference_offsets,
                                     boundary_reference_stats) ==
                    ter::Status::Completed,
                "boundary CurrentScatter authority completes");

        spec.shift = shift;
        spec.block_bytes = block_bytes;
        auto boundary_entries = boundary_fixture;
        auto boundary_offsets = offsets_for(spec.bits);
        ter::PartitionStats boundary_stats;
        require(ter::partition_chunk(boundary_entries, spec, workspace,
                                     boundary_offsets, boundary_stats) ==
                    ter::Status::Completed,
                "parallel BlockInPlace crosses stripe and block boundaries");
        require(boundary_offsets == boundary_reference_offsets,
                "boundary fixture returns exact CurrentScatter offsets");
        require_equal_bucket_record_sets(boundary_reference, boundary_entries,
                                         boundary_offsets);
        require_partition_postconditions(boundary_fixture, boundary_entries, spec,
                                         boundary_offsets);
        require(boundary_stats.record_scratch_bytes == 0U,
                "parallel BlockInPlace never reports N-record scratch");
      }
    }
    spec.shift = 0U;
    spec.block_bytes = 64U;

    const auto tiny_fixture = deterministic_entries(7U, 0xB10C7007U);
    ter::PartitionSpec tiny_reference_spec;
    auto tiny_reference = tiny_fixture;
    auto tiny_reference_offsets = offsets_for(tiny_reference_spec.bits);
    ter::PartitionWorkspace tiny_reference_workspace;
    ter::PartitionStats tiny_reference_stats;
    require(ter::partition_chunk(tiny_reference, tiny_reference_spec,
                                 tiny_reference_workspace,
                                 tiny_reference_offsets,
                                 tiny_reference_stats) == ter::Status::Completed,
            "tiny CurrentScatter authority completes");
    spec.block_bytes = 2'048U;
    auto tiny_entries = tiny_fixture;
    auto tiny_offsets = offsets_for(spec.bits);
    ter::PartitionStats tiny_stats;
    require(ter::partition_chunk(tiny_entries, spec, workspace, tiny_offsets,
                                 tiny_stats) == ter::Status::Completed,
            "parallel BlockInPlace handles fewer physical blocks than workers");
    require(tiny_offsets == tiny_reference_offsets,
            "tiny parallel BlockInPlace returns exact offsets");
    require_equal_bucket_record_sets(tiny_reference, tiny_entries, tiny_offsets);
    require_partition_postconditions(tiny_fixture, tiny_entries, spec, tiny_offsets);
    require(tiny_stats.record_scratch_bytes == 0U,
            "tiny parallel BlockInPlace has no N-record scratch");
    spec.block_bytes = 64U;
#ifdef TER_ROOT_PARTITION_TESTING
    ter::BlockInPlaceWorkerObservation first_observation;
#endif

    for (std::size_t fixture = 0U; fixture < fixtures.size(); ++fixture) {
      for (std::uint32_t repetition = 0U; repetition < 100U; ++repetition) {
        auto entries = fixtures[fixture];
        auto offsets = offsets_for(spec.bits);
        ter::PartitionStats stats;
        require(ter::partition_chunk(entries, spec, workspace, offsets, stats) ==
                    ter::Status::Completed,
                "portable parallel BlockInPlace stress call completes");
        require(offsets == reference_offsets[fixture],
                "parallel BlockInPlace returns exact CurrentScatter CSR offsets");
        require_equal_bucket_record_sets(references[fixture], entries, offsets);
        require_partition_postconditions(fixtures[fixture], entries, spec, offsets);
        require(stats.records_classified == entries.size() &&
                    stats.records_written == entries.size(),
                "parallel BlockInPlace publishes exact completion counters");
        require(stats.record_scratch_bytes == 0U,
                "parallel BlockInPlace has no CurrentScatter record scratch");
        if (repetition == 0U) {
#ifdef TER_ROOT_PARTITION_TESTING
          const auto observation = ter::block_inplace_worker_observation();
          require_parallel_block_worker_observation(observation, threads);
          if (fixture == 0U) {
            first_observation = observation;
          }
#endif
        }
#ifdef TER_ROOT_PARTITION_TESTING
        if (fixture == 0U && repetition == 1U) {
          const auto repeated_observation =
              ter::block_inplace_worker_observation();
          require_parallel_block_worker_observation(repeated_observation,
                                                    threads);
          require(repeated_observation.classification_thread_ids ==
                          first_observation.classification_thread_ids &&
                      repeated_observation.permutation_thread_ids ==
                          first_observation.permutation_thread_ids,
                  "repeated BlockInPlace calls reuse persistent worker threads");
        }
#endif
      }
    }
  }
}

void test_block_inplace_rejections_and_workspace_transitions() {
  const auto fixture = deterministic_entries(1'024U, 0xC0DEC0DEU);

  {
    ter::PartitionSpec spec;
    spec.backend = ter::PartitionBackend::BlockInPlace;
    spec.threads = 4U;
    spec.scratch_cap_bytes = 1U;
    auto entries = fixture;
    const auto before = entries;
    auto offsets = offsets_for(spec.bits);
    std::fill(offsets.begin(), offsets.end(), 0xB10C5EEDU);
    const auto offsets_before = offsets;
    ter::PartitionStats stats;
    ter::PartitionWorkspace workspace;
    require(ter::partition_chunk(entries, spec, workspace, offsets, stats) ==
                ter::Status::ResourceLimit,
            "parallel BlockInPlace refuses an insufficient first-call cap");
    require_exact_entries(entries, before,
                          "parallel first-call cap refusal preserves records");
    require(offsets == offsets_before,
            "parallel first-call cap refusal preserves sentinel offsets");
    require(stats.records_classified == 0U && stats.records_written == 0U,
            "parallel first-call cap refusal claims no completed work");
  }

  {
    ter::PartitionSpec spec;
    spec.backend = ter::PartitionBackend::BlockInPlace;
    spec.scratch_cap_bytes = 1U;
    auto entries = fixture;
    const auto before = entries;
    auto offsets = offsets_for(spec.bits);
    std::fill(offsets.begin(), offsets.end(), 0xBAD5EEDU);
    const auto offsets_before = offsets;
    ter::PartitionStats stats;
    ter::PartitionWorkspace workspace;
    require(ter::partition_chunk(entries, spec, workspace, offsets, stats) ==
                ter::Status::ResourceLimit,
            "BlockInPlace cap refusal occurs before classification");
    require_exact_entries(entries, before,
                          "BlockInPlace cap refusal leaves records unchanged");
    require(offsets == offsets_before,
            "BlockInPlace cap refusal leaves sentinel offsets unchanged");
    require(stats.records_classified == 0U && stats.records_written == 0U,
            "BlockInPlace cap refusal claims no completed work");
  }

  {
    ter::PartitionWorkspace workspace;
    ter::PartitionSpec block_spec;
    block_spec.backend = ter::PartitionBackend::BlockInPlace;
    block_spec.block_bytes = 8'192U;
    auto block_entries = fixture;
    auto block_offsets = offsets_for(block_spec.bits);
    ter::PartitionStats stats;
    require(ter::partition_chunk(block_entries, block_spec, workspace, block_offsets,
                                 stats) == ter::Status::Completed,
            "large-buffer BlockInPlace transition setup completes");

    ter::PartitionSpec scatter_spec;
    scatter_spec.scratch_cap_bytes = 10'000U;
    auto scatter_entries = deterministic_entries(2U, 0x5CA77E12U);
    const auto scatter_before = scatter_entries;
    auto scatter_offsets = offsets_for(scatter_spec.bits);
    require(ter::partition_chunk(scatter_entries, scatter_spec, workspace,
                                 scatter_offsets, stats) == ter::Status::Completed,
            "CurrentScatter releases inactive BlockInPlace retained storage");
    require_partition_postconditions(scatter_before, scatter_entries, scatter_spec,
                                     scatter_offsets);
    require(stats.scratch_peak_bytes <= scatter_spec.scratch_cap_bytes,
            "post-BlockInPlace CurrentScatter cap accounts only live storage");
  }

  {
    ter::PartitionWorkspace workspace;
    ter::PartitionSpec scatter_spec;
    scatter_spec.bits = 16U;
    scatter_spec.threads = 8U;
    auto scatter_entries = deterministic_entries(10'000U, 0x5CA77E16U);
    auto scatter_offsets = offsets_for(scatter_spec.bits);
    ter::PartitionStats stats;
    require(ter::partition_chunk(scatter_entries, scatter_spec, workspace,
                                 scatter_offsets, stats) == ter::Status::Completed,
            "wide CurrentScatter transition setup completes");

    ter::PartitionSpec block_spec;
    block_spec.backend = ter::PartitionBackend::BlockInPlace;
    block_spec.block_bytes = 64U;
    block_spec.threads = 4U;
    block_spec.scratch_cap_bytes = 250'000U;
    auto block_entries = fixture;
    const auto block_before = block_entries;
    auto block_offsets = offsets_for(block_spec.bits);
    require(ter::partition_chunk(block_entries, block_spec, workspace, block_offsets,
                                 stats) == ter::Status::Completed,
            "BlockInPlace releases inactive CurrentScatter retained storage");
    require_partition_postconditions(block_before, block_entries, block_spec,
                                     block_offsets);
    require(stats.scratch_peak_bytes <= block_spec.scratch_cap_bytes,
            "post-CurrentScatter BlockInPlace cap accounts only live storage");

    const auto retained_before = block_entries;
    const auto retained_offsets_before = block_offsets;
    const std::uint64_t retained_scratch = stats.scratch_peak_bytes;
    {
      std::vector<ter::TerPartitionEntry> singleton{{{0x42U, 91U}}};
      auto singleton_offsets = offsets_for(block_spec.bits);
      block_spec.scratch_cap_bytes = retained_scratch;
      require(ter::partition_chunk(singleton, block_spec, workspace,
                                   singleton_offsets, stats) ==
                  ter::Status::Completed,
              "trivial BlockInPlace admits retained scratch at its cap");
      require(stats.scratch_peak_bytes == retained_scratch &&
                  stats.block_buffer_bytes > 0U &&
                  stats.record_scratch_bytes == 0U,
              "successful trivial BlockInPlace reports retained scratch");
    }
    block_spec.scratch_cap_bytes = stats.scratch_peak_bytes - 1U;
    {
      std::vector<ter::TerPartitionEntry> singleton{{{0x42U, 91U}}};
      const auto singleton_before = singleton;
      auto singleton_offsets = offsets_for(block_spec.bits);
      std::fill(singleton_offsets.begin(), singleton_offsets.end(), 0xB10CU);
      const auto singleton_offsets_before = singleton_offsets;
      require(ter::partition_chunk(singleton, block_spec, workspace,
                                   singleton_offsets, stats) ==
                  ter::Status::ResourceLimit,
              "trivial BlockInPlace refuses retained scratch above cap");
      require_exact_entries(singleton, singleton_before,
                            "trivial BlockInPlace refusal preserves singleton");
      require(singleton_offsets == singleton_offsets_before,
              "trivial BlockInPlace refusal preserves offsets");
      require(stats.records_classified == 0U && stats.records_written == 0U,
              "trivial BlockInPlace refusal claims no completed work");
    }
    for (std::uint32_t repetition = 0U; repetition < 2U; ++repetition) {
      require(ter::partition_chunk(block_entries, block_spec, workspace,
                                   block_offsets, stats) ==
                  ter::Status::ResourceLimit,
              "BlockInPlace repeatedly refuses a cap below retained capacity");
      require_exact_entries(
          block_entries, retained_before,
          "repeated retained BlockInPlace cap refusal leaves records unchanged");
      require(block_offsets == retained_offsets_before,
              "repeated retained BlockInPlace refusal leaves offsets unchanged");
      require(stats.records_classified == 0U && stats.records_written == 0U,
              "retained parallel BlockInPlace refusal claims no completed work");
    }
  }
}

void test_empty_and_singleton_csr() {
  ter::PartitionWorkspace workspace;
  ter::PartitionSpec spec;
  ter::PartitionStats stats;
  auto offsets = offsets_for(spec.bits);
  std::span<ter::TerPartitionEntry> empty;

  require(ter::partition_chunk(empty, spec, workspace, offsets, stats) == ter::Status::Completed,
          "empty partition completes");
  require(stats.requested == ter::PartitionBackend::CurrentScatter &&
              stats.effective == ter::PartitionBackend::CurrentScatter,
          "empty partition reports the requested CurrentScatter backend");
  for (const auto offset : offsets) {
    require(offset == 0U, "empty partition writes all-zero CSR offsets");
  }

  spec.shift = 4U;
  std::array<ter::TerPartitionEntry, 1> entries{{{0x00000AB0U, 42U}}};
  offsets = offsets_for(spec.bits);
  const std::uint32_t bucket = (entries.front().low_k >> spec.shift) & 0xFFU;
  require(ter::partition_chunk(entries, spec, workspace, offsets, stats) == ter::Status::Completed,
          "singleton partition completes");
  require(entries.front().low_k == 0x00000AB0U && entries.front().entry_id == 42U,
          "singleton partition does not move its record");
  require(stats.records_classified == 1U, "singleton partition records its exact classification");
  // Repair item P23: the trivial path used to report classified=1 written=0,
  // which contradicted the documented "both equal N" contract.
  require(stats.records_written == 1U,
          "singleton partition counts its record as written to its output position");
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    const std::uint64_t expected = index <= bucket ? 0U : 1U;
    require(offsets[index] == expected, "singleton partition writes exact CSR offsets");
  }

  spec = {};
  spec.block_bytes = 3000U;
  entries.front() = {0x75U, 43U};
  offsets = offsets_for(spec.bits);
  require(ter::partition_chunk(entries, spec, workspace, offsets, stats) == ter::Status::Completed,
          "CurrentScatter ignores BlockInPlace block geometry");
  require(entries.front().low_k == 0x75U && entries.front().entry_id == 43U,
          "CurrentScatter singleton remains unchanged");
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    const std::uint64_t expected = index <= 0x75U ? 0U : 1U;
    require(offsets[index] == expected,
            "CurrentScatter singleton with ignored block geometry writes exact CSR offsets");
  }
}

void test_validation_and_rejection() {
  ter::PartitionWorkspace workspace;
  std::array<ter::TerPartitionEntry, 2> entries{{{0x12U, 1U}, {0x34U, 2U}}};
  const auto original_entries = entries;
  ter::PartitionStats stats;

  auto expect_status = [&](const ter::PartitionSpec& spec,
                           std::vector<std::uint64_t> offsets,
                           const ter::Status expected,
                           const std::string_view message) {
    stats.histogram_ns = 123U;
    require(ter::partition_chunk(entries, spec, workspace, offsets, stats) == expected, message);
    require(entries[0].low_k == original_entries[0].low_k &&
                entries[0].entry_id == original_entries[0].entry_id &&
                entries[1].low_k == original_entries[1].low_k &&
                entries[1].entry_id == original_entries[1].entry_id,
            "rejected partition leaves entries unchanged");
    require(stats.requested == spec.backend, "rejected partition preserves requested backend");
    require(stats.histogram_ns == 0U, "rejected partition resets statistics");
  };

  ter::PartitionSpec spec;
  spec.bits = 0U;
  expect_status(spec, {}, ter::Status::InvalidConfiguration, "zero bits is invalid");

  spec = {};
  spec.bits = 17U;
  expect_status(spec, offsets_for(spec.bits), ter::Status::InvalidConfiguration,
                "CurrentScatter rejects 17 bits");

  spec = {};
  spec.shift = 25U;
  expect_status(spec, offsets_for(spec.bits), ter::Status::InvalidConfiguration,
                "shift plus bits above 32 is invalid");

  spec = {};
  expect_status(spec, std::vector<std::uint64_t>(256U), ter::Status::InvalidConfiguration,
                "wrong offset length is invalid");

  spec = {};
  spec.threads = 0U;
  expect_status(spec, offsets_for(spec.bits), ter::Status::InvalidConfiguration,
                "zero threads is invalid");

  spec = {};
  spec.backend = ter::PartitionBackend::BlockInPlace;
  spec.block_bytes = 3000U;
  expect_status(spec, offsets_for(spec.bits), ter::Status::InvalidConfiguration,
                "BlockInPlace rejects non-power-of-two block bytes");

  spec = {};
  spec.backend = ter::PartitionBackend::BlockInPlace;
  spec.bits = 12U;
  expect_status(spec, offsets_for(spec.bits), ter::Status::NotApplicable,
                "BlockInPlace is only admitted for eight bits");

  spec = {};
  spec.backend = ter::PartitionBackend::Region2Path;
  expect_status(spec, offsets_for(spec.bits), ter::Status::NotApplicable,
                "Region2Path remains unavailable");

  spec.backend = ter::PartitionBackend::Adaptive;
  expect_status(spec, offsets_for(spec.bits), ter::Status::NotApplicable,
                "Adaptive remains unavailable");

  for (const std::size_t count : std::array<std::size_t, 2>{1U, 2U}) {
    ter::PartitionSpec invalid_backend_spec;
    invalid_backend_spec.backend = static_cast<ter::PartitionBackend>(0xFFU);
    auto invalid_entries = deterministic_entries(count, 0x1BADB002U);
    const auto invalid_before = invalid_entries;
    auto invalid_offsets = offsets_for(invalid_backend_spec.bits);
    std::fill(invalid_offsets.begin(), invalid_offsets.end(), 0x1BADU);
    const auto invalid_offsets_before = invalid_offsets;
    ter::PartitionWorkspace invalid_workspace;
    require(ter::partition_chunk(invalid_entries, invalid_backend_spec,
                                 invalid_workspace, invalid_offsets, stats) ==
                ter::Status::InvalidConfiguration,
            "out-of-range backend is invalid for trivial and nontrivial input");
    require_exact_entries(invalid_entries, invalid_before,
                          "invalid backend preserves records");
    require(invalid_offsets == invalid_offsets_before,
            "invalid backend preserves sentinel offsets");
  }

}

}  // namespace

int main() {
  test_captured_root_low32_projection();
  test_captured_root_record_conversion();
  test_partition_entry_layout();
  test_workspace_lifecycle();
  test_empty_and_singleton_csr();
  test_current_scatter_nontrivial_partitioning();
  test_current_scatter_cap_refusal_preserves_input();
  test_current_scatter_retained_capacity_accounting();
  test_parallel_current_scatter_matches_serial();
  test_parallel_current_scatter_race_stress();
  test_serial_block_inplace_differential_matrix();
  test_parallel_block_inplace_differential_and_stress();
  test_block_inplace_rejections_and_workspace_transitions();
  test_validation_and_rejection();
  std::cout << "root partition API tests passed\n";
  return 0;
}
