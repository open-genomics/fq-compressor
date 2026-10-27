# Agent Note: ARM64（AArch64）服务器适配

Status: implemented

## Problem

源码本身与架构无关（无 x86 intrinsics、显式小端序列化），但构建与运行配置默认面向
x86：Release 的 `-march=native` 在 AArch64 上只映射到 `generic`，不会选中宿主 CPU；
`conan/profiles/*` 写死 `arch=x86_64`；decompress/verify 的 decoder 池固定为 4，无法按
多核 ARM 的可用核数调节。21 核鲲鹏上默认参数只用了约一半 CPU。

## Decision

- `CMakeLists.txt` 按 `CMAKE_SYSTEM_PROCESSOR` 分支：aarch64/arm64 用 `-mcpu=native`
  （鲲鹏 920 识别为 tsv110），其余架构保持 `-march=native`。
- 新增 `conan/profiles/clang-armv8`（`arch=armv8` + clang 18 + libc++ + C++23），
  文档说明容器 cpuset 与宿主核数不一致时必须覆盖 `tools.build:jobs`。
- `DecompressionRequest` 与 `ArchiveEngine::verify` 增加 `decoderWorkers`（0 表示沿用
  流水线默认 4），CLI 增加 `--decode-workers`（decompress/verify 共用，范围 0–64）。
- 2-bit 打包与碱基校验改为 256 字节查表 + 每 4 碱基无分支展开。clang 在
  `-mcpu=tsv110` 下不会向量化 switch 版本（`-Rpass` 确认），改写后 1 GiB illumina
  压缩吞吐从约 331 提升到约 419 MiB/s（同机 A/B，+27%），归档字节不变。
- 不改 encoder 并行度，也不改 `--parse-workers` 的默认值 4。

## Alternatives considered

- **新增 `--encode-workers`**：`rejected/feature/2026-07-26-parallel-encode-speedup.md`
  已否掉"加 encoder 提速"，重提条件是 profiling 证明 encoder 成为瓶颈。1 GiB 语料上
  4 个 encoder 的 `encode` 汇总约 5.9 s、`pop` 等待约 3.7–5.9 s，encoder 有近一半时间
  在等帧；关键路径是 parser/framing 与保序写出，不是编码。条件未满足，不加。
- **默认并行度改为 `hardware_concurrency()`**：`--parse-workers` 从 4 改成自动值会改变
  K>1 时的帧边界（chunk 结束强制 flush），破坏 K==1 的逐字节归档门禁，也属于隐式行为
  变化。保持显式 CLI，文档给推荐值。
- **继续用 `-march=native`**：aarch64 上 clang 18/21 都接受该选项但 `target-cpu` 静默为
  `generic`（LLVM issue #98443），编译能过但丢失 tsv110 调优意图，因此按架构分支。
- **手写 NEON 打包**：微基准中 NEON 比无分支展开再快约 1.6×（1.48 vs 0.93 GiB/s），
  但端到端预计只多 3–5%，且会引入项目当前不存在的架构宏与第二套实现；先保留可移植
  版本，待 packing 仍是主瓶颈时再评估。
- **重写 FASTQ parser**：打包优化后 parser 成为新瓶颈（k=8 时每 worker parse 约
  1.78 s，encode 约 0.72 s）。避免 `std::getline` 与逐记录分配是更大的解析器重构，
  未纳入本次。

## Consequences

- ARM Release 走 `-mcpu=native`。256 MiB 合成语料上 tsv110 与 generic 无可测差异，
  说明当前瓶颈不是指令级；保留正确意图，不宣称性能提升。
- 21 核鲲鹏实测（1 GiB illumina 合成语料，空闲）：打包改写后 `--parse-workers 12`
  约 419 MiB/s，默认 4 约 212 MiB/s；`--frame-mib 64` 最优，16 MiB 会让 encode 汇总翻倍。
- 打包改写只改热循环实现，不改归档字节；frozen fixture 与全量 17 项测试通过。
  优化后 parser 成为新的主瓶颈，下一步优先考虑解析器缓冲/分配优化。
- 解压/校验新增 `--decode-workers`；合成语料上 decompress 受 writer/I/O 限制
  （4 decoder 只用约 2 核），收益需在真实语料上验证。
- x86 行为与默认归档字节不变；`--decode-workers 0` 与旧行为完全一致。
