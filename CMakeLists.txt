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
