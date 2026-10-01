// =============================================================================
// fq-compressor - libFuzzer harness: FASTQ parser + single-record encode
// =============================================================================
// Fuzz 目标：FastqParser::readRecord 对任意字节流的鲁棒性。解析失败/EOF 是
// 合法出口；崩溃、UB（由 ASan/UBSan 插桩捕获）才是 fuzz 要找的缺陷。每条
// 解析成功的记录再喂给 encodeFrame——parser 的结构验收与 encode 的内容校验
// 构成双层网，任何一层都不该以崩溃的方式失败。

#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

// 1 MiB 行上限：与生产 parser 的 64 MiB 上限语义一致，但把单次分配峰值压进
// libFuzzer 默认 -rss_limit，保证 fuzz 吞吐。
constexpr std::size_t kFuzzMaxLineBytes = 1U * 1024U * 1024U;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);
    std::istringstream stream(input);
    fqc::io::FastqParser parser(stream, kFuzzMaxLineBytes);
    std::vector<fqc::ReadRecord> oneRecord;
    oneRecord.reserve(1);
    while (true) {
        auto record = parser.readRecord();
        if (!record.has_value() || !record->has_value()) {
            break;
        }
        oneRecord.clear();
        oneRecord.push_back(std::move(**record));
        (void)fqc::format::encodeFrame(oneRecord, fqc::format::ArchiveOptions{});
    }
    return 0;
}
