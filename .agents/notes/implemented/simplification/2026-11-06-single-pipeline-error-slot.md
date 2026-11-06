# Agent Note: 全流水线唯一首个错误槽

Status: implemented

## Problem

三条流水线各自维护 `readerError` / `encoderError`（或 `decoderError` / `parseError`）/
`writerError` 三个错误槽，返回时按固定阶段优先级检查：

```text
writer > encoder/decoder > reader/parser
```

这和 `stage_guard.h`、CHANGELOG 已经承诺的“首个错误 + request_stop”并不一致。只要
reader 先遇到格式错误、writer 随后因级联失败也写入错误，调用方看到的就是 writer 错误，
真正的根因被阶段优先级掩盖。三个槽还意味着三份返回分支和两份错误锁，新增阶段或调整
错误策略时要同时改三处。

## Decision

每条流水线只保留一个 `std::optional<Error> firstError` 和一把 `std::mutex errorMutex`：

- reader、worker pool、writer 的正常业务错误都调用
  `recordGuardedError(errorMutex, firstError, error)`；
- 线程级异常屏障统一使用 `runGuarded(..., firstError, errorMutex, ...)`；
- `startWorkerStage` 继续接收同一组 `errorMutex` / `firstError`，不新增接口；
- 返回时只检查 `firstError`，不再按阶段排序。

错误路径加锁，正常路径完全不加锁。错误不是热路径，mutex 比原子 `optional` 或错误队列
更简单，也更符合仓库“先写清楚，再谈无锁”的取舍。单写者 `recordGuardedError(slot, error)`
和 `runGuarded` 重载保留为底层原语与测试覆盖；生产流水线统一使用多写者版本。

## Alternatives considered

- **保留按阶段优先级**：改动最小，但“首个失败优先”只存在于文档，根因仍可能被级联
  错误覆盖；三组返回分支也继续存在。
- **全局错误队列**：能保留所有错误，但调用方只返回一个 `Error`，队列没有消费者；
  属于为完整性而完整性。
- **`std::atomic<std::optional<Error>>` 或原子错误指针**：C++23 没有可直接用的原子
  optional，手写原子指针还要管理生命周期；错误路径本来就不热，mutex 的简单性更重要。
- **让 `runGuarded` 接管错误槽生命周期**：会让异常屏障知道线程拓扑和返回路径，越过
  上一轮划定的边界；错误槽仍应由调用方拥有。

## Consequences

- 三条流水线共删除 6 个错误槽和 3 组优先级分支，返回路径只剩一个 `if`；
- 行为变化：跨阶段错误从固定优先级变为真正的首个失败优先；reader 先失败时返回 reader
  错误，worker 或 writer 的级联失败不再覆盖它；
- 错误路径共享一把锁，成功路径无锁；没有新增原子操作或热路径开销；
- 归档格式、线程拓扑、队列关闭与 join 顺序都不变；
- 新增测试覆盖多写者错误槽“已有错误不被覆盖”，以及压缩 reader 解析失败能上浮为
  `kFormatError`。

## Verification

- `clang-debug` / `clang-release` / `clang-asan` / `clang-tsan`：19/19 通过；
- 新增 `StageGuardTest.LockedVariantKeepsFirstError`：多写者槽已有错误时不被异常覆盖；
- 新增 `CompressPipelineTest.ReaderFailureReturnsFormatError`：坏 FASTQ 在 reader 阶段
  失败时返回 `kFormatError`；
- `clang-format-18` 通过；`scripts/notes.sh verify` 通过（15 篇）；
- 定向 clang-tidy（LLVM 19）项目路径 0 告警；
- 单帧 fixture 与 8 帧 3.996 MB 输入，改动前后 release 二进制生成的 `.fqc` 逐字节
  `cmp` 一致；解压 round-trip 与原 FASTQ 一致，`verify` 通过；
- 未跑性能 A/B：成功路径没有新增同步操作，归档逐字节一致；新增的 mutex 只在错误
  路径写入，不在热路径。用归档一致性和路径审查代替一次噪声很大的 WSL2 对比。
