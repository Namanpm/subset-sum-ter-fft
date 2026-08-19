#include "ter/tiled_q3report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ter {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kNoRecord = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

[[nodiscard]] bool checked_add(const std::uint64_t left,
                               const std::uint64_t right,
                               std::uint64_t* const result) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  *result = left + right;
  return true;
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left,
                                    const std::uint64_t right,
                                    std::uint64_t* const result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

[[nodiscard]] bool accumulate_bytes(std::uint64_t* const total,
                                    const std::uint64_t count,
                                    const std::uint64_t element_size) noexcept {
  std::uint64_t bytes = 0;
  return checked_multiply(count, element_size, &bytes) && checked_add(*total, bytes, total);
}

[[nodiscard]] bool is_power_of_two(const std::uint64_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] std::uint32_t exact_log2(const std::uint64_t value) noexcept {
  std::uint32_t bits = 0;
  while ((std::uint64_t{1} << bits) < value) {
    ++bits;
  }
  return bits;
}

// One doubled lookup record.  `key` is 64-bit because k_z + Q can reach 2Q - 1.
struct DoubledRecord {
  std::uint64_t key = 0;
  EntryId id = 0;
};

struct Buckets {
  std::vector<TiledQEntry> ordered;
  std::vector<std::uint32_t> offsets;  // size P + 1
};

void build_buckets(const std::vector<TiledQEntry>& entries,
                   const std::uint32_t tile_bits,
                   const std::size_t bucket_count,
                   Buckets* const out) {
  out->offsets.assign(bucket_count + 1U, 0U);
  for (const auto& entry : entries) {
    ++out->offsets[static_cast<std::size_t>(entry.q >> tile_bits) + 1U];
  }
  for (std::size_t index = 1; index < out->offsets.size(); ++index) {
    out->offsets[index] += out->offsets[index - 1U];
  }
  std::vector<std::uint32_t> cursor(out->offsets.begin(), out->offsets.end());
  out->ordered.resize(entries.size());
  for (const auto& entry : entries) {
    const auto bucket = static_cast<std::size_t>(entry.q >> tile_bits);
    out->ordered[static_cast<std::size_t>(cursor[bucket]++)] = entry;
  }
}

struct Prepared {
  std::uint32_t tile_bits = 0;
  std::uint64_t tile_width = 0;
  std::uint64_t window_width = 0;
  std::size_t bucket_count = 0;
  Buckets first;
  Buckets second;
  std::vector<DoubledRecord> doubled;
};

// Shared validation and setup for both kernels.
[[nodiscard]] TiledQ3ReportStatus prepare(const std::vector<TiledQEntry>& pair_first,
                                          const std::vector<TiledQEntry>& pair_second,
                                          const std::vector<TiledQEntry>& lookup,
                                          const std::uint64_t tau,
                                          const std::uint64_t q,
                                          const TiledQ3ReportConfig& config,
                                          const bool witness_mode,
                                          Prepared* const prepared,
                                          TiledQ3ReportTelemetry* const telemetry,
                                          std::string* const message) {
  if (!is_power_of_two(q) || q > (std::uint64_t{1} << 32U)) {
    *message = "tiled Q3REPORT requires a power-of-two quotient universe of at most 2^32";
    return TiledQ3ReportStatus::NotApplicable;
  }
  if (tau >= q) {
    *message = "tau must lie inside the quotient universe";
    return TiledQ3ReportStatus::InvalidConfiguration;
  }
  if (config.preferred_tile_bits == 0U) {
    *message = "preferred tile width must be at least one bit";
    return TiledQ3ReportStatus::InvalidConfiguration;
  }
  for (const auto* list : {&pair_first, &pair_second, &lookup}) {
    if (list->size() > std::numeric_limits<std::uint32_t>::max()) {
      *message = "tiled bucket offsets require each input list to fit the 32-bit entry range";
      return TiledQ3ReportStatus::ResourceLimit;
    }
    for (const auto& entry : *list) {
      if (static_cast<std::uint64_t>(entry.q) >= q) {
        *message = "a quotient residue leaves the declared quotient universe";
        return TiledQ3ReportStatus::InvalidConfiguration;
      }
    }
  }
  // The record chain indexes doubled records with uint32_t sentinels.
  std::uint64_t doubled_count = 0;
  if (!checked_multiply(lookup.size(), 2U, &doubled_count) ||
      doubled_count >= static_cast<std::uint64_t>(kNoRecord)) {
    *message = "doubled lookup record count is not addressable by the 32-bit chain";
    return TiledQ3ReportStatus::ResourceLimit;
  }

  prepared->tile_bits = tiled_q3report_tile_bits(q, config.preferred_tile_bits);
  prepared->tile_width = std::uint64_t{1} << prepared->tile_bits;
  prepared->window_width = 2U * prepared->tile_width;
  prepared->bucket_count = static_cast<std::size_t>(q >> prepared->tile_bits);

  std::uint64_t count_bytes = 0;
  std::uint64_t report_bytes = 0;
  if (!tiled_q3report_auxiliary_bytes(pair_first.size(), pair_second.size(), lookup.size(), q,
                                      config.preferred_tile_bits, witness_mode, &count_bytes,
                                      &report_bytes)) {
    *message = "tiled auxiliary memory accounting overflows";
    return TiledQ3ReportStatus::ResourceLimit;
  }
  const std::uint64_t required = witness_mode ? report_bytes : count_bytes;
  if (config.memory_limit_bytes != 0U && required > config.memory_limit_bytes) {
    *message = "fixed tiled auxiliary memory exceeds the declared ceiling";
    return TiledQ3ReportStatus::ResourceLimit;
  }

  telemetry->tile_bits = prepared->tile_bits;
  telemetry->tile_width = prepared->tile_width;
  telemetry->window_width = prepared->window_width;
  telemetry->bucket_count = prepared->bucket_count;
  telemetry->window_count =
      prepared->bucket_count == 0U ? 0U : 2U * prepared->bucket_count - 1U;
  telemetry->doubled_lookup_records = doubled_count;
  telemetry->auxiliary_peak_bytes = required;
  if (!checked_multiply(pair_first.size(), pair_second.size(), &telemetry->logical_pairs)) {
    *message = "logical pair universe overflows";
    return TiledQ3ReportStatus::ResourceLimit;
  }
  build_buckets(pair_first, prepared->tile_bits, prepared->bucket_count, &prepared->first);
  build_buckets(pair_second, prepared->tile_bits, prepared->bucket_count, &prepared->second);

  prepared->doubled.clear();
  prepared->doubled.reserve(static_cast<std::size_t>(doubled_count));
  const std::uint64_t mask = q - 1U;
  for (const auto& entry : lookup) {
    const std::uint64_t key = (tau - static_cast<std::uint64_t>(entry.q)) & mask;
    prepared->doubled.push_back(DoubledRecord{key, entry.id});
    prepared->doubled.push_back(DoubledRecord{key + q, entry.id});
  }
  std::sort(prepared->doubled.begin(), prepared->doubled.end(),
            [](const DoubledRecord& left, const DoubledRecord& right) {
              if (left.key != right.key) {
                return left.key < right.key;
              }
              return left.id < right.id;
            });
  return TiledQ3ReportStatus::Completed;
}

}  // namespace

const char* tiled_q3report_status_name(const TiledQ3ReportStatus status) noexcept {
  switch (status) {
    case TiledQ3ReportStatus::Completed:
      return "Completed";
    case TiledQ3ReportStatus::NotApplicable:
      return "NotApplicable";
    case TiledQ3ReportStatus::InvalidConfiguration:
      return "InvalidConfiguration";
    case TiledQ3ReportStatus::ResourceLimit:
      return "ResourceLimit";
    case TiledQ3ReportStatus::Cancelled:
      return "Cancelled";
    case TiledQ3ReportStatus::ArithmeticSafetyFailure:
      return "ArithmeticSafetyFailure";
    case TiledQ3ReportStatus::InvariantViolation:
      return "InvariantViolation";
  }
  return "UNKNOWN";
}

std::uint32_t tiled_q3report_tile_bits(const std::uint64_t q,
                                        const std::uint32_t preferred_tile_bits) noexcept {
  if (!is_power_of_two(q)) {
    return 0U;
  }
  const std::uint32_t q_bits = exact_log2(q);
  return preferred_tile_bits < q_bits ? preferred_tile_bits : q_bits;
}

bool tiled_q3report_auxiliary_bytes(const std::uint64_t pair_first_size,
                                    const std::uint64_t pair_second_size,
                                    const std::uint64_t lookup_size,
                                    const std::uint64_t q,
                                    const std::uint32_t preferred_tile_bits,
                                    const bool witness_mode,
                                    std::uint64_t* const count_bytes,
                                    std::uint64_t* const report_bytes) noexcept {
  static_cast<void>(witness_mode);
  if (!is_power_of_two(q) || preferred_tile_bits == 0U) {
    return false;
  }
  const std::uint32_t tile_bits = tiled_q3report_tile_bits(q, preferred_tile_bits);
  const std::uint64_t tile_width = std::uint64_t{1} << tile_bits;
  const std::uint64_t window_width = 2U * tile_width;
  const std::uint64_t bucket_count = q >> tile_bits;
  std::uint64_t doubled = 0;
  if (!checked_multiply(lookup_size, 2U, &doubled)) {
    return false;
  }

  std::uint64_t shared = 0;
  if (!accumulate_bytes(&shared, pair_first_size, sizeof(TiledQEntry)) ||
      !accumulate_bytes(&shared, pair_second_size, sizeof(TiledQEntry)) ||
      // build_buckets holds a cursor while both completed bucket-offset arrays
      // are live.  Charge that third offsets array to the fixed peak.
      !accumulate_bytes(&shared, bucket_count + 1U, sizeof(std::uint32_t)) ||
      !accumulate_bytes(&shared, bucket_count + 1U, sizeof(std::uint32_t)) ||
      !accumulate_bytes(&shared, bucket_count + 1U, sizeof(std::uint32_t)) ||
      !accumulate_bytes(&shared, doubled, sizeof(DoubledRecord))) {
    return false;
  }

  std::uint64_t counting = shared;
  if (!accumulate_bytes(&counting, window_width, sizeof(std::uint32_t))) {
    return false;
  }
  std::uint64_t reporting = shared;
  if (!accumulate_bytes(&reporting, window_width, sizeof(std::uint32_t)) ||
      !accumulate_bytes(&reporting, doubled, sizeof(std::uint32_t))) {
    return false;
  }
  *count_bytes = counting;
  *report_bytes = reporting;
  return true;
}

TiledQ3ReportResult tiled_q3report_count(const std::vector<TiledQEntry>& pair_first,
                                         const std::vector<TiledQEntry>& pair_second,
                                         const std::vector<TiledQEntry>& lookup,
                                         const std::uint64_t tau,
                                         const std::uint64_t q,
                                         const TiledQ3ReportConfig& config) {
  TiledQ3ReportResult result;
  try {
    const auto setup_start = Clock::now();
    Prepared prepared;
    const auto status = prepare(pair_first, pair_second, lookup, tau, q, config, false, &prepared,
                                &result.telemetry, &result.message);
    if (status != TiledQ3ReportStatus::Completed) {
      result.status = status;
      return result;
    }
    std::vector<std::uint32_t> counts(static_cast<std::size_t>(prepared.window_width), 0U);
    result.telemetry.setup_ns = elapsed_ns(setup_start);

    const auto scan_start = Clock::now();
    const std::size_t bucket_count = prepared.bucket_count;
    const std::uint64_t tile_width = prepared.tile_width;
    const std::uint64_t window_width = prepared.window_width;
    const auto& doubled = prepared.doubled;
    std::uint32_t* const count_data = counts.data();
    std::size_t lo = 0;
    std::uint64_t raw = 0;
    std::uint64_t executed = 0;
    std::uint64_t nonempty = 0;

    for (std::size_t m = 0; bucket_count != 0U && m < 2U * bucket_count - 1U; ++m) {
      const std::uint64_t w0 = static_cast<std::uint64_t>(m) * tile_width;
      const std::uint64_t w1 = w0 + window_width;
      while (lo < doubled.size() && doubled[lo].key < w0) {
        ++lo;
      }
      std::size_t hi = lo;
      while (hi < doubled.size() && doubled[hi].key < w1) {
        ++count_data[static_cast<std::size_t>(doubled[hi].key - w0)];
        ++hi;
      }
      if (hi != lo) {
        ++nonempty;
        const std::size_t first_index = m >= bucket_count ? m - bucket_count + 1U : 0U;
        const std::size_t last_index = m < bucket_count ? m : bucket_count - 1U;
        for (std::size_t i = first_index; i <= last_index; ++i) {
          const std::size_t j = m - i;
          const std::uint32_t x_begin = prepared.first.offsets[i];
          const std::uint32_t x_end = prepared.first.offsets[i + 1U];
          const std::uint32_t y_begin = prepared.second.offsets[j];
          const std::uint32_t y_end = prepared.second.offsets[j + 1U];
          for (std::uint32_t xi = x_begin; xi < x_end; ++xi) {
            const std::uint64_t x_q = prepared.first.ordered[xi].q;
            for (std::uint32_t yi = y_begin; yi < y_end; ++yi) {
              const std::uint64_t sum = x_q + prepared.second.ordered[yi].q;
              raw += count_data[static_cast<std::size_t>(sum - w0)];
            }
          }
          executed += static_cast<std::uint64_t>(x_end - x_begin) *
                      static_cast<std::uint64_t>(y_end - y_begin);
        }
      }
      for (std::size_t index = lo; index < hi; ++index) {
        count_data[static_cast<std::size_t>(doubled[index].key - w0)] = 0U;
      }
    }
    result.telemetry.scan_ns = elapsed_ns(scan_start);
    result.telemetry.raw_triples = raw;
    result.telemetry.executed_pairs = executed;
    result.telemetry.nonempty_windows = nonempty;
    result.status = TiledQ3ReportStatus::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = TiledQ3ReportStatus::ResourceLimit;
    result.message = "tiled count allocation failed";
    return result;
  } catch (const std::length_error& error) {
    result.status = TiledQ3ReportStatus::ResourceLimit;
    result.message = error.what();
    return result;
  }
}

TiledQ3ReportResult tiled_q3report_report(const std::vector<TiledQEntry>& pair_first,
                                          const std::vector<TiledQEntry>& pair_second,
                                          const std::vector<TiledQEntry>& lookup,
                                          const std::uint64_t tau,
                                          const std::uint64_t q,
                                          const TiledQ3ReportConfig& config,
                                          const TiledTripleVisitor visitor,
                                          void* const context) {
  TiledQ3ReportResult result;
  if (visitor == nullptr) {
    result.status = TiledQ3ReportStatus::InvalidConfiguration;
    result.message = "witness mode requires a visitor";
    return result;
  }
  try {
    const auto setup_start = Clock::now();
    Prepared prepared;
    const auto status = prepare(pair_first, pair_second, lookup, tau, q, config, true, &prepared,
                                &result.telemetry, &result.message);
    if (status != TiledQ3ReportStatus::Completed) {
      result.status = status;
      return result;
    }
    std::vector<std::uint32_t> head(static_cast<std::size_t>(prepared.window_width), kNoRecord);
    std::vector<std::uint32_t> next(prepared.doubled.size(), kNoRecord);
    result.telemetry.setup_ns = elapsed_ns(setup_start);

    const auto scan_start = Clock::now();
    const std::size_t bucket_count = prepared.bucket_count;
    const std::uint64_t tile_width = prepared.tile_width;
    const std::uint64_t window_width = prepared.window_width;
    const auto& doubled = prepared.doubled;
    std::uint32_t* const head_data = head.data();
    std::uint32_t* const next_data = next.data();
    std::size_t lo = 0;
    std::uint64_t raw = 0;
    std::uint64_t executed = 0;
    std::uint64_t nonempty = 0;
    TiledVisitDecision decision = TiledVisitDecision::Continue;

    for (std::size_t m = 0;
         bucket_count != 0U && m < 2U * bucket_count - 1U &&
         decision == TiledVisitDecision::Continue;
         ++m) {
      const std::uint64_t w0 = static_cast<std::uint64_t>(m) * tile_width;
      const std::uint64_t w1 = w0 + window_width;
      while (lo < doubled.size() && doubled[lo].key < w0) {
        ++lo;
      }
      std::size_t hi = lo;
      while (hi < doubled.size() && doubled[hi].key < w1) {
        const auto slot = static_cast<std::size_t>(doubled[hi].key - w0);
        next_data[hi] = head_data[slot];
        head_data[slot] = static_cast<std::uint32_t>(hi);
        ++hi;
      }
      if (hi != lo) {
        ++nonempty;
        const std::size_t first_index = m >= bucket_count ? m - bucket_count + 1U : 0U;
        const std::size_t last_index = m < bucket_count ? m : bucket_count - 1U;
        for (std::size_t i = first_index;
             i <= last_index && decision == TiledVisitDecision::Continue; ++i) {
          const std::size_t j = m - i;
          const std::uint32_t x_begin = prepared.first.offsets[i];
          const std::uint32_t x_end = prepared.first.offsets[i + 1U];
          const std::uint32_t y_begin = prepared.second.offsets[j];
          const std::uint32_t y_end = prepared.second.offsets[j + 1U];
          for (std::uint32_t xi = x_begin;
               xi < x_end && decision == TiledVisitDecision::Continue; ++xi) {
            const auto& x_entry = prepared.first.ordered[xi];
            const std::uint64_t x_q = x_entry.q;
            for (std::uint32_t yi = y_begin;
                 yi < y_end && decision == TiledVisitDecision::Continue; ++yi) {
              const auto& y_entry = prepared.second.ordered[yi];
              const std::uint64_t sum = x_q + y_entry.q;
              std::uint32_t record = head_data[static_cast<std::size_t>(sum - w0)];
              while (record != kNoRecord) {
                ++raw;
                decision = visitor(context, x_entry.id, y_entry.id, doubled[record].id);
                if (decision != TiledVisitDecision::Continue) {
                  break;
                }
                record = next_data[record];
              }
            }
          }
          executed += static_cast<std::uint64_t>(x_end - x_begin) *
                      static_cast<std::uint64_t>(y_end - y_begin);
        }
      }
      for (std::size_t index = lo; index < hi; ++index) {
        head_data[static_cast<std::size_t>(doubled[index].key - w0)] = kNoRecord;
      }
    }
    result.telemetry.scan_ns = elapsed_ns(scan_start);
    result.telemetry.raw_triples = raw;
    result.telemetry.executed_pairs = executed;
    result.telemetry.nonempty_windows = nonempty;
    switch (decision) {
      case TiledVisitDecision::Continue:
        result.status = TiledQ3ReportStatus::Completed;
        break;
      case TiledVisitDecision::ResourceLimit:
        result.status = TiledQ3ReportStatus::ResourceLimit;
        result.message = "witness consumer reached its declared resource ceiling";
        break;
      case TiledVisitDecision::Cancelled:
        result.status = TiledQ3ReportStatus::Cancelled;
        result.message = "witness scan was cancelled";
        break;
      case TiledVisitDecision::InvariantViolation:
        result.status = TiledQ3ReportStatus::InvariantViolation;
        result.message = "witness consumer reported an invariant violation";
        break;
    }
    return result;
  } catch (const std::bad_alloc&) {
    result.status = TiledQ3ReportStatus::ResourceLimit;
    result.message = "tiled witness allocation failed";
    return result;
  } catch (const std::length_error& error) {
    result.status = TiledQ3ReportStatus::ResourceLimit;
    result.message = error.what();
    return result;
  }
}

}  // namespace ter
