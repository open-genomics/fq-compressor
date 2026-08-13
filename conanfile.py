# conanfile.py
# =============================================================================
# fq-compressor - Conan 2.x Package Configuration
# =============================================================================
# Dependency management for fq-compressor project.
#
# Usage:
#   conan install . --build=missing --output-folder=build
#   cmake --preset conan-release  # or use CMakePresets.json
#
# Dependencies are organized by purpose:
#   - CLI parsing: CLI11
#   - Formatting / Logging: fmt
#   - Compression: zlib-ng, zstd
#   - Checksums: xxHash
#   - Testing: GTest
# =============================================================================

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import load

import os


class FQCompressorConan(ConanFile):
    name = "fqcompressor"
    package_type = "application"
    license = "MIT"  # Project-authored code; vendored third-party code keeps its own license
    author = "open-genomics <jiashuai.mail@gmail.com>"
    url = "https://github.com/open-genomics/fq-compressor"
    description = "High-performance sequential FASTQ compressor"
    topics = ("bioinformatics", "fastq", "compression", "genomics", "archival")
    settings = "os", "compiler", "build_type", "arch"
    default_options = {
        "zlib-ng/*:zlib_compat": True,
    }
    exports = "VERSION"
    exports_sources = "VERSION", "CMakeLists.txt", "src/*", "include/*", "tests/*"

    def set_version(self):
        self.version = load(self, os.path.join(self.recipe_folder, "VERSION")).strip()

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        # =========================================================================
        # CLI Parsing (Requirement 6.1)
        # =========================================================================
        # CLI11: Modern, header-only command line parser
        self.requires("cli11/2.4.2")

        # =========================================================================
        # Logging / Formatting
        # =========================================================================
        self.requires("fmt/12.1.0")

        # =========================================================================
        # Compression Libraries (Requirement 1.1.1)
        # =========================================================================
        # zlib-ng: High-performance zlib replacement for gzip support
        self.requires("zlib-ng/2.3.2")
        # zstd: Fast compression for v2 frame streams
        self.requires("zstd/1.5.7")

        # =========================================================================
        # Checksums (Requirement 5.1, 5.2)
        # =========================================================================
        # xxHash: Extremely fast non-cryptographic hash algorithm
        self.requires("xxhash/0.8.3")

    def build_requirements(self):
        # =========================================================================
        # Testing Frameworks
        # =========================================================================
        # GTest: Google Test framework for unit testing
        self.test_requires("gtest/1.17.0")

    def generate(self):
        """Generate CMake toolchain and dependency files."""
        from conan.tools.cmake import CMakeDeps

        # Generate CMake toolchain file (conan_toolchain.cmake)
        tc = CMakeToolchain(self)
        # CMake policy floor for older transitive package configs
        tc.variables["CMAKE_POLICY_VERSION_MINIMUM"] = "3.5"
        tc.generate()

        # Generate CMake find_package config files for all dependencies
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        """Build the project using CMake."""
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        """Package the built artifacts."""
        cmake = CMake(self)
        cmake.install()
