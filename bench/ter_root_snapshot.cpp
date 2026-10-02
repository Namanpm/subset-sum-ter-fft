#include "ter_root_snapshot.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <type_traits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace ter::root_snapshot {
namespace {

static_assert(std::is_trivially_copyable_v<Header>);
constexpr char kMagic[8] = {'T', 'E', 'R', 'R', 'S', 'N', 'P', '1'};

[[nodiscard]] std::uint64_t header_checksum(Header header) noexcept {
  header.header_checksum = 0U;
  return fnv1a(reinterpret_cast<const std::uint8_t*>(&header), sizeof(Header));
}

[[nodiscard]] std::FILE* open_file(const std::string& path, const char* const mode) noexcept {
#if defined(_MSC_VER)
  std::FILE* file = nullptr;
  return fopen_s(&file, path.c_str(), mode) == 0 ? file : nullptr;
#else
  return std::fopen(path.c_str(), mode);
#endif
}

[[nodiscard]] std::uint64_t process_peak_working_set() noexcept {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
    return counters.PeakWorkingSetSize;
  }
#endif
  return 0U;
}

// Sink state for FullTerRootStream.  Owns the partial file and the running
// per-list checksum; nothing else is buffered here.
struct Writer {
  std::FILE* file = nullptr;
  Header header{};
  std::int32_t list = -1;
  std::uint64_t written = 0;
  std::uint64_t offset = sizeof(Header);
  std::string error;
};

bool begin_list(void* const context, const std::uint32_t group, const std::uint64_t entries,
                const std::vector<std::uint32_t>& target) {
  auto& writer = *static_cast<Writer*>(context);
  if (group > 2U || static_cast<std::int32_t>(group) != writer.list + 1 ||
      (writer.list >= 0 && writer.written != writer.header.entries[writer.list]) ||
      target.size() > writer.header.words32) {
    writer.error = "stream list order or target width violated";
    return false;
  }
  writer.list = static_cast<std::int32_t>(group);
  writer.written = 0U;
  writer.header.entries[group] = entries;
  writer.header.offset[group] = writer.offset;
  writer.header.checksum[group] = 0xcbf29ce484222325ULL;
  std::fill(std::begin(writer.header.child_target[group]),
            std::end(writer.header.child_target[group]), 0U);
  std::copy(target.begin(), target.end(), writer.header.child_target[group]);
  return true;
}

bool write_records(void* const context, const std::uint8_t* const records,
                   const std::size_t count) {
  auto& writer = *static_cast<Writer*>(context);
  const std::size_t bytes = count * writer.header.record_bytes;
  if (writer.list < 0 || writer.written + count > writer.header.entries[writer.list]) {
    writer.error = "stream wrote more records than it announced";
    return false;
  }
  if (std::fwrite(records, 1U, bytes, writer.file) != bytes) {
    writer.error = "snapshot write failed (disk full or I/O error)";
    return false;
  }
  writer.header.checksum[writer.list] =
      fnv1a(records, bytes, writer.header.checksum[writer.list]);
  writer.written += count;
  writer.offset += bytes;
  return true;
}

}  // namespace

std::uint64_t fnv1a(const std::uint8_t* const bytes, const std::size_t size,
                    std::uint64_t state) noexcept {
  for (std::size_t index = 0U; index < size; ++index) {
    state ^= bytes[index];
    state *= 0x100000001b3ULL;
  }
  return state;
}

std::span<const std::uint32_t> Snapshot::residue(const std::size_t list,
                                                 const std::uint64_t index) const noexcept {
  const std::uint8_t* const at = record(list, index) + header.words64 * 16U;
  return {reinterpret_cast<const std::uint32_t*>(at), header.words32};
}

int Snapshot::digit(const std::size_t list, const std::uint64_t index,
                    const std::uint32_t coordinate) const noexcept {
  const auto* const words = reinterpret_cast<const std::uint64_t*>(record(list, index));
  const std::uint64_t bit = std::uint64_t{1} << (coordinate % 64U);
  const std::uint32_t word = coordinate / 64U;
  if ((words[word] & bit) != 0U) {
    return 1;
  }
  return (words[header.words64 + word] & bit) != 0U ? -1 : 0;
}

CaptureOutcome capture_to_file(const Instance& instance, const FullTerConfig& config,
                               const std::uint32_t attempt, const std::string& path,
                               const std::uint64_t buffer_bytes, const std::string& git_commit) {
  CaptureOutcome outcome;
  const std::string partial = path + ".partial";
  std::error_code ignored;
  std::filesystem::remove(partial, ignored);

  Writer writer;
  std::memcpy(writer.header.magic, kMagic, sizeof(kMagic));
  writer.header.layout_version = kLayoutVersion;
  writer.header.header_bytes = sizeof(Header);
  writer.header.n = instance.n;
  writer.header.words64 = (instance.n + 63U) / 64U;
  writer.header.words32 = (instance.n + 31U) / 32U;
  writer.header.record_bytes = writer.header.words64 * 16U + writer.header.words32 * 4U;
  writer.header.attempt = attempt;
  writer.header.seed = config.seed;
  writer.header.memory_limit_bytes = config.memory_limit_bytes;
  std::memcpy(writer.header.git_commit, git_commit.data(),
              std::min(git_commit.size(), sizeof(writer.header.git_commit) - 1U));
  if (writer.header.words32 > kMaxWords32) {
    outcome.io_error = "n exceeds the snapshot layout's 256-bit limit";
    return outcome;
  }
  writer.file = open_file(partial, "wb");
  if (writer.file == nullptr) {
    outcome.io_error = "cannot create " + partial;
    return outcome;
  }
  // Placeholder header; the final one is written once every count is known.
  bool ok = std::fwrite(&writer.header, 1U, sizeof(Header), writer.file) == sizeof(Header);

  FullTerRootStream stream;
  stream.context = &writer;
  stream.begin_list = &begin_list;
  stream.write_records = &write_records;
  outcome.stream = stream_full_ter_root_lists(instance, config, attempt, stream, buffer_bytes);

  ok = ok && outcome.stream.status == Status::Completed && writer.list == 2 &&
       writer.written == writer.header.entries[2];
  if (ok) {
    const FullTerRootStreamResult& result = outcome.stream;
    writer.header.r1 = result.parameters.r1;
    writer.header.r2 = result.parameters.r2;
    writer.header.attempt_seed = result.attempt_seed;
    std::copy(result.root_target_words_le.begin(), result.root_target_words_le.end(),
              writer.header.root_target);
    std::copy(result.level2_sizes.begin(), result.level2_sizes.end(), writer.header.level2_sizes);
    writer.header.declared_peak_bytes = result.peak_working_bytes;
    writer.header.capture_elapsed_ns = result.elapsed_ns;
    writer.header.process_peak_working_set = process_peak_working_set();
    writer.header.header_checksum = header_checksum(writer.header);
    ok = std::fseek(writer.file, 0L, SEEK_SET) == 0 &&
         std::fwrite(&writer.header, 1U, sizeof(Header), writer.file) == sizeof(Header) &&
         std::fflush(writer.file) == 0;
    if (!ok) {
      writer.error = "snapshot header write failed";
    }
  }
  ok = std::fclose(writer.file) == 0 && ok;
  if (!ok) {
    outcome.io_error = writer.error.empty() ? "capture did not complete" : writer.error;
    std::filesystem::remove(partial, ignored);
    return outcome;
  }
  std::filesystem::remove(path, ignored);
  std::filesystem::rename(partial, path, ignored);
  if (ignored) {
    outcome.io_error = "cannot rename " + partial + " to " + path;
    return outcome;
  }
  outcome.written = true;
  outcome.file_bytes = writer.offset;
  return outcome;
}

bool load(const std::string& path, Snapshot& out, std::string& error) {
  std::FILE* const file = open_file(path, "rb");
  if (file == nullptr) {
    error = "cannot open " + path;
    return false;
  }
  const auto fail = [&](const std::string& reason) {
    std::fclose(file);
    error = reason;
    return false;
  };
  Header& header = out.header;
  if (std::fread(&header, 1U, sizeof(Header), file) != sizeof(Header)) {
    return fail("truncated snapshot header");
  }
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
      header.layout_version != kLayoutVersion || header.header_bytes != sizeof(Header)) {
    return fail("not a layout-1 root snapshot");
  }
  if (header_checksum(header) != header.header_checksum) {
    return fail("snapshot header checksum mismatch");
  }
  if (header.n == 0U || header.words64 != (header.n + 63U) / 64U ||
      header.words32 != (header.n + 31U) / 32U || header.words32 > kMaxWords32 ||
      header.record_bytes != header.words64 * 16U + header.words32 * 4U) {
    return fail("snapshot dimensions are inconsistent");
  }
  std::uint64_t expected_offset = sizeof(Header);
  for (std::size_t list = 0U; list < 3U; ++list) {
    if (header.offset[list] != expected_offset) {
      return fail("snapshot list offsets are inconsistent");
    }
    const std::uint64_t bytes = header.entries[list] * header.record_bytes;
    out.records[list].resize(static_cast<std::size_t>(bytes));
    if (std::fread(out.records[list].data(), 1U, static_cast<std::size_t>(bytes), file) != bytes) {
      return fail("truncated snapshot list " + std::to_string(list));
    }
    if (fnv1a(out.records[list].data(), static_cast<std::size_t>(bytes)) != header.checksum[list]) {
      return fail("snapshot list " + std::to_string(list) + " checksum mismatch");
    }
    expected_offset += bytes;
  }
  std::uint8_t probe = 0U;
  if (std::fread(&probe, 1U, 1U, file) != 0U) {
    return fail("trailing bytes after the last snapshot list");
  }
  std::fclose(file);
  // Structural validation: disjoint masks, no bits or residue above n.
  const std::uint32_t top64 = header.n % 64U;
  const std::uint64_t top64_mask = top64 == 0U ? ~std::uint64_t{0} : (std::uint64_t{1} << top64) - 1U;
  const std::uint32_t top32 = header.n % 32U;
  const std::uint32_t top32_mask = top32 == 0U ? ~std::uint32_t{0} : (std::uint32_t{1} << top32) - 1U;
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::uint64_t index = 0U; index < header.entries[list]; ++index) {
      const auto* const words = reinterpret_cast<const std::uint64_t*>(out.record(list, index));
      for (std::uint32_t word = 0U; word < header.words64; ++word) {
        const std::uint64_t positive = words[word];
        const std::uint64_t negative = words[header.words64 + word];
        const std::uint64_t allowed = word + 1U == header.words64 ? top64_mask : ~std::uint64_t{0};
        if ((positive & negative) != 0U || ((positive | negative) & ~allowed) != 0U) {
          error = "snapshot entry has overlapping or out-of-width ternary masks";
          return false;
        }
      }
      if ((out.residue(list, index).back() & ~top32_mask) != 0U) {
        error = "snapshot entry residue exceeds n bits";
        return false;
      }
    }
  }
  return true;
}

}  // namespace ter::root_snapshot
