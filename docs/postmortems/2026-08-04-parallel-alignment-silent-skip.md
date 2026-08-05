# 并行边界对齐静默丢弃畸形记录

- 日期：2026-08-04
- 严重度：critical
- 状态：closed
- 引入点：阶段 H 并行解析（未发布，`ParallelParsePipeline` 首版）
- 相关：`src/pipeline/parallel_parse_pipeline.cpp`（`findFirstRecordStart`）、`tests/pipeline/parallel_parse_pipeline_test.cpp`

## 症状

同一输入文件，顺序路径（`--parse-workers 0`）报错退出，并行路径（`--parse-workers 2`）却成功产出一个**少一条记录**的合法归档：`verify` 通过、往返解码无损、无任何告警。压缩工具静默丢数据。

## 复现

构造 60,000 条均匀记录（每条约 173 B），第 55,000 条序列中插入非法字符 `Z`；采样上限 50k 条，故畸形记录位于采样区之外。`K=2` 时 region 中点恰为该记录 header 之前：

```bash
fqc compress -i malformed2.fastq -o m.k0.fqc --parse-workers 0 --profile illumina  # exit 1: invalid logical FASTQ record
fqc compress -i malformed2.fastq -o m.k2.fqc --parse-workers 2 --profile illumina  # exit 0，输出 59,999 条
fqc verify -i m.k2.fqc                                                              # 通过（归档自洽）
```

## 调查

1. 通读阶段 H 新代码，注意到 `findFirstRecordStart` 的候选检查比 `FastqParser` 多一条"纯 IUPAC"内容预检——解析器本身不查字符集，字符集校验在 `encodeFrame`。两条路径的"接受集合"不一致。
2. 推理所有权规则：worker k 解析"header 落在自己 chunk 内"的记录。畸形记录的 header 落在 worker 1 的对齐区时：对齐扫描因 `Z` 拒绝该候选、对齐到下一条合法 header；worker 0 的循环在 `recordStart >= chunkEnd` 处停止，永远走不到它。该记录无人解析，也就无人报错。
3. 用 CLI 按上述布局复现，确认静默丢失；再确认畸形记录若被采样捕获（≤50k 条）或被某个 worker 顺序解析到，则正常报错——丢失仅发生在"对齐区跳过"这一几何条件下。

## 根因

对齐扫描把**内容校验**（IUPAC 字符集）当成了**对齐判据**。FASTQ 对齐本质上只需结构性判据（`+` 行、长度相等）；内容预检使扫描的接受集合成为解析器接受集合的真子集，差集中的记录被跳过而非被解析，错误从"响亮失败"退化为"静默丢弃"。同族变体：空序列、qual 长度不等、缺 `+` 行、body 被 EOF 截断的记录，只要 header 落在对齐区，统统被静默跳过。

## 修复

`findFirstRecordStart` 候选检查改为与 `FastqParser` 结构性规则完全镜像（`+` 行 + 长度相等，移除 IUPAC 预检）；被 EOF 截断在 body 中（`@` 行后已有非空内容但凑不满 4 行）的候选返回其偏移，交给 parser 报出与顺序路径一致的错误。为何针对根因：对齐只负责"找到解析器也会接受的记录起点"，接受集合两路径同源后，一切顺序路径会拒绝的输入并行路径也必然拒绝（响亮失败），而合法输入不受影响——误对齐需要"序列以 `+` 开头"，而这类输入顺序路径同样会被 `encodeFrame` 拒绝。残留保守边界：`@` 行后紧跟 EOF 或单个空行时维持 nullopt——该形状与"合法文件末尾以 `@` 起始的质量行（可带尾随空行）"不可区分，宁可漏报极罕见的裸 header 截断，也不误伤合法文件。

## 验证

- 新增单元/集成测试：`MalformedSequenceCandidateIsAccepted`、`TruncatedMidBodyCandidateIsReturned`、`BareAtLineAtEofYieldsNullopt`、`MalformedRecordInAlignmentZoneFailsLoudly`、`TruncatedTailInAlignmentZoneFailsLoudly`。
- 原复现命令：并行路径现以 `invalid logical FASTQ record`（exit 1）失败，与顺序路径一致；截断尾部输入两路径均报 `unexpected EOF`（exit 3）。
- 回归：tricky 语料（含 `@` 起始质量行、IUPAC 简并碱基、CRLF、无尾换行、小文件、gzip 输入）K=0/1/4/16 往返全部一致，K0==K1 逐字节门禁通过；debug 13/13、TSan 13/13 通过。

## 后续与教训

- 教训：**跨路径并行化时，每条路径的"接受/拒绝集合"必须与基准路径逐条对齐；对齐扫描这类预处理只能放宽到结构性判据，内容校验必须留在两路径共享的下游**。任何"比解析器更聪明"的前置过滤都是静默数据丢失的温床。
- 残留风险：上文保守边界（裸 `@` 行 + EOF/空行）下的极罕见畸形尾部仍会被丢弃；彻底堵死需要格式层记录数外参照，代价超出收益，接受。
