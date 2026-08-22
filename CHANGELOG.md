# 变更记录

## [未发布]

### 修复

- **头部尾随空格静默丢失（无损硬约束违背）**：`@id `（恰一个尾随空格）往返时丢字节。
  根因：`FastqParser` 按首个空格把头部切成 id/comment 时丢弃了分隔空格本身，comment 为空时
  重建无法区分"有分隔、空 comment"与"无 comment"。修复：comment 语义改为**从首个分隔空格起
  （含该空格）的整段文本**，重建时 `id + comment` 原样拼接，任意头部都能逐字节往返。
  **注意（向后兼容）**：这是 v2 归档 ids 流 comment 字段的语义变更——旧归档（comment 不含前导
  空格）在新二进制解压时 comment 会少一个空格（`id x` 变 `idx`）；仓库内冻结 fixture 均为无
  comment 记录，不受影响。`canonicalFastqBytes` 同步去除伪造分隔符的 +1。

- **并行解析块边界对齐误匹配 `'@'` → 幻影记录注入（静默数据损坏）**：块边界落在头部行中段且
  该处恰为 `'@'` 时，`findFirstRecordStart` 会把行尾残段误判为记录起点，注入一条截断头部的幻影
  记录；归档校验和自洽，`verify`/解压全部通过。修复：候选命中必须处于**真正的行首**（前一字节
  为 `
` 或文件起始），残段直接跳过。质量行以 `'@'` 起始（Q31）仍安全，不受影响。

- **`trimTrailingCr` 剥离所有尾随 `
`**：改为只剥离与 `
` 成对的单个 CRLF 行尾 `
`；数据中
  真实存在的尾随 `
`（如 `SEQ

`）保留一个，encode 阶段以"无效逻辑记录"响亮拒绝而非静默
  篡改（fail-closed）。

- **归档读取把底层 I/O 错误伪装成格式错误**：`readExact` 在 `badbit`（如损坏的 gzip 成员）时
  返回 `kFormatError` "truncated"，误导排障。改为 `badbit → kIOError`。

- **空文件 + `.gz` 扩展名被误判为 gzip**：0 字节输入按扩展名回退到 gzip 路径报
  "truncated gzip" 误导性错误。改为 0 字节即视为空普通输入（干净 EOF）。

- **MPMC post-close push 足枪**：`close()` 后 `push()` 不再成功入队，而是立即返回 `false`
  （此前会静默接受条目，可能滞留于消费者已退出、无人再取的队列）。

- **ChunkOrderer 重复键静默丢帧**：`submitFrame` 遇重复 `(chunk, local)` 键时输出告警日志
  （原 `emplace` 静默丢弃）。


## [0.1.0] - 2026-08-21

### 文档

- **README 收口 + 格式族契约**：首屏精简为单段格式族说明并补齐契约要点——本实现为 `fqc-sequential/v2`（magic `46 51 43 56 32 0D 0A 1A`，`FQCV2\r\n\x1A`），与 [fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust)（`fqc-indexed/v2`，magic `89 46 51 43 0D 0A 1A 0A`）同名但格式不兼容、不能互相解码；`.fqc` 扩展名不能判定格式，reader 必须检查 archive magic。新增同名二进制 `fqc` 的 `PATH` 覆盖风险提醒（`which fqc` 确认实际调用实现）。
- **文档拆分**：构建、工具链与质量门禁从 README 独立成文，新增 `docs/building.md`（README 以链接引用）；`docs/fastq-compression-survey.md` 的 README 引用改为 `#` 锚点链接。对应 openspec 变更 `document-fqc-format-family` 已归档（spec 合并入 `openspec/specs/format-governance/`，变更目录移入 `changes/archive/`，`project.md` capability 表与 Archived changes 表同步）。
- **postmortem 文档整理**：7 篇复盘文档统一中英混排表述、标点（`--` → `——`）、补充已随项目瘦身删除的基线/台账文件说明，无技术内容变更。
- **路线图关账**：阶段 H 标为完成；README / ARCHITECTURE / 开发历程同步并行解析后的数据流与合成性能。
- **真实语料验收**：新增 `docs/real-corpus.md` 与 `scripts/fetch_real_corpus.sh`。公开切片上 Illumina WXS 压缩比 4.15×（优于合成 2.96×），人类 MinION 1.96×（劣于合成，质量流近满字母表）；`--quality-level 7` 仍未过门槛。

### 变更

- **clean code 收口**：`AGENTS.md` / CLI 描述与当前 MPMC 流水线对齐；`FQC_TRY` 降低 `Result` 传播样板；profile 检测独立为 `profile.cpp`；去掉无语义的 `FastqRecord` 别名；生产注释去掉课程阶段叙事，只留不变量。
- **ENA 转写长读可 auto 识别为 ONT**：`detectProfile` 在 Illumina 长度规则之后，把多数 ID 为 `SRR`/`ERR`/`DRR` + 数字的长读判为 `ont`（档案生成 FASTQ 会丢掉 `runid=`）。短读 ENA 头仍按长度走 Illumina；无 accession 的模糊长读继续拒绝。单测覆盖 `ERR` 前缀，以及 `/ccs` 优先于 `SRR` accession。

### 新增

- **跨族 magic 识别**：打开归档时先做 8-byte magic 分派；遇到 Rust `fqc-indexed/v2`（`89 46 51 43 0D 0A 1A 0A`）返回明确的 unsupported format family 错误并指向 `open-genomics/fq-compressor-rust`；未知 magic 与截断 magic 分开报错（`verify`/`decompress` 均覆盖，不创建输出）。
- **fqc-sequential/v2 格式规范**：建立仓库内 `openspec/` 格式契约，记录 magic、版本、header/frame/footer 结构、checksum 覆盖和 rejection 行为。添加格式契约测试 (`tests/format/format_contract_test.cpp`)、冻结 SE/PE 归档 fixture (`tests/fixtures/sequential-v2/`，含生成 commit、命令与 SHA-256 manifest) 及解码兼容测试 (`tests/format/frozen_fixture_test.cpp`，逐记录比对冻结归档与 FASTQ 期望)。标注该实现为 `fqc-sequential/v2` 格式族（与 Rust `fqc-indexed/v2` 通过 magic 区分）。
- **并行解析（路线图阶段 H，压缩吞吐 +47.7%）**：未压缩普通文件输入时，解析从单线程串行升级为数据并行——K 个 parser worker 各自打开独立 ifstream，按字节块切分 `[sampleEnd, fileSize)`，经边界对齐协议解析各自块内起始的记录，帧按 `(chunkId, localId)` 两级标记，由新组件 `ChunkOrderer` 在 writer 端按字典序重组（`ReorderBuffer` 的两级推广）；encoder/writer 段与顺序路径同源。`compress --parse-workers N`（默认 4，0 = 强制顺序）。
  - **边界对齐协议**：候选行首 `@` + 结构回验（`+` 行、seq 长度==qual 长度，与 `FastqParser` 的结构性接受规则完全镜像；内容校验统一留给 `encodeFrame`），失败回退一行续扫——质量行以 `@` 起始（Q31）不会误判（含对抗 fixture 测试）。
  - **采样连续性**：profile 采样保持在主线程，采样记录作为 worker 0 的累积器种子（不是独立块），`FastqParser` 新增 `bytesConsumed()` 精确记录采样终点偏移。
  - **退化纪律**：gzip/stdin/双端输入自动走原顺序路径（gzip 流式不可随机切块、stdin 不可寻址、双端锁步复杂度爆炸）；`--parse-workers 1` 的并行机器用于分帧一致性门禁。
  - 分派探测复用 `io::detectCompressionFormat`（magic 嗅探），新增 `--parse-workers` CLI（0–64）。
  - 实测（A/B 同窗口，64 MiB random ×5）：illumina 压缩 **+47.7%**（100.75 → 148.84 MiB/s，spread 区间不重叠，reader 并行 4 路直击 Amdahl 确认的瓶颈）；ont -1.3%（长读解析占比小，噪声内）；解压路径未动。压缩比代价 2.9588 → 2.9572（块尾关帧的切分开销）。

### 变更

- **CI 新增 ASan+UBSan 门禁**：`sanitizer` job 与 build-and-test 并行，用 `clang-asan` preset 构建并跑全部测试；GTest 以 `--build=gtest*` 强制与项目同工具链从源码构建，避免预编译包混链误报；LeakSanitizer 因 ptrace 环境限制保持禁用（发布机检查项），`alloc_dealloc_mismatch` 因系统 libc++18 未插桩的异常释放误报而关闭（其余 ASan/UBSan 检查保持）。
- **文档：修正 `verify` 语义**：README/ARCHITECTURE 不再声称“不解压即可完整校验”；明确
  `verify` 完整解码并校验但不写 FASTQ，成本与 decompress 同量级；XXH64 为完整性检测而非密码学认证。
- **分帧核算从 capacity 口径改为 size 口径**：`FrameAccumulator::retainedBytes` 改用字符串 size 而非 capacity——libc++ `getline` 的字符串 capacity 取决于行相对 streambuf 缓冲相位的位置（跨界行 capacity 更大），capacity 口径使帧边界依赖于流的缓冲相位，跨路径（顺序 vs 并行）分帧不可复现。size 口径下分帧是纯内容函数，`--parse-workers 0` 与 `--parse-workers 1` 归档逐字节一致。保守性不变：内存预检 `estimateCompressionPeak` 仍按 capacity 核算；线上格式不变、旧归档完全可读。新归档与旧二进制不再逐字节一致（分帧规则变化，非格式变化）。→ 详见 [docs/postmortems/2026-07-28-getline-capacity-framing.md](docs/postmortems/2026-07-28-getline-capacity-framing.md)
- 共享 `FrameAccumulator` 组件：顺序 reader 与并行 parser worker 的分帧规则单一来源（防漂移）。

### 修复

- **并行边界对齐不再静默丢弃畸形记录（数据丢失）**：对齐扫描原以"纯 IUPAC"做候选预检，比 `FastqParser` 更严——畸形记录（非 IUPAC 序列等）的 header 若落在非首 chunk 的对齐区会被拒为候选、对齐跳到下一条记录，导致该记录无人解析，产出缺记录的合法归档（顺序路径对同一输入响亮报错）。修复：候选检查只镜像解析器的结构性规则（`+` 行、长度相等），内容校验统一由 `encodeFrame` 承担；被 EOF 截断在 body 中的候选同样返回其偏移，让 parser 报出与顺序路径一致的错误。残留保守边界：`@` 行后紧跟 EOF/单个空行的情形维持 nullopt（无法与合法文件末尾以 `@` 起始的质量行区分，宁可漏报畸形也不误伤合法输入）。→ 详见 [docs/postmortems/2026-08-04-parallel-alignment-silent-skip.md](docs/postmortems/2026-08-04-parallel-alignment-silent-skip.md)
- **gzip 输入底层 I/O 错误不再静默截断**：`GzipStreamBuf` 在底层流读失败（badbit）时原先以 EOF 收尾，解析器会把截断当正常结束；现在 decompress 无产出且未到 gzip 结尾时抛错，流经 istream badbit 由解析器上报 `kIOError`。
- 并行 worker 每个文件只开一个 ifstream（对齐与解析复用同一流），并补齐 parse 段 `seekg` 失败检查。
- `CompressPipeline` reader 对 `span<const ReadRecord>` 元素的误导性 `std::move`（实际触发拷贝）改为显式拷贝。

### 变更

- **解压路径流水线化 + 泛型 RecordSink（路线图阶段 G）**：解压从纯顺序循环升级为与压缩路径镜像的三段流水线，zstd+decode 不再与 FASTQ 写盘串行。
  - format 层拆分：`ArchiveReader::readFrame` 拆为 `readRawFrame`（I/O + 全部边界/内存预检，footer 结构解析后暂存）与 free `decodeRawFrame`（zstd 解压 + 逻辑校验和验证 + 记录解码，纯计算、worker 线程安全）；`readFrame` 保留为组合 API 且行为不变（archive_test 全部原样通过）；新增 `RawFrame`/`DecodedFrame`/`ArchiveFooter` 类型与 `footer()` 访问器；`advanceGlobalChecksum` 提升为公开函数（顺序依赖，调用方必须按帧序应用）。
  - 新 `DecompressPipeline`（reader → decoder×N → writer(reorder 按序提交)）：**滚动全局校验和严格留在 writer 有序端**（链式顺序依赖，本阶段头号正确性陷阱）；footer 校验（totals + 全局校验和）在 join 后由 writer 侧有序累积值对比，严格性与顺序版一致；内存预检在 `readRawFrame` 内完成后再入队，在途包络 = 2×4+N 与压缩路径同型。
  - 泛型 stage 落地为 `RecordSink`（`std::function<VoidResult(std::vector<ReadRecord>)>`：逐帧严格按帧序、writer 线程调用、失败经 stop_source 级联取消）：decompress sink = 写 FASTQ，verify sink = 仅计数——同一流水线的第一个复用者。worker 循环保持手写，未造 pipeline 框架（"最小核"纪律）。
  - 验证：新旧二进制解压同一归档输出**逐字节一致**（滚动校验和尾值一致的最强形式）；篡改 footer 全局校验和 → kChecksumError、篡改 footer totals → kFormatError、载荷字节翻转不可静默通过、截断归档报错、sink 失败取消无死锁、空归档正常；e2e round-trip cmp 通过（16 GiB / 64 MiB 两档）；clang-debug / clang-tsan 10/10。
  - 性能：A/B 同窗口解压 +4.2%/+2.0%（spread 重叠、噪声内——64 MiB 输入仅 ~2 帧限制重叠收益，多帧/大输入场景才有明显收益）；解压 maxRSS +21–42%（~134 MiB，顺序版持有 1 个解码帧 → 流水线在途多帧的预期代价，远低于 16 GiB 预算）。

### 新增

- **质量流 per-stream zstd level（路线图阶段 I 番外）**：`compress --quality-level N`（1–19）允许质量流单独提升 zstd 档位，ID/序列流保持 level 1。zstd 帧自描述，解码零改动、codec ID 不变、线上格式零破坏；默认 level=1 与阶段 F 归档逐字节一致。
  - **CODEC_GATES 准入门槛判定**（吞吐回退 ≤10% 且压缩比改善 ≥3% 才合入默认值）：拟真质量分布（64 MiB，Q38–41 集中）下 L7/L9 体积 −3.3%/−4.0% 但压缩吞吐 −43%/−53%，L19 体积 −10.6% 但吞吐 −97%；11 符号随机质量下 L3–L9 归档反而更大（+3–7%）。**无任何档位通过门槛 → 默认保持 level 1**，选项仅作为 CLI 实验入口保留。实测曲线见 `docs/fastq-compression-survey.md` §6。
  - 单测：高层级只影响质量流载荷（ID/序列载荷跨层级逐字节一致）、越界 level 拒绝（kUsageError）、高层级归档 round-trip 一致。

### 变更

- **zstd 下沉到 encoder worker（路线图阶段 F）**：压缩流水线的 CPU 密集段全部迁入 worker 池，writer 退化为纯 I/O。
  - format 层：新增 `CompressedFrame`（三路压缩后字节 + 帧头所需的 raw 尺寸 + 逻辑校验和）与 free 函数 `compressFrame`（纯计算、worker 线程安全；逐流压缩并立即释放对应 raw 流，常驻峰值保持 raw×3 + 单份 `ZSTD_compressBound` scratch）；`ArchiveWriter::writeEncodedFrame` 由 `writeCompressedFrame` 取代（只拼装帧头 + 写盘 + 更新校验和，不再做任何 CPU 密集工作）；`writeFrame` 重组为 encode → compress → write 三步，线上格式与 codec ID 完全不变。
  - pipeline 层：queue2 载荷从 raw 流改为压缩后流（在途占用只降不升）；分段计时新增 `encoderCompressNs`，`writerCompressNs` 更名 `writerWriteNs`，观测日志措辞同步（encoder `zstd=` / writer `write=`）。
  - 内存口径：`estimateCompressionPeak` 每帧已含 raw + compressBound，N 个 worker 并发持有 bound scratch 仍被在途 12 帧上界覆盖；实测 64 MiB 输入 maxRSS 上升 10–18%（bound scratch 从 writer×1 变为 worker×N），峰值 ~216 MiB 仍远低于 16 GiB 预算。
  - 验证：**归档逐字节一致**——基线（阶段E）与阶段F 二进制同输入 .fqc 完全相等（illumina/ont × 16 GiB/64 MiB 两档内存限制，cmp 零差异），依据是同版本同参数 `ZSTD_compress` 输出确定 + reorder 保序 + 帧 id 由 writer 计数器派生；writer 段从 zstd+io 占 wall ~20% 降为 write ~5%（趋近纯 I/O）；A/B 同窗口吞吐 delta 在环境噪声内；clang-tsan 9/9 无竞争。

### 新增

- **流水线可观测性（路线图阶段 E）**：
  - `MpmcQueue` 新增 relaxed 原子计数器（push/pop 次数、满/空阻塞次数、高水位），独占缓存行隔离计数流量与同步状态；`stats()` 返回快照。
  - `PipelineStats` 新增 `StageTimings`（reader parse/push、encoder pop/encode/push、writer pop/zstd+io、wall）与两条队列的计数器快照；worker 本地累计、退出时单次 relaxed 合并，时钟采样不进同步热路径。
  - `compress` 非 quiet 模式输出两行分段计时与队列统计（`-q` 抑制）。
- **benchmark 平台加固（`tests/e2e/test_performance.sh`）**：warmup 轮（`FQC_PERF_WARMUP`）；`FQC_PERF_REPEATS` 默认 3→5；median 之外新增 [min..max] 极差报告并写入 jsonl（附 git commit 与运行配置头）；A/B 模式（`FQC_PERF_BIN_B`）在迭代内交错跑两个二进制、各自独立 round-trip 校验、SLA 只约束被测二进制 A；`FQC_PERF_ARCHIVE=<slug>` 归档到 `perf-baselines/YYYY-MM-DD-<slug>/`。
- **计时源改单调时钟**：`date +%s.%N`（CLOCK_REALTIME）在 WSL2 主机对时回跳时会产生负时长样本——median 静默吸收所以旧脚本从未暴露，极差报告一上线就抓到；改用 `/proc/uptime`（CLOCK_BOOTTIME）从机制上杜绝负样本。

### 修复

- **并发流水线死锁**：`SpscQueue` 新增 `abort()` 机制，writer 写入失败时中止队列，解除 reader 在满队列 `push` 上的自旋阻塞。此前 writer 失败后 reader 会永久卡在 `push`，导致 `reader.join()` 挂死。
- **双向唤醒对称性**：`push` 返回 `bool` 并在自旋时检查中止标志，`pop` 在空队列时检查中止与关闭，与 `close()` 形成完整的双向唤醒。
- **SPSC `pop()` 丢失尾帧竞态**：`head_` 与 `closed_` 是独立 atomic，`pop` 先读 `head_` 再读 `closed_` 时，消费者可能看到 stale `head_` 却看到 `closed_=true`，误判队列空而返回 `nullopt`，丢失 `close` 前最后一次 `pushFrame` 的帧。修复为观察到 `closed_/aborted_` 后再 acquire 重读 `head_` 做终检。→ 详见 [docs/postmortems/2026-07-24-spsc-pop-lost-tail-frame.md](docs/postmortems/2026-07-24-spsc-pop-lost-tail-frame.md)

### 变更

- **接入并发主路径**：`ArchiveEngine::compress` 改用 `CompressPipeline`（reader/writer 双线程 + 有界 SPSC 队列）替代串行 `FrameAccumulator`，压缩流水线真正并发生效。此前 `CompressPipeline` 已存在但无任何调用点。
- `CompressPipeline::run` 扩展支持样本注入（`initialRecords`）、双端输入（`mate`）、逻辑字节统计（`logicalBytes`）。
- 删除未用的 `FrameAccumulator` 与 `retainedRecordBytes`。

### 删除

- `benchmark_v2/`（13 个 Python 文件）、`scripts/benchmark_v2.sh`、`tests/e2e/benchmark_v2_smoke_test.sh`（共约 2700 行）：多工具选型框架与单编码器场景无关，性能测量由 `tests/e2e/test_performance.sh` 覆盖。

### 测试

- 新增 `WriterFailureAbortsReaderWithoutDeadlock`（死锁验证）、`PairedRun`、`InitialRecordsEmittedFirst` 及 SPSC 队列中止场景单测。
- clang-debug / clang-asan（ASan+UBSan）/ clang-tsan（TSan）三套构建下全部 8/8 通过，无内存错误与数据竞争。
- 阶段 E：新增队列计数器单测（满/空阻塞计数、高水位、压测 pushes==pops）与流水线统计不变量集成测试（四条队列计数 == 帧数、wall/encode 非零）；clang-debug 与 clang-tsan 9/9 通过；A/B 同窗口对比（基线 vs 阶段E，64MiB random ×5）压缩比完全一致（2.9588/2.8403），吞吐差异在环境噪声区间内（无可见回退）。

### 构建

- 修复 `scripts/build.sh` 调用已删除的 `install_deps.sh` 的隐患，缺失 Conan 工具链时改为明确报错与指引。
- 修正 `tests/README.md` 对已删除 `acceptance.sh` 与 `benchmark_v2_smoke_test.sh` 的失效引用。

### 文档与清理

- `README.md` 清理指向已删除文件的死链（`README.en.md`、`performance/INDEX.md`），移除"下载二进制/多平台发布"等产品 closeout 措辞，回归以源码构建为主的项目说明。
- `ARCHITECTURE.md` 软化"发布鉴定"措辞，与业余练手定位对齐。
- `include/fqc/io/compressed_stream.h` 注释对齐实际能力：仅 gzip 可解压，bzip2/xz/zstd 仅做 magic 检测后拒绝（fail-closed），不再营造"多格式支持"的假象。
- 删除无生产调用点的纯查询 API `compressionFormatExtension`、`supportedCompressionFormats` 及其单测。
