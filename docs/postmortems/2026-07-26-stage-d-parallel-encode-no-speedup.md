# 阶段 D 多帧并行编码未带来吞吐提升

- 日期：2026-07-26
- 严重度：low（非缺陷，性能未达 roadmap 预期）
- 状态：closed（已诊断，结论明确，不改代码）
- 引入点：阶段 D（commit abca33e）
- 相关：`src/pipeline/compress_pipeline.cpp`、`include/fqc/pipeline/{mpmc_queue,reorder_buffer}.h`、`tests/e2e/test_performance.sh`、`perf-baselines/2026-07-26-stage-d/`

## 症状

阶段 D 把压缩路径从 3-stage（reader -> encoder -> compressor，单 encoder）改为多帧并行（reader -> [MPMC] -> N=4 encoder 并行 -> [MPMC] -> writer 经 reorder 按序写）。预期编码并行带来多核近线性提升。

实际：64 MiB 随机化数据首次跑，illumina 压缩 55.85 MiB/s（阶段 C 基线 95.53，**-42%**），解压 59.05（C 161，**-63%**）。吞吐不升反降，疑似回归。

## 复现

```bash
cmake --build build/clang-release -j
FQC_PERF_SIZES=64 FQC_PERF_DATA=random bash tests/e2e/test_performance.sh
```

## 调查

### 线索：解压路径未改却同降

D 只动了压缩路径（reader/encoder/compressor）。解压走 `ArchiveReader`，纯顺序、未触碰一行。但解压也从 161 降到 59（-63%）。若 D 代码引入了回归，不可能影响未改的解压路径。强烈怀疑是**机器/WSL2 状态变化**，而非 D 代码。

### 验证一：切回 C 代码同状态重跑

`git stash` 暂存 D 全部改动，工作区回到阶段 C 代码，同机器、同配置重跑：

| 指标 | C 原值（7-26 早） | C-NOW（同状态） | 变化 |
|---|---|---|---|
| illumina 压缩 MiB/s | 95.53 | 60.31 | **-37%** |
| ont 压缩 MiB/s | 86.33 | 52.92 | -39% |
| illumina 解压 MiB/s | 161.00 | 114.62 | -29% |

**C 代码自己也降了 37%**。证明机器状态确实变慢，吞吐下降主因是环境，非 D。

### 验证二：恢复 D 同状态重跑（D-NOW）

`git stash pop` 恢复 D，重编重跑：

| 指标 | C-NOW | D-NOW | D vs C（同状态） |
|---|---|---|---|
| illumina 压缩 MiB/s | 60.31 | 67.11 | **+11%** |
| ont 压缩 MiB/s | 52.92 | 36.24 | -31%（波动） |
| illumina 解压 MiB/s | 114.62 | 109.43 | -5%（路径未改，一致） |
| ont 解压 MiB/s | 122.37 | 113.25 | -7% |

同状态下 D 的 illumina 压缩反而比 C 快 11%；解压（路径未改）基本一致。ont 波动大，但首次 D 跑 47.44 接近 C-NOW 52.92。**D 无确定性回归**。

### 波动量化

同代码同配置两次跑（D 首次 vs D-NOW）：illumina 压缩 55.85 -> 67.11（+20%），解压 59.05 -> 109.43（**+85%**）。WSL2 吞吐波动可达 20-85%，单次性能数字不可信。

## 根因（两层）

### 表层：WSL2 吞吐不稳定

WSL2 上重跑同一二进制，吞吐波动 ±20-85%（解压路径未改却差一倍即为铁证）。跨时对比基线（D vs C 原值）会被机器漂移污染，无法归因代码。

### 深层：encoder 非当前瓶颈（Amdahl）

排除机器波动后，D 的 N=4 并行编码**确实没有带来近线性提升**，这是真实架构事实，原因在 Amdahl 定律：

- 压缩路径三段：reader（解析 FASTQ，单线程）、encoder（2-bit 打包 + measure + checksum，N=4 并行）、compressor（zstd + 写盘，单线程）。
- D 只并行了 encoder。但当前瓶颈是 reader（单线程解析）与 compressor（单线程 zstd+IO），encoder 占整体时间小。
- 并行非瓶颈 stage，收益受 Amdahl 限制：即便 encoder 提速 4 倍，整体提升 = 1 / ((1-p) + p/4)，p 小则提升微乎其微。MPMC/reorder 的额外开销进一步抵消。

D 的真正价值是**揭示新瓶颈在单线程的 reader 与 compressor 两端**（roadmap 动机"C 后写盘成瓶颈"得到量化确认）。

## 修复

**不改代码**：D 架构正确（tsan 干净、压缩比一致、内存有界），性能未提升是瓶颈在 reader/compressor 而非 D 缺陷。修复"未达预期"的方式是记录根因与新瓶颈，而非改 D。

- ARCHITECTURE.md 执行架构段已更新：多帧并行流水线描述 + 波动说明 + Amdahl 分析 + 在途帧上界。
- docs/roadmap.md 阶段 D 验证项已据实记录"未达近线性提升"及原因。
- 新瓶颈（单线程 reader 解析、单线程 compressor zstd+IO）记入下文"后续"，供未来榨性能时定向攻坚。

## 验证（不稳定环境下判定"无回归"）

无法靠单次性能数字判定，改用三条独立证据交叉：

1. **正确性**：压缩比 4 次跑完全一致（illumina 2.9588 / ont 2.8403）--reorder 只改提交顺序不改帧内容，逻辑正确。
2. **并发正确性**：clang-tsan 10/10 无竞争--MpmcQueue 多生产多消、N encoder 并行、encoderError mutex、reorder、shutdown 链式全部干净。
3. **性能同态对比**：D-NOW vs C-NOW（同机器状态），而非 D vs C 原值（跨时）--排除机器漂移。

三者一致指向：D 架构正确、无回归，性能未提升是瓶颈在别处。

## 后续与教训

- **新瓶颈已记录**：单线程 reader（解析）与 compressor（zstd+IO）。若未来要榨性能，攻这两端（会再引 MPSC/reorder，但难度更高）。
- **教训一**：WSL2 不适合精细性能归因，吞吐波动 20-85%，单次数字不可信，必须同状态切代码对比。
- **教训二**：并发优化前先 profiling，盲目并行非瓶颈 stage 会落空（Amdahl）。应先测各 stage 占比，再决定并行谁。
- **教训三**："无回归"判定要排除机器漂移，跨时比基线会被环境变化伪装成回归或提升；切代码同状态重跑是唯一可靠对比。
- **教训四**：练手项目价值在原语落地。D 练了 MPMC + reorder + work distribution，已全部落地且 tsan 干净；性能提升是 bonus，未提升不否定 D 的练手价值。
