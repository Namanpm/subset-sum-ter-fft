# TER^2 L1 -> L0 root backends: K2, K2_WIDE, QUEUE_DENSE, ARWLF (+ dispatcher),
# the ARWLF regime campaign, and their tests.  See README.md.
#
#   frozen/     the 29 SHA-256-pinned backend sources (frozen_root_backends.sha256);
#               never edited, only rebuilt.
#   campaign/   benchmark harnesses, snapshot capture, host helpers.
#   tests/      differential tests of the backends and of the campaign library.
#   scripts/    campaign driver, plan, summarizer, freeze checker.
#   retired/    closed experiments, not built (see retired/README.md).
#
# Executables land next to the top-level ones (<build>/<config>/), so paths such
# as build-full/Release/ter_arwlf_campaign.exe do not depend on this layout.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Confines AVX2 instruction selection to one translation unit (directory-scoped,
# so it is applied here, where every user of these sources is defined).
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

ter_enable_avx2_translation_unit(frozen/ter_microreport8_kernels_avx2.cpp)
ter_enable_avx2_translation_unit(frozen/ter_translated_tag_locator_avx2.cpp)
ter_enable_avx2_translation_unit(frozen/ter_radix13_locator_avx2.cpp)
ter_enable_avx2_translation_unit(frozen/ter_arwlf_avx2.cpp)
# The packed consumer's hardware route needs BMI2 (PEXT/PDEP) as well.  MSVC's
# /arch:AVX2 already targets BMI2; GCC/Clang need it named.
if(MSVC)
    ter_enable_avx2_translation_unit(frozen/ter_radix13_packed_bmi2.cpp)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        frozen/ter_radix13_packed_bmi2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2;-mbmi2"
    )
endif()

# The frozen backend sources, compiled once.  Interprocedural optimization stays
# off for everything here: link-time code generation could inline a function
# compiled for AVX2 into a portably compiled caller.
add_library(
    ter2_frozen
    STATIC
    frozen/ter_root_generalization_workload.cpp
    frozen/ter_root_snapshot.cpp
    frozen/ter_root_queue_kernels.cpp
    frozen/ter_root_capture_adapter.cpp
    frozen/ter_radix13_locator.cpp
    frozen/ter_radix13_locator_avx2.cpp
    frozen/ter_radix13_packed.cpp
    frozen/ter_radix13_packed_bmi2.cpp
    frozen/ter_translated_tag_dataset.cpp
    frozen/ter_translated_tag_locator.cpp
    frozen/ter_translated_tag_locator_avx2.cpp
    frozen/ter_microreport8_dataset.cpp
    frozen/ter_microreport8_kernels_avx2.cpp
    frozen/ter_a64_chqr.cpp
    frozen/ter_arwlf.cpp
    frozen/ter_arwlf_avx2.cpp
)
target_link_libraries(ter2_frozen PUBLIC ter_t0)
target_include_directories(ter2_frozen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/frozen")
ter_enable_strict_warnings(ter2_frozen)

# One executable per entry; every target links the frozen library.
function(ter2_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE ter2_frozen)
    target_include_directories(
        ${target}
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/campaign"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_compile_definitions(
        ${target}
        PRIVATE
        TER_CONFIGURE_TIME_COMMIT="${TER_CONFIGURE_TIME_COMMIT}"
        TER_CONFIGURE_TIME_DIRTY=${TER_CONFIGURE_TIME_DIRTY}
    )
    ter_enable_strict_warnings(${target})
    if(WIN32)
        target_link_libraries(${target} PRIVATE psapi)
    endif()
endfunction()

# --- campaign and harnesses ---------------------------------------------------
ter2_executable(ter_arwlf_campaign campaign/ter_arwlf_campaign_main.cpp campaign/ter_arwlf_campaign.cpp)
ter2_executable(ter_arwlf_bench campaign/ter_arwlf_bench.cpp)
ter2_executable(ter_root_snapshot_capture campaign/ter_root_snapshot_capture.cpp)

# --- tests ------------------------------------------------------------------------
function(ter2_test target)
    ter2_executable(${target} ${ARGN})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

ter2_test(ter_arwlf_tests tests/ter_arwlf_tests.cpp)
ter2_test(ter_arwlf_campaign_tests tests/ter_arwlf_campaign_tests.cpp campaign/ter_arwlf_campaign.cpp)
ter2_test(ter_a64_chqr_tests tests/ter_a64_chqr_tests.cpp)
ter2_test(ter_root_queue_generalization_tests tests/ter_root_queue_generalization_tests.cpp)
ter2_test(ter_root_snapshot_tests tests/ter_root_snapshot_tests.cpp)
ter2_test(ter_microreport8_tests tests/microreport8_tests.cpp)
ter2_test(ter_translated_tag_tests tests/translated_tag_locator_tests.cpp)
ter2_test(ter_radix13_tests tests/radix13_locator_tests.cpp)
ter2_test(ter_radix13_packed_tests tests/radix13_packed_tests.cpp)

# Fails if any frozen source differs from its recorded SHA-256 (FROZEN_ROOT_BACKENDS.md).
add_test(NAME ter_frozen_root_backends
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_frozen_root_backends.cmake)
