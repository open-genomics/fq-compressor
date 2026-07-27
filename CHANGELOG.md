# 变更记录

## [未发布]

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
