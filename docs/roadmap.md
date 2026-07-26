# 开发路线图

fq-compressor 并发流水线练手路线。定位：业余练手 C++ 并发流水线，追求轻灵巧、易维护、不引重框架。

## 设计原则

1. **一次一个并发知识点**：同步原语、取消、多级、并行分阶段练，不混。
2. **每步可验证可回退**：`clang-tsan` 验无竞争，`tests/e2e/test_performance.sh` 量化吞吐，每步独立 commit。
3. **不引重框架**：排除 TBB / boost.asio / 线程池 / 异步 I/O 孤岛。
4. **抽象克制**：泛型 stage 等有第三个 stage 重复模式再抽。

## 当前基线

`reader`(解析+帧累积) -> SPSC(深度4) -> `writer`(编码+zstd+xxh64+写盘)。`yield` 轮询、`std::thread`+`join`、`close`/`abort` 双 bool shutdown。SPSC 已修过丢尾帧竞态。

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

- 状态：未开始
- 问题：裸 `std::thread`+手动 `abort()` bool，靠主线程纪律保证 join。
- 练手点：RAII 线程、`stop_token` 协作式取消、`stop_callback`、与 A 的 CV 集成。
- 做法：`std::thread`->`jthread`，循环条件改 `!stop_token.stop_requested()`，`request_stop()` 触发 CV 唤醒。
- 验证：writer 中途失败用例，reader 经 stop_token 干净退出。

### C. 3-stage 流水线 ★★

- 状态：未开始
- 问题：`writer.writeFrame` 把 measure+encode+zstd+checksum+写盘混在一处，CPU 密集 encode 与 I/O 密集写盘未分离。
- 练手点：多级背压传递、中间 stage shutdown 双向传播（q1 close -> 中间 stage -> q2 close）、何时抽泛型 stage。
- 做法：拆 `reader -> encoder(2bit打包+measure) -> compressor(zstd+xxh64+写盘)`，两条 SPSC。
- 验证：`test_performance` 看 encode/zstd 重叠收益，揭示新瓶颈（往往写盘 I/O）。
- 陷阱：别预先写泛型 `Stage<In,Out>`，先手写第二个 stage；别破坏帧内存有界不变量。

### D. 多帧并行编码 + reorder ★★★★

- 状态：未开始
- 动机：C 后写盘成瓶颈，但帧边界天然独立（见 ARCHITECTURE.md），编码可并行。
- 练手点：MPSC 队列、reorder buffer（乱序完成按 frame id 有序提交）、乱序下内存有界、work distribution。
- 做法：`reader -> [MPSC] -> N encoder -> [reorder] -> writer 按序写`。N 固定，不引线程池。
- 验证：`test_performance` 多核近线性提升；reorder 内存峰值不超预算；`clang-tsan` 无竞争。
- 陷阱：MPSC 先 mutex+CV 版本，别钻 lock-free CAS；reorder 窗口设上限反压。

## 贯穿：量化与可观测

每阶段给队列加 `memory_order_relaxed` 原子计数器（push/pop 次数、阻塞次数、高水位），测试时打印。并发调优无观测即盲调。计数器用 relaxed 序，别拖慢热路径。

## 优先级

A -> B -> C -> D。A 是其余同步底座，必须最先；D 依赖 A/B/C 全部正确性基础，放最后。

## 不推荐

- **TBB flow graph / 线程池**：替你管调度，练不到手。
- **lock-free MPSC（复杂 CAS）**：练手阶段 mutex 版足够，无锁是另一课题。
- **异步 I/O（io_uring/aio）**：引入平台复杂度，偏离并发流水线主题。
- **过早泛型 pipeline 框架**：预先造必返工。

## 进度

| 阶段 | 状态 | commit |
|---|---|---|
| A | 完成 | 94a16d4 |
| B | 未开始 | - |
| C | 未开始 | - |
| D | 未开始 | - |
