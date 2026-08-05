# getline 容量伪影导致分帧不确定（并行解析 K=1 归档不一致）

- 日期：2026-07-28
- 严重度：high（正确性无损，但"K=1 与顺序版逐字节一致"的核心门禁失败）
- 状态：closed
- 引入点：pre-v2 baseline（capacity 口径分帧核算自 v2 回退原型即存在）
- 相关：`include/fqc/pipeline/frame_accumulator.h`、`src/pipeline/compress_pipeline.cpp`、`src/pipeline/parallel_parse_pipeline.cpp`

## 症状

阶段 H（并行解析）验收时，同一输入下 `--parse-workers 1`（并行机器、单块）与基线二进制的归档应当逐字节一致：

- 16 GiB 内存限制：illumina / ont 均一致（通过）。
- **64 MiB 内存限制：不一致**。两归档总记录数相同、round-trip 输出逐字节相同，但分帧不同：基线第 2 帧 14105 条记录，并行第 2 帧 14106 条，第 3 帧起错位一条。

## 复现

```bash
fqc --memory-limit 64 compress -i illumina_64mib.fastq -o base.fqc --profile illumina           # 基线
fqc --memory-limit 64 compress -i illumina_64mib.fastq -o par.fqc  --profile illumina --parse-workers 1
cmp base.fqc par.fqc   # 不一致
```

关键触发条件：采样记录数达到字节上限（64 MiB 预算下采样 41,944 条），使"采样尾 + 流记录"混合的帧恰好压在关帧阈值±几十字节上。16 GiB 下采样为 5 万条上限，帧阈值余量大，漂移不显形。

## 调查

按顺序排查（每一步都排除了一个假设）：

1. **采样终点偏移错误？** 给 `FastqParser` 加 `bytesConsumed()` 后与 Python 手算偏移逐字节比对：13,704,582 == 13,704,582，精确一致。排除。
2. **并行 worker 多读/漏读记录？** 两路径首条流记录均为 `read_41945`，总记录数相等，round-trip 一致。排除内容差异，锁定为分帧差异。
3. **共享 FrameAccumulator 重构改变了顺序行为？** 当前源的顺序路径与阶段 G 基线逐字节一致。排除重构。
4. **逐帧 retained 埋点**：帧 0/1 两路径完全相同（14107 条、retained 6,291,722）；种子结束后敞口 retained 两路径完全相同（6,123,580）。差异只出现在"采样尾 + 流记录"混合的第 2 帧：仅差 ~14–34 字节，而关帧阈值余量只有 ~10–24 字节——顺序版侥幸达标，并行版差 10 字节需多读一条。
5. **逐条容量对比**：同一 `read_42143`，顺序路径 `qualCap=175`，并行路径 `qualCap=151`。约每 12.3 条记录（≈4096 字节）就有一条容量更大（175/191/207/…/287）。

## 根因

libc++ `std::getline` 读取跨界于 streambuf 缓冲区（4096 字节）边界的行时，字符串增长路径不同，最终 capacity 大于常规值（151 → 175/191/…）。即：**同一内容行，其字符串 capacity 取决于它相对 streambuf 缓冲相位的位置**。

分帧核算 `retainedRecordBytes` 使用 `capacity()`，因此帧边界依赖于流的缓冲相位。顺序路径从偏移 0 连续缓冲，相位固定；并行 worker 从 `sampleEnd`/块起点新建 ifstream，相位不同 → 同一批记录的 retained 差几百字节 → 阈值刀锋上的某一帧多/少一条记录。

一句话：**capacity 不是内容的函数，分帧规则因此不是内容的函数**。

## 修复

分帧核算从 capacity 口径改为 size 口径（`FrameAccumulator::retainedBytes`）：`sizeof(ReadRecord) + id.size()+1 + comment.size()+1 + seq.size()+1 + qual.size()+1`，成为纯内容函数，与缓冲相位无关。

- 保守性不受影响：内存预检 `estimateCompressionPeak` 仍按 capacity 核算，分帧规则只决定帧边界。
- 副作用：新归档与旧二进制的归档不再逐字节一致（分帧规则变了）；**线上格式完全不变，旧归档正常解码**（verify + round-trip 实测通过）。
- 顺带收益：size ≤ capacity，帧均记录数略升（~5%），帧头开销略降；代价是 64 MiB/4 块下压缩比 2.9588 → 2.9572（块尾关帧引入的微小切分代价）。

## 验证

- 同二进制两模式：`--parse-workers 0`（顺序）vs `--parse-workers 1`（并行 K=1）在 16 GiB/64 MiB × illumina/ont 四组全部逐字节一致。
- 修复前复现用例（64 MiB、illumina）同样转为一致。
- K=4 round-trip cmp + verify 通过；'@' 质量行对抗 fixture round-trip 通过；阶段 G 旧归档 verify + round-trip 通过（向后兼容）。
- 新增回归：`FrameAccumulatorTest.RetainedBytesIsContentDeterministic`（同内容、reserve 4096 后核算不变）。
- clang-debug 13/13、clang-tsan 13/13。

## 后续与教训

- **凡是进入"确定性协议"的度量，必须是内容的纯函数。** capacity、allocator 行为、getline 增长这类实现细节都会渗入（retained 核算、哈希、排序键），用它们做跨路径一致性比较就是埋雷。
- 刀锋验证的价值：16 GiB 下"恰好通过"掩盖了问题，是 64 MiB 小预算把阈值余量压到几十字节才让漂移显形。门禁矩阵（两档内存 × 两 profile）不是形式主义。
- 埋点方法论有效：真实偏移核对 → 逐帧 retained → 种子敞口 → 逐条容量，四层自顶向下每层排除一个假设，没有一次重写。
- 残留风险：解压侧不受影响（帧自描述）；未来若给分帧加任何新度量，先问"它是内容的函数吗"。
