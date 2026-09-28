# Agent Note: 靠并行 encoder 提升压缩吞吐（阶段 D 假设）

Status: rejected — 同状态实测无提速：瓶颈在单线程 reader 与 compressor，Amdahl 限死收益

## Problem

3-stage 压缩流水线（reader → encoder → compressor）中 encoder 是唯一容易
并行的纯计算段，直觉上 N 个 encoder worker 并行应带来接近线性的吞吐提升。

## Proposal

阶段 D 把压缩路径改为 reader →[MPMC]→ N=4 encoder 并行 →[MPMC]→ writer 经
reorder 保序写盘，预期编码并行显著提升整体吞吐。代码已落地并保留——tsan
干净、压缩比逐字节一致、内存有界；被否的是"它能提速"这个性能假设，不是
代码本身（其练手价值与架构正确性仍成立）。

## Alternatives considered

- **并行 reader（解析分片）** — 解析含边界对齐与格式验证，复杂度高于
  encoder；立项时未量化各 stage 耗时占比，证据不足以立项。
- **并行 compressor（多 zstd 流 + 保序写）** — 写盘保序会再引一层 reorder
  与更复杂的背压；同样因缺 profiling 未立项。

## Risks

否决依据（同机器同状态对比 D-NOW vs C-NOW，排除 WSL2 漂移）：illumina 压缩
仅 +11%，ont 反降；encoder 占整体时间比例小，并行非瓶颈段的收益被 Amdahl
定律限死，MPMC/reorder 开销进一步抵消。真正瓶颈在两端：单线程 reader 解析
与单线程 compressor（zstd + I/O）。调查过程与 WSL2 波动 20-85% 的测量方法
论教训见 `docs/postmortems/2026-07-26-stage-d-parallel-encode-no-speedup.md`。

重提条件：只有先把 reader/compressor 两端并行化、使 encoder 重新成为瓶颈
之后，"加 encoder worker 提速"才值得重新评估；重提前必须先做各 stage 耗时
profiling。
