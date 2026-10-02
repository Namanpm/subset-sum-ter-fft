// TER^2 convolution/source-signature spike: authentic dataset exporter.
//
// SCOPE.  Standalone research tooling.  It builds the authentic n=72 TER^2
// residual families through the unmodified MicroReport8 donor
// (bench/ter_microreport8_dataset.*), re-checks every authenticated fixture
// invariant, and writes the families to a flat binary file so the GPU transform
// benchmark runs on the authentic data and the authentic job count rather than a
// same-sized synthetic stand-in.  It changes no production source.
//
// Binary layout (little-endian):
//   char[8]  magic "TERCSIG1"
//   u32      n, r1, quotient_bits, tau_low25
//   u64      seed
//   3 x 64 families in order Q_s (from A), X_t (from B), Y_u (from d):
//     u32 count, then count x (u32 value, u32 entry_id), ascending by value.

#include "ter/limitations.hpp"
#include "ter/tiled_q3report.hpp"
#include "ter_microreport8_dataset.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using ter::microreport8::kSplitRadix;
using ter::microreport8::Posting;

void put32(std::ofstream& out, const std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
                         static_cast<char>((value >> 16U) & 0xFFU),
                         static_cast<char>((value >> 24U) & 0xFFU)};
  out.write(bytes, 4);
}

void put64(std::ofstream& out, const std::uint64_t value) {
  put32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  put32(out, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::uint32_t distinct(const std::vector<Posting>& family) {
  std::uint32_t count = 0U;
  for (std::size_t index = 0U; index < family.size(); ++index) {
    if (index == 0U || family[index].value != family[index - 1U].value) {
      ++count;
    }
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: ter_convsig_dataset_export <families.bin> <summary.json>\n";
    return EXIT_FAILURE;
  }
  const auto data = ter::microreport8::build_dataset(ter::microreport8::kReferenceRootN,
                                                     ter::microreport8::kReferenceRootSeed, true);
  if (!data->ok) {
    std::cerr << "FATAL: " << data->message << '\n';
    return EXIT_FAILURE;
  }

  std::vector<std::string> failures;
  const auto require = [&](const bool condition, const char* const detail) {
    if (!condition) {
      failures.emplace_back(detail);
    }
  };
  require(data->n == 72U, "n != 72");
  require(data->seed == 19797034556212562ULL, "seed mismatch");
  require(data->quotient_bits == 47U, "Q != 47");
  require(data->r1 == 25U, "r1 != 25");
  require(data->a_size == 19109U, "|A| != 19109");
  require(data->b_size == 32496U, "|B| != 32496");
  require(data->c_size == 33505U, "|C| != 33505");
  require(data->pair_universe == 620966064ULL, "P0 != 620966064");
  require(data->tau_low25 == 5005954U, "tau_low25 != 5005954");
  require(data->tau_derivation_verified, "tau derivation not verified");
  ter::TiledQ3ReportConfig tiled;
  tiled.preferred_tile_bits = ter::limits::tiled::kDefaultPreferredTileBits;
  const auto crosscheck = ter::tiled_q3report_count(
      data->a_low25, data->b_low25, data->c_low25, data->tau_low25,
      std::uint64_t{1} << ter::microreport8::kProjectionBits, tiled);
  require(crosscheck.status == ter::TiledQ3ReportStatus::Completed &&
              crosscheck.telemetry.raw_triples == 619579ULL,
          "tiled_q3report_count != 619579");
  require(data->production_anchor_available &&
              data->production_low_prefilter_candidates == 619579ULL,
          "production low-25 anchor != 619579");
  if (!failures.empty()) {
    for (const std::string& failure : failures) {
      std::cerr << "FATAL fixture gate: " << failure << '\n';
    }
    return EXIT_FAILURE;
  }

  std::ofstream out(argv[1], std::ios::binary);
  out.write("TERCSIG1", 8);
  put32(out, data->n);
  put32(out, data->r1);
  put32(out, data->quotient_bits);
  put32(out, data->tau_low25);
  put64(out, data->seed);
  const auto write_families = [&](const std::array<std::vector<Posting>, kSplitRadix>& families) {
    for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
      put32(out, static_cast<std::uint32_t>(families[family].size()));
      for (const Posting& posting : families[family]) {
        put32(out, posting.value);
        put32(out, posting.id);
      }
    }
  };
  write_families(data->q_family);
  write_families(data->x_family);
  write_families(data->y_family);
  out.close();
  if (!out) {
    std::cerr << "FATAL: failed to write " << argv[1] << '\n';
    return EXIT_FAILURE;
  }

  std::ofstream json(argv[2]);
  json << "{\"schema\":\"ter_convsig_dataset_export\",\"n\":" << data->n
       << ",\"seed\":" << data->seed << ",\"r1\":" << data->r1
       << ",\"quotient_bits\":" << data->quotient_bits << ",\"tau_low25\":" << data->tau_low25
       << ",\"size_A\":" << data->a_size << ",\"size_B\":" << data->b_size
       << ",\"size_C\":" << data->c_size << ",\"P0\":" << data->pair_universe
       << ",\"decomposition_crosscheck\":" << crosscheck.telemetry.raw_triples
       << ",\"production_low25\":" << data->production_low_prefilter_candidates;
  const auto family_array = [&](const char* const name,
                                const std::array<std::vector<Posting>, kSplitRadix>& families,
                                const bool unique) {
    json << ",\"" << name << "\":[";
    for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
      json << (family == 0U ? "" : ",")
           << (unique ? distinct(families[family])
                      : static_cast<std::uint32_t>(families[family].size()));
    }
    json << "]";
  };
  family_array("q_postings", data->q_family, false);
  family_array("q_unique", data->q_family, true);
  family_array("x_postings", data->x_family, false);
  family_array("x_unique", data->x_family, true);
  family_array("y_postings", data->y_family, false);
  family_array("y_unique", data->y_family, true);
  json << ",\"fixture_gate\":\"pass\"}\n";
  std::cout << "exported authentic families to " << argv[1] << " and " << argv[2] << '\n';
  return EXIT_SUCCESS;
}
