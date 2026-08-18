// =============================================================================
// fq-compressor - Shared Test Helpers
// =============================================================================

#pragma once

#include "fqc/common/types.h"
#include "fqc/io/fastq_parser.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace fqc::test {

[[nodiscard]] inline auto parseAllFastq(std::istream& input) -> std::vector<ReadRecord> {
    io::FastqParser parser(input);
    std::vector<ReadRecord> records;
    for (;;) {
        auto record = parser.readRecord();
        EXPECT_TRUE(record.has_value()) << record.error().message;
        if (!record || !record->has_value()) {
            break;
        }
        records.push_back(std::move(**record));
    }
    return records;
}

[[nodiscard]] inline auto parseAllFastq(std::string_view fastq) -> std::vector<ReadRecord> {
    std::istringstream input{std::string(fastq)};
    return parseAllFastq(input);
}

[[nodiscard]] inline auto parseAllFastqFile(const std::filesystem::path& path)
    -> std::vector<ReadRecord> {
    std::ifstream stream(path);
    EXPECT_TRUE(stream) << "cannot open " << path;
    return parseAllFastq(stream);
}

class TempDir {
public:
    TempDir() {
        static std::atomic<std::uint64_t> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
            ("fqc_test_" + std::to_string(::getpid()) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    auto operator=(const TempDir&) -> TempDir& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

    void writeFile(const std::filesystem::path& relative, std::string_view content) const {
        std::ofstream file(path_ / relative, std::ios::binary);
        file << content;
    }

    [[nodiscard]] auto readFile(const std::filesystem::path& relative) const -> std::string {
        std::ifstream file(path_ / relative, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path path_;
};

}  // namespace fqc::test
