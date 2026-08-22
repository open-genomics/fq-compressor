// =============================================================================
// fq-compressor - Dataset Profile Detection
// =============================================================================

#include "fqc/commands/profile.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace fqc::commands {
namespace {

constexpr std::size_t kIlluminaMaxReadLength = 1'000;
constexpr std::size_t kIlluminaMaxAverageLength = 500;
constexpr std::initializer_list<std::string_view> kOntHeaderNeedles = {
    "runid=", " ch=", "channel="};
constexpr std::initializer_list<std::string_view> kHifiHeaderNeedles = {"/ccs", "hifi"};
constexpr std::initializer_list<std::string_view> kClrHeaderNeedles = {"pacbio", "subread"};

[[nodiscard]] auto asciiLower(char character) -> char {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

[[nodiscard]] auto lowerCopy(std::string_view value) -> std::string {
    std::string result(value);
    std::ranges::transform(result, result.begin(), asciiLower);
    return result;
}

[[nodiscard]] auto containsAny(std::string_view value,
                               std::initializer_list<std::string_view> needles) -> bool {
    return std::ranges::any_of(needles,
                               [value](std::string_view needle) { return value.contains(needle); });
}

/// Archive-generated FASTQ IDs look like `DRR171398.1` / `ERR123.1` / `SRR…`.
[[nodiscard]] auto looksLikeInsdcRunId(std::string_view id) -> bool {
    if (id.size() < 4 || (std::isdigit(static_cast<unsigned char>(id[3])) == 0)) {
        return false;
    }
    const char first = asciiLower(id[0]);
    const char second = asciiLower(id[1]);
    const char third = asciiLower(id[2]);
    return (first == 's' || first == 'e' || first == 'd') && second == 'r' && third == 'r';
}

[[nodiscard]] auto looksLikeClrHeader(std::string_view id, std::string_view header) -> bool {
    if (containsAny(header, kClrHeaderNeedles)) {
        return true;
    }
    if (id.empty() || id.front() != 'm') {
        return false;
    }
    const auto slashes = static_cast<std::size_t>(std::ranges::count(id, '/'));
    return slashes >= 2 && !containsAny(header, kHifiHeaderNeedles);
}

}  // namespace

auto detectProfile(std::span<const ReadRecord> records) -> Result<format::DatasetProfile> {
    if (records.empty()) {
        return format::DatasetProfile::kIllumina;
    }

    std::size_t ontHeaders = 0;
    std::size_t hifiHeaders = 0;
    std::size_t clrHeaders = 0;
    std::size_t insdcRunIds = 0;
    std::uint64_t totalBases = 0;
    std::size_t maxLength = 0;
    for (const auto& record : records) {
        // comment 已含前导分隔空格；对直接构造的 ReadRecord（comment 无前导空格）仍兼容，
        // 因为各特征 needle 都以子串匹配，分隔符个数不影响命中。
        const auto header = lowerCopy(record.id + record.comment);
        if (containsAny(header, kOntHeaderNeedles)) {
            ++ontHeaders;
        }
        if (looksLikeInsdcRunId(record.id)) {
            ++insdcRunIds;
        }
        if (containsAny(header, kHifiHeaderNeedles)) {
            ++hifiHeaders;
        }
        if (looksLikeClrHeader(record.id, header)) {
            ++clrHeaders;
        }
        totalBases += record.sequence.size();
        maxLength = std::max(maxLength, record.sequence.size());
    }

    const auto majority = (records.size() + 1) / 2;
    // Markers first so `/ccs` beats an SRR accession. Length before INSDC so
    // short ENA Illumina stays Illumina. Unmarked long reads fail closed.
    if (hifiHeaders >= majority) {
        return format::DatasetProfile::kPacBioHiFi;
    }
    if (ontHeaders >= majority) {
        return format::DatasetProfile::kOnt;
    }
    if (clrHeaders >= majority) {
        return format::DatasetProfile::kPacBioClr;
    }
    const auto averageLength = totalBases / records.size();
    if (maxLength <= kIlluminaMaxReadLength && averageLength <= kIlluminaMaxAverageLength) {
        return format::DatasetProfile::kIllumina;
    }
    if (insdcRunIds >= majority) {
        return format::DatasetProfile::kOnt;
    }
    return makeError<format::DatasetProfile>(
        ErrorCode::kUsageError,
        "dataset profile is ambiguous; specify illumina, ont, pacbio-hifi, or pacbio-clr");
}

}  // namespace fqc::commands
