cmake_minimum_required(VERSION 3.24)

project(ter_t0_baseline VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)
include(CTest)

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
ter_enable_strict_warnings(ter_full_bench)
ter_enable_release_ipo(ter_full_bench)

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
