# Agent Note: 收口帧提交与遥测快照

Status: implemented

## Problem

三条帧流水线（`CompressPipeline`、`ParallelParsePipeline`、`DecompressPipeline`）
各自重复两段控制流：

1. **取 credit → push → 失败归还**。每个 source 都必须先 `InFlightLimiter::acquire`，
   再 `queue.push`；push 因取消或队列关闭失败时，必须立刻 `release` 同一个 credit。
   这段逻辑在三条 pipeline 中出现三次（并行解析还多一个 owner 参数）。漏掉失败分支
   不会立刻报错，而是让 in-flight 窗口永久少一个 credit；低内存下表现为吞吐下降或
   写端等待，很难从日志定位。
2. **telemetry 快照**。每条 pipeline 在 join 后手工赋值 `timings`、两个 queue 的
   `stats()` 和 `inFlightHighWater`。`PipelineTelemetry` 新增字段时，容易只改其中
   一两条路径，导致日志口径分叉。

## Decision

新增两个小工具，不做通用流水线框架：

- `pushWithCredit(limiter, queue, item, st)`：普通 credit；
  `pushWithCredit(limiter, owner, queue, item, st)`：owner-reserved credit。
  语义固定为“acquire 成功后 push；push 失败立即归还；成功则由调用方在有序提交后
  release”。两者都放在 `in_flight_limiter.h`，与 credit 的生命周期定义相邻。
- `makeTelemetry(timings, inputQueue, outputQueue, limiter)`：在
  `pipeline_telemetry.h` 中集中构造 `PipelineTelemetry`。契约是“所有 stage 已 join
  后调用”，与 `QueueStats` / `highWater()` 的文档一致。

三条 pipeline 改用这两个 helper。`CompressPipeline` 和 `DecompressPipeline` 的
writer reorder 循环保持手写：两处重复不足以抵消参数膨胀，且两边的逐帧工作
（写盘 vs 统计+滚动校验和+sink）不同，抽象后反而更难读。

## Alternatives considered

- **抽 `drainReordered` 统一 writer 循环**：Compress/Decompress 只有两处，且需要
  同时传入 queue、reorder、limiter、错误槽、stop_source、计时引用和逐帧回调；
  参数超过可读阈值，停止/排空语义也会被藏进 helper。保留手写循环，让两个 writer
  的差异直接可见。
- **RAII credit guard**：正常路径的 release 发生在 writer 有序提交之后，而 push
  失败路径在 source 线程立即 release；一个 guard 对象无法跨线程安全表达这两个时机，
  会引入悬空引用或额外的共享状态。
- **通用 `StageGraph` / `RecordSource`**：当前只有一种 record source（FASTQ
  parser）和一种 writer 输出（ArchiveWriter + sink），没有第三个实现；按项目
  “第三个重复模式再抽”的原则，属于过早泛化。
- **把 `InFlightLimiter` 改成 push 的成员**：queue 与 limiter 的职责不同，且
  ParallelParsePipeline 的 owner 语义只在 source 侧成立；合并会让 queue 承担预算
  知识，破坏当前分层。

## Consequences

- 收益：credit 归还的失败路径只实现一次；telemetry 结构新增字段只改
  `makeTelemetry`。三条 pipeline 的 source 段各少 4–6 行样板。
- 代价：`pipeline_telemetry.h` 新增对 `in_flight_limiter.h` 的 include；
  `in_flight_limiter.h` 不反向依赖，无循环。`makeTelemetry` 是模板，编译期实例化，
  没有运行时开销。
- 行为不变：credit 窗口、帧顺序、归档字节都不变。取消路径上 item 可能在 acquire
  前已由调用点构造（函数参数求值），但不会入队；closed 随即析构，frameId 自增无
  副作用。
- 新增 4 个 limiter 单测：普通/owner 的 push 失败归还、取消不取 credit、成功路径
  在显式 release 前保持占用。

## Verification

- `clang-debug`：19/19 通过；
- `clang-format-18`：通过；
- 后续以 `clang-asan` / `clang-tsan` / CI 为准（并发路径改动必须过 TSan）。
