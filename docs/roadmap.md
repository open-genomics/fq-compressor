# 开发路线图

fq-compressor 并发流水线练手路线。定位：业余练手 C++ 并发流水线，追求轻灵巧、易维护、不引重框架。

## 设计原则

1. **一次一个并发知识点**：同步原语、取消、多级、并行分阶段练，不混。
2. **每步可验证可回退**：`clang-tsan` 验无竞争，`tests/e2e/test_performance.sh` 量化吞吐，每步独立 commit。
3. **不引重框架**：排除 TBB / boost.asio / 线程池 / 异步 I/O 孤岛。
4. **抽象克制**：泛型 stage 等有第三个 stage 重复模式再抽。

## 当前基线

阶段 F 后（commit 3629f6d）：

```text
压缩: reader(解析+帧累积) -> [MPMC 深度4] -> encoder×4(2bit打包+measure+校验和+zstd×3)
      -> [MPMC 深度4] -> writer(ReorderBuffer 按帧id排序 -> 帧头拼装+写盘，纯I/O)
解压: 纯顺序(ArchiveReader::readFrame + writeFastqRecord 单线程)
```

`jthread`+`stop_token` 协作取消；MPMC 为 mutex+CV 有界环形缓冲（带 relaxed 计数器，阶段E）；在途帧上界 12。当前串行段：reader(解析，wall ~33%) 与 writer(写盘，~5%)——zstd 已于阶段 F 下沉 worker 池。WSL2 吞吐波动 ±20-85%，一切性能结论以阶段 E 的 A/B 同窗口平台为据。

## 阶段

### A. 阻塞同步替换 yield 轮询 ★★★

- 状态：完成
- 问题：`spsc_queue.h` 的 `push`/`pop` 在满/空时 `yield` 自旋，生产消费速度不匹配时 CPU 空转。
- 练手点：`condition_variable`+`mutex`、虚假唤醒、shutdown 的 `notify_all`、CV+bool predicate 模式。
- 做法：单文件重写 `SpscQueue`，状态用 mutex 保护，`push` 满则 `wait(notFull)`、`pop` 空则 `wait(notEmpty)`，`close`/`abort` 唤醒所有等待者。顺带简化原 atomic 版本的 stale-head 终检（mutex 保证一致快照）。
- 验证：`clang-tsan` 无竞争；`test_performance` 对比 CPU 占用。
- 陷阱：notify 在解锁后；wait 用 predicate 版本防虚假唤醒；别持锁做 `std::move` 后再 notify。
- 选 CV+mutex（非 semaphore）理由：CV+shutdown 是并发最经典难题，且 predicate wait 与 B 阶段 stop_token 集成自然。

### B. jthread + stop_token 现代取消 ★★

- 状态：完成
- 问题：裸 `std::thread`+手动 `abort()` bool，靠主线程纪律保证 join。
- 练手点：RAII 线程、`stop_token` 协作式取消、`stop_callback`、与 A 的 CV 集成。
- 做法：`std::thread`->`jthread`，循环条件改 `!stop_token.stop_requested()`，`request_stop()` 触发 CV 唤醒。
- 验证：writer 中途失败用例，reader 经 stop_token 干净退出。

### C. 3-stage 流水线 ★★

- 状态：完成
- 问题：`writer.writeFrame` 把 measure+encode+zstd+checksum+写盘混在一处，CPU 密集 encode 与 I/O 密集写盘未分离。
- 练手点：多级背压传递、中间 stage shutdown 双向传播（q1 close -> 中间 stage -> q2 close）、何时抽泛型 stage。
- 做法：拆 `reader -> encoder(2bit打包+measure) -> compressor(zstd+xxh64+写盘)`，两条 SPSC。
- 验证：`test_performance` 看 encode/zstd 重叠收益，揭示新瓶颈（往往写盘 I/O）。
- 陷阱：别预先写泛型 `Stage<In,Out>`，先手写第二个 stage；别破坏帧内存有界不变量。

### D. 多帧并行编码 + reorder ★★★★

- 状态：完成
- 动机：C 后写盘成瓶颈，但帧边界天然独立（见 ARCHITECTURE.md），编码可并行。
- 练手点：MPSC 队列、reorder buffer（乱序完成按 frame id 有序提交）、乱序下内存有界、work distribution。
- 做法：`reader -> [MPMC] -> N encoder -> [MPMC] -> writer(reorder 按序写)`。MpmcQueue（mutex+CV+stop_token）两端复用，ReorderBuffer 按 frameId 有序提交，N 固定默认 4，不引线程池。
- 验证：`clang-tsan` 10/10 无竞争；64MiB random 压缩比与 C 完全一致（正确性无损）；在途帧上界 = 队列深度×2 + N = 12 帧，maxRSS 远低于预算。**未达多核近线性提升**：encoder 非当前瓶颈（reader 单线程解析、writer 单线程 zstd+IO，Amdahl），D 揭示新瓶颈为单线程两端；WSL2 吞吐波动 ±20-85% 淹没代码差异。
- 陷阱：MPSC 先 mutex+CV 版本，别钻 lock-free CAS（✓）；reorder 窗口设上限反压（✓ 上游队列深度即上限）。

### E. 可观测性与可靠 benchmark 平台 ★★（硬依赖，排第一）

- 状态：完成（commit 8af51ed）
- 动机：阶段 D 复盘教训二"并发优化前先 profiling"；WSL2 波动 ±20-85% 使单次数字不可信；下文"贯穿"节承诺的计数器在 `mpmc_queue.h` 中至今未实现——E 是补旧账 + 建底座。F 的预期收益可能就在环境噪声量级内，没有 E 则 F/G/H 全部无法可信验证。
- 练手点（新并发知识，不与 A-D 重复）：**原子操作与内存序**——`memory_order_relaxed` 统计计数器的唯一正当用例；低侵入观测（计数不拖慢热路径）。
- 做法：
  1. `MpmcQueue` 加 relaxed 原子计数器：push/pop 次数、阻塞等待次数、高水位，run 结束打印。
  2. 分段计时：reader/encoder/writer 各 stage 用 `steady_clock` 累计墙钟占比（按帧采样，不在 push/pop 热循环内取时钟），直接产出 Amdahl 证据，替代复盘里的间接推断。
  3. benchmark 脚本加固（`tests/e2e/test_performance.sh` 基础上，已有 median-of-3/jsonl/round-trip cmp/SLA）：warmup 轮、`FQC_PERF_REPEATS` 提到 ≥5、方差/极差报告、**同状态 A/B 模式**（两个二进制路径对比，把阶段 D 复盘"切代码同状态重跑"方法论工具化）、结果归档 `perf-baselines/YYYY-MM-DD-stage-x/`。
- 验证（实测）：clang-debug/tsan 9/9 无竞争；A/B 同窗口（基线 vs 阶段E，64MiB random ×5）压缩比 2.9588/2.8403 完全一致，压缩 delta +5.0%/0%、解压 0%/+6.6%——spread 区间高度重叠，差异在噪声内，无可见回退（✓ <2% 门槛）。同二进制 A/B delta 0-3%，平台可区分代码差异与环境噪声的阈值 ≈ ±5%（64MiB 配置）。基线归档 `perf-baselines/2026-07-27-stage-e/`。
- 陷阱：计数器别用 seq_cst 默认序（拖慢热路径）；A/B 模式禁止跨时比基线（复盘教训三）。
- 意外收获：极差报告上线即抓到 `date +%s.%N`（CLOCK_REALTIME）在 WSL2 对时回跳产生的负时长样本——旧 median 一直静默吸收它；计时源已改 `/proc/uptime`（CLOCK_BOOTTIME）。
- 首批 Amdahl 证据（64MiB random，wall 536ms）：reader parse 121ms（23%）、writer zstd+io 109ms（20%）、encoder 并行后 ~101ms/worker——串行两端合计 ~43%，为 F/H 的排序提供量化依据。注意 profile 采样（≤5 万条）在主线程完成、不计入 reader 段，小输入时 reader 占比被低估。

### F. zstd 下沉到 encoder worker ★★★

- 状态：完成（commit 3629f6d）
- 动机：阶段 D 复盘确认 writer（单线程 zstd+IO）是两瓶颈之一；E 的分段计时给出量化占比。zstd 帧间独立天然可并行（每帧×每流独立 `ZSTD_compress`，`archive.cpp:337-347`）；下沉后 writer 退化为 reorder+写盘。
- 练手点：**工作粒度再平衡**——把 CPU 密集工作从串行段迁入已有 worker 池后的背压再分析；**确定性验证方法论**（见验证）。
- 做法：`compress()` 提出匿名命名空间（或新 `compressFrame` free 函数）；`EncodedFrame`（`archive.h:73-80`）改携压缩后流（或新 `CompressedFrame`），queue2 载荷从 raw 变 compressed；writer 只做 reorder + 帧头拼装 + 写盘。内存口径：worker 侧 `ZSTD_compressBound` scratch ×N 计入预检；在途帧 12 上界不变，单帧峰值不变，maxRSS 预期不升（queue2 载荷变小）。
- 验证（实测）：**归档逐字节一致 ✓**——基线（a02f694）与本提交同输入 .fqc cmp 零差异（illumina/ont × 16GiB/64MiB 两档内存限制，四组全等）。writer 段趋近纯 IO ✓——zstd+io 从 wall ~20% 降为 write ~4.6%。tsan 9/9 ✓。A/B 同窗口 delta 在噪声内（压缩 +1.5%/-2.5%）——无确定性提速，符合预期：64MiB 仅 ~2 帧，4 worker 只有 2 个有活干，架构收益是 writer 不再随帧尺寸线性增长（大输入/多帧场景受益）。
- 陷阱复盘：①"预检乘 N"证实**不需要**——`estimateCompressionPeak` 每帧已含 raw+bound，N worker 并发 bound scratch 被在途 12 帧上界覆盖；实测 maxRSS +10-18%（~216MiB，远低于 16GiB 预算），系 bound scratch 从 writer×1 变 worker×N 的预期代价。②zstd 失败复用 encoderError + request_stop ✓。③逐字节门禁一次通过，未出现可归因差异。
- 注意：codec ID 不变（zstd 帧自描述，解压与 level 无关），格式零破坏 ✓。

### I（可选番外）. 轻量压缩比：per-stream zstd level ★★

- 状态：完成（commit 87562df）
- 动机：调研报告 §6 差距①（质量流零变换，见 `docs/fastq-compression-survey.md`）+ ALGORITHM.md:172；紧随 F——zstd 调用点刚动过，E 平台立即可判门槛。
- 练手点：无新并发知识——定位为**准入门槛制度练手**（CODEC_GATES 理念轻量复活，见 v1 复盘 :50 与 ARCHITECTURE.md:61-62）。
- 做法：质量流 zstd level 独立参数（扫描 3-7），ID/序列保持 1；`ArchiveOptions` 加字段；codec ID 不变、解码零改动、格式零破坏。门槛建议：压缩吞吐回退 ≤10% 且 ratio 改善 ≥3% 才合入默认值，否则只留为 CLI 选项。
- 门槛结果：**无档位过门槛，默认保持 level 1**——准入制度按设计发挥了"阻止未经度量合入"的作用。拟真质量（Q38-41 集中）L7/L9 体积 -3.3%/-4.0% 但吞吐 -43%/-53%，L19 体积 -10.6% 但吞吐 -97%；11 符号随机质量 L3-L9 体积反而 +3-7%。`--quality-level`（1-19）仅作 CLI 实验入口。完整曲线见调研报告 §6.4（两份文档联动 ✓）。
- 验证（实测）：默认 level=1 与阶段F基线归档逐字节一致（cmp 零差异）；level 3-19 round-trip cmp 与 verify 全通过；单测证明高层级只影响质量流载荷（ID/序列载荷跨层级逐字节一致）；clang-debug/tsan 9/9。
- 明确排除：跨帧字典（破坏帧独立）、任何重排序（v1 坟场）、自研 AC/range coder（复杂度）、有损分箱（无损硬约束）——本轮均未触碰 ✓。

### G. 解压路径流水线化 + 泛型 stage 抽取 ★★★

- 状态：完成（commit 6fae4a5）
- 动机：解压纯顺序（`archive_engine.cpp:353-367`），98-122 MiB/s；帧解码（zstd 解压+decode）与 FASTQ 写出零重叠。reviews F-7 当年判"不做"理由是"吞吐非瓶颈"——在 E 有数据后重审。
- 练手点：**抽象时机**——MPMC+ReorderBuffer 第二次出现、"pop-process-push 循环"第 4-6 次手写，满足设计原则 4"第三个重复模式再抽泛型"；泛型 stage 的最小接口设计（In/Out/Handler + close/stop 语义），不造 pipeline 框架。
- 做法：`ArchiveReader::readFrame` 拆为"读帧头+载荷+预检"（reader 线程）与"zstd 解压+逻辑校验和+decodeRawStreams"（N decoder worker，乱序完成）；writer 线程 reorder 按帧 id 有序提交 → **滚动校验和 + writeFastqRecord**。`verify` 命令复用同一流水线（writer 换空 handler）——泛型 stage 的第一个复用者。落地形态：泛型面 = `RecordSink`（`std::function<VoidResult(vector<ReadRecord>)>`，逐帧按序、失败级联取消）；worker 循环保持手写。
- 验证（实测）：新旧二进制解压同一归档**输出逐字节一致** ✓（滚动校验和尾值一致的最强形式）；round-trip cmp（16GiB/64MiB 两档）✓；archive_test 全部原样通过（readFrame 组合 API 行为不变）✓；tsan 10/10 ✓；A/B 解压 +4.2%/+2.0%（噪声内，64MiB 仅 ~2 帧）；解压 maxRSS +21-42%（~134MiB，在途多帧的预期代价）。
- 陷阱复盘：①滚动校验和严格留在 writer 有序端 ✓（footer 校验在 join 后由 writer 侧累积值对比，严格性与顺序版一致）；②RawFrame/DecodedFrame 均 unique_ptr 跨队列 ✓；③预检在 readRawFrame 内完成后再入队 ✓；④泛型只抽 RecordSink，未造 DAG/调度器 ✓。额外发现并设计修正：footerData 不能由 writer 线程读（reader 写、跨线程无 join 屏障的共享会违反单写者不变量）——footer 校验整体移到主线程 join 后。
- 故障路径新单测 8 个：footer 校验和/ totals 篡改、载荷翻转不可静默、截断、空归档、sink 失败取消无死锁。

### H. 并行解析（限未压缩普通文件）★★★★

- 状态：未开始
- 动机：F 之后 reader（单线程解析+帧累积）是压缩路径唯一串行瓶颈（E 数据确认）。
- 练手点：**数据并行**（区别于 D 的任务并行）——字节块切分 + 记录边界对齐协议 + 有序重组（复用 frame id/reorder）；条件启用策略（输入类型分派）。
- 做法：输入为未压缩普通文件时启用——fstat 得大小 → 等分 K 块 → 每 worker 从块起点扫描定位下一条完整记录（4 行结构验证：seq 长度==qual 长度、'+' 行）→ 解析累积成帧 → 帧 id 按块序分配 → reorder 保序。.gz/stdin/双端输入走现有单 reader 路径（范围裁剪：双端 R1/R2 锁步在并行下复杂度爆炸，不做）。profile 采样保持在主线程，并行解析从采样终点字节偏移接续。
- 验证：同输入归档与单 reader 版逐字节一致（块边界对齐协议的强检验）；含 '@' 出现在质量行的对抗 fixture（构造质量行恰好以 `@` 起始的记录）；tsan；E 平台量吞吐。
- 陷阱：①'@' 歧义——不能找行首 '@' 就当记录起点，必须 4 行结构回验，失败续扫；②采样终点偏移与块切分的衔接；③退化路径覆盖测试（gz/stdin/paired 全部走旧路径且行为不变）。
- 范围纪律：gzip 流式输入无法随机切块（`GzipStreamBuf` 纯 inflate 流）。**inflate/parse 分离设计（gz 下 inflate 串行、parse 扇出）明确不做**——除非 E 的分段计时证明 gz 路径 parse 占比 > inflate 占比，否则属投机优化。benchmark 脚本恰生成未压缩文件，H 的收益在平台上可直接量化，不构成"只为 benchmark 优化"。

## 贯穿：量化与可观测

每阶段给队列加 `memory_order_relaxed` 原子计数器（push/pop 次数、阻塞次数、高水位），测试时打印。并发调优无观测即盲调。计数器用 relaxed 序，别拖慢热路径。

注：计数器在 A-D 阶段未兑现（`mpmc_queue.h` 无计数器），已由阶段 E（commit 8af51ed）统一补上并工具化。

## 优先级

A -> B -> C -> D（已完成，同步底座与多级流水线）-> E -> F -> I -> G -> H。

- E 是 F/G/H/I 的验证底座（WSL2 波动下无测量即盲调），必须最先。
- F 验证成本最低（归档逐字节一致门禁）、直击已确认瓶颈；I 依赖 F 落定后的 zstd 调用点。
- G 低风险稳赢（解压侧当前零重叠），产出的泛型 stage 供 H 可选复用；H 最难放最后。
- 弹性条款：若 E 数据显示 reader 占压缩路径 >60%，允许 H 提前到 G 之前——排序决策交给 E 的数据（呼应"先 profiling"教训）。

## 不推荐

- **TBB flow graph / 线程池**：替你管调度，练不到手。
- **lock-free MPSC（复杂 CAS）**：练手阶段 mutex 版足够，无锁是另一课题。
- **异步 I/O（io_uring/aio）**：引入平台复杂度，偏离并发流水线主题。
- **过早泛型 pipeline 框架**：预先造必返工。
- **自研熵编码器（算术编码/range coder/手写 FSE）**：复杂度与验证成本超练手预算，熵编码交给 zstd（v1 SCM 已删，见 `docs/fastq-compression-survey.md` §7）。
- **read 重排序 / 全局相似聚类（SPRING 路线）**：v1 坟场——重排序/编码器状态/内存核算耦合错位，内存上限无法强制执行（v1 复盘根因）。
- **跨帧 zstd 字典**：破坏帧独立性——帧独立是阶段 D 并行编码的切分点（ARCHITECTURE.md:97）。
- **有参压缩（参考基因组）**：项目定位外。
- **gz 输入的 inflate/parse 分离并行**：除非阶段 E 数据证明 gz 路径 parse 占比超 inflate，否则不做（反投机）。

## 进度

| 阶段 | 状态 | commit |
|---|---|---|
| A | 完成 | 94a16d4 |
| B | 完成 | bafcd79 |
| C | 完成 | 965c084 |
| D | 完成 | abca33e |
| E | 完成 | 8af51ed |
| F | 完成 | 3629f6d |
| I（番外） | 完成 | 87562df |
| G | 完成 | 6fae4a5 |
| H | 未开始 | — |
