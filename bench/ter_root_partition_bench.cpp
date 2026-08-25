#include "ter/root_partition.hpp"
#include "ter/instance.hpp"
#include "ter/limitations.hpp"
#include "ter_root_capture_adapter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSchemaVersion = 1U;
constexpr std::uint32_t kHotNumerator = 9U;
constexpr std::uint32_t kHotDenominator = 10U;

struct Options {
  std::string dataset = "synthetic";
  ter::PartitionBackend backend = ter::PartitionBackend::CurrentScatter;
  std::string backend_name;
  std::vector<std::uint64_t> sizes;
  std::uint8_t bits = 0U;
  std::uint8_t shift = 0U;
  std::vector<std::uint32_t> threads;
  std::string distribution;
  std::uint64_t seed = 0U;
  std::uint32_t warmup = 0U;
  std::uint32_t repeat = 0U;
  std::uint32_t block_bytes = 2'048U;
  std::filesystem::path output;
  std::uint32_t root_n = 0U;
  std::uint64_t root_seed = 0U;
  std::string root_mode;
  std::string list;
};

struct CaptureMetadata {
  std::uint64_t generation_ns = 0U;
  std::array<std::uint64_t, 3> sizes{};
  std::uint32_t n = 0U;
  std::uint64_t seed = 0U;
  std::uint32_t r1 = 0U;
  std::uint32_t quotient_bits = 0U;
  std::string mode;
  std::string target_mode;
  std::string solver_status;
};

struct ReferenceDirectory {
  std::vector<std::vector<ter::TerPartitionEntry>> buckets;
};

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

template <typename Integer>
[[nodiscard]] Integer parse_unsigned(const std::string_view text, const std::string_view label) {
  Integer value{};
  if (text.empty()) {
    throw std::runtime_error(std::string(label) + " must not be empty");
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::runtime_error("invalid " + std::string(label) + ": " + std::string(text));
  }
  return value;
}

template <typename Integer>
[[nodiscard]] std::vector<Integer> parse_list(const std::string_view text,
                                              const std::string_view label) {
  std::vector<Integer> values;
  std::size_t begin = 0U;
  while (begin <= text.size()) {
    const std::size_t comma = text.find(',', begin);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    values.push_back(parse_unsigned<Integer>(text.substr(begin, end - begin), label));
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1U;
  }
  return values;
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  if (argc < 3 || (argc % 2) == 0) {
    throw std::runtime_error(
        "usage: ter_root_partition_bench --backend current-scatter|block-inplace "
        "--n sizes --bits 8|12|16 --shift integer --threads list --distribution "
        "uniform|hot|alternating|sorted|reverse|duplicates --seed uint64 "
        "--warmup integer --repeat integer --output path.jsonl "
        "[--block-bytes power-of-two] "
        "[--dataset synthetic] OR --dataset captured-root --root-n uint32 "
        "--root-seed uint64 --root-mode fixture|full --list A|B|C|all");
  }

  Options options;
  std::array<bool, 16> seen{};
  for (int index = 1; index < argc; index += 2) {
    const std::string_view name(argv[index]);
    const std::string_view value(argv[index + 1]);
    std::size_t slot = 0U;
    if (name == "--backend") {
      slot = 0U;
      options.backend_name = value;
      if (value == "current-scatter") {
        options.backend = ter::PartitionBackend::CurrentScatter;
      } else if (value == "block-inplace") {
        options.backend = ter::PartitionBackend::BlockInPlace;
      } else {
        throw std::runtime_error("invalid --backend: " + std::string(value));
      }
    } else if (name == "--n") {
      slot = 1U;
      options.sizes = parse_list<std::uint64_t>(value, "--n");
    } else if (name == "--bits") {
      slot = 2U;
      const auto parsed = parse_unsigned<std::uint32_t>(value, "--bits");
      if (parsed != 8U && parsed != 12U && parsed != 16U) {
        throw std::runtime_error("--bits must be 8, 12, or 16");
      }
      options.bits = static_cast<std::uint8_t>(parsed);
    } else if (name == "--shift") {
      slot = 3U;
      const auto parsed = parse_unsigned<std::uint32_t>(value, "--shift");
      if (parsed > std::numeric_limits<std::uint8_t>::max()) {
        throw std::runtime_error("--shift is out of range");
      }
      options.shift = static_cast<std::uint8_t>(parsed);
    } else if (name == "--threads") {
      slot = 4U;
      options.threads = parse_list<std::uint32_t>(value, "--threads");
    } else if (name == "--distribution") {
      slot = 5U;
      options.distribution = value;
      constexpr std::array<std::string_view, 6> allowed{
          "uniform", "hot", "alternating", "sorted", "reverse", "duplicates"};
      if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
        throw std::runtime_error("invalid --distribution: " + std::string(value));
      }
    } else if (name == "--seed") {
      slot = 6U;
      options.seed = parse_unsigned<std::uint64_t>(value, "--seed");
    } else if (name == "--warmup") {
      slot = 7U;
      options.warmup = parse_unsigned<std::uint32_t>(value, "--warmup");
    } else if (name == "--repeat") {
      slot = 8U;
      options.repeat = parse_unsigned<std::uint32_t>(value, "--repeat");
    } else if (name == "--output") {
      slot = 9U;
      options.output = std::filesystem::path(value);
    } else if (name == "--dataset") {
      slot = 10U;
      options.dataset = value;
      if (value != "synthetic" && value != "captured-root") {
        throw std::runtime_error("invalid --dataset: " + std::string(value));
      }
    } else if (name == "--root-n") {
      slot = 11U;
      options.root_n = parse_unsigned<std::uint32_t>(value, "--root-n");
    } else if (name == "--root-seed") {
      slot = 12U;
      options.root_seed = parse_unsigned<std::uint64_t>(value, "--root-seed");
    } else if (name == "--root-mode") {
      slot = 13U;
      options.root_mode = value;
      if (value != "fixture" && value != "full") {
        throw std::runtime_error("invalid --root-mode: " + std::string(value));
      }
    } else if (name == "--list") {
      slot = 14U;
      options.list = value;
      if (value != "A" && value != "B" && value != "C" && value != "all") {
        throw std::runtime_error("invalid --list: " + std::string(value));
      }
    } else if (name == "--block-bytes") {
      slot = 15U;
      options.block_bytes = parse_unsigned<std::uint32_t>(value, "--block-bytes");
    } else {
      throw std::runtime_error("unknown option: " + std::string(name));
    }
    if (seen[slot]) {
      throw std::runtime_error("duplicate option: " + std::string(name));
    }
    seen[slot] = true;
  }

  if (options.output.empty()) {
    throw std::runtime_error("--output must not be empty");
  }

  constexpr std::array<std::size_t, 7> common_slots{0U, 2U, 3U, 4U, 7U, 8U, 9U};
  for (const std::size_t slot : common_slots) {
    if (!seen[slot]) {
      throw std::runtime_error("a required benchmark option is missing");
    }
  }
  if (options.dataset == "synthetic") {
    constexpr std::array<std::size_t, 3> synthetic_slots{1U, 5U, 6U};
    for (const std::size_t slot : synthetic_slots) {
      if (!seen[slot]) {
        throw std::runtime_error("synthetic dataset requires --n, --distribution, and --seed");
      }
    }
    for (std::size_t slot = 11U; slot <= 14U; ++slot) {
      if (seen[slot]) {
        throw std::runtime_error("synthetic dataset does not accept captured-root options");
      }
    }
  } else {
    for (std::size_t slot = 11U; slot <= 14U; ++slot) {
      if (!seen[slot]) {
        throw std::runtime_error(
            "captured-root requires --root-n, --root-seed, --root-mode, and --list");
      }
    }
    if (seen[1U] || seen[5U] || seen[6U]) {
      throw std::runtime_error(
          "captured-root derives N/distribution/seed and rejects synthetic-only options");
    }
    if (options.root_n == 0U) {
      throw std::runtime_error("--root-n must be positive");
    }
  }
  if (options.repeat == 0U) {
    throw std::runtime_error("--repeat must be positive");
  }
  if (static_cast<std::uint32_t>(options.shift) + static_cast<std::uint32_t>(options.bits) >
      32U) {
    throw std::runtime_error("--shift plus --bits must not exceed 32");
  }
  if (options.block_bytes < sizeof(ter::TerPartitionEntry) ||
      (options.block_bytes & (options.block_bytes - 1U)) != 0U ||
      options.block_bytes % sizeof(ter::TerPartitionEntry) != 0U) {
    throw std::runtime_error(
        "--block-bytes must be a power of two at least as large as one record");
  }
  if (options.backend == ter::PartitionBackend::BlockInPlace && options.bits != 8U) {
    throw std::runtime_error("block-inplace benchmark requires --bits 8");
  }
  for (const std::uint64_t size : options.sizes) {
    if (size == 0U || size > std::numeric_limits<ter::EntryId>::max() ||
        size > std::numeric_limits<std::size_t>::max()) {
      throw std::runtime_error("each --n value must fit a nonzero EntryId range");
    }
  }
  for (const std::uint32_t thread_count : options.threads) {
    if (thread_count == 0U) {
      throw std::runtime_error("each --threads value must be positive");
    }
  }
  return options;
}

[[nodiscard]] std::uint32_t digit_mask(const std::uint8_t bits) noexcept {
  return (std::uint32_t{1} << bits) - 1U;
}

void add_value(ter::Instance& instance, const std::uint32_t index) {
  std::uint64_t carry = 0U;
  const std::size_t base = static_cast<std::size_t>(index) * instance.words_per_value;
  for (std::uint32_t word = 0U; word < instance.words_per_value; ++word) {
    const std::uint64_t original = instance.root_target_le[word];
    const std::uint64_t first = original + instance.values_le[base + word];
    const std::uint64_t carry_first = first < original ? 1U : 0U;
    const std::uint64_t second = first + carry;
    const std::uint64_t carry_second = second < first ? 1U : 0U;
    instance.root_target_le[word] = second;
    carry = carry_first | carry_second;
  }
}

[[nodiscard]] ter::Instance benchmark_instance(const std::uint32_t n,
                                               const std::uint64_t seed) {
  auto built = ter::build_instance(
      n, ter::limits::benchmark::kFullTerFixtureModulusBits, seed);
  if (built.status != ter::Status::Completed) {
    throw std::runtime_error("failed to build captured-root benchmark instance");
  }
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(), instance.planted_root.end(), std::uint8_t{0});
  std::fill(instance.root_target_le.begin(), instance.root_target_le.end(), 0ULL);
  const std::uint32_t half_width = instance.n / 2U;
  const std::uint32_t planted_per_half = instance.n / 4U;
  for (std::uint32_t half = 0U; half < 2U; ++half) {
    const std::uint32_t begin = half * half_width;
    for (std::uint32_t offset = 0U; offset < planted_per_half; ++offset) {
      const std::uint32_t coordinate = begin + offset;
      instance.planted_root[coordinate] = 1U;
      add_value(instance, coordinate);
    }
  }
  const std::uint32_t top_word_bits = instance.n % 64U;
  if (top_word_bits != 0U) {
    instance.root_target_le.back() &= (std::uint64_t{1} << top_word_bits) - 1U;
  }
  return instance;
}

[[nodiscard]] const char* status_name(const ter::Status status) noexcept {
  switch (status) {
    case ter::Status::Completed:
      return "Completed";
    case ter::Status::Found:
      return "Found";
    case ter::Status::NoWitnessThisAttempt:
      return "NoWitnessThisAttempt";
    case ter::Status::AttemptLimitReached:
      return "AttemptLimitReached";
    case ter::Status::InvalidConfiguration:
      return "InvalidConfiguration";
    case ter::Status::InvalidShape:
      return "InvalidShape";
    case ter::Status::GenerationIncomplete:
      return "GenerationIncomplete";
    case ter::Status::ResourceLimit:
      return "ResourceLimit";
    case ter::Status::Cancelled:
      return "Cancelled";
    case ter::Status::InvariantViolation:
      return "InvariantViolation";
    case ter::Status::NotApplicable:
      return "NotApplicable";
    case ter::Status::UnsupportedWidth:
      return "UnsupportedWidth";
  }
  return "InvalidStatus";
}

[[nodiscard]] std::uint32_t independent_quotient_low32(
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
      const std::uint32_t top_mask = (std::uint32_t{1} << (n % 32U)) - 1U;
      residue_word &= top_mask;
      target_word &= top_mask;
    }
    const std::uint64_t subtrahend = static_cast<std::uint64_t>(target_word) + borrow;
    delta[word] = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(residue_word) - subtrahend);
    borrow = static_cast<std::uint64_t>(residue_word) < subtrahend ? 1U : 0U;
  }
  if ((n % 32U) != 0U) {
    delta.back() &= (std::uint32_t{1} << (n % 32U)) - 1U;
  }
  const std::uint32_t quotient_bits = std::min<std::uint32_t>(32U, n - r1);
  std::uint32_t quotient = 0U;
  for (std::uint32_t bit = 0U; bit < quotient_bits; ++bit) {
    const std::uint32_t source_bit = r1 + bit;
    quotient |= ((delta[source_bit / 32U] >> (source_bit % 32U)) & 1U) << bit;
  }
  return quotient;
}

void verify_capture_records(const ter::FullTerRootCapture& capture,
                            const std::size_t list_index,
                            const std::span<const ter::TerPartitionEntry> records,
                            const std::uint8_t bits,
                            const std::uint8_t shift) {
  if (list_index >= capture.children.size() ||
      records.size() != capture.children[list_index].size() ||
      records.size() != capture.residues_words_le[list_index].size()) {
    throw std::runtime_error("captured-root record dimensions changed before timing");
  }
  const std::uint32_t mask = digit_mask(bits);
  for (std::size_t index = 0U; index < records.size(); ++index) {
    const std::uint32_t expected = independent_quotient_low32(
        capture.residues_words_le[list_index][index],
        capture.child_targets_words_le[list_index], capture.parameters.n,
        capture.parameters.r1);
    if (records[index].entry_id != static_cast<ter::EntryId>(index) ||
        records[index].low_k != expected ||
        ((records[index].low_k >> shift) & mask) != ((expected >> shift) & mask)) {
      throw std::runtime_error(
          "captured-root record failed independent quotient/bucket verification");
    }
  }
}

[[nodiscard]] std::uint32_t install_digit(const std::uint32_t random_value,
                                          const std::uint32_t digit,
                                          const std::uint8_t bits,
                                          const std::uint8_t shift) noexcept {
  const std::uint32_t mask = digit_mask(bits);
  const std::uint32_t projected_mask = mask << shift;
  return (random_value & ~projected_mask) | (digit << shift);
}

[[nodiscard]] std::vector<ter::TerPartitionEntry> generate_input(
    const std::uint64_t requested_size, const std::uint8_t bits, const std::uint8_t shift,
    const std::string_view distribution, const std::uint64_t seed) {
  const std::size_t size = static_cast<std::size_t>(requested_size);
  const std::uint32_t bucket_count = std::uint32_t{1} << bits;
  const std::uint32_t mask = bucket_count - 1U;
  std::vector<ter::TerPartitionEntry> entries;
  entries.reserve(size);
  std::uint64_t state = seed;
  const std::uint32_t hot_bucket = static_cast<std::uint32_t>(seed) & mask;
  const std::size_t hot_count = (size / kHotDenominator) * kHotNumerator +
                                ((size % kHotDenominator) * kHotNumerator) /
                                    kHotDenominator;
  const std::size_t records_per_bucket =
      std::max<std::size_t>(1U, (size + static_cast<std::size_t>(bucket_count) - 1U) /
                                    static_cast<std::size_t>(bucket_count));
  std::array<std::uint32_t, 8> duplicate_keys{};
  for (std::size_t index = 0U; index < duplicate_keys.size(); ++index) {
    duplicate_keys[index] = static_cast<std::uint32_t>(splitmix64(state));
  }

  for (std::size_t index = 0U; index < size; ++index) {
    const std::uint32_t random_value = static_cast<std::uint32_t>(splitmix64(state));
    std::uint32_t low_k = random_value;
    if (distribution == "hot") {
      std::uint32_t digit = hot_bucket;
      if (index >= hot_count) {
        digit = static_cast<std::uint32_t>(splitmix64(state) % (bucket_count - 1U));
        if (digit >= hot_bucket) {
          ++digit;
        }
      }
      low_k = install_digit(random_value, digit, bits, shift);
    } else if (distribution == "alternating") {
      low_k = install_digit(random_value, (index & 1U) == 0U ? 0U : mask, bits, shift);
    } else if (distribution == "sorted" || distribution == "reverse") {
      const std::uint32_t ascending = std::min<std::uint32_t>(
          mask, static_cast<std::uint32_t>(index / records_per_bucket));
      const std::uint32_t digit = distribution == "sorted" ? ascending : mask - ascending;
      low_k = install_digit(random_value, digit, bits, shift);
    } else if (distribution == "duplicates") {
      low_k = duplicate_keys[index % duplicate_keys.size()];
    }
    entries.push_back(
        {low_k, static_cast<ter::EntryId>(static_cast<std::uint64_t>(index))});
  }
  return entries;
}

[[nodiscard]] ReferenceDirectory build_reference(
    const std::span<const ter::TerPartitionEntry> input, const std::uint8_t bits,
    const std::uint8_t shift) {
  const std::size_t bucket_count = std::size_t{1} << bits;
  const std::uint32_t mask = digit_mask(bits);
  ReferenceDirectory reference;
  reference.buckets.resize(bucket_count);
  for (const ter::TerPartitionEntry& entry : input) {
    const std::uint32_t digit = (entry.low_k >> shift) & mask;
    reference.buckets[static_cast<std::size_t>(digit)].push_back(entry);
  }
  return reference;
}

[[nodiscard]] bool entry_less(const ter::TerPartitionEntry& left,
                              const ter::TerPartitionEntry& right) noexcept {
  return left.low_k < right.low_k ||
         (left.low_k == right.low_k && left.entry_id < right.entry_id);
}

void verify_output(const std::span<const ter::TerPartitionEntry> output,
                   const std::span<const std::uint64_t> offsets,
                   const ReferenceDirectory& reference, const std::uint8_t bits,
                   const std::uint8_t shift) {
  if (offsets.size() != reference.buckets.size() + 1U || offsets.front() != 0U ||
      offsets.back() != static_cast<std::uint64_t>(output.size())) {
    throw std::runtime_error("CSR boundary mismatch");
  }
  const std::uint32_t mask = digit_mask(bits);
  for (std::size_t bucket = 0U; bucket < reference.buckets.size(); ++bucket) {
    const std::uint64_t begin64 = offsets[bucket];
    const std::uint64_t end64 = offsets[bucket + 1U];
    if (begin64 > end64 || end64 > static_cast<std::uint64_t>(output.size()) ||
        end64 - begin64 !=
            static_cast<std::uint64_t>(reference.buckets[bucket].size())) {
      throw std::runtime_error("bucket count mismatch at bucket " + std::to_string(bucket));
    }
    const std::size_t begin = static_cast<std::size_t>(begin64);
    const std::size_t end = static_cast<std::size_t>(end64);
    std::vector<ter::TerPartitionEntry> actual(output.begin() +
                                                  static_cast<std::ptrdiff_t>(begin),
                                              output.begin() +
                                                  static_cast<std::ptrdiff_t>(end));
    std::vector<ter::TerPartitionEntry> expected = reference.buckets[bucket];
    for (const ter::TerPartitionEntry& entry : actual) {
      if (static_cast<std::size_t>((entry.low_k >> shift) & mask) != bucket) {
        throw std::runtime_error("CSR digit postcondition mismatch at bucket " +
                                 std::to_string(bucket));
      }
    }
    std::sort(actual.begin(), actual.end(), entry_less);
    std::sort(expected.begin(), expected.end(), entry_less);
    if (actual.size() != expected.size()) {
      throw std::runtime_error("bucket membership size mismatch");
    }
    for (std::size_t index = 0U; index < actual.size(); ++index) {
      if (actual[index].low_k != expected[index].low_k ||
          actual[index].entry_id != expected[index].entry_id) {
        throw std::runtime_error("full-record membership mismatch at bucket " +
                                 std::to_string(bucket));
      }
    }
  }
}

void fingerprint_mix(std::uint64_t* hash, const std::uint64_t value) noexcept {
  constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
  for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
    *hash ^= (value >> (byte * 8U)) & 0xFFU;
    *hash *= kFnvPrime;
  }
}

[[nodiscard]] std::uint64_t input_fingerprint(
    const std::span<const ter::TerPartitionEntry> entries) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  fingerprint_mix(&hash, static_cast<std::uint64_t>(entries.size()));
  for (const ter::TerPartitionEntry& entry : entries) {
    fingerprint_mix(&hash, entry.low_k);
    fingerprint_mix(&hash, entry.entry_id);
  }
  return hash;
}

[[nodiscard]] std::uint64_t output_fingerprint(
    const std::span<const ter::TerPartitionEntry> entries,
    const std::span<const std::uint64_t> offsets) {
  std::uint64_t hash = 1469598103934665603ULL;
  fingerprint_mix(&hash, static_cast<std::uint64_t>(offsets.size()));
  fingerprint_mix(&hash, static_cast<std::uint64_t>(entries.size()));
  for (std::size_t bucket = 0U; bucket + 1U < offsets.size(); ++bucket) {
    fingerprint_mix(&hash, bucket);
    fingerprint_mix(&hash, offsets[bucket + 1U] - offsets[bucket]);
    const std::size_t begin = static_cast<std::size_t>(offsets[bucket]);
    const std::size_t end = static_cast<std::size_t>(offsets[bucket + 1U]);
    std::vector<ter::TerPartitionEntry> canonical(
        entries.begin() + static_cast<std::ptrdiff_t>(begin),
        entries.begin() + static_cast<std::ptrdiff_t>(end));
    std::sort(canonical.begin(), canonical.end(), entry_less);
    for (const ter::TerPartitionEntry& entry : canonical) {
      fingerprint_mix(&hash, entry.low_k);
      fingerprint_mix(&hash, entry.entry_id);
    }
  }
  return hash;
}

[[nodiscard]] std::string hex64(const std::uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

[[nodiscard]] std::string json_escape(const std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20U) {
          throw std::runtime_error("control character is not permitted in JSON field");
        }
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

void write_row(std::ostream& output, const Options& options, const std::uint64_t size,
               const std::uint32_t threads, const std::uint32_t repetition,
               const std::uint64_t elapsed_ns, const ter::PartitionStats& stats,
               const std::uint64_t input_hash, const std::uint64_t output_hash,
               const CaptureMetadata* const capture_metadata = nullptr,
               const std::string_view capture_list = {}) {
  const std::string distribution = capture_metadata == nullptr
                                       ? options.distribution
                                       : "captured-root-" + std::string(capture_list);
  const std::uint64_t seed = capture_metadata == nullptr ? options.seed
                                                         : capture_metadata->seed;
  output << "{\"schema_version\":" << kSchemaVersion << ",\"backend\":\""
         << json_escape(options.backend_name) << "\",\"N\":" << size << ",\"bits\":"
         << static_cast<std::uint32_t>(options.bits) << ",\"shift\":"
         << static_cast<std::uint32_t>(options.shift) << ",\"threads_requested\":" << threads
         << ",\"threads_used\":" << threads << ",\"distribution\":\""
         << json_escape(distribution) << "\",\"seed\":" << seed
         << ",\"repetition\":" << repetition << ",\"elapsed_ns\":" << elapsed_ns
         << ",\"histogram_ns\":" << stats.histogram_ns << ",\"prefix_ns\":"
         << stats.prefix_ns << ",\"scatter_ns\":" << stats.scatter_ns
         << ",\"local_classification_ns\":" << stats.local_classification_ns
         << ",\"block_permute_ns\":" << stats.block_permute_ns << ",\"cleanup_ns\":"
         << stats.cleanup_ns << ",\"scratch_peak_bytes\":" << stats.scratch_peak_bytes
         << ",\"record_scratch_bytes\":" << stats.record_scratch_bytes
         << ",\"block_buffer_bytes\":" << stats.block_buffer_bytes
         << ",\"records_classified\":" << stats.records_classified
         << ",\"records_written\":" << stats.records_written << ",\"blocks_flushed\":"
         << stats.blocks_flushed << ",\"blocks_permuted\":" << stats.blocks_permuted
         << ",\"input_fingerprint\":\"" << hex64(input_hash)
         << "\",\"output_bucket_fingerprint\":\"" << hex64(output_hash) << "\"";
  if (capture_metadata != nullptr) {
    output << ",\"dataset\":\"captured-root\",\"capture_generation_ns\":"
           << capture_metadata->generation_ns << ",\"root_n\":" << capture_metadata->n
           << ",\"root_seed\":" << capture_metadata->seed << ",\"root_mode\":\""
           << json_escape(capture_metadata->mode)
           << "\",\"root_target_mode\":\""
           << json_escape(capture_metadata->target_mode)
           << "\",\"capture_solver_status\":\""
           << json_escape(capture_metadata->solver_status)
           << "\",\"capture_status\":\"Completed\",\"capture_A_size\":"
           << capture_metadata->sizes[0] << ",\"capture_B_size\":"
           << capture_metadata->sizes[1] << ",\"capture_C_size\":"
           << capture_metadata->sizes[2] << ",\"capture_r1\":"
           << capture_metadata->r1 << ",\"capture_Q\":"
           << capture_metadata->quotient_bits << ",\"capture_list\":\""
           << json_escape(capture_list) << "\"";
  }
  output << ",\"status\":\"Completed\"}\n";
  if (!output) {
    throw std::runtime_error("failed while writing benchmark JSONL");
  }
}

void run_input_case(std::ostream& output, const Options& options,
                    const std::span<const ter::TerPartitionEntry> input,
                    const std::uint32_t threads,
                    const ter::FullTerRootCapture* const capture = nullptr,
                    const std::size_t capture_list_index = 0U,
                    const CaptureMetadata* const capture_metadata = nullptr,
                    const std::string_view capture_list = {}) {
  const std::uint64_t size = static_cast<std::uint64_t>(input.size());
  const ReferenceDirectory reference = build_reference(input, options.bits, options.shift);
  const std::uint64_t input_hash = input_fingerprint(input);
  const std::size_t bucket_count = std::size_t{1} << options.bits;
  ter::PartitionSpec spec;
  spec.backend = options.backend;
  spec.bits = options.bits;
  spec.shift = options.shift;
  spec.threads = threads;
  spec.block_bytes = options.block_bytes;
  ter::PartitionWorkspace workspace;

  const auto invoke = [&](const bool measured, const std::uint32_t repetition) {
    std::vector<ter::TerPartitionEntry> working(input.begin(), input.end());
    std::vector<std::uint64_t> offsets(bucket_count + 1U, 0U);
    ter::PartitionStats stats;
    const std::uint64_t repetition_input_hash = input_fingerprint(working);
    if (repetition_input_hash != input_hash) {
      throw std::runtime_error("input reset fingerprint mismatch before partition_chunk");
    }
    if (capture != nullptr) {
      verify_capture_records(*capture, capture_list_index, working, options.bits,
                             options.shift);
    }
    const auto started = Clock::now();
    const ter::Status status = ter::partition_chunk(working, spec, workspace, offsets, stats);
    const auto finished = Clock::now();
    if (status != ter::Status::Completed) {
      throw std::runtime_error("partition_chunk returned a non-Completed status");
    }
    verify_output(working, offsets, reference, options.bits, options.shift);
    if (input_fingerprint(input) != input_hash) {
      throw std::runtime_error("input fixture changed between repetitions");
    }
    if (measured) {
      const auto duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
      if (duration <= 0) {
        throw std::runtime_error("partition_chunk elapsed time was nonpositive");
      }
      write_row(output, options, size, threads, repetition,
                static_cast<std::uint64_t>(duration), stats, input_hash,
                output_fingerprint(working, offsets), capture_metadata, capture_list);
    }
  };

  for (std::uint32_t warmup = 0U; warmup < options.warmup; ++warmup) {
    invoke(false, warmup);
  }
  for (std::uint32_t repetition = 0U; repetition < options.repeat; ++repetition) {
    invoke(true, repetition);
  }
}

void run_synthetic_case(std::ostream& output, const Options& options,
                        const std::uint64_t size, const std::uint32_t threads) {
  const auto input = generate_input(size, options.bits, options.shift, options.distribution,
                                    options.seed);
  run_input_case(output, options, input, threads);
}

struct GeneratedCapture {
  ter::FullTerRootCapture capture;
  CaptureMetadata metadata;
};

[[nodiscard]] GeneratedCapture generate_capture(const Options& options) {
  const ter::Instance instance = benchmark_instance(options.root_n, options.root_seed);
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = options.root_mode == "fixture"
                              ? ter::TerExecutionMode::FixtureTer
                              : ter::TerExecutionMode::FullTer;
  // Match ter_full_bench's default target construction. The capture is
  // populated before the root stage, so the later solver status is telemetry,
  // not a prerequisite or a substitute for capture.status.
  config.target_mode = ter::TerTargetMode::Random;
  config.seed = options.root_seed;
  config.attempt_limit = ter::limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = ter::limits::benchmark::kFullTerMemoryBytes;
  config.fixture_level3_limit = ter::limits::full_ter::kDefaultFixtureLevel3Limit;
  config.audit = false;

  GeneratedCapture generated;
  config.root_capture = &generated.capture;
  const auto started = Clock::now();
  const ter::FullTerResult solved = ter::solve_full_ter(instance, config);
  const auto finished = Clock::now();
  generated.metadata.generation_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  generated.metadata.n = options.root_n;
  generated.metadata.seed = options.root_seed;
  generated.metadata.mode = options.root_mode;
  generated.metadata.target_mode = "random";
  generated.metadata.solver_status = status_name(solved.status);
  if (generated.capture.status != ter::Status::Completed) {
    throw std::runtime_error(
        "captured-root seam did not complete (status=" +
        std::string(status_name(generated.capture.status)) + "): " +
        generated.capture.message);
  }
  generated.metadata.r1 = generated.capture.parameters.r1;
  generated.metadata.quotient_bits =
      generated.capture.parameters.n - generated.capture.parameters.r1;
  for (std::size_t list = 0U; list < generated.capture.children.size(); ++list) {
    generated.metadata.sizes[list] =
        static_cast<std::uint64_t>(generated.capture.children[list].size());
  }

  ter::FullTerRootCapture replay_capture;
  config.root_capture = &replay_capture;
  const ter::FullTerResult replay = ter::solve_full_ter(instance, config);
  if (replay.status != solved.status || replay.witness != solved.witness ||
      replay.telemetry.fingerprint != solved.telemetry.fingerprint ||
      replay_capture.status != generated.capture.status ||
      replay_capture.seed != generated.capture.seed ||
      replay_capture.execution_mode != generated.capture.execution_mode ||
      replay_capture.parameters.n != generated.capture.parameters.n ||
      replay_capture.parameters.r1 != generated.capture.parameters.r1 ||
      replay_capture.children != generated.capture.children ||
      replay_capture.residues_words_le != generated.capture.residues_words_le ||
      replay_capture.child_targets_words_le != generated.capture.child_targets_words_le ||
      replay_capture.root_target_words_le != generated.capture.root_target_words_le) {
    throw std::runtime_error("equivalent captured-root runs were not deterministic");
  }
  return generated;
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    GeneratedCapture captured;
    if (options.dataset == "captured-root") {
      captured = generate_capture(options);
      if (static_cast<std::uint32_t>(options.shift) +
              static_cast<std::uint32_t>(options.bits) >
          std::min<std::uint32_t>(32U, captured.metadata.quotient_bits)) {
        throw std::runtime_error(
            "--shift plus --bits exceeds the captured low-quotient width");
      }
      std::cerr << "captured-root: status=Completed solver_status="
                << captured.metadata.solver_status << " n=" << captured.metadata.n
                << " seed=" << captured.metadata.seed << " mode=" << captured.metadata.mode
                << " target=" << captured.metadata.target_mode
                << " A=" << captured.metadata.sizes[0]
                << " B=" << captured.metadata.sizes[1]
                << " C=" << captured.metadata.sizes[2]
                << " r1=" << captured.metadata.r1
                << " Q=" << captured.metadata.quotient_bits
                << " generation_ns=" << captured.metadata.generation_ns << '\n';
    }

    const std::filesystem::path parent = options.output.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    std::ofstream output(options.output, std::ios::out | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("could not open output path: " + options.output.string());
    }
    if (options.dataset == "synthetic") {
      for (const std::uint64_t size : options.sizes) {
        for (const std::uint32_t threads : options.threads) {
          run_synthetic_case(output, options, size, threads);
        }
      }
    } else {
      constexpr std::array<std::string_view, 3> list_names{"A", "B", "C"};
      for (std::size_t list_index = 0U; list_index < list_names.size(); ++list_index) {
        if (options.list != "all" && options.list != list_names[list_index]) {
          continue;
        }
        const auto converted = ter::root_partition_bench::capture_list_to_records(
            captured.capture, list_index);
        if (converted.status != ter::Status::Completed) {
          throw std::runtime_error(
              "captured-root list conversion failed (status=" +
              std::string(status_name(converted.status)) + "): " + converted.message);
        }
        if (converted.records.empty()) {
          throw std::runtime_error("captured-root selected list is empty");
        }
        verify_capture_records(captured.capture, list_index, converted.records,
                               options.bits, options.shift);
        for (const std::uint32_t threads : options.threads) {
          run_input_case(output, options, converted.records, threads, &captured.capture,
                         list_index, &captured.metadata, list_names[list_index]);
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ter_root_partition_bench: " << error.what() << '\n';
    return 1;
  }
}
