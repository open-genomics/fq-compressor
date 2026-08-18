# =============================================================================
# fq-compressor - High Performance FASTQ Compressor
# =============================================================================
# CMake build configuration for fq-compressor project
# 
# Requirements: CMake 3.28+, C++23 compiler (GCC 14+ or Clang 18+)
# Dependencies: CLI11, fmt, zlib, zstd, xxHash, GTest (via Conan 2.x)
#
# Build presets are defined in CMakePresets.json for consistent builds.
# =============================================================================

cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FQC_VERSION)
string(STRIP "${FQC_VERSION}" FQC_VERSION)
if(NOT FQC_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must contain a valid semantic version: '${FQC_VERSION}'")
endif()
set(FQC_VERSION_CORE "${CMAKE_MATCH_1}")

project(FQCompressor
    LANGUAGES CXX
    VERSION ${FQC_VERSION_CORE}
    DESCRIPTION "High-performance FASTQ compressor"
    HOMEPAGE_URL "https://github.com/open-genomics/fq-compressor"
)

# =============================================================================
# Project Configuration
# =============================================================================

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS 
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# =============================================================================
# C++ Standard Configuration
# =============================================================================

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# Compiler Version Check
# =============================================================================
# Enforce minimum compiler versions required for C++23 support.

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "14.0")
        message(FATAL_ERROR 
            "GCC version ${CMAKE_CXX_COMPILER_VERSION} is too old. "
            "fq-compressor requires GCC 14.0 or newer for C++23 support.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
        message(FATAL_ERROR 
            "Clang version ${CMAKE_CXX_COMPILER_VERSION} is too old. "
            "fq-compressor requires Clang 18.0 or newer for C++23 support.")
    endif()
else()
    message(WARNING 
        "Compiler ${CMAKE_CXX_COMPILER_ID} is not officially supported. "
        "Your build may fail due to lack of C++23 support.")
endif()

# =============================================================================
# Compiler Warning Flags
# =============================================================================

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wcast-qual
        -Wformat=2
        -Wundef
        -Werror=return-type
        -Wno-unused-parameter  # Allow unused parameters in interface implementations
    )
    
    # Additional warnings for GCC
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wnull-dereference
        )
    endif()
    
    # Additional warnings for Clang
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(
            -Wshadow-all
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
        )
    endif()
    
    # Portable build option: disable -march=native for distributable binaries
    option(FQC_PORTABLE "Build portable binaries without -march=native" OFF)

    if(NOT FQC_PORTABLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-march=native)
    endif()
endif()

# =============================================================================
# Conan Integration
# =============================================================================
# Dependencies are managed via Conan 2.x. Run `conan install . --build=missing`
# before configuring CMake. The conan_toolchain.cmake will be generated in the
# build directory.

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(EXISTS "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
        include("${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
    else()
        message(STATUS 
            "No Conan toolchain detected. "
            "Run 'conan install . --build=missing' to install dependencies.")
    endif()
endif()

# =============================================================================
# libc++ Experimental Stop-Token / jthread Enable
# =============================================================================
# libc++ < 19 still gates std::jthread and std::stop_token behind an
# "experimental" guard: <__config> defines _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN
# unless _LIBCPP_ENABLE_EXPERIMENTAL is set, and clang-18 (Ubuntu 24.04's
# packaged toolchain) does not advertise __has_feature(experimental_library).
# The guard then strips std::jthread/std::stop_token even under -std=c++23,
# breaking the pipeline's cooperative-cancellation code. Define the enable
# macro explicitly when building against libc++. No-op on libstdc++ (no such
# macro) and on libc++ >= 19 (which auto-defines _LIBCPP_ENABLE_EXPERIMENTAL).
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([[
#include <version>
#ifndef _LIBCPP_VERSION
#error not libc++
#endif
int main() { return 0; }
]] FQC_USING_LIBCXX)
if(FQC_USING_LIBCXX)
    add_compile_definitions(_LIBCPP_ENABLE_EXPERIMENTAL)
endif()

# =============================================================================
# Find Dependencies (handled by Conan)
# =============================================================================

# CLI parsing library
find_package(CLI11 CONFIG REQUIRED)
message(STATUS "Found CLI11: ${CLI11_VERSION}")

# Formatting library
find_package(fmt CONFIG REQUIRED)
message(STATUS "Found fmt: ${fmt_VERSION}")

# Compression libraries
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)

# Checksum library
find_package(xxHash CONFIG REQUIRED)
message(STATUS "Found xxHash: ${xxHash_VERSION}")

# Handle fmt header-only vs compiled library
if(TARGET fmt::fmt-header-only AND NOT TARGET fmt::fmt)
    add_library(fmt::fmt ALIAS fmt::fmt-header-only)
endif()

# =============================================================================
# Project Include Directories
# =============================================================================

# Define the public include directory for the project
set(FQC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# =============================================================================
# Add Subdirectories
# =============================================================================

# Source files (libraries and main executable)
add_subdirectory(src)

# =============================================================================
# Testing Configuration
# =============================================================================

include(CTest)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "=== fq-compressor Configuration Summary ===")
message(STATUS "  Version:        ${FQC_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build testing:  ${BUILD_TESTING}")
message(STATUS "============================================")
message(STATUS "")
