#pragma once

// Host state for the single-threaded root benchmarks: AC power, process
// memory, the hybrid CPU's efficiency classes, and P-core pinning.  Windows
// only; elsewhere every query reports "unavailable" (-1 or 0) and pinning fails,
// so a pinned label can never be attached to an unpinned run.

#include <cstddef>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace ter::bench_host {

// 1 = on AC, 0 = on battery, -1 = unknown.
[[nodiscard]] inline int ac_line_status() noexcept {
#if defined(_WIN32)
  SYSTEM_POWER_STATUS status{};
  return GetSystemPowerStatus(&status) == 0 ? -1 : static_cast<int>(status.ACLineStatus);
#else
  return -1;
#endif
}

struct Memory {
  double working_set_mib = 0.0;
  double peak_working_set_mib = 0.0;
};

[[nodiscard]] inline Memory process_memory() noexcept {
  Memory memory;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
    memory.working_set_mib = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    memory.peak_working_set_mib = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
#endif
  return memory;
}

[[nodiscard]] inline double peak_working_set_mib() noexcept { return process_memory().peak_working_set_mib; }

// Efficiency class of every logical processor of group 0 (higher = faster
// core; on this hybrid CPU 1 = P-core, 0 = E-core).  Empty if unavailable.
[[nodiscard]] inline std::vector<int> efficiency_classes() {
  std::vector<int> classes;
#if defined(_WIN32)
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  std::vector<unsigned char> buffer(length);
  auto* const first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
  if (length == 0U || GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length) == 0) {
    return classes;
  }
  classes.assign(64, -1);
  for (DWORD offset = 0; offset < length;) {
    const auto* const info =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
    if (info->Relationship == RelationProcessorCore && info->Processor.GroupMask[0].Group == 0) {
      for (int bit = 0; bit < 64; ++bit) {
        if (((info->Processor.GroupMask[0].Mask >> bit) & 1U) != 0U) {
          classes[static_cast<std::size_t>(bit)] = info->Processor.EfficiencyClass;
        }
      }
    }
    offset += info->Size;
  }
#endif
  return classes;
}

[[nodiscard]] inline int class_of(const std::vector<int>& classes, const int cpu) noexcept {
  return cpu >= 0 && static_cast<std::size_t>(cpu) < classes.size() ? classes[static_cast<std::size_t>(cpu)] : -1;
}

[[nodiscard]] inline int current_cpu() noexcept {
#if defined(_WIN32)
  return static_cast<int>(GetCurrentProcessorNumber());
#else
  return -1;
#endif
}

// Opts the process out of Windows power throttling (EcoQoS) and hard-pins the
// calling thread to the highest-numbered logical processor of the highest
// efficiency class.  Returns that processor, or -1 on any failure.
[[nodiscard]] inline int pin_to_p_core(const std::vector<int>& classes) {
#if defined(_WIN32)
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = 0;
  if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) == 0) {
    return -1;
  }
  int best_class = -1;
  int chosen = -1;
  for (std::size_t cpu = 0U; cpu < classes.size(); ++cpu) {
    if (classes[cpu] >= best_class && classes[cpu] >= 0) {
      best_class = classes[cpu];
      chosen = static_cast<int>(cpu);
    }
  }
  if (chosen < 0 ||
      SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << static_cast<unsigned>(chosen)) == 0) {
    return -1;
  }
  return chosen;
#else
  static_cast<void>(classes);
  return -1;
#endif
}

}  // namespace ter::bench_host
