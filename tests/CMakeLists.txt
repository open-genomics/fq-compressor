# =============================================================================
# tests/CMakeLists.txt
# =============================================================================
# Test directory CMake configuration for fq-compressor
#
# This file configures the test targets using Google Test and optionally
# RapidCheck for property-based testing.
# =============================================================================

# =============================================================================
# Test Configuration
# =============================================================================

# Common test include directories
set(FQC_TEST_INCLUDE_DIRS
    ${FQC_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# =============================================================================
# Helper Function: Add Test Executable
# =============================================================================

function(fqc_add_test test_name)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs SOURCES LIBRARIES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${test_name} ${ARG_SOURCES})

    target_include_directories(${test_name}
        PRIVATE
            ${FQC_TEST_INCLUDE_DIRS}
    )

    target_link_libraries(${test_name}
        PRIVATE
            fqc_core
            GTest::gtest
            GTest::gtest_main
            ${ARG_LIBRARIES}
    )

    # Register with CTest
    add_test(NAME ${test_name} COMMAND ${test_name})

    # Set test properties
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 300
        LABELS "unit"
    )
endfunction()

# =============================================================================
# Unit Tests
# =============================================================================

# Error Handling Tests
fqc_add_test(error_test
    SOURCES common/error_test.cpp
)

# FQC v2 sequential archive tests
fqc_add_test(archive_test
    SOURCES format/archive_test.cpp
)

# fqc-sequential/v2 format contract tests
fqc_add_test(format_contract_test
    SOURCES format/format_contract_test.cpp
)

# Frozen fixture decoder compatibility tests
fqc_add_test(frozen_fixture_test
    SOURCES format/frozen_fixture_test.cpp
)
target_compile_definitions(frozen_fixture_test PRIVATE
    FQC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sequential-v2"
)

# Cross-family magic recognition (indexed reject)
fqc_add_test(format_family_test
    SOURCES format/format_family_test.cpp
    LIBRARIES fqc_cli
)
target_compile_definitions(format_family_test PRIVATE
    FQC_FOREIGN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/foreign-indexed-v2"
    FQC_SEQUENTIAL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sequential-v2"
)

# FASTQ parser tests
fqc_add_test(fastq_parser_test
    SOURCES io/fastq_parser_test.cpp
)

# Compressed Stream Tests
fqc_add_test(compressed_stream_test
    SOURCES io/compressed_stream_test.cpp
)

# V2 end-to-end archive engine tests
fqc_add_test(archive_engine_test
    SOURCES commands/archive_engine_test.cpp
    LIBRARIES fqc_cli
)

# MPMC queue unit tests
fqc_add_test(mpmc_queue_test
    SOURCES pipeline/mpmc_queue_test.cpp
)

# Reorder buffer unit tests
fqc_add_test(reorder_buffer_test
    SOURCES pipeline/reorder_buffer_test.cpp
)

# Compression pipeline integration tests
fqc_add_test(pipeline_test
    SOURCES pipeline/pipeline_test.cpp
)

# Decompression pipeline integration tests
fqc_add_test(decompress_pipeline_test
    SOURCES pipeline/decompress_pipeline_test.cpp
)

# Frame accumulator unit tests
fqc_add_test(frame_accumulator_test
    SOURCES pipeline/frame_accumulator_test.cpp
)

# Chunk orderer unit tests
fqc_add_test(chunk_orderer_test
    SOURCES pipeline/chunk_orderer_test.cpp
)

# Parallel parse pipeline integration tests
fqc_add_test(parallel_parse_pipeline_test
    SOURCES pipeline/parallel_parse_pipeline_test.cpp
)

add_test(
    NAME v2_cli_smoke_test
    COMMAND ${CMAKE_COMMAND} -E env
        FQC_BIN=$<TARGET_FILE:fqc>
        bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_smoke_test.sh
)
set_tests_properties(v2_cli_smoke_test PROPERTIES
    TIMEOUT 60
    LABELS "e2e"
)

# =============================================================================
# Test Summary
# =============================================================================

message(STATUS "")
message(STATUS "=== Test Configuration ===")
message(STATUS "  Unit tests:     ON")
message(STATUS "==========================")
message(STATUS "")
