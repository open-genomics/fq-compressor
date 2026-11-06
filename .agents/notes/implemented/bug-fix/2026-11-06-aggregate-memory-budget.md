# Agent Note: 聚合内存预算与采样边界

Status: implemented

## Problem

CLI 把 `--memory-limit` 描述为硬上限，README 也承诺“最低 64 MiB、不会 OOM”。但用一份
316 MB / 1M reads 的 150 bp FASTQ 实测，64 MiB 预算下峰值 RSS 为：

| 路径 | 修复前峰值 RSS |
|---|---:|
| 顺序压缩 | 71.8 MiB |
| 并行压缩 K=4 | 133.5 MiB |
| 并行压缩 K=8 | 200.5 MiB |
| 解压 | 92.5 MiB |

根因有三层：

1. `estimateCompressionPeak` / `estimateDecompressionPeak` 把 16 MiB 的
   `kCodecMemoryReserve` 加进**每一帧**的峰值。流水线同时有约 10 帧在途时，这个固定预留
   被乘了 10 次。
2. `encodeFrame` / `readRawFrame` 用整机 `memoryLimitBytes` 检查单帧峰值；
   `maxFrameBytes` 只限制单路 raw stream。也就是说，单帧能占满整机预算，多帧叠加没有
   上界。
3. Profile 采样只按碱基限流，ID/comment/quality 的持有字节没有计入；采样在流水线期间
   常驻，还会被复制进首帧。

修复过程中又发现一个更危险的采样边界 bug：第一版按持有字节限流时，代码在
`readRecordPair` 之后、加入采样之前判断“超预算就 break”。此时 parser 已经消费了这对
记录，并行路径却从采样终点继续，结果丢了一对记录。`verify` 仍然通过（归档自洽），但
decompress 的 round-trip 在 line 40721 与输入不一致。这属于“校验和正确但数据丢失”的
静默损坏，必须由“先加入、后停止”的纪律和端到端回归测试守住。

## Decision

format 层：

- `estimateCompressionPeak` / `estimateDecompressionPeak` 返回**帧本地峰值**，不含
  `kCodecMemoryReserve`；全局预留只由调用方计一次。
- `encodeFrame` / `readRawFrame` 同时检查：
  - 帧本地峰值 ≤ `maxFrameBytes`（逐帧预算）；
  - 帧本地峰值 ≤ `memoryLimitBytes - kCodecMemoryReserve`（单帧全局上界）。

command 层按实际流水线形态分摊预算：

```text
maxBufferedFrames = 2 × (queueDepth - 1) + workerCount + sourceLocalFrames
maxFrameBytes = (memoryLimit - 16 MiB - sampleHeldBytes) / maxBufferedFrames
targetFrameBytes = min(userTarget, maxFrameBytes / 4)
```

- `sourceLocalFrames` 在顺序压缩/解压为 1，在并行解析为 parser 数 K；它尚未进入
  `InFlightLimiter`，必须显式计入。
- `workerCount` 用实际值：顺序压缩 4，并行压缩 `min(K,4)`，解压为配置的 decoder 数。
- 目标帧大小取峰值预算的 1/4：`estimate*Peak` 约是 retained FASTQ 字节的 4 倍
  （记录字符串 + 原始流 + Zstd 输出上界 + vector 元数据）。

采样层：

- `recordHeldBytes` 按 `2 × sizeof(ReadRecord) + 字符串 capacity` 计费，覆盖 vector 的
  几何增长和实际分配容量。
- 采样预算改为持有字节预算，`kProfileSampleMaxBases` 同步更名为
  `kProfileSampleMaxBytes`。
- **先加入、后停止**：读到的记录必须进入 sample 或让整个操作失败，绝不能消费后丢弃。
- 最后一对记录允许越过采样预算；随后用
  `sampleBytes >= memoryLimit - reserve` 显式失败，保证它没有越过整个可用内存。

默认 16 GiB 下 `maxFrameBytes` 仍为 512 MiB、目标帧仍为 64 MiB，因此归档字节不变。
64 MiB 下目标帧缩到约 0.5–1 MiB。

## Alternatives considered

- **保留旧公式，只改文档说“预算是软上限”**：最省事，但 CLI 和 README 的硬上限承诺是
  这个项目的核心卖点；把 bug 写成文档等于放弃承诺。
- **直接用 magic divisor 缩小帧**：例如把 `/8` 改成 `/64`。能让 RSS 降下来，但无法解释
  为什么是这个数，换输入/换并行度就会再次失配。按最大同存帧数推导更清楚。
- **字节级 credit limiter**：按每帧实际峰值申请额度，比帧数窗口更精确；但 parser-local
  帧和 per-chunk 防饿死会重新引入上一轮已经解决的环形等待问题，复杂度和收益不成比例。
  当前“帧数窗口 + 保守逐帧预算”已经能守住 64 MiB，保留为后续可选。
- **采样超预算时回退流位置**：gzip/stdin 不可 seek，回退不可行；“先加入、后停止”更简单，
  且不会丢数据。
- **把 16 MiB 预留继续放在逐帧估算里**：会迫使我们把在途窗口压到 3 帧以下，队列和
  per-chunk 预留都无法成立；把它识别为全局预留才是正确模型。

## Consequences

- 低内存下帧更小、帧数更多，压缩比略降：64 MiB / 316 MB 输入的归档从 127,599,977 B
  增至 127,988,317 B（+0.30%）。这是用吞吐/体积换硬上限的明确取舍。
- 默认 16 GiB 行为与归档逐字节不变；性能路径没有新增原子或锁。
- 低内存下 profile 采样更早停止；150 bp 短读仍有约 1.7 万条，足够 profile 判定。
- `maxFrameBytes` 的语义从“单路 raw stream 上限”变为“帧本地峰值预算”；raw stream
  检查保留为廉价早退，但不再是唯一约束。
- 新增回归测试 `ArchiveEngineTest.SampleBoundaryPreservesAllRecordsAt64MiB`：
  20k reads / 64 MiB，跨越采样预算，验证 compress → decompress 逐字节一致。
- `kProfileSampleMaxBases` 更名为 `kProfileSampleMaxBytes`；这是 `fqc::commands` 的
  源码级常量，项目不承诺对外 API/ABI 兼容。

## Verification

- `clang-debug` / `clang-release` / `clang-asan` / `clang-tsan`：19/19 通过；
- `clang-format-18` 通过；`scripts/notes.sh verify` 16 篇通过；定向 clang-tidy
  （LLVM 19）项目路径 0 告警；
- 64 MiB / 316 MB / 1M reads 实测峰值 RSS：
  - 顺序压缩 18,632 KiB ≈ 18.2 MiB
  - 并行压缩 K=4 26,496 KiB ≈ 25.9 MiB
  - 并行压缩 K=8 30,068 KiB ≈ 29.4 MiB
  - 解压 17,500 KiB ≈ 17.1 MiB
- 64 MiB 归档 decompress 回原 FASTQ 逐字节一致，`verify` 通过；
- 默认 16 GiB 下，改动前后 release 二进制对同一 316 MB 输入生成的归档逐字节一致
  （127,599,977 B），decompress round-trip 一致；
- 未跑默认 16 GiB 的性能 A/B：成功路径的帧大小、编码路径和归档字节都没有变化，新增的
  只是命令层的常量计算；64 MiB 的帧缩小是有意的行为变化，不用它去推断默认路径性能。

## Pitfalls

- **采样消费后丢弃是静默数据损坏。** 只要 parser 已经前进，记录就必须被保留或让整个
  操作失败；不能用 `break` 把已消费的记录留在流水线之外。
- **校验和通过不等于 round-trip 正确。** 归档可以完全自洽，同时少一对记录；必须用
  原始输入的逐字节 `cmp` 作为门禁。
- **固定预留要区分“每进程”和“每帧”。** 16 MiB 的 codec/runtime 预留是每进程的；
  逐帧峰值只能计帧自己的内存。
- **预算公式要跟着流水线拓扑走。** 队列深度、worker 数和 source-local 帧数变了，
  `maxBufferedFrames` 必须同步变；不要留下一个与拓扑无关的 magic divisor。
- **剩余边界：** 单条记录本身超过预算时，parser 仍会先分配该记录；本轮没有把
  `maxLineBytes` 与 `--memory-limit` 联动。对正常 FASTQ 和 64 MiB 预算的实测已经守住，
  但“恶意单行正好等于 64 MiB 行上限”仍需后续处理。
