#include "ter/full_ter.hpp"
#include "ter/limitations.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef TER_CONFIGURE_TIME_COMMIT
#define TER_CONFIGURE_TIME_COMMIT "unknown"
#endif
#ifndef TER_CONFIGURE_TIME_DIRTY
#define TER_CONFIGURE_TIME_DIRTY 1
#endif

namespace {

// ---------------------------------------------------------------------------
// Executable identity (P29).
//
// A benchmark row that cannot be tied to the binary that produced it is not
// reproducible evidence.  The SHA-256 below hashes this process's own image so
// a stale executable is detectable even when the recorded commit is not.
// ---------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

class Sha256 {
 public:
  void update(const unsigned char* data, std::size_t length) noexcept {
    total_bytes_ += length;
    while (length != 0U) {
      const std::size_t take = std::min(length, sizeof(buffer_) - buffered_);
      std::copy(data, data + take, buffer_ + buffered_);
      buffered_ += take;
      data += take;
      length -= take;
      if (buffered_ == sizeof(buffer_)) {
        compress(buffer_);
        buffered_ = 0U;
      }
    }
  }

  [[nodiscard]] std::string hex() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8U;
    unsigned char padding = 0x80U;
    update(&padding, 1U);
    padding = 0x00U;
    while (buffered_ != 56U) {
      update(&padding, 1U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      const unsigned char byte = static_cast<unsigned char>((bit_length >> shift) & 0xFFU);
      // Length bytes must bypass update()'s own length counter.
      buffer_[buffered_++] = byte;
    }
    compress(buffer_);
    std::string out;
    out.reserve(64U);
    for (const std::uint32_t word : state_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back("0123456789abcdef"[(word >> shift) & 0xFU]);
      }
    }
    return out;
  }

 private:
  static std::uint32_t rotate_right(const std::uint32_t value,
                                    const std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
  }

  void compress(const unsigned char* const block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16U; ++index) {
      schedule[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24) |
                        (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16) |
                        (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8) |
                        static_cast<std::uint32_t>(block[index * 4U + 3U]);
    }
    for (std::size_t index = 16U; index < 64U; ++index) {
      const std::uint32_t s0 = rotate_right(schedule[index - 15U], 7U) ^
                               rotate_right(schedule[index - 15U], 18U) ^
                               (schedule[index - 15U] >> 3U);
      const std::uint32_t s1 = rotate_right(schedule[index - 2U], 17U) ^
                               rotate_right(schedule[index - 2U], 19U) ^
                               (schedule[index - 2U] >> 10U);
      schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
    }
    std::array<std::uint32_t, 8> working = state_;
    for (std::size_t index = 0; index < 64U; ++index) {
      const std::uint32_t S1 = rotate_right(working[4], 6U) ^ rotate_right(working[4], 11U) ^
                               rotate_right(working[4], 25U);
      const std::uint32_t choose =
          (working[4] & working[5]) ^ (~working[4] & working[6]);
      const std::uint32_t temp1 =
          working[7] + S1 + choose + kSha256RoundConstants[index] + schedule[index];
      const std::uint32_t S0 = rotate_right(working[0], 2U) ^ rotate_right(working[0], 13U) ^
                               rotate_right(working[0], 22U);
      const std::uint32_t majority =
          (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      const std::uint32_t temp2 = S0 + majority;
      working[7] = working[6];
      working[6] = working[5];
      working[5] = working[4];
      working[4] = working[3] + temp1;
      working[3] = working[2];
      working[2] = working[1];
      working[1] = working[0];
      working[0] = temp1 + temp2;
    }
    for (std::size_t index = 0; index < 8U; ++index) {
      state_[index] += working[index];
    }
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  unsigned char buffer_[64]{};
  std::size_t buffered_ = 0U;
  std::uint64_t total_bytes_ = 0U;
};

[[nodiscard]] bool sha256_self_test() {
  Sha256 empty;
  if (empty.hex() != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    return false;
  }
  Sha256 abc;
  const std::string_view text{"abc"};
  abc.update(reinterpret_cast<const unsigned char*>(text.data()), text.size());
  return abc.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

[[nodiscard]] std::string executable_sha256(const char* const path) {
  if (path == nullptr) {
    return "unavailable";
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "unavailable";
  }
  Sha256 digest;
  std::vector<char> chunk(1U << 16);
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize read = file.gcount();
    if (read > 0) {
      digest.update(reinterpret_cast<const unsigned char*>(chunk.data()),
                    static_cast<std::size_t>(read));
    }
  }
  return file.bad() ? std::string{"unavailable"} : digest.hex();
}

// ---------------------------------------------------------------------------
// Status naming (P1/N0).
//
// The previous row printed a two-way ternary, so any status other than Found
// was rendered as "AttemptLimitReached".  With a timeout now reachable, that
// would have printed a cancelled run as an exhausted search.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view status_name(const ter::Status status) noexcept {
  switch (status) {
    case ter::Status::Completed: return "Completed";
    case ter::Status::Found: return "Found";
    case ter::Status::NoWitnessThisAttempt: return "NoWitnessThisAttempt";
    case ter::Status::AttemptLimitReached: return "AttemptLimitReached";
    case ter::Status::InvalidConfiguration: return "InvalidConfiguration";
    case ter::Status::InvalidShape: return "InvalidShape";
    case ter::Status::GenerationIncomplete: return "GenerationIncomplete";
    case ter::Status::ResourceLimit: return "ResourceLimit";
    case ter::Status::Cancelled: return "Cancelled";
    case ter::Status::InvariantViolation: return "InvariantViolation";
    case ter::Status::NotApplicable: return "NotApplicable";
    case ter::Status::UnsupportedWidth: return "UnsupportedWidth";
  }
  return "UnknownStatus";
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

ter::Instance benchmark_instance(std::uint32_t n, std::uint64_t seed) {
  auto built = ter::build_instance(n,
                                   ter::limits::benchmark::kFullTerFixtureModulusBits,
                                   seed);
  if (built.status != ter::Status::Completed) {
    return {};
  }
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(),
            instance.planted_root.end(),
            std::uint8_t{0});
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

std::uint64_t median(std::vector<std::uint64_t> values) {
  if (values.empty()) {
    return 0U;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

// ---------------------------------------------------------------------------
// Terminal-attempt selection (P2).
//
// The successful attempt is pushed last and the solver returns immediately, so
// a Found result's telemetry is attempts.back().  Selecting by status rather
// than by position keeps that explicit, and an empty attempt vector (an early
// cancellation or resource refusal) must never be indexed.
// ---------------------------------------------------------------------------

[[nodiscard]] const ter::FullTerAttemptTelemetry* terminal_attempt(
    const ter::FullTerTelemetry& telemetry) noexcept {
  if (telemetry.attempts.empty()) {
    return nullptr;
  }
  for (const auto& attempt : telemetry.attempts) {
    if (attempt.status == ter::Status::Found) {
      return &attempt;
    }
  }
  return &telemetry.attempts.back();
}

[[nodiscard]] std::size_t terminal_attempt_index(
    const ter::FullTerTelemetry& telemetry) noexcept {
  const auto* const attempt = terminal_attempt(telemetry);
  if (attempt == nullptr) {
    return 0U;
  }
  return static_cast<std::size_t>(attempt - telemetry.attempts.data());
}

enum class BenchMode : std::uint8_t { AttemptCost, TimeToFound, SuccessCurve };

[[nodiscard]] std::string_view mode_name(const BenchMode mode) noexcept {
  switch (mode) {
    case BenchMode::AttemptCost: return "attempt-cost";
    case BenchMode::TimeToFound: return "time-to-found";
    case BenchMode::SuccessCurve: return "success-curve";
  }
  return "unknown";
}

struct Options {
  std::uint32_t n = ter::limits::benchmark::kFullTerFixtureN;
  ter::ShapeFamily family = ter::ShapeFamily::FiniteSanity;
  BenchMode mode = BenchMode::AttemptCost;
  bool audit = false;
  bool guided = false;
  bool fixture = false;
  bool preflight = false;
  bool require_found = false;
  std::uint64_t fixture_level3_limit = ter::limits::full_ter::kDefaultFixtureLevel3Limit;
  std::uint32_t attempt_limit = ter::limits::full_ter::kDefaultAttemptLimit;
  std::uint64_t seed = ter::limits::benchmark::kFullTerFixtureSeed;
  std::uint32_t warmup = 1U;
  std::uint32_t repeat = ter::limits::benchmark::kFullTerTimedRepetitions;
  std::uint64_t time_limit_ms = 0U;   // 0 = no wall-clock guard
  std::uint64_t memory_limit_bytes = ter::limits::benchmark::kFullTerMemoryBytes;
  std::uint32_t seeds = 32U;          // success-curve trial count
};

[[nodiscard]] std::string_view family_name(const ter::ShapeFamily family) noexcept {
  return family == ter::ShapeFamily::PaperRounded ? "PaperRounded" : "FiniteSanity";
}

[[nodiscard]] bool parse_unsigned(const std::string_view text,
                                  std::uint64_t* const out) noexcept {
  if (out == nullptr || text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0U;
  for (const char character : text) {
    if (character < '0' || character > '9' ||
        parsed > (std::numeric_limits<std::uint64_t>::max() -
                  static_cast<std::uint64_t>(character - '0')) / 10U) {
      return false;
    }
    parsed = parsed * 10U + static_cast<std::uint64_t>(character - '0');
  }
  *out = parsed;
  return true;
}

void print_usage() {
  std::cerr <<
      "usage: ter_full_bench [options]\n"
      "  --n=<finite TER dimension>        default 36\n"
      "  --family=finite-sanity|paper-rounded\n"
      "  --mode=attempt-cost|time-to-found|success-curve\n"
      "  --attempt-limit=<count>           default 1 (one attempt only)\n"
      "  --require-found                   exit nonzero unless status is Found\n"
      "  --warmup=<count>                  default 1\n"
      "  --repeat=<count>                  default 5 timed replays; >=1\n"
      "  --time-limit-ms=<ms>              cooperative wall-clock cancel; 0 = none\n"
      "  --memory-limit-mib=<mib>          default 4096\n"
      "  --memory-limit-bytes=<bytes>      exact cap, for fail-closed testing\n"
      "  --seed=<uint64>                   base seed\n"
      "  --seeds=<count>                   success-curve trials, default 32\n"
      "  --preflight                       print derived geometry and exit\n"
      "  --fixture --fixture-limit=<n> --guided --audit\n";
}

[[nodiscard]] bool parse_options(const int argc, char** const argv, Options* const options) {
  bool repeat_set = false;
  for (int argument = 1; argument < argc; ++argument) {
    const std::string_view value(argv[argument]);
    std::uint64_t parsed = 0U;
    if (value == "--audit") { options->audit = true; continue; }
    if (value == "--guided") { options->guided = true; continue; }
    if (value == "--fixture") { options->fixture = true; continue; }
    if (value == "--preflight") { options->preflight = true; continue; }
    if (value == "--require-found") { options->require_found = true; continue; }
    if (value.starts_with("--n=")) {
      if (!parse_unsigned(value.substr(4), &parsed) ||
          parsed == 0U || parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "invalid --n value\n";
        return false;
      }
      options->n = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (value.starts_with("--family=")) {
      const auto text = value.substr(9);
      if (text == "finite-sanity") {
        options->family = ter::ShapeFamily::FiniteSanity;
      } else if (text == "paper-rounded") {
        options->family = ter::ShapeFamily::PaperRounded;
      } else {
        std::cerr << "invalid --family value\n";
        return false;
      }
      continue;
    }
    if (value.starts_with("--mode=")) {
      const auto text = value.substr(7);
      if (text == "attempt-cost") {
        options->mode = BenchMode::AttemptCost;
      } else if (text == "time-to-found") {
        options->mode = BenchMode::TimeToFound;
      } else if (text == "success-curve") {
        options->mode = BenchMode::SuccessCurve;
      } else {
        std::cerr << "invalid --mode value\n";
        return false;
      }
      continue;
    }
    if (value.starts_with("--fixture-limit=")) {
      if (!parse_unsigned(value.substr(16), &options->fixture_level3_limit) ||
          options->fixture_level3_limit == 0U) {
        std::cerr << "invalid --fixture-limit value\n";
        return false;
      }
      continue;
    }
    if (value.starts_with("--attempt-limit=")) {
      if (!parse_unsigned(value.substr(16), &parsed) || parsed == 0U ||
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "invalid --attempt-limit value\n";
        return false;
      }
      options->attempt_limit = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (value.starts_with("--warmup=")) {
      if (!parse_unsigned(value.substr(9), &parsed) ||
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "invalid --warmup value\n";
        return false;
      }
      options->warmup = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (value.starts_with("--repeat=")) {
      if (!parse_unsigned(value.substr(9), &parsed) || parsed == 0U ||
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "invalid --repeat value\n";
        return false;
      }
      options->repeat = static_cast<std::uint32_t>(parsed);
      repeat_set = true;
      continue;
    }
    if (value.starts_with("--time-limit-ms=")) {
      if (!parse_unsigned(value.substr(16), &options->time_limit_ms)) {
        std::cerr << "invalid --time-limit-ms value\n";
        return false;
      }
      continue;
    }
    if (value.starts_with("--memory-limit-bytes=")) {
      // Byte granularity exists so fail-closed resource refusal is testable
      // below one MiB; --memory-limit-mib remains the ergonomic form.
      if (!parse_unsigned(value.substr(21), &parsed) || parsed == 0U) {
        std::cerr << "invalid --memory-limit-bytes value\n";
        return false;
      }
      options->memory_limit_bytes = parsed;
      continue;
    }
    if (value.starts_with("--memory-limit-mib=")) {
      if (!parse_unsigned(value.substr(19), &parsed) || parsed == 0U ||
          parsed > std::numeric_limits<std::uint64_t>::max() / ter::limits::kMiB) {
        std::cerr << "invalid --memory-limit-mib value\n";
        return false;
      }
      options->memory_limit_bytes = parsed * ter::limits::kMiB;
      continue;
    }
    if (value.starts_with("--seed=")) {
      if (!parse_unsigned(value.substr(7), &options->seed)) {
        std::cerr << "invalid --seed value\n";
        return false;
      }
      continue;
    }
    if (value.starts_with("--seeds=")) {
      if (!parse_unsigned(value.substr(8), &parsed) || parsed == 0U ||
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "invalid --seeds value\n";
        return false;
      }
      options->seeds = static_cast<std::uint32_t>(parsed);
      continue;
    }
    print_usage();
    return false;
  }
  // A success curve measures outcomes across independent seeds; replaying one
  // deterministic configuration many times would only restate a single result.
  if (options->mode == BenchMode::SuccessCurve && repeat_set) {
    std::cerr << "--repeat does not apply to --mode=success-curve; use --seeds\n";
    return false;
  }
  return true;
}

ter::FullTerConfig make_config(const Options& options, const std::uint64_t seed,
                               const std::atomic_bool* const cancellation) {
  ter::FullTerConfig config;
  config.family = options.family;
  config.execution_mode = options.fixture ? ter::TerExecutionMode::FixtureTer
                                          : ter::TerExecutionMode::FullTer;
  config.target_mode = options.guided ? ter::TerTargetMode::GuidedPlanted
                                      : ter::TerTargetMode::Random;
  config.seed = seed;
  config.attempt_limit = options.attempt_limit;
  config.memory_limit_bytes = options.memory_limit_bytes;
  config.fixture_level3_limit = options.fixture_level3_limit;
  config.audit = options.audit;
  config.cancellation = cancellation;
  return config;
}

// Cooperative wall-clock guard (P5). The solver polls config.cancellation, so a
// timeout surfaces as Status::Cancelled and never as an exhausted search.
class TimeGuard {
 public:
  TimeGuard(const std::uint64_t limit_ms, std::atomic_bool* const flag)
      : flag_(flag) {
    if (limit_ms == 0U || flag == nullptr) {
      return;
    }
    armed_ = true;
    worker_ = std::thread([this, limit_ms]() {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
      while (!stop_.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          flag_->store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }

  TimeGuard(const TimeGuard&) = delete;
  TimeGuard& operator=(const TimeGuard&) = delete;

  ~TimeGuard() {
    stop_.store(true, std::memory_order_relaxed);
    if (armed_ && worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  std::atomic_bool* flag_ = nullptr;
  std::atomic_bool stop_{false};
  std::thread worker_;
  bool armed_ = false;
};

void print_metadata(const Options& options, const std::string& exe_hash,
                    const std::string& command_line,
                    const std::uint64_t attempts_executed) {
#if defined(_MSC_VER)
  std::cout << "# compiler=MSVC-" << _MSC_VER;
#else
  std::cout << "# compiler=non-MSVC";
#endif
#if defined(NDEBUG)
  std::cout << " build=Release";
#else
  std::cout << " build=Debug";
#endif
#if defined(TER_NATIVE_OPT_ENABLED)
  std::cout << " native_opt=on";
#else
  std::cout << " native_opt=off";
#endif
  // configure_time_commit is stamped when CMake last configured this build
  // tree, so it can lag HEAD; exe_sha256 always identifies this exact binary.
  std::cout << " threads=1"
            << " configure_time_commit=" << TER_CONFIGURE_TIME_COMMIT
            << " configure_time_dirty=" << (TER_CONFIGURE_TIME_DIRTY ? "yes" : "no")
            << " build_stamp=" << __DATE__ << "T" << __TIME__
            << " exe_sha256=" << exe_hash
            << " mode=" << mode_name(options.mode)
            << " attempt_limit=" << options.attempt_limit
            << " attempts_executed=" << attempts_executed
            << " warmup=" << options.warmup
            << " repeat=" << options.repeat
            << " time_limit_ms=" << options.time_limit_ms
            << " memory_limit_bytes=" << options.memory_limit_bytes
            << " seed=" << options.seed
            << " execution_mode=" << (options.fixture ? "FixtureTer" : "FullTer")
            << " target=" << (options.guided ? "GuidedPlanted" : "Random")
            << " audit=" << (options.audit ? "on" : "off")
            << " family=" << family_name(options.family)
            << " command=\"" << command_line << "\"\n";
}

// ---------------------------------------------------------------------------
// Preflight (P7). Derives geometry and backend selection without running the
// merge, so a pair-materializing fallback is visible before the run starts.
// ---------------------------------------------------------------------------

int run_preflight(const Options& options) {
  // Must mirror solve_full_ter_impl() exactly: the solver derives FullTER
  // shape with modulus_bits == instance.n (src/full_ter.cpp:2805), not with
  // the instance's generator modulus width.  Deriving with anything else
  // reports a shape the solver would never use.
  const auto parameters =
      ter::derive_full_ter_parameters(options.family, options.n, options.n);
  std::cout << "preflight n=" << options.n
            << " family=" << family_name(options.family) << '\n';
  std::cout << "  shape_status=" << status_name(parameters.status) << '\n';
  if (parameters.status != ter::Status::Completed) {
    std::cout << "  message=" << parameters.message << '\n';
    std::cout << "  VERDICT: dimension is not a constructible exact TER shape\n";
    return EXIT_FAILURE;
  }
  const auto& value = parameters.value;
  const std::uint32_t root_q = value.r1 < options.n ? options.n - value.r1 : 0U;
  const std::uint32_t middle_q = value.r2 < value.r1 ? value.r1 - value.r2 : 0U;
  // The bounded streaming root now admits any well-formed shape (P12/P14).
  // native_root_quotient_supported() still answers a narrower question: whether
  // the quotient congruence alone decides, or whether the arbitrary-width
  // residue words settle the survivors of a low-64 prefilter.
  const bool root_native = ter::limits::full_ter::bounded_root_supported(options.n, value.r1);
  const bool root_quotient_decides =
      ter::limits::full_ter::native_root_quotient_supported(options.n, value.r1);
  const bool middle_native = ter::limits::tiled::bounded_full_ter_l2_l1_supported(
      options.n, value.r1, value.r2);
  const bool middle_projection_decides =
      ter::limits::tiled::native_full_ter_l2_l1_supported(value.r1, value.r2);
  std::cout << "  r1=" << value.r1 << " r2=" << value.r2
            << " root_Q=" << root_q << " l2_to_l1_Q=" << middle_q << '\n';
  std::cout << "  representation1=" << value.representation1.decimal
            << " floor_log2=" << value.representation1.floor_log2 << '\n';
  std::cout << "  representation2=" << value.representation2.decimal
            << " floor_log2=" << value.representation2.floor_log2 << '\n';
  std::cout << "  l2_to_l1_backend="
            << (middle_native ? "bounded tiled (streaming, no pair index)"
                              : "generic ternary_mitm")
            << '\n';
  std::cout << "  l2_to_l1_decision="
            << (middle_projection_decides
                    ? "quotient projection is decisive (r1-r2 <= 32)"
                    : "low-32 quotient prefilter + arbitrary-width residue "
                      "authority (r1-r2 > 32)")
            << '\n';
  std::cout << "  root_backend=" << (root_native ? "bounded tiled (streaming, no pair index)"
                                                 : "generic ternary_mitm")
            << '\n';
  std::cout << "  root_decision=" << (root_quotient_decides
                                          ? "quotient congruence is decisive (Q <= 64)"
                                          : "low-64 quotient prefilter + arbitrary-width "
                                            "residue authority (Q > 64)")
            << '\n';
  std::cout << "  memory_limit_bytes=" << options.memory_limit_bytes << '\n';
  std::cout << "  execution_mode=" << (options.fixture ? "FixtureTer" : "FullTer") << '\n';
  const bool pair_materializing = !root_native || !middle_native;
  if (pair_materializing) {
    std::cout << "  *** WARNING: a pair-materializing fallback would be selected ***\n";
    if (!root_native) {
      std::cout << "  *** root Q=" << root_q
                << " is outside the bounded tiled root; L1->L0 would build an\n"
                   "  *** A x B PairRecord array (see repair item P12).\n";
    }
    if (!middle_native) {
      std::cout << "  *** L2->L1 quotient width " << middle_q
                << " exceeds the bounded tiled gate; that stage would build an\n"
                   "  *** A x B PairRecord array (see repair item P13).\n";
    }
  } else {
    std::cout << "  pair_materializing_fallback=no\n";
  }
  std::cout << "  VERDICT: "
            << (pair_materializing ? "UNBOUNDED pair materialization expected"
                                   : "bounded exact path on both merge stages")
            << '\n';
  return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Success curve (P3). Independent seeds, not repeated deterministic replays.
// ---------------------------------------------------------------------------

int run_success_curve(const Options& options) {
  std::vector<std::uint64_t> attempts_to_found;
  std::vector<std::uint64_t> time_to_found_ns;
  std::vector<std::uint64_t> found_seeds;
  std::uint64_t found = 0U;
  std::uint64_t exhausted = 0U;
  std::uint64_t censored = 0U;
  for (std::uint32_t trial = 0; trial < options.seeds; ++trial) {
    // Golden-ratio stride keeps trial seeds well separated and reproducible.
    const std::uint64_t seed = options.seed + trial * 0x9E3779B97F4A7C15ULL;
    const auto instance = benchmark_instance(options.n, seed);
    if (instance.n == 0U) {
      std::cerr << "failed to build benchmark instance for seed " << seed << '\n';
      return EXIT_FAILURE;
    }
    std::atomic_bool cancellation{false};
    const TimeGuard guard(options.time_limit_ms, &cancellation);
    const auto config = make_config(options, seed, &cancellation);
    const auto result = ter::solve_full_ter(instance, config);
    if (result.status == ter::Status::Found) {
      ++found;
      attempts_to_found.push_back(
          static_cast<std::uint64_t>(terminal_attempt_index(result.telemetry)) + 1U);
      time_to_found_ns.push_back(result.telemetry.total_solver_ns);
      found_seeds.push_back(seed);
    } else if (result.status == ter::Status::AttemptLimitReached) {
      ++exhausted;
    } else {
      ++censored;
      std::cerr << "trial seed=" << seed << " status=" << status_name(result.status)
                << " message=" << result.message << '\n';
    }
  }
  const auto percentile95 = [](std::vector<std::uint64_t> values) -> std::uint64_t {
    if (values.empty()) {
      return 0U;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min<std::size_t>(
        values.size() - 1U,
        static_cast<std::size_t>(0.95 * static_cast<double>(values.size())));
    return values[index];
  };
  const double fraction =
      options.seeds == 0U ? 0.0
                          : static_cast<double>(found) / static_cast<double>(options.seeds);
  std::cout << "independent_runs,found,attempt_limit_reached,censored,success_fraction,"
               "median_attempts_to_found,p95_attempts_to_found,"
               "median_time_to_found_ns,p95_time_to_found_ns\n";
  std::cout << options.seeds << ',' << found << ',' << exhausted << ',' << censored << ','
            << fraction << ',' << median(attempts_to_found) << ','
            << percentile95(attempts_to_found) << ',' << median(time_to_found_ns) << ','
            << percentile95(time_to_found_ns) << '\n';
  // Per-run detail so the distribution is inspectable, not just summarized.
  std::cout << "run_index,run_seed,attempts_to_found,time_to_found_ns\n";
  for (std::size_t index = 0; index < attempts_to_found.size(); ++index) {
    std::cout << index << ',' << found_seeds[index] << ',' << attempts_to_found[index]
              << ',' << time_to_found_ns[index] << '\n';
  }
  std::cout << "attempt_budget,found_by_budget,cumulative_success_fraction\n";
  for (std::uint64_t budget = 1U; budget <= options.attempt_limit; budget *= 2U) {
    std::uint64_t reached = 0U;
    for (const std::uint64_t attempts : attempts_to_found) {
      if (attempts <= budget) {
        ++reached;
      }
    }
    const double cumulative =
        options.seeds == 0U
            ? 0.0
            : static_cast<double>(reached) / static_cast<double>(options.seeds);
    std::cout << budget << ',' << reached << ',' << cumulative << '\n';
  }
  if (options.require_found && found == 0U) {
    std::cerr << "no trial reached Found and --require-found was requested\n";
    return EXIT_FAILURE;
  }
  return censored == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(const int argc, char** const argv) {
  if (!sha256_self_test()) {
    std::cerr << "internal SHA-256 self-test failed\n";
    return EXIT_FAILURE;
  }
  Options options;
  if (!parse_options(argc, argv, &options)) {
    return EXIT_FAILURE;
  }
  std::string command_line;
  for (int argument = 0; argument < argc; ++argument) {
    if (argument != 0) {
      command_line.push_back(' ');
    }
    command_line.append(argv[argument]);
  }
  if (options.preflight) {
    return run_preflight(options);
  }
  const std::string exe_hash = executable_sha256(argc > 0 ? argv[0] : nullptr);
  if (options.mode == BenchMode::SuccessCurve) {
    print_metadata(options, exe_hash, command_line, 0U);
    return run_success_curve(options);
  }

  const auto instance = benchmark_instance(options.n, options.seed);
  if (instance.n == 0U) {
    std::cerr << "failed to build benchmark instance\n";
    return EXIT_FAILURE;
  }

  std::atomic_bool cancellation{false};
  // Exactly `warmup` discarded solves, then exactly `repeat` measured solves
  // (repair item P4).  An earlier revision of this file ran a separate
  // reporting solve whenever warmup was zero, so --warmup=0 --repeat=1 cost two
  // full solves instead of one -- the same hidden-cost problem P4 is about.
  {
    const TimeGuard guard(options.time_limit_ms, &cancellation);
    const auto config = make_config(options, options.seed, &cancellation);
    for (std::uint32_t index = 0; index < options.warmup; ++index) {
      const auto discarded = ter::solve_full_ter(instance, config);
      if (!ter::limits::benchmark::full_ter_warmup_acceptable(discarded.status)) {
        std::cerr << "full TER warmup did not reach a comparable terminal status: "
                  << status_name(discarded.status) << ": " << discarded.message << '\n';
        return EXIT_FAILURE;
      }
    }
  }

  // The first measured solve is the reported reference; every later one must
  // reproduce it exactly.  No extra solve is performed for reporting.
  ter::FullTerResult warmup{};
  std::vector<std::uint64_t> totals, l3, binary, middle, root, verification;
  for (std::uint32_t repetition = 0; repetition < options.repeat; ++repetition) {
    std::atomic_bool repeat_cancellation{false};
    const TimeGuard guard(options.time_limit_ms, &repeat_cancellation);
    const auto config = make_config(options, options.seed, &repeat_cancellation);
    auto measured = ter::solve_full_ter(instance, config);
    if (!ter::limits::benchmark::full_ter_warmup_acceptable(measured.status)) {
      std::cerr << "full TER run did not reach a comparable terminal status: "
                << status_name(measured.status) << ": " << measured.message << '\n';
      return EXIT_FAILURE;
    }
    const auto* const attempt = terminal_attempt(measured.telemetry);
    if (attempt == nullptr) {
      std::cerr << "measured run produced no attempt telemetry; status="
                << status_name(measured.status) << '\n';
      return EXIT_FAILURE;
    }
    totals.push_back(measured.telemetry.total_solver_ns);
    l3.push_back(measured.telemetry.level3_generation_ns);
    binary.push_back(attempt->level3_to_level2.elapsed_ns);
    middle.push_back(attempt->level2_to_level1.elapsed_ns);
    root.push_back(attempt->level1_to_level0.elapsed_ns);
    verification.push_back(attempt->verification_ns);
    if (repetition == 0U) {
      warmup = std::move(measured);
    } else if (measured.status != warmup.status ||
               measured.witness != warmup.witness ||
               measured.telemetry.fingerprint != warmup.telemetry.fingerprint) {
      std::cerr << "full TER benchmark changed its exact result\n";
      return EXIT_FAILURE;
    }
  }

  const auto* const attempt_ptr = terminal_attempt(warmup.telemetry);
  if (attempt_ptr == nullptr) {
    std::cerr << "run produced no attempt telemetry; status="
              << status_name(warmup.status) << '\n';
    return EXIT_FAILURE;
  }
  // Attempt-budget invariants, checked here rather than asserted by a CTest
  // regex so the regression tests semantics instead of one random outcome.
  const std::size_t executed = warmup.telemetry.attempts.size();
  if (executed < 1U || executed > options.attempt_limit) {
    std::cerr << "attempt-budget invariant violated: executed=" << executed
              << " attempt_limit=" << options.attempt_limit << '\n';
    return EXIT_FAILURE;
  }
  if (warmup.status == ter::Status::AttemptLimitReached &&
      executed != options.attempt_limit) {
    std::cerr << "AttemptLimitReached must consume the whole budget: executed="
              << executed << " attempt_limit=" << options.attempt_limit << '\n';
    return EXIT_FAILURE;
  }
  if (warmup.status == ter::Status::Found &&
      attempt_ptr->status != ter::Status::Found) {
    std::cerr << "Found result did not select a Found attempt\n";
    return EXIT_FAILURE;
  }
  const auto& attempt = *attempt_ptr;
  const std::size_t terminal_index = terminal_attempt_index(warmup.telemetry);

  print_metadata(options, exe_hash, command_line,
                 static_cast<std::uint64_t>(warmup.telemetry.attempts.size()));
  std::cout << "status,n,r1,r2,L3_left,L3_right,L2_sizes,L1_sizes,L0,"
               "l3_generation_ns,l3_to_l2_ns,l2_to_l1_ns,l1_to_l0_ns,"
               "verification_ns,total_solver_ns,"
               "binary_pairs,binary_modular,binary_structural_rejects,"
               "binary_distribution_rejects,binary_survivors,binary_deduplicated,"
               "middle_pairs,middle_modular,middle_structural_rejects,"
               "middle_distribution_rejects,middle_survivors,middle_deduplicated,"
               "root_pairs,root_modular,root_structural_rejects,"
               "root_distribution_rejects,root_survivors,root_deduplicated,"
               "l3_list_bytes,arithmetic_bytes,l2_list_bytes,l1_list_bytes,l0_list_bytes,"
               "l2_active_bytes,l1_active_bytes,root_active_bytes,"
               "peak_working_bytes,hot_entry_bytes,packed_vector_bytes,fingerprint,"
               "l2_to_l1_tiled_used,l2_to_l1_pair_records,l2_to_l1_tile_bits,"
               "l2_to_l1_tiled_pair_probes,l2_to_l1_tiled_nonempty_windows,"
               "l2_to_l1_tiled_auxiliary_bytes,l2_to_l1_tiled_setup_ns,"
               "l2_to_l1_tiled_scan_ns,"
               "l1_to_l0_tiled_used,l1_to_l0_pair_records,l1_to_l0_quotient_bits,"
               "l1_to_l0_prefilter_bits,l1_to_l0_low_prefilter_candidates,"
               "l1_to_l0_full_quotient_rejects,l1_to_l0_tile_bits,"
               "l1_to_l0_tiled_pair_probes,l1_to_l0_tiled_nonempty_windows,"
               "l1_to_l0_tiled_auxiliary_bytes,l1_to_l0_tiled_setup_ns,"
               "l1_to_l0_tiled_scan_ns,"
               "l1_to_l0_root_policy,l1_to_l0_valid_roots,"
               "l1_to_l0_early_stopped,l1_to_l0_output_peak_bytes,"
               "l1_to_l0_output_live_bytes,l1_to_l0_working_peak_bytes,"
               "l1_to_l0_first_witness_ceiling_bytes,"
               // Appended terminal-attempt and aggregate columns (P2).
               "attempts_used,terminal_attempt_index,terminal_attempt_seed,"
               "terminal_attempt_status,sum_attempt_ns,mean_attempt_ns,"
               "median_attempt_ns,total_root_pair_candidates\n";
  std::cout << status_name(warmup.status)
            << ',' << instance.n << ','
            << warmup.telemetry.parameters.r1 << ','
            << warmup.telemetry.parameters.r2 << ','
            << warmup.telemetry.physical_level3_sizes[0] << ','
            << warmup.telemetry.physical_level3_sizes[1] << ",\"";
  for (std::size_t index = 0; index < attempt.level2_sizes.size(); ++index) {
    std::cout << (index == 0U ? "" : ":") << attempt.level2_sizes[index];
  }
  std::cout << "\",\"";
  for (std::size_t index = 0; index < attempt.level1_sizes.size(); ++index) {
    std::cout << (index == 0U ? "" : ":") << attempt.level1_sizes[index];
  }
  std::cout << "\"," << attempt.level0_size << ',' << median(l3) << ','
            << median(binary) << ',' << median(middle) << ',' << median(root) << ','
            << median(verification) << ',' << median(totals) << ','
            << attempt.level3_to_level2.pair_candidates << ','
            << attempt.level3_to_level2.modular_candidates << ','
            << attempt.level3_to_level2.structural_rejects << ','
            << attempt.level3_to_level2.distribution_rejects << ','
            << attempt.level3_to_level2.survivors_before_dedup << ','
            << attempt.level3_to_level2.survivors_after_dedup << ','
            << attempt.level2_to_level1.pair_candidates << ','
            << attempt.level2_to_level1.modular_candidates << ','
            << attempt.level2_to_level1.structural_rejects << ','
            << attempt.level2_to_level1.distribution_rejects << ','
            << attempt.level2_to_level1.survivors_before_dedup << ','
            << attempt.level2_to_level1.survivors_after_dedup << ','
            << attempt.level1_to_level0.pair_candidates << ','
            << attempt.level1_to_level0.modular_candidates << ','
            << attempt.level1_to_level0.structural_rejects << ','
            << attempt.level1_to_level0.distribution_rejects << ','
            << attempt.level1_to_level0.survivors_before_dedup << ','
            << attempt.level1_to_level0.survivors_after_dedup << ','
            << warmup.telemetry.level3_allocated_bytes << ','
            << warmup.telemetry.arithmetic_allocated_bytes << ','
            << attempt.level2_allocated_bytes << ','
            << attempt.level1_allocated_bytes << ','
            << attempt.level0_allocated_bytes << ','
            << attempt.level3_to_level2.allocated_bytes << ','
            << attempt.level2_to_level1.allocated_bytes << ','
            << attempt.level1_to_level0.allocated_bytes << ','
            << warmup.telemetry.peak_working_bytes << ','
            << warmup.telemetry.hot_entry_bytes << ','
            << warmup.telemetry.packed_vector_bytes << ','
            << warmup.telemetry.fingerprint << ','
            << (attempt.level2_to_level1.tiled_used ? 1 : 0) << ','
            << attempt.level2_to_level1.pair_records << ','
            << attempt.level2_to_level1.tile_bits << ','
            << attempt.level2_to_level1.tiled_pair_probes << ','
            << attempt.level2_to_level1.tiled_nonempty_windows << ','
            << attempt.level2_to_level1.tiled_auxiliary_bytes << ','
            << attempt.level2_to_level1.tiled_setup_ns << ','
            << attempt.level2_to_level1.tiled_scan_ns << ','
            << (attempt.level1_to_level0.tiled_used ? 1 : 0) << ','
            << attempt.level1_to_level0.pair_records << ','
            << attempt.level1_to_level0.quotient_bits << ','
            << attempt.level1_to_level0.prefilter_bits << ','
            << attempt.level1_to_level0.low_prefilter_candidates << ','
            << attempt.level1_to_level0.full_quotient_rejects << ','
            << attempt.level1_to_level0.tile_bits << ','
            << attempt.level1_to_level0.tiled_pair_probes << ','
            << attempt.level1_to_level0.tiled_nonempty_windows << ','
            << attempt.level1_to_level0.tiled_auxiliary_bytes << ','
            << attempt.level1_to_level0.tiled_setup_ns << ','
            << attempt.level1_to_level0.tiled_scan_ns << ','
            << static_cast<unsigned int>(attempt.level1_to_level0.effective_root_output_policy)
            << ',' << attempt.level1_to_level0.valid_root_count << ','
            << (attempt.level1_to_level0.early_stopped ? 1 : 0) << ','
            << attempt.level1_to_level0.output_peak_bytes << ','
            << attempt.level1_to_level0.output_live_bytes << ','
            << attempt.level1_to_level0.working_peak_bytes << ','
            << attempt.level1_to_level0.first_witness_output_ceiling_bytes;

  std::uint64_t sum_attempt_ns = 0U;
  std::uint64_t total_root_pairs = 0U;
  std::vector<std::uint64_t> attempt_times;
  attempt_times.reserve(warmup.telemetry.attempts.size());
  for (const auto& each : warmup.telemetry.attempts) {
    sum_attempt_ns += each.total_ns;
    total_root_pairs += each.level1_to_level0.pair_candidates;
    attempt_times.push_back(each.total_ns);
  }
  const std::uint64_t attempts_used =
      static_cast<std::uint64_t>(warmup.telemetry.attempts.size());
  std::cout << ',' << attempts_used
            << ',' << terminal_index
            << ',' << attempt.seed
            << ',' << status_name(attempt.status)
            << ',' << sum_attempt_ns
            << ',' << (attempts_used == 0U ? 0U : sum_attempt_ns / attempts_used)
            << ',' << median(attempt_times)
            << ',' << total_root_pairs << '\n';

  if (options.require_found && warmup.status != ter::Status::Found) {
    std::cerr << "--require-found: terminal status was "
              << status_name(warmup.status) << ", not Found\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
