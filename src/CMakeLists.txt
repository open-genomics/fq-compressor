# =============================================================================
# src/CMakeLists.txt
# =============================================================================
# Source directory CMake configuration for fq-compressor
#
# This file defines the library targets and the main executable.
# =============================================================================

# =============================================================================
# Core Library (fqc_core)
# =============================================================================
# Contains the core compression/decompression logic, format handling, and
# algorithm implementations.

# Collect source files for the core library
set(FQC_CORE_SOURCES
    format/archive.cpp
    io/fastq_parser.cpp
    io/compressed_stream.cpp
    pipeline/compress_pipeline.cpp
    pipeline/decompress_pipeline.cpp
    pipeline/parallel_parse_pipeline.cpp
)

# Command sources
set(FQC_COMMAND_SOURCES
    commands/archive_engine.cpp
)

# Create the core library
add_library(fqc_core STATIC ${FQC_CORE_SOURCES})

target_include_directories(fqc_core
    PUBLIC
        ${FQC_INCLUDE_DIR}
)

target_compile_features(fqc_core PUBLIC cxx_std_23)

# Link dependencies
target_link_libraries(fqc_core
    PUBLIC
        fmt::fmt
        ZLIB::ZLIB
        $<TARGET_NAME_IF_EXISTS:zstd::libzstd_shared>
        $<TARGET_NAME_IF_EXISTS:zstd::libzstd_static>
        xxHash::xxhash
)

# =============================================================================
# CLI Library (fqc_cli)
# =============================================================================
# Contains command-line interface logic and command implementations.

add_library(fqc_cli STATIC ${FQC_COMMAND_SOURCES})

target_include_directories(fqc_cli
    PUBLIC
        ${FQC_INCLUDE_DIR}
)

target_link_libraries(fqc_cli
    PUBLIC
        fqc_core
        CLI11::CLI11
)

# =============================================================================
# Main Executable (fqc)
# =============================================================================

add_executable(fqc main.cpp)

target_link_libraries(fqc
    PRIVATE
        fqc_cli
)

target_compile_definitions(fqc
    PRIVATE
        FQC_VERSION="${FQC_VERSION}"
)

target_include_directories(fqc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FQC_INCLUDE_DIR}
)

# =============================================================================
# Installation
# =============================================================================

# The installed product surface is the CLI executable only. The static targets above are internal
# build and test boundaries and are not exported as a C++ package.
install(TARGETS fqc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
)
