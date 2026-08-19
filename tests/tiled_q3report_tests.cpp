// Standalone arithmetic tests for the exact cache-tiled Q3REPORT primitive.
// These are independent of Full TER: they test only the tiling math against
// brute-force references.

#include "ter/tiled_q3report.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

std::uint64_t g_checks = 0;

void require(const bool condition, const std::string_view message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class SplitMix64 {
 public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

[[nodiscard]] std::vector<ter::TiledQEntry> make_list(const std::vector<std::uint32_t>& quotients) {
  std::vector<ter::TiledQEntry> list;
  list.reserve(quotients.size());
  for (std::size_t index = 0; index < quotients.size(); ++index) {
    list.push_back(ter::TiledQEntry{quotients[index], static_cast<ter::EntryId>(index)});
  }
  return list;
}

// Brute-force cubic reference: the authority every tiled result is checked against.
[[nodiscard]] std::uint64_t cubic_count(const std::vector<ter::TiledQEntry>& a,
                                        const std::vector<ter::TiledQEntry>& b,
                                        const std::vector<ter::TiledQEntry>& c,
                                        const std::uint64_t tau,
                                        const std::uint64_t q) {
  std::uint64_t total = 0;
  for (const auto& x : a) {
    for (const auto& y : b) {
      for (const auto& z : c) {
        const std::uint64_t sum =
            (static_cast<std::uint64_t>(x.q) + y.q + z.q) % q;
        total += sum == tau ? 1U : 0U;
      }
    }
  }
  return total;
}

using Triple = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

[[nodiscard]] std::vector<Triple> cubic_witnesses(const std::vector<ter::TiledQEntry>& a,
                                                  const std::vector<ter::TiledQEntry>& b,
                                                  const std::vector<ter::TiledQEntry>& c,
                                                  const std::uint64_t tau,
                                                  const std::uint64_t q) {
  std::vector<Triple> out;
  for (const auto& x : a) {
    for (const auto& y : b) {
      for (const auto& z : c) {
        if ((static_cast<std::uint64_t>(x.q) + y.q + z.q) % q == tau) {
          out.emplace_back(x.id, y.id, z.id);
        }
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

struct Collector {
  std::vector<Triple> triples;
  std::uint64_t limit = 0;  // 0 means unlimited
};

ter::TiledVisitDecision collect(void* context,
                                const ter::EntryId x,
                                const ter::EntryId y,
                                const ter::EntryId z) {
  auto* const collector = static_cast<Collector*>(context);
  if (collector->limit != 0U && collector->triples.size() >= collector->limit) {
    return ter::TiledVisitDecision::ResourceLimit;
  }
  collector->triples.emplace_back(x, y, z);
  return ter::TiledVisitDecision::Continue;
}

struct CancellationCollector {
  std::uint64_t visited = 0;
};

ter::TiledVisitDecision cancel_immediately(void* context,
                                           const ter::EntryId,
                                           const ter::EntryId,
                                           const ter::EntryId) {
  auto* const collector = static_cast<CancellationCollector*>(context);
  ++collector->visited;
  return ter::TiledVisitDecision::Cancelled;
}

[[nodiscard]] ter::TiledQ3ReportConfig config_with(const std::uint32_t tile_bits) {
  ter::TiledQ3ReportConfig config;
  config.preferred_tile_bits = tile_bits;
  return config;
}

// --- Deterministic tile policy ----------------------------------------------

void tile_policy_is_deterministic_and_clamped() {
  require(ter::tiled_q3report_tile_bits(1U << 4U, 15U) == 4U,
          "q_bits below the preference clamps to q_bits");
  require(ter::tiled_q3report_tile_bits(1U << 15U, 15U) == 15U, "q_bits equal to the preference");
  require(ter::tiled_q3report_tile_bits(1U << 20U, 15U) == 15U,
          "q_bits above the preference keeps the preference");
  require(ter::tiled_q3report_tile_bits(1U << 2U, 1U) == 1U, "explicit small tiles are honoured");
  require(ter::tiled_q3report_tile_bits(3U, 15U) == 0U, "a non-power-of-two Q has no tile policy");
}

// --- Boundary and degenerate inputs -----------------------------------------

void boundary_and_degenerate_inputs_are_exact() {
  constexpr std::uint64_t kQ = 64U;
  const auto empty = make_list({});

  {  // empty lists in every position
    const auto some = make_list({0U, 5U, 63U});
    for (const auto* trio : {&empty, &some}) {
      static_cast<void>(trio);
    }
    require(ter::tiled_q3report_count(empty, some, some, 0U, kQ, config_with(15U))
                    .telemetry.raw_triples == 0U,
            "empty first list yields no witnesses");
    require(ter::tiled_q3report_count(some, empty, some, 0U, kQ, config_with(15U))
                    .telemetry.raw_triples == 0U,
            "empty second list yields no witnesses");
    require(ter::tiled_q3report_count(some, some, empty, 0U, kQ, config_with(15U))
                    .telemetry.raw_triples == 0U,
            "empty lookup list yields no witnesses");
  }
  {  // singletons hitting exactly 0, Q-1, Q and 2Q-2 as pair sums
    struct Case {
      std::uint32_t a;
      std::uint32_t b;
      std::uint32_t c;
      std::uint64_t tau;
      bool expected;
    };
    const Case cases[] = {
        {0U, 0U, 0U, 0U, true},
        {0U, 0U, 1U, 0U, false},
        {63U, 0U, 1U, 0U, true},          // pair sum Q-1, wraps with c
        {63U, 1U, 0U, 0U, true},          // pair sum Q exactly
        {63U, 63U, 2U, 0U, true},         // pair sum 2Q-2
        {63U, 63U, 1U, 63U, true},        // pair sum 2Q-2, tau = Q-1
        {32U, 32U, 0U, 0U, true},         // pair sum exactly Q
        {32U, 31U, 1U, 0U, true},         // pair sum Q-1
    };
    for (const auto& item : cases) {
      const auto a = make_list({item.a});
      const auto b = make_list({item.b});
      const auto c = make_list({item.c});
      for (const std::uint32_t bits : {1U, 2U, 4U, 6U, 15U}) {
        const auto counted = ter::tiled_q3report_count(a, b, c, item.tau, kQ, config_with(bits));
        require(counted.status == ter::TiledQ3ReportStatus::Completed, "boundary case completes");
        require(counted.telemetry.raw_triples == (item.expected ? 1U : 0U),
                "boundary pair sums at 0, Q-1, Q and 2Q-2 are exact");
        require(counted.telemetry.raw_triples == cubic_count(a, b, c, item.tau, kQ),
                "boundary case agrees with the cubic reference");
      }
    }
  }
  {  // duplicate and all-identical quotients keep multiplicity
    const auto a = make_list({7U, 7U, 7U});
    const auto b = make_list({9U, 9U});
    const auto c = make_list({kQ - 16U, kQ - 16U, kQ - 16U, kQ - 16U});
    const auto counted = ter::tiled_q3report_count(a, b, c, 0U, kQ, config_with(4U));
    require(counted.telemetry.raw_triples == 3U * 2U * 4U,
            "duplicate quotients remain distinct witnesses");
    require(counted.telemetry.raw_triples == cubic_count(a, b, c, 0U, kQ),
            "multiplicity matches the cubic reference");
  }
  {  // Q = 2 is the smallest legal universe
    const auto a = make_list({0U, 1U});
    const auto b = make_list({0U, 1U});
    const auto c = make_list({0U, 1U});
    for (std::uint64_t tau = 0; tau < 2U; ++tau) {
      const auto counted = ter::tiled_q3report_count(a, b, c, tau, 2U, config_with(15U));
      require(counted.status == ter::TiledQ3ReportStatus::Completed, "Q=2 completes");
      require(counted.telemetry.raw_triples == cubic_count(a, b, c, tau, 2U),
              "Q=2 agrees with the cubic reference");
    }
  }
}

// --- Fail-closed configuration ----------------------------------------------

void unsupported_configurations_fail_closed() {
  const auto list = make_list({0U, 1U});
  require(ter::tiled_q3report_count(list, list, list, 0U, 48U, config_with(15U)).status ==
              ter::TiledQ3ReportStatus::NotApplicable,
          "a non-power-of-two Q is NotApplicable");
  require(ter::tiled_q3report_count(list, list, list, 0U, 0U, config_with(15U)).status ==
              ter::TiledQ3ReportStatus::NotApplicable,
          "Q = 0 is NotApplicable");
  require(ter::tiled_q3report_count(list, list, list, 64U, 64U, config_with(15U)).status ==
              ter::TiledQ3ReportStatus::InvalidConfiguration,
          "tau outside the universe is InvalidConfiguration");
  const auto oversized = make_list({99U});
  require(ter::tiled_q3report_count(oversized, list, list, 0U, 64U, config_with(15U)).status ==
              ter::TiledQ3ReportStatus::InvalidConfiguration,
          "a quotient outside the universe is InvalidConfiguration");
  require(ter::tiled_q3report_count(list, list, list, 0U, 64U, config_with(0U)).status ==
              ter::TiledQ3ReportStatus::InvalidConfiguration,
          "a zero tile preference is InvalidConfiguration");

  ter::TiledQ3ReportConfig tiny;
  tiny.preferred_tile_bits = 15U;
  tiny.memory_limit_bytes = 1U;
  require(ter::tiled_q3report_count(list, list, list, 0U, 64U, tiny).status ==
              ter::TiledQ3ReportStatus::ResourceLimit,
          "a one-byte ceiling stops the count kernel before it scans");
  Collector collector;
  require(ter::tiled_q3report_report(list, list, list, 0U, 64U, tiny, collect, &collector)
                  .status == ter::TiledQ3ReportStatus::ResourceLimit,
          "a one-byte ceiling stops the witness kernel before it scans");
  require(collector.triples.empty(), "a refused kernel must not report witnesses");
  require(ter::tiled_q3report_report(list, list, list, 0U, 64U, config_with(15U), nullptr,
                                     nullptr)
                  .status == ter::TiledQ3ReportStatus::InvalidConfiguration,
          "witness mode requires a visitor");

  std::uint64_t count_bytes = 0;
  std::uint64_t report_bytes = 0;
  require(ter::tiled_q3report_auxiliary_bytes(10U, 10U, 10U, 64U, 15U, true, &count_bytes,
                                              &report_bytes),
          "auxiliary accounting succeeds for a small case");
  require(report_bytes > count_bytes, "witness mode declares strictly more auxiliary memory");
  require(!ter::tiled_q3report_auxiliary_bytes(10U, 10U, 10U, 48U, 15U, true, &count_bytes,
                                               &report_bytes),
          "auxiliary accounting refuses a non-power-of-two Q");
}

// --- Partition proof --------------------------------------------------------

void every_pair_belongs_to_exactly_one_window() {
  for (const std::uint32_t q_bits : {2U, 3U, 5U, 6U}) {
    const std::uint64_t q = std::uint64_t{1} << q_bits;
    for (const std::uint32_t tile_bits : {1U, 2U, 3U, 15U}) {
      const std::uint32_t sb = ter::tiled_q3report_tile_bits(q, tile_bits);
      const std::uint64_t s = std::uint64_t{1} << sb;
      const std::uint64_t p = q >> sb;
      for (std::uint64_t qa = 0; qa < q; ++qa) {
        for (std::uint64_t qb = 0; qb < q; ++qb) {
          const std::uint64_t i = qa >> sb;
          const std::uint64_t j = qb >> sb;
          std::uint64_t owners = 0;
          for (std::uint64_t m = 0; m < 2U * p - 1U; ++m) {
            owners += (i + j == m) ? 1U : 0U;
          }
          require(owners == 1U, "exactly one window group owns each pair");
          const std::uint64_t m = i + j;
          const std::uint64_t w0 = m * s;
          require(qa + qb >= w0 && qa + qb < w0 + 2U * s,
                  "the pair sum always lies inside its own group's window");
        }
      }
    }
  }
}

// --- Differential against the cubic reference -------------------------------

void random_fixtures_agree_with_the_cubic_reference() {
  SplitMix64 random(0x7166'5265'706f'7274ULL);
  for (const std::uint32_t q_bits : {1U, 2U, 4U, 7U, 10U}) {
    const std::uint64_t q = std::uint64_t{1} << q_bits;
    for (std::uint32_t trial = 0; trial < 12U; ++trial) {
      const auto size_a = static_cast<std::size_t>(random.next() % 9U);
      const auto size_b = static_cast<std::size_t>(random.next() % 9U);
      const auto size_c = static_cast<std::size_t>(random.next() % 9U);
      std::vector<std::uint32_t> qa(size_a);
      std::vector<std::uint32_t> qb(size_b);
      std::vector<std::uint32_t> qc(size_c);
      for (auto& value : qa) {
        value = static_cast<std::uint32_t>(random.next() % q);
      }
      for (auto& value : qb) {
        value = static_cast<std::uint32_t>(random.next() % q);
      }
      for (auto& value : qc) {
        value = static_cast<std::uint32_t>(random.next() % q);
      }
      const auto a = make_list(qa);
      const auto b = make_list(qb);
      const auto c = make_list(qc);
      const std::uint64_t tau = random.next() % q;
      const std::uint64_t expected = cubic_count(a, b, c, tau, q);
      const auto expected_witnesses = cubic_witnesses(a, b, c, tau, q);

      // Invariance across every legal tile width.
      for (const std::uint32_t tile_bits : {1U, 2U, 3U, 8U, 15U}) {
        const auto counted = ter::tiled_q3report_count(a, b, c, tau, q, config_with(tile_bits));
        require(counted.status == ter::TiledQ3ReportStatus::Completed, "random count completes");
        require(counted.telemetry.raw_triples == expected,
                "tiled count equals the cubic reference at every tile width");
        require(counted.telemetry.logical_pairs ==
                    static_cast<std::uint64_t>(size_a) * size_b,
                "logical pair universe is reported honestly");
        require(counted.telemetry.executed_pairs <= counted.telemetry.logical_pairs,
                "executed pairs never exceed the logical pair universe");
        require(counted.telemetry.doubled_lookup_records == 2U * size_c,
                "every lookup entry contributes exactly two doubled records");

        Collector collector;
        const auto reported =
            ter::tiled_q3report_report(a, b, c, tau, q, config_with(tile_bits), collect,
                                       &collector);
        require(reported.status == ter::TiledQ3ReportStatus::Completed,
                "random report completes");
        require(reported.telemetry.raw_triples == expected,
                "witness count equals the cubic reference");
        std::sort(collector.triples.begin(), collector.triples.end());
        require(collector.triples == expected_witnesses,
                "the reported witness multiset equals the cubic witness multiset");
        for (const auto& triple : collector.triples) {
          const std::uint64_t sum = (static_cast<std::uint64_t>(qa[std::get<0>(triple)]) +
                                     qb[std::get<1>(triple)] + qc[std::get<2>(triple)]) %
                                    q;
          require(sum == tau, "every emitted witness satisfies the modular equation");
        }
      }
    }
  }
}

// --- Empty-window skipping is exact and honestly reported --------------------

void empty_windows_are_skipped_exactly() {
  constexpr std::uint64_t kQ = 1024U;
  // A and B are deliberately spread across many buckets so that most bucket
  // groups really do carry pairs, while the single lookup key makes almost
  // every window empty.  Those groups must be skipped, not merely counted.
  const auto a = make_list({0U, 100U, 200U, 300U});
  const auto b = make_list({0U, 4U, 8U, 12U});
  const auto c = make_list({0U});
  const auto counted = ter::tiled_q3report_count(a, b, c, 0U, kQ, config_with(2U));
  require(counted.status == ter::TiledQ3ReportStatus::Completed, "sparse case completes");
  require(counted.telemetry.raw_triples == cubic_count(a, b, c, 0U, kQ),
          "skipping empty windows does not lose witnesses");
  require(counted.telemetry.nonempty_windows < counted.telemetry.window_count,
          "a sparse lookup list leaves most windows empty");
  require(counted.telemetry.executed_pairs < counted.telemetry.logical_pairs,
          "skipped groups are reported as unexecuted rather than silently counted");
  // Only the single bucket group whose window holds the lookup key may run.
  require(counted.telemetry.executed_pairs == 1U,
          "exactly the one live bucket group is executed");
  require(counted.telemetry.logical_pairs == 16U, "the logical pair universe is still |A||B|");

  // A completely empty lookup list executes nothing at all.
  const auto none = make_list({});
  const auto skipped = ter::tiled_q3report_count(a, b, none, 0U, kQ, config_with(2U));
  require(skipped.telemetry.executed_pairs == 0U, "an empty lookup list executes no pairs");
  require(skipped.telemetry.nonempty_windows == 0U, "an empty lookup list has no live windows");
}

// --- Visitor control flow ----------------------------------------------------

void visitor_decisions_stop_the_scan() {
  // C covers every residue, so each of the 16 pairs has exactly one witness and
  // the visitor is guaranteed to reach its ceiling.
  constexpr std::uint64_t kQ = 4U;
  const auto a = make_list({0U, 1U, 2U, 3U});
  const auto b = make_list({0U, 1U, 2U, 3U});
  const auto c = make_list({0U, 1U, 2U, 3U});
  require(cubic_count(a, b, c, 0U, kQ) == 16U, "the fixture really has 16 witnesses");
  Collector collector;
  collector.limit = 3U;
  const auto reported =
      ter::tiled_q3report_report(a, b, c, 0U, kQ, config_with(15U), collect, &collector);
  require(reported.status == ter::TiledQ3ReportStatus::ResourceLimit,
          "a visitor resource limit surfaces as ResourceLimit");
  require(collector.triples.size() == 3U, "the scan stops at the visitor's ceiling");
  require(reported.telemetry.raw_triples <= cubic_count(a, b, c, 0U, kQ),
          "a stopped scan never over-reports");

  CancellationCollector cancellation;
  const auto cancelled =
      ter::tiled_q3report_report(a, b, c, 0U, kQ, config_with(15U), cancel_immediately,
                                 &cancellation);
  require(cancelled.status == ter::TiledQ3ReportStatus::Cancelled,
          "a visitor cancellation surfaces as Cancelled");
  require(cancellation.visited == 1U, "cancellation stops after the requesting visitor call");
}

}  // namespace

int main() {
  tile_policy_is_deterministic_and_clamped();
  boundary_and_degenerate_inputs_are_exact();
  unsupported_configurations_fail_closed();
  every_pair_belongs_to_exactly_one_window();
  random_fixtures_agree_with_the_cubic_reference();
  empty_windows_are_skipped_exactly();
  visitor_decisions_stop_the_scan();
  std::cout << "tiled q3report tests passed (" << g_checks << " assertions)\n";
  return 0;
}
