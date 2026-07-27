# fq-compressor 并发流水线开发历程

业余练手项目，目标：练手 C++ 并发流水线编程，追求轻灵巧、易维护，不引重框架。
本文串起从架构修复到阶段 D 的开发脉络，详细阶段计划见 `docs/roadmap.md`，问题复盘见
`docs/postmortems/`。

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

## 路线图 A -> B -> C -> D

| 阶段 | 知识点 | commit |
|---|---|---|
| A | condition_variable + mutex 阻塞同步替 yield | 94a16d4 |
| B | jthread + stop_token 协作式取消 | bafcd79 |
| C | 3-stage 流水线，encode 与 zstd/IO 分离 | 965c084 |
| D | 多帧并行编码 + reorder buffer | abca33e |

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

## 性能结论

N=4 并行编码**未带来近线性提升**。诊断（详见 `docs/postmortems/2026-07-26-stage-d-parallel-encode-no-speedup.md`）：

- 表层：WSL2 吞吐波动 20-85%（同代码同配置两次跑差一倍），单次数字不可信。
- 深层：Amdahl--encoder 非当前瓶颈。reader（单线程解析）与 compressor（单线程 zstd+IO）才是，
  encoder 占比小，N 路并行收益有限。D 揭示新瓶颈在单线程两端。

压缩比与 C 完全一致（illumina 2.9588 / ont 2.8403），正确性无损。

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
