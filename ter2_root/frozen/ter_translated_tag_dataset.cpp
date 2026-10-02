#include "ter_translated_tag_dataset.hpp"

#include "ter/instance.hpp"
#include "ter/limitations.hpp"
#include "ter_microreport8_dataset.hpp"
#include "ter_root_capture_adapter.hpp"

#include <algorithm>

namespace ter::translated_tag {
namespace {

// ---------------------------------------------------------------------------
// Verbatim transplant from bench/ter_root_pairspace_probe.cpp, identical to the
// copy in bench/ter_microreport8_dataset.cpp.
//
// These two functions define the authentic n=72 root fixture. They are copied
// byte-for-byte so this spike observes exactly the same root as the existing
// pair-space probe, and the donor file is left untouched. The behavioural
// identity gate is the observed geometry (Q, |A|, |B|, |C|), checked below.
// Do not "improve" either function: divergence silently changes the dataset.
// ---------------------------------------------------------------------------
void add_value(ter::Instance& instance, std::uint32_t index) {
  std::uint64_t carry = 0;
  const std::size_t base = static_cast<std::size_t>(index) * instance.words_per_value;
  for (std::uint32_t word = 0; word < instance.words_per_value; ++word) {
    const std::uint64_t original = instance.root_target_le[word];
    const std::uint64_t first = original + instance.values_le[base + word];
    const std::uint64_t carry_first = first < original ? 1U : 0U;
    const std::uint64_t second = first + carry;
    const std::uint64_t carry_second = second < first ? 1U : 0U;
    instance.root_target_le[word] = second;
    carry = carry_first | carry_second;
  }
}

ter::Instance benchmark_instance(std::uint32_t n, std::uint64_t seed) {
  auto built =
      ter::build_instance(n, ter::limits::benchmark::kFullTerFixtureModulusBits, seed);
  if (built.status != ter::Status::Completed) {
    return {};
  }
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(), instance.planted_root.end(), std::uint8_t{0});
  std::fill(instance.root_target_le.begin(), instance.root_target_le.end(), 0ULL);
  const std::uint32_t half_width = instance.n / 2U;
  const std::uint32_t planted_per_half = instance.n / 4U;
  for (std::uint32_t half = 0; half < 2U; ++half) {
    const std::uint32_t begin = half * half_width;
    for (std::uint32_t offset = 0; offset < planted_per_half; ++offset) {
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
// --------------------------- end verbatim transplant -----------------------

[[nodiscard]] std::size_t word_count_for(const std::uint32_t n) noexcept {
  return (static_cast<std::size_t>(n) + 31U) / 32U;
}

void mask_to_width(std::vector<std::uint32_t>& words, const std::uint32_t n) noexcept {
  const std::size_t needed = word_count_for(n);
  words.resize(needed, 0U);
  if ((n % 32U) != 0U) {
    words[needed - 1U] &= (std::uint32_t{1} << (n % 32U)) - 1U;
  }
}

}  // namespace

AuthorityTables build_authority_tables(const std::uint32_t root_n,
                                       const std::uint64_t root_seed) {
  AuthorityTables tables;

  const auto instance = benchmark_instance(root_n, root_seed);
  if (instance.n == 0U) {
    tables.message = "failed to build the authentic root probe instance";
    return tables;
  }

  FullTerRootCapture capture;
  FullTerConfig config;
  config.family = ShapeFamily::FiniteSanity;
  config.execution_mode = TerExecutionMode::FullTer;
  config.target_mode = TerTargetMode::Random;
  config.seed = root_seed;
  config.attempt_limit = limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = limits::benchmark::kFullTerMemoryBytes;
  config.root_capture = &capture;
  const auto solved = solve_full_ter(instance, config);
  static_cast<void>(solved);
  if (capture.status != Status::Completed) {
    tables.message = "authority root capture did not complete: " + capture.message;
    return tables;
  }

  tables.n = capture.parameters.n;
  tables.r1 = capture.parameters.r1;
  tables.quotient_bits = capture.parameters.n - capture.parameters.r1;
  tables.residue_words = static_cast<std::uint32_t>(word_count_for(tables.n));

  for (std::size_t group = 0U; group < 3U; ++group) {
    auto converted = root_partition_bench::capture_list_to_records(capture, group);
    if (converted.status != Status::Completed) {
      tables.message = "authority list conversion failed: " + converted.message;
      return tables;
    }
    const std::size_t size = converted.records.size();
    tables.list_sizes[group] = size;
    tables.quotient[group].resize(size);
    for (std::size_t index = 0U; index < size; ++index) {
      tables.quotient[group][index] = converted.records[index].low_k;
    }
    tables.residue[group].assign(size * tables.residue_words, 0U);
    for (std::size_t index = 0U; index < size; ++index) {
      const auto& words = capture.residues_words_le[group][index];
      for (std::size_t word = 0U; word < words.size() && word < tables.residue_words;
           ++word) {
        tables.residue[group][index * tables.residue_words + word] = words[word];
      }
    }
    tables.digits[group].assign(size * tables.n, std::int8_t{0});
    for (std::size_t index = 0U; index < size; ++index) {
      const auto& vector = capture.children[group][index];
      if (vector.size() != tables.n) {
        tables.message = "captured child entry has malformed digit width";
        return tables;
      }
      std::copy(vector.begin(), vector.end(),
                tables.digits[group].begin() + static_cast<std::ptrdiff_t>(index) * tables.n);
    }
  }

  tables.root_target = capture.root_target_words_le;
  mask_to_width(tables.root_target, tables.n);

  // tau, derived exactly as the donor derives it, from the same donor routine.
  std::array<std::vector<std::uint32_t>, 3> child_targets;
  for (std::size_t group = 0U; group < 3U; ++group) {
    child_targets[group] = capture.child_targets_words_le[group];
    mask_to_width(child_targets[group], tables.n);
  }
  const auto sum32 = microreport8::sum_children_mod_2n_limb32(child_targets, tables.n);
  const auto sum16 = microreport8::sum_children_mod_2n_limb16(child_targets, tables.n);
  if (sum32 != sum16 ||
      !microreport8::sum_children_residue_check(child_targets, sum32, tables.n)) {
    tables.message = "authority tau child-target summation failed independent verification";
    return tables;
  }
  tables.tau_low32 = root_partition_bench::quotient_low32_from_words(
      capture.root_target_words_le, sum32, tables.n, tables.r1);

  const auto shape = build_shape(config.family, tables.n);
  tables.shape_available = shape.status == Status::Completed;
  if (tables.shape_available) {
    tables.expected_parent = shape.shape.parent;
  }

  tables.bytes = 0;
  for (std::size_t group = 0U; group < 3U; ++group) {
    tables.bytes += tables.quotient[group].size() * sizeof(std::uint32_t);
    tables.bytes += tables.residue[group].size() * sizeof(std::uint32_t);
    tables.bytes += tables.digits[group].size() * sizeof(std::int8_t);
  }
  tables.ok = true;
  return tables;
}

bool residue_sum_matches_root(const AuthorityTables& tables,
                              const EntryId first,
                              const EntryId second,
                              const EntryId third) noexcept {
  const std::uint32_t words = tables.residue_words;
  const std::uint32_t* const a = tables.residue[0].data() + static_cast<std::size_t>(first) * words;
  const std::uint32_t* const b = tables.residue[1].data() + static_cast<std::size_t>(second) * words;
  const std::uint32_t* const c = tables.residue[2].data() + static_cast<std::size_t>(third) * words;
  const std::uint32_t top_bits = tables.n % 32U;
  std::uint64_t carry = 0U;
  for (std::uint32_t word = 0U; word < words; ++word) {
    std::uint64_t total = carry;
    total += a[word];
    total += b[word];
    total += c[word];
    std::uint32_t value = static_cast<std::uint32_t>(total & 0xFFFFFFFFU);
    carry = total >> 32U;
    if (word + 1U == words && top_bits != 0U) {
      value &= (std::uint32_t{1} << top_bits) - 1U;
    }
    if (value != tables.root_target[word]) {
      return false;
    }
  }
  return true;
}

bool structural_parent_valid(const AuthorityTables& tables,
                             const EntryId first,
                             const EntryId second,
                             const EntryId third) noexcept {
  if (!tables.shape_available) {
    return false;
  }
  const std::uint32_t width = tables.n;
  const std::int8_t* const a = tables.digits[0].data() + static_cast<std::size_t>(first) * width;
  const std::int8_t* const b = tables.digits[1].data() + static_cast<std::size_t>(second) * width;
  const std::int8_t* const c = tables.digits[2].data() + static_cast<std::size_t>(third) * width;
  DigitCounts counts;
  for (std::uint32_t index = 0U; index < width; ++index) {
    const int sum = static_cast<int>(a[index]) + b[index] + c[index];
    if (sum < -1 || sum > 1) {
      return false;  // ternary closure violated: not a valid parent digit
    }
    if (sum > 0) {
      ++counts.positive;
    } else if (sum < 0) {
      ++counts.negative;
    } else {
      ++counts.zero;
    }
  }
  return counts.negative == tables.expected_parent.negative &&
         counts.zero == tables.expected_parent.zero &&
         counts.positive == tables.expected_parent.positive;
}

}  // namespace ter::translated_tag
