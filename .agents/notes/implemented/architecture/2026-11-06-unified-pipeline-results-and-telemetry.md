# Agent Note: 统一流水线结果与遥测模型

Status: implemented

## Problem

三条流水线各自返回一套统计结构：

- `CompressPipeline` / `ParallelParsePipeline` 返回 `pipeline::PipelineStats`；
- `DecompressPipeline` 返回 `pipeline::DecompressStats`；
- 两者又各自重复声明 `frameCount` / `recordCount` / `totalBases` / `encodedBytes`，
  与 `format::ArchiveStats` 是同一组概念的第三、第四份副本。

结果是命令层需要写 `toArchiveStats` 做手工翻译，压缩侧有阶段计时和队列快照，
解压侧只有计数与 in-flight 高水位。新增一个 pipeline 或一个观测项时，必须记得改
所有结构体和转换函数；字段语义也容易漂移。

## Decision

引入 `include/fqc/pipeline/pipeline_telemetry.h`，把流水线结果收敛为：

- `format::ArchiveMetadata metadata`；
- `format::ArchiveStats archive`（与 writer/reader 使用同一类型）；
- `PipelineTelemetry telemetry`：
  - `StageTimings`：`readerWorkNs`、`readerPushNs`、`workerPopNs`、
    `workerPrimaryNs`、`workerSecondaryNs`、`workerPushNs`、`writerPopNs`、
    `writerWorkNs`、`wallNs`；
  - `inputQueueStats` / `outputQueueStats`；
  - `inFlightHighWater`；
- `logicalBytes`：规范 FASTQ 字节数。

`PipelineStats` 与 `DecompressStats` 继承 `PipelineRunStats`，只追加各自的 worker 数量。
阶段名保持中性：压缩把 encode 放入 `workerPrimaryNs`、zstd 放入 `workerSecondaryNs`；
解压把 decode 放入 `workerPrimaryNs`，`workerSecondaryNs` 为 0。

字段名表达方向角色而不是位置序号：`inputQueueStats` / `outputQueueStats` 直接对应
reader -> worker / worker -> writer，`workerPrimaryNs` 与 `workerSecondaryNs` 对称。
日志也使用 input/output，不再要求读者记住 q1/q2 的方向。

`logicalBytes` 由流水线自己累计：压缩在 reader 侧，解压/verify 在 writer 侧对每条
记录调用 `canonicalFastqBytes`。命令层不再各自数一遍。

压缩 `run` 成功时负责调用 `ArchiveWriter::finish()`（写 footer + flush），然后才
快照 `archive`；这样 `PipelineStats.archive.encodedBytes` 是完整归档，而不是“还差
footer”的中间值。`ArchiveEngine` 不再重复 finish。

## Alternatives considered

- **保留三套统计结构，只加转换函数**：改动最小，但重复字段和转换点继续存在，
  解压侧仍然没有阶段遥测，和“架构更清晰、框架更通用”的目标相反。
- **抽一个通用 DAG / stage 调度框架**：能进一步消除 worker 循环重复，但会引入
  调度器、节点生命周期和新的同步面；当前只有固定的三段帧流水线，收益不足以
  抵消概念成本，也违背 `docs/roadmap.md` 的“抽象克制”。
- **让 pipeline 只返回 telemetry，归档计数仍由命令层从 writer 读取**：可以避免
  `run` 负责 finish，但调用方必须知道“何时读 stats 才完整”，结果模型不闭合；
  让一次成功的 run 负责完整归档更直接。
- **为解压单独定义 `DecoderTimings`**：字段更贴切，但会让两条方向继续分叉；
  中性字段加注释已经足够表达 encode/decode 的差异。

## Consequences

- 命令层删除 `toArchiveStats`，`OperationStats` 直接消费 canonical `ArchiveStats`；
- 解压/verify 也输出阶段计时、队列计数和 in-flight 高水位，排障信息与压缩对称；
- 压缩流水线多承担一次 `finish()` 的所有权，但换来了“成功 run = 完整归档”的清晰契约；
- `StageTimings` 字段名变成阶段中性，原有 `encoderEncodeNs` 等名字不再存在；
  测试和日志已同步；
- 归档字节布局不变：`finish()` 只是从命令层提前到 pipeline 的 join 之后，
  footer 内容与顺序完全相同；
- 遥测字段从位置命名改为角色命名，调用方和日志的阅读成本更低；这是源码级契约变更，
  项目不承诺对外 API/ABI 兼容。

## Verification

- `clang-debug`：17/17 通过；
- `clang-release`：17/17 通过；
- `clang-asan` / `clang-tsan`：17/17 通过；
- 1 MiB realistic 端到端 benchmark：压缩/解压/verify/round-trip 通过；
- 解压测试新增断言：`decoderWorkers`、`logicalBytes`、两条队列计数、worker/writer
  阶段计时均非零且与 frameCount 对齐；
- 空输入测试确认：即使没有帧，成功 run 也会写出 header + footer。
