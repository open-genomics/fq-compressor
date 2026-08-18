# fq-compressor 并发流水线开发历程

业余练手项目，目标：练手 C++ 并发流水线编程，追求轻灵巧、易维护，不引重框架。
本文串起从架构修复到阶段 H 收束的开发脉络，详细阶段计划见 `docs/roadmap.md`，问题复盘见
`docs/postmortems/`，真实语料验收见 `docs/real-corpus.md`。

## 定位与约束

- **业余练手**：练并发流水线，不是产品。排除产品 closeout 流程与重框架。
- **不引重框架**：TBB / 线程池 / 异步 I/O（io_uring）/ lock-free MPMC / 过早泛型 pipeline 框架，一概不做。
- **一次一个并发知识点**：同步原语、取消、多级、并行分阶段练，不混。
- **每步可验证可回退**：`clang-tsan` 验无竞争，`test_performance.sh` 量化，每步独立 commit。

## 起点：架构修复（2026-07-24 评审后）

架构评审发现 5 项问题并全部修复（commit 8e3dd95、3739f9b）：README 死链与产品措辞、
compressed_stream 多格式过度承诺、并发同步缺注释、采样/读取逻辑重复、散落未跟踪文档。

修复后确立基线：`reader`(解析+帧累积) -> SPSC(深度4) -> `writer`(编码+zstd+xxh64+写盘)，
`yield` 轮询 + 裸 `std::thread` + `close`/`abort` 双 bool shutdown。SPSC 已修过丢尾帧竞态。

## 路线图 A -> H

| 阶段 | 知识点 | commit |
|---|---|---|
| A | condition_variable + mutex 阻塞同步替 yield | 94a16d4 |
| B | jthread + stop_token 协作式取消 | bafcd79 |
| C | 3-stage 流水线，encode 与 zstd/IO 分离 | 965c084 |
| D | 多帧并行编码 + reorder buffer | abca33e |
| E | relaxed 计数器 + 分段计时 + A/B benchmark | 8af51ed |
| F | zstd 下沉到 encoder worker | 3629f6d |
| I | 质量流 per-stream zstd level（未过门槛） | 87562df |
| G | 解压流水线 + RecordSink | 6fae4a5 |
| H | 未压缩文件并行解析 + ChunkOrderer | a50c0ce |

### A：阻塞同步（CV + mutex）

`push`/`pop` 满/空时 `yield` 自旋改为 `condition_variable` + `mutex` + predicate `wait`。
`close` = drain（`notify_all` 唤醒所有等待者）。陷阱：notify 在解锁后；`wait` 用 predicate
版本防虚假唤醒。选 CV+mutex（非 semaphore）：CV+shutdown 是并发最经典难题，且 predicate
wait 与 B 阶段 stop_token 集成自然。

### B：现代取消（jthread + stop_token）

裸 `std::thread` + 手动 `abort()` bool 改为 `std::jthread` + `stop_source`。`push`/`pop`
加可选 `std::stop_token` + `condition_variable_any`（stop 时经 CV 唤醒，不自旋）。`close`
（drain）与 `stop`（cancel）分离。环境升级 libc++ 19 -> 21 解锁 jthread（Ubuntu libc++-19
禁用了 stop_token，`__cpp_lib_jthread` 未定义）。

### C：3-stage 流水线

`writer.writeFrame` 拆成 free `encodeFrame`（measure + 2-bit 打包 + checksum，CPU 密集）+
`ArchiveWriter::writeEncodedFrame`（zstd + 写盘，I/O 密集）。`reader -> q1 -> encoder -> q2 ->
compressor`，shutdown 链式 close（reader close q1 -> encoder 见 nullopt close q2 -> compressor
见 nullopt）。揭示：encode 与 zstd 可分离，写盘成新瓶颈。

### D：多帧并行编码 + reorder

`reader -> [MPMC] -> N encoder 并行 -> [MPMC] -> writer(reorder 按帧序写)`。新增
`MpmcQueue`（mutex + `condition_variable_any` + `stop_token`，多生产多消费）与
`ReorderBuffer`（乱序完成按单调 `frameId` 有序提交）。`frameId` 由 reader 单线程分配，
reorder 恢复顺序，保证 archive 的单调帧 ID 契约（磁盘帧 ID = writer 的 `stats_.frameCount`，
解码侧校验递增）。`encoderError` 用 mutex 保护（N encoder 竞争）；join 顺序
`reader -> encoders -> close queue2 -> compressor`。

### E–H：测量、下沉、镜像、数据并行

D 之后按测量推进，不再猜瓶颈：

- **E** 补上 relaxed 队列计数器、分段计时和 A/B 同窗口脚本，把 WSL2 噪声量化成约 ±5% 的可分辨阈值。
- **F** 把 zstd 从 writer 迁入 encoder worker，writer 退化为纯 I/O；归档与下沉前逐字节一致。
- **I** 质量流单独提档，拟真与随机质量上都未过「体积 ≥3% 且吞吐回退 ≤10%」门槛，默认保持 level 1。
- **G** 解压做成压缩的镜像三段流水线；`verify` 换空 `RecordSink` 复用同一条路径。
- **H** 对未压缩普通文件做字节块并行解析。短读压缩 +47.7%；长读解析占比小，收益落在噪声内。对齐协议曾静默丢弃非首块畸形记录，已按 critical 复盘修好。

## 性能结论

N=4 并行编码**未带来近线性提升**。诊断（详见 `docs/postmortems/2026-07-26-stage-d-parallel-encode-no-speedup.md`）：

- 表层：WSL2 吞吐波动 20-85%（同代码同配置两次跑差一倍），单次数字不可信。
- 深层：Amdahl——当时 encoder 非瓶颈。reader（单线程解析）与 compressor（单线程 zstd+IO）才是。

后续阶段按这个诊断收口：F 拿掉 writer 侧 zstd，H 并行化未压缩解析。短读路径的收益能从 A/B 同窗口里分开；长读和 gzip 不在 H 的范围内。合成数据压缩比在 H 后仍约 2.96× / 2.84×（illumina / ont）；真实语料见 `docs/real-corpus.md`。

## 验证方法

每阶段：clang-debug 全绿 + **clang-tsan 无竞争**（核心）+ clang-format + `test_performance.sh`
基线。不稳定环境下用"验证三角"判无回归：

1. 正确性：压缩比一致（reorder 不改内容）。
2. 并发正确性：clang-tsan 无竞争。
3. 性能同态对比：切代码同状态重跑（D-NOW vs C-NOW），而非跨时比基线（排除机器漂移）。

## 收获

1. 一次一个并发知识点，每步 tsan 验证可回退。
2. 并发优化前先 profiling--盲目并行非瓶颈 stage 会落空（Amdahl 教训）。
3. WSL2 不适合精细性能归因，吞吐波动淹没代码差异。
4. 抽象克制：有第三个重复模式再抽泛型，别预先造框架。
5. 不稳定环境靠多证据交叉判无回归，不靠单次性能数字。
6. 课程有终点：A–H 收束后不再为了练而叠阶段；下一步是真实语料验收，而不是阶段 J。
