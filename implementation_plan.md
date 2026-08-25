# TER Stage-C/P2 Hybrid Partition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement, verify, and benchmark the current approved TER L1 -> L0 Stage-C/P2 partition substrate: an exact EntryId-preserving one-digit radix API with `CurrentScatter` as the frozen baseline and a portable IPS2Ra-derived radix8 `BlockInPlace` candidate, then validate it on genuine `FullTerRootCapture` data without replacing the production root search.

**Architecture:** This plan intentionally stops at Stage C/P2. The public abstraction is `partition_chunk(entries, spec, workspace, bucket_offsets, stats)`. The donor `ssp_oracle.cpp` supplies mature histogram/prefix/parallel-scatter mechanics; IPS2Ra supplies narrowly copied/adapted block-classification, full-block permutation, and margin-cleanup mechanics. `Region2Path`, `Adaptive`, exact region pruning, carry refinement, witness recovery, BBF policy, and production-root replacement are forbidden in this plan.

**Tech Stack:** C++20, CMake >= 3.24, existing TER `Status`/`EntryId`/`QuotientResidue`/`limits::checked`, `std::thread`/`std::condition_variable`, CTest, Python 3 for paired benchmark analysis, and the existing `FullTerRootCapture` replay seam. IPS2Ra is read-only source/reference material; TBB and upstream IPS2Ra are not runtime/build dependencies.

**Spec:** `TER_L1_to_L0_Radix_Refined_Root_Architecture_v2.1_CODEX.pdf` and `TER_Hybrid_Partition_Backend_v2.0_CODEX.pdf`.

## Global Constraints

- DO NOT run `git reset --hard`, `git clean`, destructive checkout/restore commands, or any command that could erase uncommitted work.
- The local uncommitted `ssp_oracle.cpp` is required donor/reference material. Locate it in the local TER worktree and preserve it.
- `C:\Users\naman\Desktop\ips2ra` is read-only. Do not edit, commit, fetch/rebase, or add it as a submodule from this plan.
- Committed TER baseline is `3eac235f871c2e4cf07a73152644e6113eaf39e1`; local worktree state is authoritative for uncommitted files.
- Upstream IPS2Ra reference HEAD for source mapping is `ef3407632348f58a6ab75832a749cde99f3b9ad8`.
- Preserve C++20, existing strict warnings, existing sanitizer/native/IPO options, and `include/ter/limitations.hpp` as the only owner of numeric runtime policy defaults.
- Do not add TBB, an IPS2Ra submodule, or a mandatory OpenMP dependency.
- `TerPartitionEntry` is exactly 8 bytes: `QuotientResidue low_k` + `EntryId entry_id`.
- Partition classification uses only `low_k`; the whole record moves; within-bucket stability is not required.
- CurrentScatter physical widths: 8, 12, 16. BlockInPlace v1 physical width: 8 only.
- `Region2Path` and `Adaptive` are deferred. They exist as enum values but return `Status::NotApplicable` until their gates are explicitly opened.
- No O(|A||B|) pair-record or pair-sum materialization in the production root.
- No persistent full-width quotient-limb vector per L1 entry.
- NTT/convolution remains comparator-only unless a later benchmark explicitly revises the specs.
- Every performance claim must separate pair-work reduction from per-open-pair/partition acceleration and fixed setup overhead.
- All copied/adapted IPS2Ra source must preserve BSD-2-Clause provenance.
- **STOP RULE:** after captured-root Stage-C validation and the paired CurrentScatter/BlockInPlace decision, stop. Do not implement Stage D or later root stages under this plan.

---

## File Structure Locked by This Plan

### New files

- Create: `include/ter/root_partition.hpp` - public one-digit partition API only.
- Create: `src/root_partition.cpp` - validation, dispatch, pImpl workspace, shared helpers.
- Create: `src/root_partition_current_scatter.cpp` - serial + parallel exact CurrentScatter backend.
- Create: `src/root_partition_block_inplace.cpp` - portable radix8 IPS2Ra-derived BlockInPlace backend.
- Create: `tests/root_partition_tests.cpp` - property/differential/resource/threading tests.
- Create: `bench/ter_root_partition_bench.cpp` - synthetic + genuine-root partition benchmark.
- Create: `bench/analyse_root_partition.py` - paired geometric-mean speedup + bootstrap CI analysis.
- Create: `third_party/licenses/ips2ra-BSD-2-Clause.txt` - upstream BSD-2-Clause text.
- Create: `docs/root_v2_baseline.md` - pre-edit safety/baseline record.
- Create: `docs/root_partition_decision.md` - measured backend decision.

### Existing files allowed to change

- Modify: `include/ter/types.hpp` - append `NotApplicable`, `UnsupportedWidth` only; do not reorder existing status values.
- Modify: `include/ter/limitations.hpp` - add partition-only numeric defaults/gates to the central control panel.
- Modify: `CMakeLists.txt` - add partition sources/test/benchmark and private `Threads::Threads` only.
- Modify only if capture regression coverage is necessary: `tests/full_ter_tests.cpp`.

### Files explicitly out of scope

- Do not rewrite `src/full_ter.cpp` in this plan.
- Do not introduce `root_regions.*`, `root_recovery.*`, BBF root code, NTT root code, or an Adaptive planner.
- Do not stage/commit the entire donor `ssp_oracle.cpp` as the implementation merely because it is present locally.

---

### Task 1: Safety Audit and Reproducible Baseline

**Files:**
- Read only: entire local TER worktree.
- Read only: local uncommitted `ssp_oracle.cpp`.
- Read only: `C:\Users\naman\Desktop\ips2ra`.
- Create after verification: `docs/root_v2_baseline.md`.

**Interfaces:**
- Consumes: current local working tree exactly as found.
- Produces: a human/auditable baseline record used by every later task.

- [ ] **Step 1: Record the TER working-tree state without modifying it**

Run in PowerShell from the TER repo:

```powershell
git status --short --branch
git rev-parse HEAD
git remote -v
```

Expected committed HEAD: `3eac235f871c2e4cf07a73152644e6113eaf39e1` unless the local repo has intentionally advanced. If HEAD differs, record the actual SHA; do not reset.

- [ ] **Step 2: Locate the unique local donor `ssp_oracle.cpp`**

```powershell
$hits = Get-ChildItem -Path . -Recurse -File -Filter ssp_oracle.cpp
$hits | Select-Object FullName,Length,LastWriteTime
if ($hits.Count -ne 1) { throw "Expected exactly one local ssp_oracle.cpp donor; stop before editing." }
```

Expected: exactly one donor file. Do not stage or move it.

- [ ] **Step 3: Verify the IPS2Ra clone is readable and clean without changing it**

```powershell
git -C C:\Users\naman\Desktop\ips2ra rev-parse HEAD
git -C C:\Users\naman\Desktop\ips2ra status --short
git -C C:\Users\naman\Desktop\ips2ra remote -v
```

Expected upstream reference HEAD: `ef3407632348f58a6ab75832a749cde99f3b9ad8` if the clone is current. If different, record it and compare the specific source files named in the specs before copying code. Do not update the clone automatically.

- [ ] **Step 4: Build and run the current TER test suite before any edits**

```powershell
cmake -S . -B build-baseline
cmake --build build-baseline --config Release --parallel
ctest --test-dir build-baseline -C Release --output-on-failure
```

Expected: current tests pass. If any fail, record exact failing tests in `docs/root_v2_baseline.md` and stop; do not begin architecture work on a dirty correctness baseline.

- [ ] **Step 5: Capture current root telemetry on at least one existing benchmark/replay path**

Run the existing root/full benchmark available in the build and record:

```text
commit SHA
command
seed/n/config
L1 A/B/C sizes if exposed
root elapsed_ns
pair_candidates
pair_records
prefilter bits
low-prefilter candidates
full-quotient rejects
working/output peak bytes
status
```

- [ ] **Step 6: Create `docs/root_v2_baseline.md` with the exact outputs**

The file must contain no estimates presented as measurements. Include the local donor path and IPS2Ra HEAD actually observed.

- [ ] **Step 7: Commit only the baseline document if the user wants the baseline committed now**

If committing is part of the current execution instruction:

```powershell
git add docs/root_v2_baseline.md
git commit -m "docs: freeze TER root v2 baseline"
```

Do not add `ssp_oracle.cpp` in this commit unless the user explicitly asks.

---

### Task 2: Append-Only Status and Central Policy Surface

**Files:**
- Modify: `include/ter/types.hpp`
- Modify: `include/ter/limitations.hpp`
- Test: `tests/limitations_tests.cpp`

**Interfaces:**
- Consumes: existing `Status`, `limits::control_panel`, checked resource arithmetic.
- Produces: `Status::NotApplicable`, `Status::UnsupportedWidth`, and centralized partition defaults.

- [ ] **Step 1: Write failing enum-preservation tests**

Add compile/runtime assertions that existing status numeric values remain unchanged and the new values are appended.

Example:

```cpp
static_assert(static_cast<std::uint8_t>(ter::Status::Completed) == 0);
static_assert(static_cast<std::uint8_t>(ter::Status::InvariantViolation) == 9);
static_assert(static_cast<std::uint8_t>(ter::Status::NotApplicable) == 10);
static_assert(static_cast<std::uint8_t>(ter::Status::UnsupportedWidth) == 11);
```

- [ ] **Step 2: Run the focused test and verify failure before implementation**

```powershell
cmake --build build-baseline --config Release --target ter_limitations_tests --parallel
ctest --test-dir build-baseline -C Release -R ter_limitations_tests --output-on-failure
```

Expected: compile failure because new enum values do not exist.

- [ ] **Step 3: Append the status values only**

In `include/ter/types.hpp`, after `InvariantViolation`:

```cpp
NotApplicable,
UnsupportedWidth,
```

Do not reorder existing values.

- [ ] **Step 4: Add partition defaults to the central control panel**

Under a new `namespace root_partition` in `limits::control_panel`:

```cpp
inline constexpr std::uint32_t kDefaultThreads = 1U;
inline constexpr std::uint32_t kCurrentScatterMaximumBits = 16U;
inline constexpr std::uint32_t kBlockInPlaceBits = 8U;
inline constexpr std::uint32_t kDefaultBlockBytes = 2U * 1024U;
inline constexpr std::uint32_t kMinimumBlockBytes = 64U;
inline constexpr std::uint32_t kMaximumPhysicalBits = 16U;
inline constexpr std::uint32_t kMaximumProjectionBits = 32U;
```

Do not add performance thresholds for Adaptive yet.

- [ ] **Step 5: Add tests for the policy constants and checked arithmetic**

Verify default block bytes is power-of-two and divisible by `sizeof(TerPartitionEntry)` only after Task 3 defines the record; until then test pure numeric properties.

- [ ] **Step 6: Run all existing tests**

```powershell
cmake --build build-baseline --config Release --parallel
ctest --test-dir build-baseline -C Release --output-on-failure
```

Expected: all pass.

- [ ] **Step 7: Commit**

```powershell
git add include/ter/types.hpp include/ter/limitations.hpp tests/limitations_tests.cpp
git commit -m "feat: add root partition status and policy surface"
```

---

### Task 3: Freeze the Public Partition API

**Files:**
- Create: `include/ter/root_partition.hpp`
- Create: `src/root_partition.cpp`
- Create: `tests/root_partition_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces exact signatures consumed by all later partition/root tasks.

- [ ] **Step 1: Create a failing API/ABI-size test**

In `tests/root_partition_tests.cpp`:

```cpp
#include "ter/root_partition.hpp"

#include <cassert>
#include <type_traits>

static void test_partition_entry_layout() {
  static_assert(sizeof(ter::TerPartitionEntry) == 8);
  static_assert(std::is_trivially_copyable_v<ter::TerPartitionEntry>);
}
```

Also instantiate `PartitionWorkspace`, `PartitionSpec`, `PartitionStats` and call `partition_chunk` on an empty span.

- [ ] **Step 2: Add the test target to CMake and confirm compile failure**

Add:

```cmake
add_executable(ter_root_partition_tests tests/root_partition_tests.cpp)
target_link_libraries(ter_root_partition_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_root_partition_tests)
ter_enable_release_ipo(ter_root_partition_tests)
add_test(NAME ter_root_partition_tests COMMAND ter_root_partition_tests)
```

Run and expect missing-header failure.

- [ ] **Step 3: Implement the exact public header from the companion spec**

Use the frozen types and pImpl `PartitionWorkspace`. Include `<memory>`, `<span>`, `<type_traits>`, `<cstdint>`.

- [ ] **Step 4: Implement pImpl lifecycle and validation-only dispatcher skeleton**

`partition_chunk` must reset `stats`, set `requested`, validate the spec, and return:

```text
Completed for N=0/1 after exact offsets are written
NotApplicable for Region2Path/Adaptive
InvalidConfiguration for invalid bits/offset size/block bytes
```

Do not implement CurrentScatter yet.

- [ ] **Step 5: Add validation tests**

Exact cases:

```text
N=0, bits=8, offsets size=257 -> Completed, offsets all zero
N=1 -> Completed, exactly one bucket span of size one
bits=0 -> InvalidConfiguration
bits=17 CurrentScatter -> InvalidConfiguration
bits=12 BlockInPlace -> NotApplicable
shift=25,bits=8 -> InvalidConfiguration
wrong offsets length -> InvalidConfiguration
block_bytes=3000 -> InvalidConfiguration
Region2Path -> NotApplicable
Adaptive -> NotApplicable
```

- [ ] **Step 6: Run focused tests**

```powershell
cmake -S . -B build-root-v2
cmake --build build-root-v2 --config Release --target ter_root_partition_tests --parallel
ctest --test-dir build-root-v2 -C Release -R ter_root_partition_tests --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Commit**

```powershell
git add include/ter/root_partition.hpp src/root_partition.cpp tests/root_partition_tests.cpp CMakeLists.txt
git commit -m "feat: define exact TER root partition API"
```

---

### Task 4: CurrentScatter Serial Baseline

**Files:**
- Create: `src/root_partition_current_scatter.cpp`
- Modify: `src/root_partition.cpp`
- Test: `tests/root_partition_tests.cpp`

**Interfaces:**
- Produces: exact serial one-digit partition for 8/12/16 bits.

- [ ] **Step 1: Write the differential/property tests before implementation**

Create a test helper that verifies:

```cpp
bucket_offsets.front() == 0;
bucket_offsets.back() == entries.size();
std::is_sorted(bucket_offsets.begin(), bucket_offsets.end());
```

For every bucket span, recompute digit from `low_k` and assert equality. Compare sorted EntryId multisets before/after.

Required deterministic fixtures:

```text
empty
singleton
all one bucket
one per 8-bit bucket
alternating 0/255
already partitioned
reverse partitioned
random 10k records with random EntryIds
```

- [ ] **Step 2: Run and verify CurrentScatter nontrivial tests fail**

Expected: `NotApplicable`/skeleton failure.

- [ ] **Step 3: Implement serial histogram -> prefix -> scatter**

Internal backend signature:

```cpp
Status partition_current_scatter(
    std::span<TerPartitionEntry> entries,
    const PartitionSpec& spec,
    PartitionWorkspace::Impl& workspace,
    std::span<std::uint64_t> bucket_offsets,
    PartitionStats& stats);
```

Use checked allocation arithmetic before resizing scratch. Classification:

```cpp
const auto mask = (std::uint32_t{1} << spec.bits) - 1U;
const auto digit = (entry.low_k >> spec.shift) & mask;
```

- [ ] **Step 4: Preserve exact original offsets**

After exclusive prefix, keep immutable `bucket_offsets`. Use a separate cursor array for scatter; never mutate the returned offsets during the write loop.

- [ ] **Step 5: Fill telemetry**

At minimum:

```text
histogram_ns
prefix_ns
scatter_ns
records_classified=N
records_written=N
record_scratch_bytes=capacity*sizeof(record)
histogram_bytes
offset_bytes
scratch_peak_bytes
logical_bytes_read
logical_bytes_written
```

- [ ] **Step 6: Run focused tests in Release and ASan configurations**

```powershell
cmake --build build-root-v2 --config Release --target ter_root_partition_tests --parallel
ctest --test-dir build-root-v2 -C Release -R ter_root_partition_tests --output-on-failure
cmake -S . -B build-root-v2-asan -DTER_ENABLE_ASAN=ON
cmake --build build-root-v2-asan --config Debug --target ter_root_partition_tests --parallel
ctest --test-dir build-root-v2-asan -C Debug -R ter_root_partition_tests --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Commit**

```powershell
git add src/root_partition_current_scatter.cpp src/root_partition.cpp tests/root_partition_tests.cpp CMakeLists.txt
git commit -m "feat: add serial EntryId-preserving CurrentScatter partition"
```

---

### Task 5: CurrentScatter Parallel Donor Extraction

**Files:**
- Modify: `src/root_partition_current_scatter.cpp`
- Modify: `src/root_partition.cpp`
- Test: `tests/root_partition_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: local donor `ssp_oracle.cpp` worker-state/pool logic.
- Produces: deterministic parallel CurrentScatter using the same exact public contract.

- [ ] **Step 1: Inspect and record donor source sections before copying**

Find in local `ssp_oracle.cpp`:

```text
RadixWorkerState
RadixWorkerPool
parallel_radix_sort_unsigned_values
radix_sort_unsigned_values
radix_worker_state_bytes
parallel radix telemetry update sites
```

Copy only the minimum algorithmic/threading logic needed. Do not copy oracle ABI, Bloom, SS, cache, or query code.

- [ ] **Step 2: Add thread dependency cleanly**

In `CMakeLists.txt`:

```cmake
find_package(Threads REQUIRED)
target_link_libraries(ter_t0 PRIVATE Threads::Threads)
```

- [ ] **Step 3: Write tests comparing threads=1 vs 2/4/8**

For the same 100k-record fixture and each physical width 8/12/16:

- offsets must match exactly;
- per-bucket EntryId sets must match exactly;
- within-bucket order may differ;
- repeated identical runs must produce stable offsets and logical work counters.

- [ ] **Step 4: Implement `PartitionWorkerState` and persistent worker pool**

Use `alignas(64)` around per-worker histogram/offset vectors. Preserve donor semantics: one synchronous task generation, contiguous source stripe per worker, no unrelated shared TER state.

- [ ] **Step 5: Implement bucket-major/worker-major prefix**

Each worker receives a disjoint output segment for each bucket. Verify the final global bucket end equals N before scatter.

- [ ] **Step 6: Implement parallel complete-record scatter**

Each worker reads its source stripe and writes `TerPartitionEntry`, not just `low_k`.

- [ ] **Step 7: Run race-oriented stress tests**

Repeat random fixtures 100 times for thread counts 2/4/8/16 when available. Include 99.99% hot bucket and all-one-bucket.

- [ ] **Step 8: Run full CTest**

Expected: no regression in existing TER tests.

- [ ] **Step 9: Commit**

```powershell
git add src/root_partition_current_scatter.cpp src/root_partition.cpp tests/root_partition_tests.cpp CMakeLists.txt
git commit -m "feat: port parallel radix scatter substrate to TER records"
```

---

### Task 6: Freeze the Isolated CurrentScatter Benchmark Before Candidate Tuning

**Files:**
- Create: `bench/ter_root_partition_bench.cpp`
- Create: `bench/analyse_root_partition.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces a stable measurement path used later for BlockInPlace comparison.

- [ ] **Step 1: Add benchmark CLI schema**

The benchmark must accept at minimum:

```text
--backend current-scatter|block-inplace
--n comma-separated sizes
--bits 8|12|16
--shift integer
--threads comma-separated list
--distribution uniform|hot|alternating|sorted|reverse|duplicates
--seed uint64
--warmup integer
--repeat integer
--output path.jsonl
```

- [ ] **Step 2: Emit one JSON object per measured run**

Required fields:

```text
schema_version
backend
N
bits
shift
threads_requested
threads_used
distribution
seed
repetition
elapsed_ns
histogram_ns
prefix_ns
scatter_ns
local_classification_ns
block_permute_ns
cleanup_ns
scratch_peak_bytes
record_scratch_bytes
block_buffer_bytes
records_classified
records_written
blocks_flushed
blocks_permuted
input_fingerprint
output_bucket_fingerprint
status
```

- [ ] **Step 3: Make benchmark verify correctness before recording timing**

Every generated input is first partitioned through an untimed reference path; timed result must match bucket counts/membership.

- [ ] **Step 4: Implement paired analysis script now, before BlockInPlace results exist**

`bench/analyse_root_partition.py` reads JSONL, pairs runs by dataset/N/bits/threads/seed/repetition, computes:

```text
S_i = current_ns / candidate_ns
x_i = ln(S_i)
geometric mean = exp(mean(x_i))
bootstrap paired x_i 10,000 times
95% CI = exp(percentiles 2.5,97.5)
```

The script must refuse unpaired rows.

- [ ] **Step 5: Run baseline matrix**

At minimum:

```text
N: 20k, 32496, 64k, 0.5M, 1M
bits: 8,12,16
threads: 1,2,4,8,16 where available
5 warmups, 30 measured
```

Store results under `results/partition/current_scatter_baseline.jsonl` if results are intended to be versioned; otherwise keep them untracked but record the command in the baseline doc.

- [ ] **Step 6: Commit benchmark code, not fabricated result claims**

```powershell
git add bench/ter_root_partition_bench.cpp bench/analyse_root_partition.py CMakeLists.txt
git commit -m "bench: freeze TER root partition baseline harness"
```

---

### Task 7: Add IPS2Ra License Provenance Before Copying Source

**Files:**
- Create: `third_party/licenses/ips2ra-BSD-2-Clause.txt`
- Later header target: `src/root_partition_block_inplace.cpp`

**Interfaces:**
- Produces legal/provenance boundary before derived code enters TER.

- [ ] **Step 1: Copy the exact BSD-2-Clause license text from the local IPS2Ra clone**

Source: `C:\Users\naman\Desktop\ips2ra\LICENSE`.

- [ ] **Step 2: Add a provenance note at the top of the future derived file**

The header must name:

```text
Upstream: https://github.com/ips4o/ips2ra
Reference commit: ef3407632348f58a6ab75832a749cde99f3b9ad8
Derived concepts/files: partitioning.hpp, local_classification.hpp,
block_permutation.hpp, cleanup_margins.hpp, bucket_pointers.hpp,
config.hpp, memory/buffer helpers as actually used.
License: BSD-2-Clause; full text in third_party/licenses/ips2ra-BSD-2-Clause.txt
```

- [ ] **Step 3: Commit the notice before derived source**

```powershell
git add third_party/licenses/ips2ra-BSD-2-Clause.txt
git commit -m "docs: add IPS2Ra BSD license provenance"
```

---

### Task 8: BlockInPlace Serial Radix8 Reference

**Files:**
- Create: `src/root_partition_block_inplace.cpp`
- Modify: `src/root_partition.cpp`
- Test: `tests/root_partition_tests.cpp`

**Interfaces:**
- Produces a correct radix8 in-place backend before concurrency.

- [ ] **Step 1: Add failing BlockInPlace differential tests**

For threads=1, compare to CurrentScatter on:

```text
N=0,1
255,256,257
511,512,513
10k random
all one bucket
99.99% hot bucket
alternating two buckets
sorted/reverse
random EntryIds
```

Expected before implementation: `NotApplicable`.

- [ ] **Step 2: Freeze radix8/block geometry helpers**

Use:

```cpp
constexpr std::uint32_t kBuckets = 256;
records_per_block = spec.block_bytes / sizeof(TerPartitionEntry);
```

Validate power-of-two block record count.

- [ ] **Step 3: Implement serial local classification buffers**

Adapt IPS2Ra's concept, not its recursive classifier. Per bucket:

- fixed-capacity logical block buffer;
- bucket count;
- full-buffer flush into local classified full-block area;
- partial buffer retained.

Classifier is exactly `(low_k >> shift) & 0xFF`.

- [ ] **Step 4: Compute global bucket offsets**

For serial path, global count equals local bucket count. Preserve returned offsets independently of mutable block pointers.

- [ ] **Step 5: Implement serial block pointer state and displaced-block permutation**

Use plain integer read/write positions in serial mode. Port the minimum `classifyAndReadBlock` / `swapBlock` / `permuteBlocks` idea. Add a debug progress counter bounded by a conservative multiple of the number of full blocks; exceeding it returns `InvariantViolation`.

- [ ] **Step 6: Implement margin/partial-buffer cleanup**

Drain every retained partial buffer into the exact unfilled slots of its bucket. After cleanup, all buffers are empty and every output digit is correct.

- [ ] **Step 7: Fill BlockInPlace telemetry**

Populate local classification, block permutation, cleanup time, blocks flushed/permuted, partial records repaired, buffer bytes, scratch peak.

- [ ] **Step 8: Run differential + ASan tests**

Expected: all BlockInPlace serial fixtures equal CurrentScatter bucket membership.

- [ ] **Step 9: Commit**

```powershell
git add src/root_partition_block_inplace.cpp src/root_partition.cpp tests/root_partition_tests.cpp
git commit -m "feat: add serial radix8 BlockInPlace TER partition"
```

---

### Task 9: Portable Parallel BlockInPlace

**Files:**
- Modify: `src/root_partition_block_inplace.cpp`
- Modify: `src/root_partition.cpp`
- Test: `tests/root_partition_tests.cpp`

**Interfaces:**
- Consumes: persistent worker pool from CurrentScatter.
- Produces: portable multi-thread radix8 BlockInPlace without TBB or 128-bit atomic requirement.

- [ ] **Step 1: Add failing threads=2/4/8/16 differential tests**

Use N values that cross stripe/block boundaries and the hot-bucket case.

- [ ] **Step 2: Add portable `BucketBlockPointers`**

Required state per bucket:

```cpp
std::uint64_t write_block;
std::uint64_t read_block;
std::mutex pointer_mutex;
std::atomic<std::uint32_t> readers{0};
```

Provide operations equivalent to:

```text
claim_read_block()
release_read_block()
claim_write_block()
is_reading()
```

Do not introduce `unsigned __int128` in the required path.

- [ ] **Step 3: Parallelize local classification by deterministic stripes**

Each worker has independent 256 bucket counts/buffers/swap blocks. No worker writes another worker's local buffer.

- [ ] **Step 4: Reduce counts and establish global bucket countries**

Use a deterministic single-thread prefix after workers finish classification unless profiling later proves it material. Correctness first.

- [ ] **Step 5: Parallelize full-block permutation**

Workers choose deterministic starting buckets but claim read/write positions through `BucketBlockPointers`. Preserve the reader guard before overwriting a destination block.

- [ ] **Step 6: Parallel cleanup only if spans are disjoint; otherwise keep cleanup serial initially**

The first correct implementation may use serial cleanup. Record its time separately so the benchmark can decide whether parallel cleanup is worthwhile.

- [ ] **Step 7: Stress for deadlock/livelock**

Run at least 100 repetitions each for uniform, all-one-bucket, alternating, and hot-bucket fixtures at available thread counts. Add timeout protection in the test runner, not by returning false completion from the algorithm.

- [ ] **Step 8: Run full Release + ASan suite**

Expected: no mismatches, no sanitizer reports.

- [ ] **Step 9: Commit**

```powershell
git add src/root_partition_block_inplace.cpp src/root_partition.cpp tests/root_partition_tests.cpp
git commit -m "feat: parallelize portable BlockInPlace TER partition"
```

---

### Task 10: Genuine `FullTerRootCapture` Stage-C Adapter

**Files:**
- Modify: `bench/ter_root_partition_bench.cpp`
- Test: `tests/root_partition_tests.cpp`
- Modify only if a capture-seam regression is needed: `tests/full_ter_tests.cpp`

**Interfaces:**
- Consumes: existing `FullTerRootCapture`, `FullTerParameters::r1`, child residue words/targets, and `partition_chunk`.
- Produces: exact 32-bit low-quotient movement records for A/B/C and genuine-root partition measurements. It does **not** replace the production root search.

- [ ] **Step 1: Add an exact low-32 quotient projection helper test before implementation**

For one child residue `r`, child target `t`, width `n`, and `r1`, define:

```text
delta = (r - t) mod 2^n
q = delta >> r1
expected = q mod 2^min(32, n-r1)
```

In the test, compute `expected` with the repository's independent arbitrary-width/reference arithmetic. Add cases with `r1` at 0, 7, 8, 15, 16, 25, 31, 32, 33, 63 and widths that cross 32/64-bit boundaries.

- [ ] **Step 2: Run the focused test and verify it fails**

```powershell
cmake --build build-root-v2 --config Release --target ter_root_partition_tests --parallel
ctest --test-dir build-root-v2 -C Release -R ter_root_partition_tests --output-on-failure
```

Expected: compile/link failure because the capture projection helper does not exist in the benchmark/test helper layer yet.

- [ ] **Step 3: Implement low-32 projection without constructing a persistent wide quotient**

Use 32-bit little-endian subtraction with a 64-bit temporary and borrow. Stream only far enough to obtain the quotient slice.

```cpp
std::uint32_t quotient_low32_from_words(
    std::span<const std::uint32_t> residue,
    std::span<const std::uint32_t> child_target,
    std::uint32_t n,
    std::uint32_t r1) {
  if (r1 >= n) return 0U;
  const std::uint32_t q_bits = std::min<std::uint32_t>(32U, n - r1);
  const std::size_t first = r1 / 32U;
  const std::uint32_t offset = r1 % 32U;
  const std::size_t need_through = first + (offset == 0U ? 0U : 1U);

  std::uint64_t borrow = 0U;
  std::uint32_t word0 = 0U;
  std::uint32_t word1 = 0U;
  for (std::size_t i = 0; i <= need_through; ++i) {
    const std::uint64_t rv = i < residue.size() ? residue[i] : 0U;
    const std::uint64_t tv = i < child_target.size() ? child_target[i] : 0U;
    const std::uint64_t sub = tv + borrow;
    std::uint32_t out = static_cast<std::uint32_t>(rv - sub);
    borrow = rv < sub ? 1U : 0U;
    if (i == first) word0 = out;
    if (i == first + 1U) word1 = out;
  }

  std::uint32_t q = offset == 0U
      ? word0
      : static_cast<std::uint32_t>((word0 >> offset) | (word1 << (32U - offset)));
  if (q_bits < 32U) q &= (std::uint32_t{1} << q_bits) - 1U;
  return q;
}
```

Before accepting this code, verify the repository's top partial-word convention. If `n` is not a multiple of 32, mask the streamed top source word consistently with the existing capture representation before using bits above `n`. The independent reference test is the authority.

- [ ] **Step 4: Build exact movement records from a captured root**

For each list `g in {A,B,C}`:

```cpp
std::vector<ter::TerPartitionEntry> records(capture.children[g].size());
for (std::size_t i = 0; i < records.size(); ++i) {
  records[i].low_k = quotient_low32_from_words(
      capture.residues_words_le[g][i],
      capture.child_targets_words_le[g],
      capture.parameters.n,
      capture.parameters.r1);
  records[i].entry_id = static_cast<ter::EntryId>(i);
}
```

Preflight `records.size() <= numeric_limits<EntryId>::max()`; otherwise return/report `UnsupportedWidth` or a benchmark refusal before casting.

- [ ] **Step 5: Obtain the capture through the existing in-memory seam**

In the benchmark, mirror the existing `ter_full_bench.cpp` instance/config construction rather than inventing a serialized format:

```cpp
ter::FullTerRootCapture capture;
ter::FullTerConfig config = /* same benchmark profile used by ter_full_bench */;
config.root_capture = &capture;
const auto solved = ter::solve_full_ter(instance, config);
if (capture.status != ter::Status::Completed) {
  // report capture failure; do not interpret it as empty input
}
```

Keep the capture generation outside the timed partition interval.

- [ ] **Step 6: Add captured-root benchmark modes**

Add CLI fields with exact syntax:

```text
--dataset synthetic|captured-root
--root-n <uint32>
--root-seed <uint64>
--root-mode fixture|full
--list A|B|C|all
```

For `captured-root`, emit capture generation time separately and time only the requested partition operation. Record actual A/B/C sizes and `r1`/`Q`.

- [ ] **Step 7: Differentially verify capture records**

For every EntryId before timing:

```text
record.entry_id indexes the original capture list
record.low_k equals the independent exact quotient low32
CurrentScatter bucket span contains exactly the expected EntryIds
```

- [ ] **Step 8: Run genuine-root CurrentScatter baseline before interpreting BlockInPlace results**

At minimum collect the available n=72-style root capture using the same seed/config used by existing root benchmarks, then run physical bits 8, 12, 16 for CurrentScatter. Record the exact command and capture geometry.

- [ ] **Step 9: Commit the adapter**

```powershell
git add bench/ter_root_partition_bench.cpp tests/root_partition_tests.cpp tests/full_ter_tests.cpp
git commit -m "bench: add genuine TER root partition replay"
```

If `tests/full_ter_tests.cpp` was not changed, omit it from `git add`.

---

### Task 11: Paired Partition Bake-Off and Backend Decision

**Files:**
- Modify only if needed for instrumentation: benchmark/analysis files.
- Create: `docs/root_partition_decision.md`

**Interfaces:**
- Consumes: frozen benchmark harness plus genuine capture adapter.
- Produces: measured backend decision. `Adaptive` remains `NotApplicable` in this plan even if a clean threshold appears; threshold installation requires a separate reviewed change.

- [ ] **Step 1: Run identical paired matrix**

Required:

```text
N = 20k, 32496, 64k, 0.5M, 1M, 2M, 5M where machine memory/time allows
bits = 8 for direct backend comparison
threads = 1,2,4,8,16 where available
distributions = uniform,hot,alternating,sorted,reverse,duplicates
captured genuine L1 A/B/C lists (available after Task 10)
warmup = 5
repeat >= 30
```

- [ ] **Step 2: Sweep BlockInPlace block bytes**

At minimum:

```text
1024, 2048, 4096
```

Do not sweep hundreds of knobs before establishing basic viability.

- [ ] **Step 3: Run paired bootstrap analysis**

The analysis must output geometric mean speedup, 95% CI, median, p95 candidate/current ratio, and peak scratch ratio.

- [ ] **Step 4: Apply the gates exactly**

Speed default only if lower 95% CI > 1.05x on representative captured roots and tails are acceptable.

Memory backend if scratch reduction >=4x and wall time <=1.10 * CurrentScatter.

- [ ] **Step 5: Write `docs/root_partition_decision.md`**

State one of:

```text
A. BlockInPlace promoted as speed default for specified regime.
B. BlockInPlace retained as memory backend only.
C. BlockInPlace retained experimental; CurrentScatter remains default.
```

Include actual measured numbers and commands. Do not infer Region2Path performance.

- [ ] **Step 6: Keep Adaptive disabled in this implementation plan**

If the data suggests a deterministic threshold, write it as a recommendation in `docs/root_partition_decision.md`; do not install it yet. `Adaptive` continues to return `Status::NotApplicable`.

- [ ] **Step 7: Commit benchmark decision**

```powershell
git add docs/root_partition_decision.md
git commit -m "bench: decide TER root partition backend from paired data"
```

---

### Task 12: Stage-C Acceptance Gate, Full Regression, and Stop

**Files:**
- Modify if needed only for documentation: `docs/root_partition_decision.md`
- No new algorithm source is permitted in this task.

**Interfaces:**
- Consumes: Tasks 1-11.
- Produces: a reviewed P2/Stage-C substrate ready for the next separately specified research stage.

- [ ] **Step 1: Run Release build from a fresh build directory**

```powershell
cmake -S . -B build-root-partition-final -DCMAKE_BUILD_TYPE=Release
cmake --build build-root-partition-final --config Release --parallel
ctest --test-dir build-root-partition-final -C Release --output-on-failure
```

Expected: every pre-existing TER test plus `ter_root_partition_tests` passes.

- [ ] **Step 2: Run sanitizer build where the active compiler supports it**

```powershell
cmake -S . -B build-root-partition-asan -DTER_ENABLE_ASAN=ON
cmake --build build-root-partition-asan --config Debug --parallel
ctest --test-dir build-root-partition-asan -C Debug --output-on-failure
```

If the selected Windows toolchain does not support the repository's sanitizer configuration, record that fact and run the sanitizer configuration under the supported GCC/Clang environment. Do not silently skip it.

- [ ] **Step 3: Verify the exact P2 invariants by inspection and tests**

Required final assertions:

```text
TerPartitionEntry size == 8
classification key == low_k only
EntryId multiset preserved
bucket offsets monotone, offsets[0]=0, offsets[B]=N
CurrentScatter: bits 8/12/16
BlockInPlace: bits 8 only
Region2Path -> NotApplicable
Adaptive -> NotApplicable
no TBB dependency
no IPS2Ra submodule/dependency
no dense 24-bit worker histogram
no within-bucket stability assumption
all IPS2Ra-derived source has BSD provenance
```

- [ ] **Step 4: Verify memory-accounting claims against formulas**

For 16 workers and 8-byte records, the documentation/reference checks are:

```text
b=8  dense hist+offset+global ~= 0.0645 MiB
b=12 dense hist+offset+global ~= 1.0313 MiB
b=16 dense hist+offset+global ~= 16.50 MiB
5M CurrentScatter full record scratch ~= 38.15 MiB
radix8 BlockInPlace raw 16*256*2048 bucket buffers = 8 MiB
```

Measured allocator/peak fields may differ because of capacity/alignment; the code must label those separately rather than pretending the model equals observed RSS.

- [ ] **Step 5: Verify benchmark claim hygiene**

`docs/root_partition_decision.md` must distinguish:

```text
MODELLED sensitivity: e.g. 1.15x/1.25x/1.50x from R/S assumptions
MEASURED partition speedup: actual paired data only
ROOT pair-work reduction: not produced by Stage C alone
WHOLE-ATTEMPT speedup: not inferable from partition microbenchmarks
```

- [ ] **Step 6: Check repository diff for accidental scope creep**

```powershell
git status --short
git diff --stat
git diff -- CMakeLists.txt include/ter/types.hpp include/ter/limitations.hpp include/ter/root_partition.hpp src/root_partition.cpp src/root_partition_current_scatter.cpp src/root_partition_block_inplace.cpp tests/root_partition_tests.cpp bench/ter_root_partition_bench.cpp bench/analyse_root_partition.py
```

Reject the change if it contains production Stage-D+ code, TBB, an IPS2Ra submodule, unrelated refactors, or destructive edits to the donor file.

- [ ] **Step 7: Run final genuine-root replay**

Run the captured-root partition benchmark for A/B/C with the backend selected by the decision document and with CurrentScatter as the differential control. Require identical bucket counts/membership and successful statuses.

- [ ] **Step 8: Stop**

Do **not** begin exact region pruning, carry refinement, witness recovery, BBF integration, production-root replacement, `Region2Path`, or `Adaptive` in this plan. Those require a new reviewed spec/plan.

---

## Final Self-Review Before Giving This Plan to Codex

**Spec coverage:** every implementation-ready Stage-C/P2 requirement in the two refined specifications maps to Tasks 1-12. Later root stages are deliberately research-gated rather than left to agent interpretation.

**Completion-marker scan:** no unfinished implementation directive remains. Every deferred backend has a defined runtime result and an explicit gate.

**Type consistency:** the same names are used everywhere: `TerPartitionEntry`, `PartitionBackend`, `PartitionSpec`, `PartitionStats`, `PartitionWorkspace`, `partition_chunk`, `Status::NotApplicable`, and `Status::UnsupportedWidth`.

**Scope consistency:** this plan never requires editing the local IPS2Ra clone, linking upstream IPS2Ra, adding TBB, or replacing the production root. The local uncommitted `ssp_oracle.cpp` remains donor/reference input only.

**Evidence discipline:** CurrentScatter is frozen before candidate tuning; BlockInPlace is promoted only by paired measurements; Stage C is never described as the source of structural pair-space reduction.
