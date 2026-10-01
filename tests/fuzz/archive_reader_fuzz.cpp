// =============================================================================
// fq-compressor - libFuzzer harness: FQC v2 archive reader (readFrame path)
// =============================================================================
// Fuzz 目标：ArchiveReader 对任意字节流的鲁棒性——magic/profile 拒绝、varint
// 边界、帧头尺寸字段、zstd 流、逐帧校验和与 footer 校验。输入被拒绝、校验
// 失败、提前 EOF 都是合法出口；崩溃与 UB（ASan/UBSan 插桩）才是 fuzz 目标。
// 冻结 fixture（tests/fixtures/sequential-v2/*.fqc）作为种子，让变异从格式
// 深处开始。

#include "fqc/format/archive.h"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace {

// 8 MiB 帧内存信封：既允许小的合法帧完整走通 readFrame 全路径（zstd + 解码
// + 校验和），又把单帧解码峰值压进 libFuzzer 默认 -rss_limit。
constexpr std::size_t kFuzzMaxFrameBytes = 8U * 1024U * 1024U;
constexpr std::size_t kFuzzMemoryLimitBytes = 8U * 1024U * 1024U;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);
    std::istringstream stream(input);
    fqc::format::ArchiveReader reader(stream, kFuzzMaxFrameBytes, kFuzzMemoryLimitBytes);
    if (!reader.open().has_value()) {
        return 0;  // magic/版本/profile 拒绝是常态
    }
    while (true) {
        auto frame = reader.readFrame();
        if (!frame.has_value() || !frame->has_value()) {
            break;
        }
    }
    return 0;
}
