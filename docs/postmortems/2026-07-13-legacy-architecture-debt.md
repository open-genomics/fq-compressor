# v1 遗留架构债（ABC/SCM/reorder/无界内存）

- 日期：2026-07-13
- 严重度：high
- 状态：closed
- 引入点：pre-v2 baseline（commit 80047d1）
- 相关：—

## 症状

pre-v2 代码库承载着分裂的压缩架构，存在多条冗余路径与无界内存行为：

- 流水线执行通常绕过全局重排序，而短读 ABC 实现假设相似读段在局部相邻。
- 中长读段采用长度前缀 ASCII 后接 Zstd；没有重叠编码器。
- Quality Order-2 为每个 worker 分配一张稠密上下文表，并对每个符号执行全字母表累积更新。
- 生产背压限制的是 token 数量，而非实际驻留字节；配置的内存上限并非准入控制。
- 压缩与解压缩各有独立的单线程实现与流水线实现。
- 已追踪的证据仅限 Illumina，未确立对三代测序的适用性。

## 复现

在 commit `80047d1` 处读取 pre-v2 源码树：`include/fqc/algo/`、`include/fqc/commands/`、重复的流水线源码，以及 16 目标的 CTest 拆分。

## 调查

- 确认该架构无法增量修复，因为准入控制、重排序与编码器模型在各模块间相互纠缠。
- 首先构建了一次性回退原型（`fqc_v2_fallback_prototype`），以证明有界内存顺序路径能跨越 50/100 MiB/s 下限，再投入生产级重写。
- 原型结果（release，64 MiB 合成数据）：短读 61.1/187.3 MiB/s，长读 69.1/171.3 MiB/s，max RSS 低于 260 MiB。256 MiB 运行未出现超线性扩展。

## 根因

v1 设计在错误的层级耦合了重排序、编码器状态与内存核算，导致生产内存上限仅为建议值而非强制执行，且编码器假设与实际数据流不匹配。

## 修复

commit `75b7400` 以有界 FQC v2 替换遗留压缩器：

- 顺序独立帧、打包 DNA + 精确异常、三流 Zstd、XXH64 校验和。
- 精确原始流度量与保守的编码/解码峰值内存预检。
- 单一 compress/decompress/verify 引擎；删除 v1、ABC/SCM、随机访问、ReorderMap、TBB、异步 I/O 及重复流水线（删除 27,835 行）。

## 验证

- 8/8 个 CTest 目标在 clang-debug、clang-release、clang-asan（ASan+UBSan）、clang-tsan 下全部通过。
- `./scripts/lint.sh format-check` 通过。
- 随机化 64 MiB 全路径运行跨越 x86_64 50/100 下限（基线文件原载于 `performance/baselines/2026-07-13-v0.2.0.md`，已随项目瘦身删除）。

## 后续与教训

ARM64 与代表性生物学语料仍为发布环境门禁；编码器准入台账原载于 `benchmark_v2/CODEC_GATES.md`（已随项目瘦身删除），其理念——任何编码器改动须过体积与吞吐双门槛——留存于 `ARCHITECTURE.md`。
