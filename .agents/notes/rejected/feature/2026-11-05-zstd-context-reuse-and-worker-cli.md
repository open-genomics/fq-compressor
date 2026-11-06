# Agent Note: zstd context 复用与 encoder/decoder 并行度旋钮（回退）

Status: rejected — 端到端收益不稳定，CLI 旋钮还与 in-flight 不变量不一致

> 2026-10-27 后续：ARM64 适配独立引入了 decompress/verify 的 `--decode-workers`
> （0 = 默认 4），用于多核 ARM 解码调优；本笔记否定的是当时 compress/decompress
> 双侧旋钮及 `--encode-workers` 与 in-flight credit 的冲突，不覆盖该后续决定。

## Problem

曾尝试两项性能改动：

1. 在 `fqc::format` 增加 `CompressionContext` / `DecompressionContext`，让每个
   encoder/decoder worker 跨帧复用 `ZSTD_CCtx` / `ZSTD_DCtx`；
2. 增加 `compress --encode-workers`、`decompress/verify --decode-workers`，让
   encoder/decoder 池可独立调节。

## Proposal

- context 复用：`ZSTD_CCtx_reset(session_and_parameters)` + level +
  `ZSTD_CCtx_setPledgedSrcSize` + `ZSTD_compress2`；解压侧
  `ZSTD_DCtx_reset(session_only)` + `ZSTD_decompressDCtx`。
- 并行度旋钮：`0 = auto`，并行解析路径 `min(parseWorkers, 4)`，顺序路径 4，
  解压/校验 4。

## Alternatives considered

- **保留压缩侧 context、只回退解压侧**：破坏 API 对称性，后来者会问“为什么只有
  压缩有 context”；不是真正的简化。
- **只回退 CLI、保留两侧 context**：context 仍增加两个公开类型、两个重载和常驻
  内存核算；压缩端到端没有稳定收益，解压收益方向不一致。
- **固定 encoder/decoder 为 4**：默认路径最短，行为可复现；需要调优时再基于
  profiler 证据引入，而不是先加旋钮。

## Acceptance criteria

- 归档字节与 HEAD baseline 逐字节一致；
- 压缩/解压端到端有稳定、可重复的收益，且超过 A/A 噪声；
- 新增旋钮不改变并发不变量，CLI 语义一致，并有 e2e 解析测试。

## Risks

- **未达到收益门槛**：64 MiB realistic A/B 中压缩端到端差异落在 WSL2 噪声
  （约 ±2–5%）内；解压 Illumina +4.3%、ONT −4.3%，方向不一致。另一次 59 MiB
  8 次交错 A/B 的 user CPU 约 −6.7%，只能算可测但不稳定的小收益。
- **CLI 旋钮与 in-flight 窗口不一致**：`ParallelParsePipeline` 的
  `inFlightLimit` 仍是 `queue1 + queue2 + parserParallelism_`，
  `InFlightLimiter` 的保留 credit 也按 parser 数计算。默认
  `encoder <= parser` 时没问题；但 `--encode-workers 8 --parse-workers 4` 时
  8 个 encoder 不可能都稳定持有帧，实际并行度被 12 个 credit 的窗口压住。
  不会必然死锁（每个 parser 仍有保留 credit），但旋钮语义与实现不一致，
  8-worker 实测也不能代表完整 8 路 encoder 的上限。
- **CLI 语义冲突**：`--encode-workers 0` 表示 auto，而 `--parse-workers 0`
  表示 sequential；同一个 `0` 在相邻选项里含义不同。
- **API 不对称**：`decompress` 用 `DecompressionRequest.decodeWorkers`，
  `verify` 用独立位置参数，接口不统一。
- **内存预算未闭环**：worker 常驻 zstd context 没有纳入逐帧预检，尤其
  `--quality-level 19` 的 context 峰值未单独核算。

结论：两项都回退。最终只保留 parser 行缓冲、`sync_with_stdio(false)`、
realistic benchmark 默认和共享 `worker_error.h`；这些都是低概念成本、
容易解释的改动。
