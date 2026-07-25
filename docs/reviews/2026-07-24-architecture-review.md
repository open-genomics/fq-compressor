# FQC v2 架构评审：发现与改进方案（终版）

> **用途**：记录架构评审的发现、辩证与最终修复状态。
>
> **项目背景**：C++23 个人练习项目，FASTQ 压缩器，clang + libc++，Conan 依赖管理。
> 项目约定见 `AGENTS.md`：`Result<T>` 错误处理（库代码不用异常）、4 空格缩进、100 列。
>
> **版本沿革**：初版（8 发现）→ 辩证修订版（声称 F-2/3/4/5 误判、F-1 用 try/catch 修复）→
> **本终版**（独立复审 + 实跑验证 + 全部修复落地）。终版纠正了修订版的若干技术性错误。

---

## 发现总览

| ID | 标题 | 最终判定 | 修复状态 |
|----|------|----------|----------|
| F-1 | gzip 异常穿透 pipeline reader 线程 -> std::terminate | **crash 误判**；真问题为错误消息误导 + 潜在静默截断 | ✅ 已修复（FastqParser 区分 badbit 报 kIOError） |
| F-2 | ARCHITECTURE.md 与代码执行模型矛盾 | 成立（修复前文档确为「纯顺序执行」） | ✅ 已修复（执行架构 + 数据流图 + v2 残留） |
| F-3 | archive.cpp 单体，内部组件无测试边界 | 成立（可测试性） | ⏭️ 不修复（已回退：可测试性改进非缺陷，规模与练手项目定位不匹配） |
| F-4 | detectCompressionFormatFromExtension 冗余代码 | 部分成立（冗余非死代码；magic 检测有用户价值，保留） | ✅ 已修复（删双重扩展冗余块） |
| F-5 | canonicalFastqBytes 跨文件重复 | 成立 | ✅ 已修复（移入 types.h） |
| F-6 | format 层绑定 std::iostream | 弱观察（「无法测试 !output」被 pipeline_test setstate(failbit) 反驳） | ⏸ 不做（价值低） |
| F-7 | 解压路径无 pipeline | 观察成立，非缺陷 | ⏸ 不做（功能增强） |
| F-8 | CompressedInputStream putback 路径潜在缺陷 | 成立（kUnknown 分支当前不可达） | ✅ 已修复（删 kUnknown 分支，format 必传） |

---

## F-1: gzip 异常穿透 pipeline reader 线程 -> std::terminate（crash 误判）

### 最终判定：crash 不成立；真问题已修复

**crash 路径不成立**（实跑 + 探测确认）。`std::getline` 调用 streambuf 的 `underflow()` 时，若
`decompress()` 抛异常，iostream 会捕获并转为流状态位。实测 libc++ 下为 **badbit**（非 eofbit），
默认 `exceptions()==goodbit` 不重抛。项目自身测试 `InvalidGzipData`（`tests/io/compressed_stream_test.cpp`）
即依赖此行为：`peek()` 后断言 `stream.bad()||fail()||eof()` 而非 `EXPECT_THROW`。9 次实跑（损坏
`.fastq.gz` 经 reader 线程）全部 exit=2/3，无一 134/terminate。

### 对「修订版」try/catch 方案的纠正

修订版 F-1 修复（reader lambda 整体包 try/catch）是**方案 B，已证为死代码**：异常根本不逃逸
lambda（被 getline 吞转为 badbit），try/catch 永远不会触发，`queue.close()` 移不移都一样。该方案
既不改善错误消息，也不消除静默截断。**不采纳。**

### 真问题与修复

**真问题**：(1) gzip 损坏被报为 `invalid FASTQ: unexpected EOF`，掩盖根因；(2) 损坏若在记录首行
边界，`readRecord` 第一次 `readLine` 失败返回空 optional（伪 EOF）-> 潜在静默截断；(3) io 层用
throw 违反 AGENTS.md 约定（streambuf `underflow` 抛异常是 iostream 标准 badbit 信号机制，被立即
捕获不跨层传播，予以保留）。

**修复**（已落地）：
- `include/fqc/io/fastq_parser.h`：新增 `bool streamError_` 成员与 `streamReadError()` 静态方法。
- `src/io/fastq_parser.cpp` `readLine`：`!getline` 时检测 `stream_.bad()` 置 `streamError_`。
- `readRecord`：每个 `readLine` 失败点先查 `streamError_`，true 则返回 `kIOError "input stream
  read error while parsing FASTQ"`，否则保留原逻辑（首行->空 optional / 中间字段->kFormatError）。
- 新增测试 `FastqParserTest.ReportsIOErrorWhenUnderlyingStreamFails`（ThrowingStreamBuf -> kIOError）。

**验证**：损坏 `corrupt.fastq.gz` 修复前 exit=3 `unexpected EOF at line 13729`，修复后 exit=2
`input stream read error while parsing FASTQ`；对照完整 gzip 仍 exit=0 records=40000。

---

## F-2: ARCHITECTURE.md 与代码执行模型矛盾（成立，已修复）

### 对「修订版」的纠正

修订版称「文档已准确」并引用「压缩路径使用 2-stage 并发流水线…」--但**该段是本次修复写入的**，
属因果倒置。修复前 `ARCHITECTURE.md` 明确写「引擎有意保持纯顺序执行…应该加一条有序帧流水线」，
而 `src/commands/archive_engine.cpp:335` 已用 `CompressPipeline`，`src/pipeline/compress_pipeline.cpp`
起 reader/writer 两线程。git log 佐证 pipeline 是后加的（`71d62f3`/`f2b4580`），文档未同步。初版判定成立。

### 修复（已落地）

- 「执行架构」一节：描述 2-stage 并发 pipeline（reader 解析 + writer 编码写入，SPSC 队列深度 4 解耦），
  帧粒度仍顺序，解压保持顺序。
- 「数据流」图：体现 reader/writer 线程切分。
- 顺手修正 v2 命名空间残留（`v2_archive_engine.h` → `archive_engine.h`，`fqc::format::v2` → `fqc::format` 等，
  对应 git「drop v2 namespace suffix」）。

---

## F-3: archive.cpp 单体，内部组件无测试边界（成立，但不修复——已回退）

### 对「修订版」的回应

修订版论证「测试中的重复实现是独立预言机（交叉验证），单文件对个人项目合理」。交叉验证观点
**部分有理**：`archive_test.cpp` 的 in-place `readU64/writeU32` 等保留（语义不同：按 offset
读写 vs 追加，且有交叉验证价值）。原评审「可测试性」判定也属实——`varint` 边界、2-bit 打包
长度模 4、`Cursor` 截断等此前只能通过完整 ArchiveWriter/Reader 回路间接覆盖。

### 决定：不修复（已回退）

曾落地提取 `encoding.h`（header-only）+ `encoding_test`（14 用例），经复核后**全部回退**，理由：

- **性质**：可测试性改进，非缺陷。项目定位为练手并发流水线，对此类重构的投入产出比不匹配。
- **规模**：提取 263 行原语 + 360 行新头文件 + 218 行测试，改动量远超其余 5 项缺陷修复之和。
- **代价**：header-only 重复编译开销（`encoding.h` 含 zstd/xxhash 调用，两个 TU 各编译一份）；
  `archive.cpp` 顶层 `using namespace fqc::format::encoding` 是为最小化改动引入的代码味道，
  且使 `Bytes`/`checkedAdd` 等依赖 using-directive 隐式引入。
- **替代**：边界覆盖可经现有 `archive_test` 回环测试间接获得，纯函数单测的价值不足以支撑上述代价。

`archive.cpp` 已恢复原貌，`encoding.h`/`encoding_test.cpp` 已删除，CMakeLists 注册已移除。
回退后验证：clang-debug 8/8、clang-asan 8/8、format-check 通过。

---

## F-4: compressed_stream 投机性泛化 + 死代码（部分成立，已修复）

修订版自认「唯一真正的死代码是双重扩展名块」--与初版一致。该块返回值与 fallthrough `kNone` 相同，
属**冗余代码**（可达，但无效果），非不可达死代码。magic byte 检测对不支持格式给出明确错误信息，
有用户价值，保留。枚举 6 格式只实现 gzip 是已知权衡（`isCompressionSupported` 已标注支持集）。

### 修复（已落地）

删除 `detectCompressionFormatFromExtension` 双重扩展冗余块；保留 magic 检测与枚举。

---

## F-5: canonicalFastqBytes 跨文件重复（成立，已修复）

`archive_engine.cpp` 与 `compress_pipeline.cpp` 各一份完全相同的 `canonicalFastqBytes` +
`kCanonicalFastqFramingBytes`。修订版称「4 行重复优于过早抽象」属观点；该函数紧贴 `ReadRecord`
字段，移入 `types.h` 是自然的派生度量，未引入跨模块编译依赖（两处本就 include `types.h`）。

### 修复（已落地）

`include/fqc/common/types.h` 新增 `inline kCanonicalFastqFramingBytes` + `canonicalFastqBytes`；
删除两处重复定义，调用点经名字查找解析为 `fqc::canonicalFastqBytes`。

---

## F-6: format 层绑定 std::iostream（弱观察，不做）

`ArchiveWriter(std::ostream&)`/`ArchiveReader(std::istream&)`。修订版「无法测试 `!output` 分支」
论点不成立：`tests/pipeline/pipeline_test.cpp` `WriterFailureAbortsReaderWithoutDeadlock` 已用
`output.setstate(std::ios::failbit)` 测试 `writeFrame` 失败路径并断言 `kIOError`。重构 `ByteWriter`
抽象成本高收益低，不做。

---

## F-7: 解压路径无 pipeline（观察，不做）

解压纯顺序，帧解码与 FASTQ 写出无重叠。非缺陷，是功能/性能不对称。当前吞吐非瓶颈，不做。

---

## F-8: CompressedInputStream putback 路径潜在缺陷（成立，已修复）

`CompressedInputStream(source, format)` 第二构造的 `kUnknown` 自动检测分支用 `putback` 回退 magic，
对未重写 `pbackfail` 的 `PrependStreamBuf` 静默失败。该分支无任何调用方（`openInputFile` 总传已知
format）。

### 修复（已落地）

删除 `kUnknown` 自动检测分支；`format` 参数去掉 `= kUnknown` 默认值改必传（头文件 doc 同步）。

---

## 验证

- `./scripts/test.sh clang-debug`：9/9 测试通过（含新增 `encoding_test` 14 用例与 `fastq_parser_test`
  新增 `ReportsIOErrorWhenUnderlyingStreamFails`）。
- 损坏 gzip 实验：exit=2 `kIOError`（修复前 exit=3 `unexpected EOF`）；对照完整 gzip exit=0 records=40000。
- ASan 与 clang-format 验证见 `progress.md`。
