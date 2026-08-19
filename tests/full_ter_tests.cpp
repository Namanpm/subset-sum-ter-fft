#include "ter/full_ter.hpp"
#include "ter/merge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

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

ter::Instance balanced_instance(std::uint32_t n, std::uint64_t seed) {
  auto built = ter::build_instance(n, 10U, seed);
  require(built.status == ter::Status::Completed, "balanced test instance must build");
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(),
            instance.planted_root.end(),
            std::uint8_t{0});
  std::fill(instance.root_target_le.begin(), instance.root_target_le.end(), 0U);
  for (std::uint32_t half = 0; half < 2U; ++half) {
    const std::uint32_t begin = half * (n / 2U);
    for (std::uint32_t offset = 0; offset < n / 4U; ++offset) {
      const std::uint32_t coordinate = begin + offset;
      instance.planted_root[coordinate] = 1U;
      add_value(instance, coordinate);
    }
  }
  const std::uint32_t high_bits = n % 64U;
  if (high_bits != 0U) {
    instance.root_target_le.back() &= (std::uint64_t{1} << high_bits) - 1U;
  }
  return instance;
}

bool independent_weight_half_solution_exists(const ter::Instance& instance) {
  require(instance.n == 36U && instance.words_per_value == 1U,
          "independent test oracle is specialized to the complete n=36 case");
  constexpr std::uint32_t half = 18U;
  constexpr std::uint32_t subset_count = 1U << half;
  constexpr std::uint64_t modulus_mask = (std::uint64_t{1} << 36U) - 1U;
  std::array<std::vector<std::uint64_t>, half + 1U> left_by_weight;
  std::vector<std::uint64_t> sums(subset_count, 0U);
  std::vector<std::uint8_t> weights(subset_count, 0U);
  for (std::uint32_t subset = 1U; subset < subset_count; ++subset) {
    const std::uint32_t bit =
        static_cast<std::uint32_t>(std::countr_zero(subset));
    const std::uint32_t previous = subset & (subset - 1U);
    sums[subset] =
        (sums[previous] + instance.values_le[bit]) & modulus_mask;
    weights[subset] = static_cast<std::uint8_t>(weights[previous] + 1U);
  }
  for (std::uint32_t subset = 0U; subset < subset_count; ++subset) {
    left_by_weight[weights[subset]].push_back(sums[subset]);
  }
  for (auto& residues : left_by_weight) {
    std::sort(residues.begin(), residues.end());
  }

  std::fill(sums.begin(), sums.end(), 0U);
  std::fill(weights.begin(), weights.end(), std::uint8_t{0});
  for (std::uint32_t subset = 1U; subset < subset_count; ++subset) {
    const std::uint32_t bit =
        static_cast<std::uint32_t>(std::countr_zero(subset));
    const std::uint32_t previous = subset & (subset - 1U);
    sums[subset] =
        (sums[previous] + instance.values_le[half + bit]) & modulus_mask;
    weights[subset] = static_cast<std::uint8_t>(weights[previous] + 1U);
  }
  for (std::uint32_t subset = 0U; subset < subset_count; ++subset) {
    const std::uint32_t right_weight = weights[subset];
    if (right_weight > half) {
      continue;
    }
    const std::uint32_t left_weight = half - right_weight;
    const std::uint64_t required =
        (instance.root_target_le[0] - sums[subset]) & modulus_mask;
    const auto& candidates = left_by_weight[left_weight];
    if (std::binary_search(candidates.begin(), candidates.end(), required)) {
      return true;
    }
  }
  return false;
}

void test_parameters() {
  const auto finite =
      ter::derive_full_ter_parameters(ter::ShapeFamily::FiniteSanity, 36U, 36U);
  require(finite.status == ter::Status::Completed,
          "finite sanity parameters must derive");
  require(finite.value.level1.negative == 0U &&
              finite.value.level1.positive == 6U,
          "finite sanity L1 distribution must be D[0,1/6]");
  require(finite.value.level2.negative == 0U &&
              finite.value.level2.positive == 2U,
          "finite sanity L2 distribution must be D[0,1/18]");
  require(finite.value.level3_active_half.positive == 1U,
          "finite sanity L3 active half must contain one positive digit");
  require(finite.value.r2 > 0U && finite.value.r2 < finite.value.r1 &&
              finite.value.r1 < 36U,
          "representation counts must produce nested modulus bits");
  require(finite.value.representation1.decimal == "17153136" &&
              finite.value.representation2.decimal == "90" &&
              finite.value.r1 == 12U && finite.value.r2 == 3U,
          "finite sanity representation counts must match exact multinomials");
  require(!finite.value.representation1.decimal.empty() &&
              !finite.value.representation2.decimal.empty(),
          "exact representation counts must be exposed");

  const auto paper =
      ter::derive_full_ter_parameters(ter::ShapeFamily::PaperRounded, 360U, 360U);
  require(paper.status == ter::Status::Completed,
          "paper-rounded n=360 parameters must derive");
  require(paper.value.level1.negative == 6U &&
              paper.value.level2.negative == 2U,
          "paper-rounded n=360 finite distributions must match the T0 shape");
  require(paper.value.r1 > 180U && paper.value.r1 < 230U &&
              paper.value.r2 > 45U && paper.value.r2 < 80U,
          "paper-rounded exact representation moduli must remain near asymptotic values");
  require(paper.value.asymptotic.time_exponent == 0.2400 &&
              paper.value.asymptotic.space_exponent == 0.2221,
          "paper asymptotic sanity references must remain telemetry, not pruning rules");

  const auto larger_paper =
      ter::derive_full_ter_parameters(ter::ShapeFamily::PaperRounded, 3600U, 3600U);
  require(larger_paper.status == ter::Status::Completed &&
              larger_paper.value.epsilon1_level2 % 2U == 0U &&
              larger_paper.value.epsilon0_level2 % 2U == 0U &&
              larger_paper.value.epsilon_negative1_level2 % 2U == 0U,
          "paper-rounded policy must correct non-integral level-2 rates deterministically");

  const auto invalid =
      ter::derive_full_ter_parameters(ter::ShapeFamily::FiniteSanity, 35U, 35U);
  require(invalid.status == ter::Status::InvalidConfiguration,
          "non-divisible finite dimensions must be rejected");

  const auto paper_l3 = ter::generate_level3_lists(paper.value,
                                                   ter::TerExecutionMode::FullTer,
                                                   1U,
                                                   512ULL * 1024ULL * 1024ULL);
  require(paper_l3.status == ter::Status::ResourceLimit,
          "complete paper-rounded n=360 L3 generation must refuse impossible resources");
  const auto paper_fixture = ter::generate_level3_lists(
      paper.value, ter::TerExecutionMode::FixtureTer, 9U, 16ULL * 1024ULL * 1024ULL);
  require(paper_fixture.status == ter::Status::Completed &&
              !paper_fixture.value.complete &&
              paper_fixture.value.complete_cardinality_overflow &&
              paper_fixture.value.first_half.size() == 9U &&
              paper_fixture.value.second_half.size() == 9U,
          "reduced n=360 fixture must expose, not hide, complete-cardinality overflow");
}

void test_level3_generation() {
  const auto parameters =
      ter::derive_full_ter_parameters(ter::ShapeFamily::FiniteSanity, 36U, 36U);
  const auto generated = ter::generate_level3_lists(parameters.value,
                                                    ter::TerExecutionMode::FullTer,
                                                    1U,
                                                    16U * 1024U * 1024U);
  require(generated.status == ter::Status::Completed,
          "complete tiny L3 generation must succeed");
  require(generated.value.complete && generated.value.complete_cardinality == 18U,
          "complete tiny L3 cardinality must equal the multinomial count");
  require(generated.value.first_half.size() == 18U &&
              generated.value.second_half.size() == 18U,
          "two physical L3 buffers must materialize the complete sets");
  for (const auto& vector : generated.value.first_half) {
    const auto counts = ter::count_digits(vector);
    require(counts.has_value() &&
                counts->negative == parameters.value.level3_active_half.negative &&
                counts->positive == parameters.value.level3_active_half.positive,
            "odd L3 vectors must have the exact active-half distribution");
    require(std::all_of(vector.begin() + 18, vector.end(),
                        [](std::int8_t digit) { return digit == 0; }),
            "odd L3 list must have first-half support only");
  }
  for (const auto& vector : generated.value.second_half) {
    const auto counts = ter::count_digits(vector);
    require(counts.has_value() &&
                counts->negative == parameters.value.level3_active_half.negative &&
                counts->positive == parameters.value.level3_active_half.positive,
            "even L3 vectors must have the exact active-half distribution");
    require(std::all_of(vector.begin(), vector.begin() + 18,
                        [](std::int8_t digit) { return digit == 0; }),
            "even L3 list must have second-half support only");
  }
  const auto replay = ter::generate_level3_lists(parameters.value,
                                                 ter::TerExecutionMode::FullTer,
                                                 1U,
                                                 16U * 1024U * 1024U);
  require(replay.status == ter::Status::Completed &&
              replay.value.first_half == generated.value.first_half &&
              replay.value.second_half == generated.value.second_half,
          "complete L3 combinatorial generation must replay deterministically");

  std::vector<ter::ReferenceTernaryVector> naive_first;
  std::vector<ter::ReferenceTernaryVector> naive_second;
  for (std::uint32_t coordinate = 0; coordinate < 18U; ++coordinate) {
    ter::ReferenceTernaryVector first(36U, 0);
    ter::ReferenceTernaryVector second(36U, 0);
    first[coordinate] = 1;
    second[18U + coordinate] = 1;
    naive_first.push_back(std::move(first));
    naive_second.push_back(std::move(second));
  }
  require(naive_first == generated.value.first_half &&
              naive_second == generated.value.second_half,
          "specialized L3 generation must equal the tiny naive authority");
}

void test_guided_pipeline_and_replay() {
  const auto instance = balanced_instance(36U, 0x544552FULL);
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = 91U;
  config.attempt_limit = 1U;
  config.fixture_level3_limit = 18U;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  const auto first = ter::solve_full_ter(instance, config);
  require(first.status == ter::Status::Found,
          "guided 18-to-9-to-3-to-1 pipeline must find its planted root");
  require(first.telemetry.logical_level3_lists == 18U &&
              first.telemetry.physical_level3_lists == 2U,
          "L3 sharing must preserve the logical 18-node topology");
  require(first.telemetry.hot_entry_bytes >= sizeof(std::uint64_t) &&
              first.telemetry.packed_vector_bytes == 16U,
          "full TER telemetry must expose its real entry and packed-vector widths");
  require(first.telemetry.attempts.size() == 1U &&
              first.telemetry.attempts[0].level0_size > 0U,
          "guided execution must populate the root list");
  require(first.telemetry.attempts[0].seed == config.seed &&
              first.telemetry.level3_allocated_bytes > 0U &&
              first.telemetry.attempts[0].level2_allocated_bytes > 0U &&
              first.telemetry.attempts[0].level1_allocated_bytes > 0U &&
              first.telemetry.attempts[0].level0_allocated_bytes > 0U &&
              first.telemetry.attempts[0].level3_to_level2.allocated_bytes > 0U &&
              first.telemetry.attempts[0].level2_to_level1.allocated_bytes > 0U &&
              first.telemetry.attempts[0].level1_to_level0.allocated_bytes > 0U &&
              first.telemetry.attempts[0].level3_to_level2.output_fingerprint != 0U &&
              first.telemetry.attempts[0].level2_to_level1.output_fingerprint != 0U &&
              first.telemetry.attempts[0].level1_to_level0.output_fingerprint != 0U,
          "full TER telemetry must expose attempt seeds and per-level allocation");
  require(first.telemetry.attempts[0].level2_to_level1.tiled_used,
          "finite-sanity native L2-to-L1 must select the exact tiled reporter");
  require(first.telemetry.attempts[0].level2_to_level1.pair_records == 0U,
          "the tiled L2-to-L1 path must not materialize native pair records");
  require(first.telemetry.terminal_status == ter::Status::Found &&
              first.telemetry.witness_found &&
              first.telemetry.total_solver_ns > 0U &&
              first.telemetry.fingerprint != 0U,
          "successful full TER telemetry must expose terminal run state");
  require(ter::verify_full_ter_witness(instance, first.witness).status ==
              ter::Status::Completed,
          "independent root witness verification must pass");
  require(independent_weight_half_solution_exists(instance),
          "independent binary MITM oracle must confirm the end-to-end instance is SAT");

  const auto second = ter::solve_full_ter(instance, config);
  require(second.status == first.status && second.witness == first.witness &&
              second.telemetry.fingerprint == first.telemetry.fingerprint,
          "fixed-seed guided execution must replay exactly");

  config.execution_mode = ter::TerExecutionMode::FixtureTer;
  config.fixture_level3_limit = 9U;
  const auto reduced = ter::solve_full_ter(instance, config);
  require(reduced.status == ter::Status::Found &&
              reduced.telemetry.execution_mode == ter::TerExecutionMode::FixtureTer,
          "explicit reduced guided fixture must retain a complete planted path");

  auto invalid_witness = first.witness;
  invalid_witness[0] = -1;
  require(ter::verify_full_ter_witness(instance, invalid_witness).status ==
              ter::Status::InvariantViolation,
          "non-binary root witnesses must fail closed");

  auto wrong_weight = first.witness;
  const auto selected = std::find(wrong_weight.begin(), wrong_weight.end(), 1);
  require(selected != wrong_weight.end(), "guided witness must contain a selected item");
  *selected = 0;
  require(ter::verify_full_ter_witness(instance, wrong_weight).status ==
              ter::Status::InvariantViolation,
          "wrong-weight binary root witnesses must fail closed");

  auto wrong_target_instance = instance;
  wrong_target_instance.root_target_le[0] ^= 1U;
  require(ter::verify_full_ter_witness(wrong_target_instance, first.witness).status ==
              ter::Status::InvariantViolation,
          "right-weight binary witnesses with the wrong full target must fail closed");
}

void test_authentic_l2_capture_tiled_merge_matches_exact_authorities() {
  const auto instance = balanced_instance(36U, 0x54494C4544434150ULL);
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = 0x4C324C31ULL;
  config.attempt_limit = 1U;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  config.audit = true;
  const auto captured = ter::capture_full_ter_l2_merge_snapshot(instance, config, 0U);
  require(captured.status == ter::Status::Completed,
          "the bounded complete FullTer L2 capture must succeed");
  if (captured.status != ter::Status::Completed) {
    return;
  }
  const auto cubic = ter::run_cubic_oracle(captured.snapshot, ter::OutputPolicy::Correctness);
  const auto baseline = ter::run_pair_baseline(captured.snapshot, ter::OutputPolicy::Correctness);
  const auto tiled = ter::run_pair_tiled(captured.snapshot, ter::OutputPolicy::Correctness);
  require(cubic.status == ter::Status::Completed && baseline.status == ter::Status::Completed &&
              tiled.status == ter::Status::Completed,
          "every exact L2-to-L1 authority must complete on the authentic bounded capture");
  require(tiled.parents == baseline.parents && tiled.parents == cubic.parents,
          "tiled authentic L2-to-L1 parents must equal baseline and cubic parents");
  require(tiled.valid_provenance == baseline.valid_provenance &&
              tiled.valid_provenance == cubic.valid_provenance,
          "tiled authentic L2-to-L1 provenance must remain canonical and exact");
  require(tiled.telemetry.candidate_triples == baseline.telemetry.candidate_triples &&
              tiled.telemetry.raw_valid_triples == baseline.telemetry.raw_valid_triples,
          "tiled authentic L2-to-L1 candidate and survivor counts must match the baseline");
}

void test_paper_rounded_multiword_fixture() {
  const auto instance = balanced_instance(360U, 0x4d554c5449574f52ULL);
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::PaperRounded;
  config.execution_mode = ter::TerExecutionMode::FixtureTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = 0x5041504552544552ULL;
  config.attempt_limit = 1U;
  config.fixture_level3_limit = 9U;
  config.memory_limit_bytes = 128ULL * 1024ULL * 1024ULL;
  const auto result = ter::solve_full_ter(instance, config);
  if (result.status != ter::Status::Found) {
    std::cerr << "multiword fixture status=" << static_cast<unsigned>(result.status)
              << " message=" << result.message << '\n';
  }
  require(result.status == ter::Status::Found,
          "paper-rounded n=360 fixture must traverse the complete guided tree");
  require(result.telemetry.parameters.r1 > 64U &&
              result.telemetry.modulus_bits == 360U &&
              result.telemetry.packed_vector_bytes == 96U,
          "paper-rounded fixture must exercise multiword packed and arithmetic paths");
  require(!result.telemetry.attempts.empty() &&
              !result.telemetry.attempts[0].level2_to_level1.tiled_used,
          "unsupported multiword L2-to-L1 arithmetic must use the pre-output exact fallback");
  require(ter::verify_full_ter_witness(instance, result.witness).status ==
              ter::Status::Completed,
          "multiword fixture witness must pass independent full-modulus validation");
  const auto replay = ter::solve_full_ter(instance, config);
  require(replay.status == result.status && replay.witness == result.witness &&
              replay.telemetry.fingerprint == result.telemetry.fingerprint,
          "multiword paper-rounded fixture must replay exactly");
}

void test_failure_semantics() {
  const auto instance = balanced_instance(36U, 17U);
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FixtureTer;
  config.target_mode = ter::TerTargetMode::Random;
  config.seed = 3U;
  config.attempt_limit = 2U;
  config.fixture_level3_limit = 1U;
  config.memory_limit_bytes = 16U * 1024U * 1024U;
  const auto result = ter::solve_full_ter(instance, config);
  require(result.status == ter::Status::AttemptLimitReached ||
              result.status == ter::Status::Found,
          "random retries may find a witness or exhaust attempts, never claim UNSAT");
  for (std::size_t attempt = 0; attempt < result.telemetry.attempts.size(); ++attempt) {
    require(result.telemetry.attempts[attempt].seed == config.seed + attempt,
            "random attempt telemetry must expose sequential deterministic seeds");
  }
  if (result.status == ter::Status::AttemptLimitReached) {
    require(result.telemetry.attempts.size() == 2U,
            "attempt exhaustion must record every deterministic attempt");
    require(result.telemetry.attempts[0].seed == config.seed &&
                result.telemetry.attempts[1].seed == config.seed + 1U,
            "each deterministic attempt must expose its exact seed");
  } else {
    require(ter::verify_full_ter_witness(instance, result.witness).status ==
                ter::Status::Completed,
            "any random-mode witness must pass independent verification");
  }
  const auto replay = ter::solve_full_ter(instance, config);
  require(replay.status == result.status && replay.witness == result.witness &&
              replay.telemetry.fingerprint == result.telemetry.fingerprint,
          "fixed-seed random attempts must replay exactly");

  config.memory_limit_bytes = 1U;
  const auto limited = ter::solve_full_ter(instance, config);
  require(limited.status == ter::Status::ResourceLimit,
          "resource-limited execution must fail closed");

  std::atomic_bool cancelled{true};
  config.memory_limit_bytes = 16U * 1024U * 1024U;
  config.cancellation = &cancelled;
  const auto stopped = ter::solve_full_ter(instance, config);
  require(stopped.status == ter::Status::Cancelled,
          "pre-cancelled execution must return Cancelled");
  require(stopped.telemetry.terminal_status == ter::Status::Cancelled &&
              stopped.telemetry.total_solver_ns > 0U &&
              stopped.telemetry.fingerprint != 0U,
          "failure telemetry must expose terminal state, elapsed time, and fingerprint");

  config.cancellation = nullptr;
  config.attempt_limit = 0U;
  const auto invalid = ter::solve_full_ter(instance, config);
  require(invalid.status == ter::Status::InvalidConfiguration &&
              invalid.telemetry.terminal_status == ter::Status::InvalidConfiguration &&
              invalid.telemetry.total_solver_ns > 0U,
          "invalid full-solver configurations must fail closed with run telemetry");

  config.attempt_limit = 1U;
  config.execution_mode = static_cast<ter::TerExecutionMode>(255U);
  const auto invalid_mode = ter::solve_full_ter(instance, config);
  require(invalid_mode.status == ter::Status::InvalidConfiguration,
          "invalid public execution-mode enum values must fail closed");
}

}  // namespace

int main() {
  test_parameters();
  test_level3_generation();
  test_guided_pipeline_and_replay();
  test_authentic_l2_capture_tiled_merge_matches_exact_authorities();
  test_paper_rounded_multiword_fixture();
  test_failure_semantics();
  std::cout << "full TER tests passed\n";
  return 0;
}
