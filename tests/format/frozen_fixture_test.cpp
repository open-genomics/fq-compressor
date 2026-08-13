// =============================================================================
// fqc-sequential/v2 Frozen Fixture Decoder Compatibility Tests
// =============================================================================
// Decode the committed frozen archives (tests/fixtures/sequential-v2/, see
// MANIFEST.md for generator provenance and SHA-256) and compare every record
// and metadata field against the committed FASTQ expectations. These tests
// protect the decoder contract: a future change that breaks reading of
// already-published v2 archives fails here even when writer round-trip tests
// keep passing (writer and reader could otherwise drift together).
// =============================================================================

#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fqc::format::test {
namespace {

[[nodiscard]] auto fixturePath(std::string_view name) -> std::string {
    return std::string(FQC_FIXTURE_DIR) + "/" + std::string(name);
}

[[nodiscard]] auto parseFastq(const std::string& path) -> std::vector<ReadRecord> {
    std::ifstream stream(path);
    EXPECT_TRUE(stream) << "cannot open " << path;
    io::FastqParser parser(stream);
    std::vector<ReadRecord> records;
    for (;;) {
        auto record = parser.readRecord();
        EXPECT_TRUE(record) << "parse error in " << path;
        if (!record || !record->has_value()) {
            break;
        }
        records.push_back(std::move(**record));
    }
    return records;
}

struct DecodedArchive {
    ArchiveMetadata metadata;
    ArchiveStats stats;
    std::vector<ReadRecord> records;
};

[[nodiscard]] auto decodeArchive(const std::string& path) -> DecodedArchive {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream) << "cannot open " << path;
    ArchiveReader reader(stream);
    auto metadata = reader.open();
    EXPECT_TRUE(metadata) << "reader rejected " << path;

    DecodedArchive decoded{.metadata = *metadata, .stats = {}, .records = {}};
    for (;;) {
        auto frame = reader.readFrame();
        EXPECT_TRUE(frame) << "decode error in " << path;
        if (!frame || !frame->has_value()) {
            break;
        }
        decoded.records.insert(decoded.records.end(), std::make_move_iterator((*frame)->begin()),
                               std::make_move_iterator((*frame)->end()));
    }
    EXPECT_TRUE(reader.finished());
    decoded.stats = reader.stats();
    return decoded;
}

}  // namespace

TEST(FrozenFixture, SingleEndArchiveDecodesToCommittedFastq) {
    const auto decoded = decodeArchive(fixturePath("frozen_se.fqc"));
    EXPECT_EQ(decoded.metadata.version, kArchiveVersion);
    EXPECT_EQ(decoded.metadata.profile, DatasetProfile::kIllumina);
    EXPECT_FALSE(decoded.metadata.paired);
    EXPECT_EQ(decoded.stats.frameCount, 1U);
    EXPECT_EQ(decoded.stats.recordCount, 3U);
    EXPECT_EQ(decoded.stats.totalBases, 150U);
    EXPECT_EQ(decoded.records, parseFastq(fixturePath("input_se.fastq")));
}

TEST(FrozenFixture, PairedEndArchiveDecodesToInterleavedFastq) {
    const auto decoded = decodeArchive(fixturePath("frozen_pe.fqc"));
    EXPECT_EQ(decoded.metadata.version, kArchiveVersion);
    EXPECT_EQ(decoded.metadata.profile, DatasetProfile::kIllumina);
    EXPECT_TRUE(decoded.metadata.paired);
    EXPECT_EQ(decoded.stats.frameCount, 1U);
    EXPECT_EQ(decoded.stats.recordCount, 6U);
    EXPECT_EQ(decoded.stats.totalBases, 300U);

    const auto r1 = parseFastq(fixturePath("input_r1.fastq"));
    const auto r2 = parseFastq(fixturePath("input_r2.fastq"));
    ASSERT_EQ(r1.size(), r2.size());
    std::vector<ReadRecord> interleaved;
    interleaved.reserve(r1.size() + r2.size());
    for (std::size_t index = 0; index < r1.size(); ++index) {
        interleaved.push_back(r1[index]);
        interleaved.push_back(r2[index]);
    }
    EXPECT_EQ(decoded.records, interleaved);
}

}  // namespace fqc::format::test
