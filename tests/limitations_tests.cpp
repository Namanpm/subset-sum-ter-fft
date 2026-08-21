#include "ter/limitations.hpp"
#include "ter/full_ter.hpp"
#include "ter/instance.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void test_full_ter_shape_policy() {
  require(!ter::limits::full_ter::finite_dimension_supported(0U),
          "zero is not a finite Full TER dimension");
  require(ter::limits::full_ter::finite_dimension_supported(36U),
          "n=36 is a valid finite Full TER dimension");
  require(ter::limits::full_ter::finite_dimension_supported(72U),
          "n=72 is a valid finite Full TER dimension");
  require(!ter::limits::full_ter::finite_dimension_supported(70U),
          "n=70 is not a valid finite Full TER dimension");

  require(!ter::limits::full_ter::modulus_width_supported(36U, 0U),
          "zero modulus width is invalid");
  require(ter::limits::full_ter::modulus_width_supported(36U, 1U),
          "one-bit modulus width is valid");
  require(ter::limits::full_ter::modulus_width_supported(36U, 36U),
          "n-bit modulus width is valid");
  require(!ter::limits::full_ter::modulus_width_supported(36U, 37U),
          "modulus width above n is invalid");
  require(ter::limits::full_ter::native_root_quotient_supported(72U, 25U) &&
              !ter::limits::full_ter::native_root_quotient_supported(130U, 65U),
          "M2 root quotient gate accepts 47 bits and rejects widths beyond uint64");
  require(ter::limits::full_ter::root_prefilter_configuration_supported(24U, 15U, 47U) &&
              ter::limits::full_ter::root_prefilter_configuration_supported(64U, 15U, 47U) &&
              !ter::limits::full_ter::root_prefilter_configuration_supported(0U, 15U, 47U) &&
              !ter::limits::full_ter::root_prefilter_configuration_supported(24U, 25U, 47U),
          "M2 root policy clamps oversized widths and rejects invalid tile settings");
}

void test_control_panel_wiring() {
  namespace panel = ter::limits::control_panel;
  require(ter::limits::kMiB == panel::kBytesPerMiB &&
              ter::limits::full_ter::kDefaultMemoryBytes == panel::full_ter::kDefaultMemoryBytes &&
              ter::limits::full_ter::kDefaultRootPrefilterBits ==
                  panel::full_ter::kDefaultRootPrefilterBits &&
              ter::limits::full_ter::kDefaultRootPreferredTileBits ==
                  panel::full_ter::kDefaultRootPreferredTileBits &&
              ter::limits::generator::kDefaultAttemptLimit ==
                  panel::generator::kDefaultAttemptLimit &&
              ter::limits::paper_generator::kDefaultN == panel::paper_generator::kDefaultN &&
              ter::limits::oracle::kCubicCandidateTripleCap ==
                  panel::oracle::kCubicCandidateTripleCap &&
              ter::limits::tiled::kDefaultPreferredTileBits ==
                  panel::tiled::kDefaultPreferredTileBits &&
              ter::limits::benchmark::kFullTerFixtureN ==
                  panel::benchmark::kFullTerFixtureN &&
              ter::limits::benchmark::kCrossoverListSizeLadder ==
                  panel::benchmark::kCrossoverListSizeLadder &&
              ter::limits::benchmark::kPaperListSizeLadder ==
                  panel::benchmark::kPaperListSizeLadder,
          "the control panel drives every core runtime-policy alias");
  require(ter::limits::ntt::kQ3ReportMaximumPower == panel::ntt::kQ3ReportMaximumPower &&
              ter::limits::q3report::kDefaultMemoryBytes == panel::q3report::kDefaultMemoryBytes &&
              ter::limits::crossover::kDefaultMemoryBytes == panel::crossover::kDefaultMemoryBytes &&
              ter::limits::paper_crossover::kDefaultMemoryBytes ==
                  panel::paper_crossover::kDefaultMemoryBytes &&
              ter::limits::benchmark::kFullTerMemoryBytes ==
                  panel::benchmark::kFullTerMemoryBytes,
          "the control panel drives NTT, Q3, crossover, and benchmark policies");
}

void test_checked_arithmetic_policy() {
  std::uint64_t value = 0U;
  require(ter::limits::checked::checked_add(41U, 1U, &value) && value == 42U,
          "checked addition preserves ordinary sums");
  require(!ter::limits::checked::checked_add(std::numeric_limits<std::uint64_t>::max(), 1U,
                                              &value),
          "checked addition rejects overflow");
  require(ter::limits::checked::checked_multiply(6U, 7U, &value) && value == 42U,
          "checked multiplication preserves ordinary products");
  require(!ter::limits::checked::checked_multiply(
              std::numeric_limits<std::uint64_t>::max(), 2U, &value),
          "checked multiplication rejects overflow");
  require(ter::limits::checked::fits_memory_budget(40U, 60U, 100U) &&
              !ter::limits::checked::fits_memory_budget(40U, 61U, 100U),
          "checked memory fitting preserves the declared ceiling");
  require(ter::limits::checked::remaining_memory(40U, 100U, &value) && value == 60U &&
              !ter::limits::checked::remaining_memory(101U, 100U, &value),
          "remaining-memory accounting is fail-closed");
}

void test_oracle_and_tiled_policy() {
  require(ter::limits::oracle::cubic_work_supported(1'000'000U),
          "the cubic oracle accepts its declared maximum work");
  require(!ter::limits::oracle::cubic_work_supported(1'000'001U),
          "the cubic oracle rejects work beyond its declared maximum");

  require(ter::limits::tiled::native_full_ter_l2_l1_supported(63U, 31U),
          "native Full TER tiling accepts its r1 and quotient-width boundary");
  require(!ter::limits::tiled::native_full_ter_l2_l1_supported(64U, 31U),
          "native Full TER tiling rejects r1 outside its machine-width bound");
  require(!ter::limits::tiled::native_full_ter_l2_l1_supported(40U, 7U),
          "native Full TER tiling rejects a quotient wider than 32 bits");

  require(ter::limits::arithmetic::quotient_arithmetic_supported(36U, 3U, 35U),
          "generic quotient arithmetic accepts its 32-bit width boundary");
  require(!ter::limits::arithmetic::quotient_arithmetic_supported(36U, 2U, 35U),
          "generic quotient arithmetic rejects a width above 32 bits");
  require(!ter::limits::arithmetic::quotient_arithmetic_supported(36U, 3U, 36U),
          "generic quotient arithmetic requires b1 below n");

  require(ter::limits::q3report::signed_quotient_arithmetic_supported(62U, 30U),
          "signed Q3 arithmetic accepts its r1=62 boundary");
  require(!ter::limits::q3report::signed_quotient_arithmetic_supported(63U, 31U),
          "signed Q3 arithmetic rejects r1=63");
  require(ter::limits::q3report::native_capture_quotient_supported(62U, 30U) &&
              !ter::limits::q3report::native_capture_quotient_supported(62U, 29U),
          "native capture keeps its distinct 32-bit quotient boundary");
}

void test_ntt_and_benchmark_policy() {
  require(ter::limits::ntt::q3report_length_supported(std::uint64_t{1} << 23U),
          "the count-only NTT supports length 2^23");
  require(!ter::limits::ntt::q3report_length_supported(std::uint64_t{1} << 24U),
          "the count-only NTT rejects length 2^24");
  require(ter::limits::benchmark::full_ter_warmup_acceptable(ter::Status::Found),
          "a found full TER warmup is acceptable");
  require(ter::limits::benchmark::full_ter_warmup_acceptable(
              ter::Status::AttemptLimitReached),
          "an attempt-limited full TER warmup is acceptable");
  require(!ter::limits::benchmark::full_ter_warmup_acceptable(ter::Status::ResourceLimit) &&
              !ter::limits::benchmark::full_ter_warmup_acceptable(ter::Status::Cancelled) &&
              !ter::limits::benchmark::full_ter_warmup_acceptable(
                  ter::Status::InvariantViolation),
          "failed full TER warmups remain failures");
  require(ter::limits::benchmark::paper_crossover_schedule_for(4'096U, false).executable &&
              !ter::limits::benchmark::paper_crossover_schedule_for(32'768U, false)
                   .executable,
          "the paper benchmark schedule keeps its declared executable stop policy");
}

void test_paper_generator_defaults() {
  require(ter::limits::paper_generator::kDefaultN == 360U &&
              ter::limits::paper_generator::kDefaultMemoryBytes ==
                  512ULL * 1024ULL * 1024ULL &&
              ter::limits::paper_generator::kDefaultHalfPoolSize == 4'096U &&
              ter::limits::paper_generator::kDefaultAttemptLimit == 5'000'000U,
          "paper benchmark generator defaults remain explicit and unchanged");
}

void test_paper_crossover_admission_policy() {
  require(ter::limits::paper_crossover::snapshot_configuration_supported(
              2U, 2U, 2U, 1U),
          "paper snapshot admission accepts its inclusive minimum boundary");
  require(!ter::limits::paper_crossover::snapshot_configuration_supported(
              1U, 2U, 2U, 1U) &&
              !ter::limits::paper_crossover::snapshot_configuration_supported(
                  2U, 1U, 2U, 1U) &&
              !ter::limits::paper_crossover::snapshot_configuration_supported(
                  2U, 2U, 1U, 1U) &&
              !ter::limits::paper_crossover::snapshot_configuration_supported(
                  2U, 2U, 2U, 0U),
          "paper snapshot admission preserves every current refusal condition");
  require(ter::limits::paper_crossover::run_configuration_supported(1U, 1U) &&
              !ter::limits::paper_crossover::run_configuration_supported(0U, 1U) &&
              !ter::limits::paper_crossover::run_configuration_supported(1U, 0U),
          "paper comparator run admission preserves zero-budget refusals");
}

void test_n72_full_benchmark_ceiling_regression() {
  const auto instance = ter::build_instance(72U, 10U, 0x7200ULL);
  require(instance.status == ter::Status::Completed,
          "the n=72 constrained regression instance must build");

  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::Random;
  config.seed = 0x7201ULL;
  config.attempt_limit = ter::limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = ter::limits::benchmark::kFullTerMemoryBytes;
  config.audit = false;

  const auto result = ter::solve_full_ter(instance.value, config);
  if (result.status != ter::Status::AttemptLimitReached && result.status != ter::Status::Found) {
    std::cerr << "n72 M2 regression status=" << static_cast<unsigned>(result.status)
              << " message=" << result.message << '\n';
  }
  require(result.status == ter::Status::AttemptLimitReached || result.status == ter::Status::Found,
          "n=72 at the full benchmark ceiling must pass the old root-pair barrier");
  require(!result.telemetry.attempts.empty() &&
              result.telemetry.attempts[0].level1_to_level0.tiled_used &&
              result.telemetry.attempts[0].level1_to_level0.pair_records == 0U &&
              result.telemetry.attempts[0].level1_to_level0.quotient_bits == 47U,
          "n=72 must use M2's bounded 47-bit tiled root path");
}

}  // namespace

int main() {
  test_control_panel_wiring();
  test_full_ter_shape_policy();
  test_checked_arithmetic_policy();
  test_oracle_and_tiled_policy();
  test_ntt_and_benchmark_policy();
  test_paper_generator_defaults();
  test_paper_crossover_admission_policy();
  test_n72_full_benchmark_ceiling_regression();
  std::cout << "limitations policy tests passed\n";
  return 0;
}
