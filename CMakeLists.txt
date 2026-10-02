cmake_minimum_required(VERSION 3.24)

project(ter_t0_baseline VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)
include(CTest)

# Benchmark provenance (repair item P29).  Stamped when this build tree is
# configured, so it can lag HEAD; ter_full_bench prints it as
# configure_time_commit alongside an always-accurate executable SHA-256.
find_package(Git QUIET)
set(TER_CONFIGURE_TIME_COMMIT "unknown")
set(TER_CONFIGURE_TIME_DIRTY 1)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE TER_GIT_COMMIT_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE TER_GIT_COMMIT_RESULT
    )
    if(TER_GIT_COMMIT_RESULT EQUAL 0 AND NOT TER_GIT_COMMIT_RAW STREQUAL "")
        set(TER_CONFIGURE_TIME_COMMIT "${TER_GIT_COMMIT_RAW}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE TER_GIT_DIRTY_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE TER_GIT_DIRTY_RESULT
    )
    if(TER_GIT_DIRTY_RESULT EQUAL 0 AND TER_GIT_DIRTY_RAW STREQUAL "")
        set(TER_CONFIGURE_TIME_DIRTY 0)
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TER_ENABLE_ASAN "Enable AddressSanitizer where supported" OFF)
option(TER_NATIVE_OPT "Enable native CPU tuning in Release builds" OFF)

include(CheckIPOSupported)
check_ipo_supported(
    RESULT TER_IPO_SUPPORTED
    OUTPUT TER_IPO_ERROR
    LANGUAGES CXX
)

function(ter_enable_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(
            ${target}
            PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Wsign-conversion
            -Wnon-virtual-dtor
            -Woverloaded-virtual
        )
    endif()
endfunction()

function(ter_enable_release_ipo target)
    if(TER_IPO_SUPPORTED)
        set_property(
            TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        )
        target_compile_definitions(${target} PRIVATE TER_IPO_ENABLED=1)
    endif()
endfunction()

if(TER_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        add_link_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
    endif()
endif()

if(TER_NATIVE_OPT)
    if(MSVC)
        add_compile_options($<$<CONFIG:Release>:/arch:AVX2>)
    else()
        add_compile_options(
            $<$<CONFIG:Release>:-march=native>
            $<$<CONFIG:Release>:-mtune=native>
        )
    endif()

    add_compile_definitions(TER_NATIVE_OPT_ENABLED=1)
endif()

add_library(
    ter_t0
    src/arithmetic.cpp
    src/full_ter.cpp
    src/generator.cpp
    src/instance.cpp
    src/merge.cpp
    src/packed.cpp
    src/q3report_audit.cpp
    src/q3report_crossover.cpp
    src/q3report_paper_crossover.cpp
    src/reference_vector.cpp
    src/root_partition.cpp
    src/root_partition_block_inplace.cpp
    src/root_partition_current_scatter.cpp
    src/shape.cpp
    src/stress_generator.cpp
    src/tiled_q3report.cpp
    src/validator.cpp
)

target_include_directories(
    ter_t0
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(ter_t0 PRIVATE Threads::Threads)
target_compile_definitions(
    ter_t0
    PRIVATE
    $<$<BOOL:${BUILD_TESTING}>:TER_ROOT_PARTITION_TESTING=1>
)

ter_enable_strict_warnings(ter_t0)
ter_enable_release_ipo(ter_t0)

enable_testing()

add_executable(
    ter_shape_tests
    tests/shape_tests.cpp
)
target_link_libraries(ter_shape_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_shape_tests)
ter_enable_release_ipo(ter_shape_tests)
add_test(NAME ter_shape_tests COMMAND ter_shape_tests)

add_executable(
    ter_core_tests
    tests/core_tests.cpp
)
target_link_libraries(ter_core_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_core_tests)
ter_enable_release_ipo(ter_core_tests)
add_test(NAME ter_core_tests COMMAND ter_core_tests)

add_executable(
    ter_generator_tests
    tests/generator_tests.cpp
)
target_link_libraries(ter_generator_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_generator_tests)
ter_enable_release_ipo(ter_generator_tests)
add_test(NAME ter_generator_tests COMMAND ter_generator_tests)

add_executable(
    ter_stress_generator_tests
    tests/stress_generator_tests.cpp
)
target_link_libraries(ter_stress_generator_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_stress_generator_tests)
ter_enable_release_ipo(ter_stress_generator_tests)
add_test(NAME ter_stress_generator_tests COMMAND ter_stress_generator_tests)

add_executable(
    ter_merge_tests
    tests/merge_tests.cpp
)
target_link_libraries(ter_merge_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_merge_tests)
ter_enable_release_ipo(ter_merge_tests)
add_test(NAME ter_merge_tests COMMAND ter_merge_tests)

add_executable(
    ter_packed_tests
    tests/packed_tests.cpp
)
target_link_libraries(ter_packed_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_packed_tests)
ter_enable_release_ipo(ter_packed_tests)
add_test(NAME ter_packed_tests COMMAND ter_packed_tests)

add_executable(
    ter_full_tests
    tests/full_ter_tests.cpp
)
target_link_libraries(ter_full_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_full_tests)
ter_enable_release_ipo(ter_full_tests)
add_test(NAME ter_full_tests COMMAND ter_full_tests)

add_executable(
    ter_q3report_audit_tests
    tests/q3report_audit_tests.cpp
)
target_link_libraries(ter_q3report_audit_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_q3report_audit_tests)
ter_enable_release_ipo(ter_q3report_audit_tests)
add_test(NAME ter_q3report_audit_tests COMMAND ter_q3report_audit_tests)

add_executable(
    ter_q3report_crossover_tests
    tests/q3report_crossover_tests.cpp
)
target_link_libraries(ter_q3report_crossover_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_q3report_crossover_tests)
ter_enable_release_ipo(ter_q3report_crossover_tests)
add_test(
    NAME ter_q3report_crossover_tests
    COMMAND ter_q3report_crossover_tests
)

add_executable(
    ter_q3report_paper_crossover_tests
    tests/q3report_paper_crossover_tests.cpp
)
target_link_libraries(
    ter_q3report_paper_crossover_tests
    PRIVATE
    ter_t0
)
ter_enable_strict_warnings(ter_q3report_paper_crossover_tests)
ter_enable_release_ipo(ter_q3report_paper_crossover_tests)
add_test(
    NAME ter_q3report_paper_crossover_tests
    COMMAND ter_q3report_paper_crossover_tests
)

add_executable(
    ter_tiled_q3report_tests
    tests/tiled_q3report_tests.cpp
)
target_link_libraries(ter_tiled_q3report_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_tiled_q3report_tests)
ter_enable_release_ipo(ter_tiled_q3report_tests)
add_test(
    NAME ter_tiled_q3report_tests
    COMMAND ter_tiled_q3report_tests
)

add_executable(
    ter_limitations_tests
    tests/limitations_tests.cpp
)
target_link_libraries(ter_limitations_tests PRIVATE ter_t0)
ter_enable_strict_warnings(ter_limitations_tests)
ter_enable_release_ipo(ter_limitations_tests)
add_test(
    NAME ter_limitations_tests
    COMMAND ter_limitations_tests
)

add_executable(
    ter_root_partition_tests
    tests/root_partition_tests.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_root_partition_tests PRIVATE ter_t0)
target_include_directories(
    ter_root_partition_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
target_compile_definitions(
    ter_root_partition_tests
    PRIVATE
    $<$<BOOL:${BUILD_TESTING}>:TER_ROOT_PARTITION_TESTING=1>
)
ter_enable_strict_warnings(ter_root_partition_tests)
ter_enable_release_ipo(ter_root_partition_tests)
add_test(NAME ter_root_partition_tests COMMAND ter_root_partition_tests)

add_executable(
    ter_baseline_bench
    bench/ter_baseline_bench.cpp
)
target_link_libraries(ter_baseline_bench PRIVATE ter_t0)
ter_enable_strict_warnings(ter_baseline_bench)
ter_enable_release_ipo(ter_baseline_bench)

add_executable(
    ter_root_pairspace_probe
    bench/ter_root_pairspace_probe.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_root_pairspace_probe PRIVATE ter_t0)
target_include_directories(
    ter_root_pairspace_probe
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_root_pairspace_probe)
ter_enable_release_ipo(ter_root_pairspace_probe)

# ---------------------------------------------------------------------------
# TER^2 MicroReport8 research spike (ter_microreport8_probe).
#
# Isolated feasibility experiment for a packed 8x8 TER^2 leaf reporter. It links
# ter_t0 read-only through the existing FullTerRootCapture seam and the existing
# root-capture adapter; it changes no production source and no reporter
# semantics.
#
# Only bench/ter_microreport8_kernels_avx2.cpp is compiled for AVX2. The probe,
# the dataset builder, the scalar oracle, the portable fallbacks and the runtime
# dispatch stay in ordinary portable compilation, so the rest of the executable
# cannot silently acquire AVX2 instruction selection. Dispatch into the AVX2
# translation unit additionally requires OSXSAVE, AVX, XCR0[2:1] == 11b and
# AVX2 at run time, not merely the AVX2 CPUID feature bit.
# ---------------------------------------------------------------------------
function(ter_enable_avx2_translation_unit source)
    if(MSVC)
        set_source_files_properties(
            ${source}
            PROPERTIES COMPILE_OPTIONS "/arch:AVX2"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(
            ${source}
            PROPERTIES COMPILE_OPTIONS "-mavx2"
        )
    endif()
endfunction()

add_executable(
    ter_microreport8_probe
    bench/ter_microreport8_probe.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_microreport8_probe PRIVATE ter_t0)
target_include_directories(
    ter_microreport8_probe
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
target_compile_definitions(
    ter_microreport8_probe
    PRIVATE
    TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
    TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
)
ter_enable_strict_warnings(ter_microreport8_probe)

add_executable(
    ter_microreport8_tests
    tests/microreport8_tests.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_microreport8_tests PRIVATE ter_t0)
target_include_directories(
    ter_microreport8_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_microreport8_tests)
add_test(NAME ter_microreport8_tests COMMAND ter_microreport8_tests)

# Confine AVX2 instruction selection to the one translation unit that needs it.
ter_enable_avx2_translation_unit(bench/ter_microreport8_kernels_avx2.cpp)

# Interprocedural optimization is deliberately NOT enabled on these two targets.
# Link-time code generation may inline a function compiled for AVX2 into a
# caller compiled portably, which would place AVX2 instructions outside the
# gated translation unit. Both the packed kernels and the scalar baseline are
# therefore built without it, so the comparison stays internally consistent.

add_executable(
    ter_root_partition_bench
    bench/ter_root_partition_bench.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_root_partition_bench PRIVATE ter_t0)
ter_enable_strict_warnings(ter_root_partition_bench)
ter_enable_release_ipo(ter_root_partition_bench)

add_executable(
    ter_full_bench
    bench/ter_full_bench.cpp
)
target_link_libraries(ter_full_bench PRIVATE ter_t0)
target_compile_definitions(
    ter_full_bench
    PRIVATE
    TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
    TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
)
ter_enable_strict_warnings(ter_full_bench)
ter_enable_release_ipo(ter_full_bench)

# ---------------------------------------------------------------------------
# ter_full_bench CLI regressions (repair items P0/P1/P5/P6/P7/P10/P29).
#
# These assert semantics, never a particular randomized solver outcome.  The
# attempt-budget relationship itself is checked inside the benchmark, so a test
# that merely runs successfully already proves 1 <= executed <= attempt_limit.
# ---------------------------------------------------------------------------
function(ter_add_bench_cli_test name)
    cmake_parse_arguments(ARG "WILL_FAIL" "MATCH" "ARGS" ${ARGN})
    add_test(NAME ${name} COMMAND ter_full_bench ${ARG_ARGS})
    if(ARG_WILL_FAIL)
        set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    endif()
    if(ARG_MATCH)
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARG_MATCH}")
    endif()
endfunction()

# Attempt-limit parsing: reject zero, malformed, and overflowing values.
ter_add_bench_cli_test(ter_bench_cli_attempt_limit_zero
    ARGS --attempt-limit=0 WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_attempt_limit_malformed
    ARGS --attempt-limit=12x WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_attempt_limit_overflow
    ARGS --attempt-limit=99999999999999999999 WILL_FAIL)

# attempt-limit=1 is the historical one-attempt behavior: exactly one attempt.
ter_add_bench_cli_test(ter_bench_cli_attempt_limit_one
    ARGS --n=36 --attempt-limit=1 --warmup=0 --repeat=1
    MATCH "attempt_limit=1 attempts_executed=1")
# attempt-limit>1 permits retries; the in-binary invariant proves the bound.
ter_add_bench_cli_test(ter_bench_cli_attempt_limit_many
    ARGS --n=36 --attempt-limit=4 --warmup=0 --repeat=1
    MATCH "attempt_limit=4")

# require-found must separate Found from AttemptLimitReached, never fuse them.
ter_add_bench_cli_test(ter_bench_cli_require_found_rejects_exhausted
    ARGS --n=36 --attempt-limit=1 --warmup=0 --repeat=1 --require-found WILL_FAIL)

# A cancelled run reports Cancelled, never AttemptLimitReached (N0).
# PASS_REGULAR_EXPRESSION intentionally decides this test: the requirement is
# the reported classification, and the nonzero exit is incidental.
ter_add_bench_cli_test(ter_bench_cli_timeout_is_cancelled
    ARGS --n=72 --time-limit-ms=1 --warmup=0 --repeat=1
    MATCH "did not reach a comparable terminal status: Cancelled")

# A one-byte cap must fail closed as a resource refusal.
ter_add_bench_cli_test(ter_bench_cli_tiny_memory_cap
    ARGS --n=36 --memory-limit-bytes=4096 --warmup=0 --repeat=1 WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_memory_limit_zero
    ARGS --memory-limit-mib=0 WILL_FAIL)

# Preflight must mirror solve_full_ter()'s own shape derivation.
ter_add_bench_cli_test(ter_bench_cli_preflight_n72
    ARGS --n=72 --preflight
    MATCH "r1=25 r2=7 root_Q=47")
ter_add_bench_cli_test(ter_bench_cli_preflight_n72_bounded
    ARGS --n=72 --preflight
    MATCH "pair_materializing_fallback=no")
ter_add_bench_cli_test(ter_bench_cli_preflight_n108
    ARGS --n=108 --preflight
    MATCH "r1=39 r2=12 root_Q=69")
# Before repair item P12 this dimension reported "UNBOUNDED pair materialization
# expected"; the bounded streaming root now covers Q=69, and the quotient
# congruence is explicitly downgraded to a prefilter with the arbitrary-width
# residue words as the deciding authority.
ter_add_bench_cli_test(ter_bench_cli_preflight_n108_bounded
    ARGS --n=108 --preflight
    MATCH "bounded exact path on both merge stages")
ter_add_bench_cli_test(ter_bench_cli_preflight_n108_wide_authority
    ARGS --n=108 --preflight
    MATCH "arbitrary-width .*residue authority")
# n=144 has r1-r2=37 > 32 and root Q=91 > 64, so before repair items P12/P13 it
# fell back to a pair-materializing merge at BOTH stages. It must now report a
# bounded streaming path on both, with each wide quotient explicitly demoted to
# a prefilter backed by the arbitrary-width residue authority.
ter_add_bench_cli_test(ter_bench_cli_preflight_n144_bounded
    ARGS --n=144 --preflight
    MATCH "bounded exact path on both merge stages")
ter_add_bench_cli_test(ter_bench_cli_preflight_n144_wide_l2_l1
    ARGS --n=144 --preflight
    MATCH "l2_to_l1_decision=low-32 quotient prefilter")
ter_add_bench_cli_test(ter_bench_cli_preflight_n144_wide_root
    ARGS --n=144 --preflight
    MATCH "root_decision=low-64 quotient prefilter")
# The finite shape domain is exactly 36 | n; see docs/ter_dimension_domain.md.
ter_add_bench_cli_test(ter_bench_cli_preflight_n100_unsupported
    ARGS --n=100 --preflight
    MATCH "dimension is not a constructible exact TER shape")

# Family and mode selection.
ter_add_bench_cli_test(ter_bench_cli_family_invalid
    ARGS --family=nonsense WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_mode_invalid
    ARGS --mode=nonsense WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_unknown_option
    ARGS --not-an-option WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_family_finite_sanity
    ARGS --n=36 --family=finite-sanity --warmup=0 --repeat=1
    MATCH "family=FiniteSanity")

# warmup/repeat controls: no hidden 1+5 giant executions.
ter_add_bench_cli_test(ter_bench_cli_repeat_zero_rejected
    ARGS --repeat=0 WILL_FAIL)
ter_add_bench_cli_test(ter_bench_cli_warmup_zero_repeat_one
    ARGS --n=36 --warmup=0 --repeat=1
    MATCH "warmup=0 repeat=1")

# Provenance metadata must identify the exact binary (P29).
ter_add_bench_cli_test(ter_bench_cli_provenance_metadata
    ARGS --n=36 --warmup=0 --repeat=1
    MATCH "exe_sha256=[0-9a-f][0-9a-f]+ ")

# Success curve is an independent-seed campaign, not a deterministic replay.
ter_add_bench_cli_test(ter_bench_cli_success_curve
    ARGS --n=36 --mode=success-curve --seeds=3 --attempt-limit=2
    MATCH "independent_runs,found,attempt_limit_reached,censored")
ter_add_bench_cli_test(ter_bench_cli_success_curve_rejects_repeat
    ARGS --mode=success-curve --repeat=3 WILL_FAIL)

add_executable(
    ter_stress_bench
    bench/ter_stress_bench.cpp
)
target_link_libraries(ter_stress_bench PRIVATE ter_t0)
ter_enable_strict_warnings(ter_stress_bench)
ter_enable_release_ipo(ter_stress_bench)

add_executable(
    ter_q3report_bench
    bench/ter_q3report_bench.cpp
)
target_link_libraries(ter_q3report_bench PRIVATE ter_t0)
ter_enable_strict_warnings(ter_q3report_bench)
ter_enable_release_ipo(ter_q3report_bench)

add_executable(
    ter_q3report_crossover_bench
    bench/ter_q3report_crossover_bench.cpp
)
target_link_libraries(ter_q3report_crossover_bench PRIVATE ter_t0)
ter_enable_strict_warnings(ter_q3report_crossover_bench)
ter_enable_release_ipo(ter_q3report_crossover_bench)

add_executable(
    ter_q3report_paper_crossover_bench
    bench/ter_q3report_paper_crossover_bench.cpp
)
target_link_libraries(
    ter_q3report_paper_crossover_bench
    PRIVATE
    ter_t0
)
ter_enable_strict_warnings(ter_q3report_paper_crossover_bench)
ter_enable_release_ipo(ter_q3report_paper_crossover_bench)

add_executable(
    ter_tiled_parameter_sweep
    bench/ter_tiled_parameter_sweep.cpp
)
target_link_libraries(ter_tiled_parameter_sweep PRIVATE ter_t0)
ter_enable_strict_warnings(ter_tiled_parameter_sweep)
ter_enable_release_ipo(ter_tiled_parameter_sweep)

# Complete exact merge comparison.  Its baseline is the pair-materializing
# public merge, which is NOT the Q3REPORT DIRECT_PRODUCTION comparator; see the
# labelling note at the top of the source.
add_executable(
    ter_tiled_complete_merge_bench
    bench/ter_tiled_complete_merge_bench.cpp
)
target_link_libraries(ter_tiled_complete_merge_bench PRIVATE ter_t0)
ter_enable_strict_warnings(ter_tiled_complete_merge_bench)
ter_enable_release_ipo(ter_tiled_complete_merge_bench)

# ---------------------------------------------------------------------------
# ter_tiled_parameter_sweep CLI regressions.
#
# These assert that an explicit experiment override is honoured and that a
# malformed one is refused.  They never assert a timing, so no machine can fail
# them for being slow.  One (--w, --SB) selection is one row, which keeps the
# smoke cheap while still exercising the independent exact-count oracle.
# ---------------------------------------------------------------------------

# seed_base=8195 with w=16 must report seed 8211 and pass the independent
# exact count.  The trailing ",1" is independent_exactness_passed.
add_test(
    NAME ter_tiled_sweep_cli_seed_override
    COMMAND ter_tiled_parameter_sweep --w=16 --SB=15 --seed-base=8195 --repetitions=1
)
set_tests_properties(
    ter_tiled_sweep_cli_seed_override
    PROPERTIES
    PASS_REGULAR_EXPRESSION
    "TER_SHAPED_L2_MERGE_MICROBENCH,72,4096,8211,16,15,15,[0-9,]+,1"
)

# The default seed base stays 0x1001, so w=16 without an override is seed 4113.
add_test(
    NAME ter_tiled_sweep_cli_default_seed_base
    COMMAND ter_tiled_parameter_sweep --w=16 --SB=15 --repetitions=1
)
set_tests_properties(
    ter_tiled_sweep_cli_default_seed_base
    PROPERTIES
    PASS_REGULAR_EXPRESSION
    "TER_SHAPED_L2_MERGE_MICROBENCH,72,4096,4113,16,15,15,[0-9,]+,1"
)

add_test(
    NAME ter_tiled_sweep_cli_repetitions_zero_rejected
    COMMAND ter_tiled_parameter_sweep --repetitions=0
)
set_tests_properties(
    ter_tiled_sweep_cli_repetitions_zero_rejected PROPERTIES WILL_FAIL TRUE
)

add_test(
    NAME ter_tiled_sweep_cli_repetitions_malformed_rejected
    COMMAND ter_tiled_parameter_sweep --repetitions=3x
)
set_tests_properties(
    ter_tiled_sweep_cli_repetitions_malformed_rejected PROPERTIES WILL_FAIL TRUE
)

add_test(
    NAME ter_tiled_sweep_cli_seed_base_malformed_rejected
    COMMAND ter_tiled_parameter_sweep --seed-base=zzz
)
set_tests_properties(
    ter_tiled_sweep_cli_seed_base_malformed_rejected PROPERTIES WILL_FAIL TRUE
)

# ---------------------------------------------------------------------------
# TER^2 residual windowing research spike (ter_windowing_probe).
#
# Isolated geometry experiment asking whether residual-window locality can
# remove the cost/selectivity tradeoff behind the MicroReport8 TOO_SLOW result.
# It reuses bench/ter_microreport8_dataset.cpp unmodified as the authentic
# n=72 TER^2 dataset donor and changes no production source, no production
# reporter semantics and no MicroReport8 result.
#
# No AVX2 translation unit is linked here: the windowing spike is a geometry
# and modelling experiment, so every translation unit stays in ordinary
# portable compilation. bench/ter_microreport8_kernels_avx2.cpp is still linked
# because the donor dataset header declares the kernel entry points and the
# truth oracle is shared; its AVX2 confinement is unchanged.
# ---------------------------------------------------------------------------
add_executable(
    ter_windowing_probe
    bench/ter_windowing_probe.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_windowing_probe PRIVATE ter_t0)
target_include_directories(
    ter_windowing_probe
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
target_compile_definitions(
    ter_windowing_probe
    PRIVATE
    TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
    TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
)
ter_enable_strict_warnings(ter_windowing_probe)

add_executable(
    ter_windowing_tests
    tests/windowing_tests.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_windowing_tests PRIVATE ter_t0)
target_include_directories(
    ter_windowing_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_windowing_tests)
add_test(NAME ter_windowing_tests COMMAND ter_windowing_tests)

# Interprocedural optimization is deliberately NOT enabled on either target,
# for the same reason as the MicroReport8 spike: link-time code generation may
# inline a function compiled for AVX2 into a portably compiled caller, placing
# AVX2 instructions outside the gated translation unit.

# ---------------------------------------------------------------------------
# TER^2 translated-tag locator research spike (ter_translated_tag_bench).
#
# Isolated matched-scope benchmark asking whether an exact translated 11-bit
# position + 8-bit tag set-alignment locator beats the frozen K2 coarse-gather
# frontend on the authentic n=72 TER^2 root.  It reuses
# bench/ter_microreport8_dataset.cpp and bench/ter_microreport8_kernels_avx2.cpp
# unmodified as the authentic dataset donor and the frozen comparator, and
# changes no production source, no production reporter semantics, and no
# previous spike result.
#
# Only bench/ter_translated_tag_locator_avx2.cpp is compiled for AVX2.  The
# builders, the portable router, the scalar reference, the benchmark driver and
# the dispatch stay in ordinary portable compilation, so the rest of the
# executable cannot silently acquire AVX2 instruction selection.
# ---------------------------------------------------------------------------
add_executable(
    ter_translated_tag_bench
    bench/ter_translated_tag_bench.cpp
    bench/ter_translated_tag_locator.cpp
    bench/ter_translated_tag_locator_avx2.cpp
    bench/ter_radix13_locator.cpp
    bench/ter_radix13_locator_avx2.cpp
    bench/ter_radix13_packed.cpp
    bench/ter_radix13_packed_bmi2.cpp
    bench/ter_translated_tag_dataset.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_translated_tag_bench PRIVATE ter_t0)
if(WIN32)
    target_link_libraries(ter_translated_tag_bench PRIVATE psapi)
endif()
target_include_directories(
    ter_translated_tag_bench
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
target_compile_definitions(
    ter_translated_tag_bench
    PRIVATE
    TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
    TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
)
ter_enable_strict_warnings(ter_translated_tag_bench)

add_executable(
    ter_translated_tag_tests
    tests/translated_tag_locator_tests.cpp
    bench/ter_translated_tag_locator.cpp
    bench/ter_translated_tag_locator_avx2.cpp
    bench/ter_translated_tag_dataset.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_translated_tag_tests PRIVATE ter_t0)
target_include_directories(
    ter_translated_tag_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_translated_tag_tests)
add_test(NAME ter_translated_tag_tests COMMAND ter_translated_tag_tests)

# ---------------------------------------------------------------------------
# TER^2 radix-13 occupancy locator research spike (ter_radix13_tests).
#
# The radix-13 locator is measured as an additional comparator path inside
# ter_translated_tag_bench, under that harness's identical matched scope, shared
# index, exhaustive truth and downstream authority.  Its own test binary checks
# the pieces that the matched sweep cannot isolate: builder invariants, the
# circular shifted-view seam, carry seams, rotation extremes, dense tag buckets,
# and portable/AVX2 kernel agreement on adversarial synthetic classes.
# ---------------------------------------------------------------------------
add_executable(
    ter_radix13_tests
    tests/radix13_locator_tests.cpp
    bench/ter_radix13_locator.cpp
    bench/ter_radix13_locator_avx2.cpp
    bench/ter_translated_tag_locator.cpp
    bench/ter_translated_tag_locator_avx2.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_radix13_tests PRIVATE ter_t0)
target_include_directories(
    ter_radix13_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_radix13_tests)
add_test(NAME ter_radix13_tests COMMAND ter_radix13_tests)

# ---------------------------------------------------------------------------
# TER^2 Radix-13 occupancy guard + packed-tag mask consumer (ter_radix13_packed_tests).
#
# Measured inside ter_translated_tag_bench under the identical matched scope.
# This binary checks the pieces the matched sweep cannot isolate: exhaustive
# PEXT rank mapping, the expansion table, the packed 6-bit relation for every
# (a, b, alpha, carry), carry seams at every bit offset, deck capacities, and
# agreement of every packed route with value-derived truth and, in order, with
# the plain radix-13 dense kernel.
# ---------------------------------------------------------------------------
add_executable(
    ter_radix13_packed_tests
    tests/radix13_packed_tests.cpp
    bench/ter_radix13_packed.cpp
    bench/ter_radix13_packed_bmi2.cpp
    bench/ter_radix13_locator.cpp
    bench/ter_radix13_locator_avx2.cpp
    bench/ter_translated_tag_locator.cpp
    bench/ter_translated_tag_locator_avx2.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_radix13_packed_tests PRIVATE ter_t0)
target_include_directories(
    ter_radix13_packed_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_radix13_packed_tests)
add_test(NAME ter_radix13_packed_tests COMMAND ter_radix13_packed_tests)

# ---------------------------------------------------------------------------
# TER^2 root-queue generalization experiment.
#
# ter_root_queue_generalization_bench asks whether QUEUE_DENSE (branch-free
# guard-word queue + radix-13 exact dense leaf) beats the frozen K2 frontend
# across authentic Full TER roots (n = 36, 72, 108 where capturable under the
# frozen 4 GiB benchmark limit) and across controlled residual-density sweeps
# (SYNTHETIC_UNIFORM and RESIDUAL_PROFILE_EMULATION, never labelled authentic).
# Every backend is an unmodified donor kernel; the new files own only workload
# construction, the exact sink, the oracle, scheduling and reporting.
# ---------------------------------------------------------------------------
set(TER_ROOT_QUEUE_GENERALIZATION_SOURCES
    bench/ter_root_generalization_workload.cpp
    bench/ter_root_snapshot.cpp
    bench/ter_root_queue_kernels.cpp
    bench/ter_radix13_locator.cpp
    bench/ter_radix13_locator_avx2.cpp
    bench/ter_radix13_packed.cpp
    bench/ter_radix13_packed_bmi2.cpp
    bench/ter_translated_tag_dataset.cpp
    bench/ter_translated_tag_locator.cpp
    bench/ter_translated_tag_locator_avx2.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
add_executable(
    ter_root_queue_generalization_bench
    bench/ter_root_queue_generalization_bench.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
target_link_libraries(ter_root_queue_generalization_bench PRIVATE ter_t0)
target_include_directories(
    ter_root_queue_generalization_bench
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
target_compile_definitions(
    ter_root_queue_generalization_bench
    PRIVATE
    TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
    TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
)
ter_enable_strict_warnings(ter_root_queue_generalization_bench)

add_executable(
    ter_root_queue_generalization_tests
    tests/ter_root_queue_generalization_tests.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
target_link_libraries(ter_root_queue_generalization_tests PRIVATE ter_t0)
target_include_directories(
    ter_root_queue_generalization_tests
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_root_queue_generalization_tests)
add_test(NAME ter_root_queue_generalization_tests COMMAND ter_root_queue_generalization_tests)

# Phase A of the n=108 root experiment: group-staged, capture-only extraction
# of one attempt's three L1 root lists into a packed snapshot file, benchmarked
# later by a fresh ter_root_queue_generalization_bench process (--snapshot=).
add_executable(
    ter_root_snapshot_capture
    bench/ter_root_snapshot_capture.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
add_executable(
    ter_root_snapshot_tests
    tests/ter_root_snapshot_tests.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
foreach(target ter_root_snapshot_capture ter_root_snapshot_tests)
    target_link_libraries(${target} PRIVATE ter_t0)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
    target_compile_definitions(
        ${target}
        PRIVATE
        TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
        TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
    )
    ter_enable_strict_warnings(${target})
endforeach()
add_test(NAME ter_root_snapshot_tests COMMAND ter_root_snapshot_tests)

# A64-CHQR (TER_n108_A64_CHQR_Compiler_God_Spec): carry-aware hierarchical
# refinement of the full root quotient, replayed from packed root snapshots,
# timed against the frozen K2 frontend and the K2_WIDE control.
add_executable(
    ter_a64_chqr_bench
    bench/ter_a64_chqr_bench.cpp
    bench/ter_a64_chqr.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
add_executable(
    ter_a64_chqr_tests
    tests/ter_a64_chqr_tests.cpp
    bench/ter_a64_chqr.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
foreach(target ter_a64_chqr_bench ter_a64_chqr_tests)
    target_link_libraries(${target} PRIVATE ter_t0)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
    target_compile_definitions(
        ${target}
        PRIVATE
        TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
        TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
    )
    ter_enable_strict_warnings(${target})
endforeach()
add_test(NAME ter_a64_chqr_tests COMMAND ter_a64_chqr_tests)

# ARWLF: adaptive radix wide-leaf filter.  Terminal 13-bit leaves, a
# carry-exact 32-bit wide leaf filter (AVX2 translation unit only), and a
# probe-based dispatcher over QUEUE_DENSE / ARWLF / K2_WIDE.
set(TER_ARWLF_SOURCES
    bench/ter_arwlf.cpp
    bench/ter_arwlf_avx2.cpp
    bench/ter_a64_chqr.cpp
    ${TER_ROOT_QUEUE_GENERALIZATION_SOURCES}
)
add_executable(ter_arwlf_bench bench/ter_arwlf_bench.cpp ${TER_ARWLF_SOURCES})
add_executable(ter_arwlf_tests tests/ter_arwlf_tests.cpp ${TER_ARWLF_SOURCES})
foreach(target ter_arwlf_bench ter_arwlf_tests)
    target_link_libraries(${target} PRIVATE ter_t0)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
    target_compile_definitions(
        ${target}
        PRIVATE
        TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
        TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
    )
    ter_enable_strict_warnings(${target})
endforeach()
add_test(NAME ter_arwlf_tests COMMAND ter_arwlf_tests)
ter_enable_avx2_translation_unit(bench/ter_arwlf_avx2.cpp)

# ARWLF regime campaign: geometry census, stratified selection, deterministic
# thinning and matched timing over the unchanged backends.
add_executable(ter_arwlf_campaign bench/ter_arwlf_campaign_main.cpp bench/ter_arwlf_campaign.cpp
               ${TER_ARWLF_SOURCES})
add_executable(ter_arwlf_campaign_tests tests/ter_arwlf_campaign_tests.cpp bench/ter_arwlf_campaign.cpp
               ${TER_ARWLF_SOURCES})
foreach(target ter_arwlf_campaign ter_arwlf_campaign_tests)
    target_link_libraries(${target} PRIVATE ter_t0)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
    target_compile_definitions(
        ${target}
        PRIVATE
        TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
        TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
    )
    ter_enable_strict_warnings(${target})
endforeach()
add_test(NAME ter_arwlf_campaign_tests COMMAND ter_arwlf_campaign_tests)

# Frozen root backends (FROZEN_ROOT_BACKENDS.md): fails if any of their source
# files differs from the recorded SHA-256.
add_test(NAME ter_frozen_root_backends
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_frozen_root_backends.cmake)
if(WIN32)
    foreach(target ter_root_snapshot_capture ter_root_snapshot_tests
                   ter_a64_chqr_bench ter_a64_chqr_tests ter_arwlf_bench ter_arwlf_tests
                   ter_arwlf_campaign ter_arwlf_campaign_tests
                   ter_root_queue_generalization_bench ter_root_queue_generalization_tests)
        target_link_libraries(${target} PRIVATE psapi)
    endforeach()
endif()


# Confine AVX2 instruction selection to the one translation unit that needs it.
ter_enable_avx2_translation_unit(bench/ter_translated_tag_locator_avx2.cpp)
ter_enable_avx2_translation_unit(bench/ter_radix13_locator_avx2.cpp)
# The packed consumer's hardware route needs BMI2 (PEXT/PDEP) as well.  MSVC's
# /arch:AVX2 already targets BMI2; GCC/Clang need it named.
if(MSVC)
    ter_enable_avx2_translation_unit(bench/ter_radix13_packed_bmi2.cpp)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        bench/ter_radix13_packed_bmi2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2;-mbmi2"
    )
endif()

# Interprocedural optimization is deliberately NOT enabled on either target, for
# the same reason as the MicroReport8 and windowing spikes: link-time code
# generation may inline a function compiled for AVX2 into a portably compiled
# caller, placing AVX2 instructions outside the gated translation unit.

# ---------------------------------------------------------------------------
# TER^2 convolution/source-signature inference research spike.
#
# ter_convsig_dataset_export builds the authentic n=72 TER^2 residual families
# through the unmodified MicroReport8 donor, re-checks every authenticated
# fixture invariant, and exports them so the standalone GPU transform benchmark
# measures the authentic job count on authentic data.  No production source and
# no previous spike result is modified.  No AVX2 translation unit is compiled
# for AVX2 here beyond the donor's existing confinement.
# ---------------------------------------------------------------------------
add_executable(
    ter_convsig_dataset_export
    bench/ter_convsig_dataset_export.cpp
    bench/ter_microreport8_dataset.cpp
    bench/ter_microreport8_kernels_avx2.cpp
    bench/ter_root_capture_adapter.cpp
)
target_link_libraries(ter_convsig_dataset_export PRIVATE ter_t0)
target_include_directories(
    ter_convsig_dataset_export
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bench"
)
ter_enable_strict_warnings(ter_convsig_dataset_export)
