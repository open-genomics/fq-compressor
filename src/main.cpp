// =============================================================================
// fq-compressor - FQC v2 Command-Line Interface
// =============================================================================

#include "fqc/commands/archive_engine.h"
#include "fqc/common/error.h"
#include "fqc/format/archive.h"
#include "fqc/log.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <CLI/CLI.hpp>

namespace {

#ifndef FQC_VERSION
#error "FQC_VERSION must be provided by the build system"
#endif

constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;

struct GlobalOptions {
    std::uint64_t memoryLimitMiB = std::uint64_t{16} * 1024;
    bool quiet = false;
};

struct CompressOptions {
    std::string input;
    std::string mate;
    std::string output;
    std::string profile = "auto";
    std::uint64_t frameMiB = 64;
    int qualityLevel = 1;
    std::uint64_t parseWorkers = 4;
    bool force = false;
};

struct DecompressOptions {
    std::string input;
    std::string output;
    bool force = false;
};

struct VerifyOptions {
    std::string input;
};

[[nodiscard]] auto checkedMiB(std::uint64_t value, std::string_view option)
    -> fqc::Result<std::size_t> {
    if (value == 0 || value > std::numeric_limits<std::size_t>::max() / kBytesPerMiB) {
        return fqc::makeError<std::size_t>(fqc::ErrorCode::kUsageError,
                                           std::string(option) + " is out of range");
    }
    return static_cast<std::size_t>(value * kBytesPerMiB);
}

[[nodiscard]] auto resolveProfile(std::string_view value)
    -> fqc::Result<std::optional<fqc::format::DatasetProfile>> {
    if (value == "auto") {
        return std::optional<fqc::format::DatasetProfile>{};
    }
    auto profile = fqc::format::parseProfile(value);
    if (!profile) {
        return fqc::makeError<std::optional<fqc::format::DatasetProfile>>(profile.error());
    }
    return std::optional<fqc::format::DatasetProfile>(*profile);
}

void logStats(std::string_view operation, const fqc::commands::OperationStats& stats, bool quiet) {
    if (quiet) {
        return;
    }
    FQC_LOG_INFO(
        "{} complete: profile={}, paired={}, frames={}, records={}, bases={}, input={} "
        "bytes, output={} bytes",
        operation,
        fqc::format::profileToString(stats.profile),
        stats.paired,
        stats.frameCount,
        stats.recordCount,
        stats.totalBases,
        stats.inputBytes,
        stats.outputBytes);
}

template <typename T>
[[nodiscard]] auto reportError(const fqc::Result<T>& result) -> int {
    FQC_LOG_ERROR("{}", result.error().message);
    return fqc::toExitCode(result.error().code);
}

}  // namespace

int main(int argc, char* argv[]) {
    GlobalOptions global;
    CompressOptions compress;
    DecompressOptions decompress;
    VerifyOptions verify;

    CLI::App app{"fq-compressor v2: bounded-memory, lossless FASTQ compression"};
    app.set_version_flag("-V,--version", FQC_VERSION);
    app.require_subcommand(1);
    app.fallthrough();

    app.add_option("--memory-limit", global.memoryLimitMiB, "Hard memory limit in MiB")
        ->default_val(16 * 1024)
        ->check(CLI::PositiveNumber);
    app.add_flag("-q,--quiet", global.quiet, "Suppress non-error status messages");

    auto* compressCommand =
        app.add_subcommand("compress", "Compress FASTQ into an FQC v2 sequential-frame archive");
    compressCommand->alias("c");
    compressCommand
        ->add_option("-i,--input,-1", compress.input, "Primary FASTQ input, or '-' for stdin")
        ->required();
    compressCommand->add_option("-2,--mate", compress.mate, "Mate FASTQ input; pairs stay atomic");
    compressCommand->add_option("-o,--output", compress.output, "FQC v2 output, or '-' for stdout")
        ->required();
    compressCommand
        ->add_option("--profile",
                     compress.profile,
                     "Dataset profile: auto, illumina, ont, pacbio-hifi, pacbio-clr")
        ->default_val("auto")
        ->check(CLI::IsMember({"auto", "illumina", "ont", "pacbio-hifi", "pacbio-clr"}));
    compressCommand
        ->add_option("--frame-mib", compress.frameMiB, "Target raw FASTQ bytes per frame")
        ->default_val(64)
        ->check(CLI::PositiveNumber);
    compressCommand
        ->add_option("--quality-level",
                     compress.qualityLevel,
                     "zstd level for the quality stream (ID/sequence stay at 1)")
        ->default_val(1)
        ->check(CLI::Range(1, 19));
    compressCommand
        ->add_option("--parse-workers",
                     compress.parseWorkers,
                     "parser workers for parallel parsing of uncompressed regular "
                     "files (0 = sequential reader)")
        ->default_val(4)
        ->check(CLI::Range(0, 64));
    compressCommand->add_flag("-f,--force", compress.force, "Overwrite an existing output");

    auto* decompressCommand =
        app.add_subcommand("decompress", "Decompress FQC v2 into canonical FASTQ order");
    decompressCommand->alias("d");
    decompressCommand->alias("x");
    decompressCommand->add_option("-i,--input", decompress.input, "FQC v2 input")->required();
    decompressCommand
        ->add_option("-o,--output", decompress.output, "FASTQ output, or '-' for stdout")
        ->required();
    decompressCommand->add_flag("-f,--force", decompress.force, "Overwrite an existing output");

    auto* verifyCommand = app.add_subcommand("verify", "Fully decode and verify an FQC v2 archive");
    verifyCommand->alias("v");
    verifyCommand->add_option("input,-i,--input", verify.input, "FQC v2 input")->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const auto cliExitCode = app.exit(error);
        return cliExitCode == 0 ? 0 : fqc::toExitCode(fqc::ErrorCode::kUsageError);
    }

    try {
        fqc::log::threshold = global.quiet ? fqc::log::Level::kError : fqc::log::Level::kInfo;
        auto memoryLimit = checkedMiB(global.memoryLimitMiB, "--memory-limit");
        if (!memoryLimit) {
            return reportError(memoryLimit);
        }

        const fqc::commands::ArchiveEngine engine;
        if (*compressCommand) {
            auto targetFrameBytes = checkedMiB(compress.frameMiB, "--frame-mib");
            if (!targetFrameBytes) {
                return reportError(targetFrameBytes);
            }
            auto profile = resolveProfile(compress.profile);
            if (!profile) {
                return reportError(profile);
            }
            auto result = engine.compress({.inputPath = compress.input,
                                           .matePath = compress.mate,
                                           .outputPath = compress.output,
                                           .profile = *profile,
                                           .memoryLimitBytes = *memoryLimit,
                                           .targetFrameBytes = *targetFrameBytes,
                                           .qualityZstdLevel = compress.qualityLevel,
                                           .parseWorkers = compress.parseWorkers,
                                           .forceOverwrite = compress.force});
            if (!result) {
                return reportError(result);
            }
            logStats("compression", *result, global.quiet);
        } else if (*decompressCommand) {
            auto result = engine.decompress({.inputPath = decompress.input,
                                             .outputPath = decompress.output,
                                             .memoryLimitBytes = *memoryLimit,
                                             .forceOverwrite = decompress.force});
            if (!result) {
                return reportError(result);
            }
            logStats("decompression", *result, global.quiet);
        } else if (*verifyCommand) {
            auto result = engine.verify(verify.input, *memoryLimit);
            if (!result) {
                return reportError(result);
            }
            logStats("verification", *result, global.quiet);
        }
        return 0;
    } catch (const std::exception& error) {
        FQC_LOG_ERROR("unexpected error: {}", error.what());
        return 3;
    }
}
