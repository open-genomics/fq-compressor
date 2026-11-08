# Agent Note: 并行对齐补全 '@' 质量行的块边界判定与残段可行性

Status: implemented

## Problem

`ParallelParsePipeline` 的边界对齐（`src/pipeline/parallel_parse_pipeline.cpp` 的
`findFirstRecordStart`）在两种精确几何下会拒绝合法 FASTQ。两者都源于把上一记录以
`@` 开头（Phred+33 Q31）的质量行当成了下一条记录的候选/残段：

1. **块边界落在质量行首（或其前导 `\n`）**：此时被丢弃的残段为空，
   `partialFirstChar` 没有值，`isQualityLine` 的 `p==0` 分支只承认残段首字符是
   `'+'`，于是该质量行被当作畸形记录头返回。worker 的 `FastqParser` 从质量行开始
   解析，第三行不是 `'+'`，整批报 `invalid FASTQ at line 3: expected '+'`。
   实测 5 条 14 B 记录（`@r0\nAAA\n+\n@II\n` 重复，共 70 B，K=3 时
   step=ceil(70/3)=24）即命中；同一文件顺序路径 compress → verify → decompress
   往返一致，证明输入合法。
2. **块边界落在 header 行内部**：窗口残段是上一记录完整的 `[seq, '+', qual]`，
   其中 quality 行以 `@` 开头而被标记 `skipped`；`residueFeasible` 又把这些
   `skipped` 行剔除，残段退化成 `[seq, '+']`，第 2 类形状要求 `r[0][0]=='+'`
   不成立，合法 header 被判不可行，窗口收满后报
   `no valid FASTQ record starts within the alignment window`。实测 6 条 13 B
   记录（固定宽 header，共 78 B，K=4 时 step=20）时边界 40 正好落在 rec3 的
   header 内即命中。

两种几何都只取决于文件长度与 worker 数，不涉及数据内容之外的概率，因此尽管
命中比例低（块边界落在行首/行内的组合），也是可稳定复现的合法输入拒绝，属于
核心功能损坏。

## Decision

两个最小修复，均在 `findFirstRecordStart` 内：

- **`p==0` 的质量行判定补上字节几何**：窗口收集完成后，若窗口首行紧跟一条裸
  `'+'` 行（LF 的 `+\n`，或 CRLF 的 `+\r\n`；parser 已拒绝非裸 `+` 行，这是唯一
  合法形态），则窗口首行只可能是上一记录的 quality 行（有效记录的空 quality 会被
  parser 拒绝）。判定通过回读首行前 2–3 个字节完成，块边界落在质量行首或其前导
  `\n` 时都能命中。
- **`residueFeasible` 计入物理存在的 `skipped` 行**：`skipped` 行是物理存在于
  候选之前的上一记录 quality 行，正是文档化可行形状 `[qual]`、`['+', qual]`、
  `[seq, '+', qual]` 的成员。剔除它会把 `[seq, '+', qual]` 压成 `[seq, '+']`，
  制造假不可行；可行性判定的语义应是"候选前的物理行能构成合法记录后缀"，而不是
  "排除假警报后的行能构成"。

失败方向保持 fail-closed：结构拒绝且无法解释为质量行的 `@` 候选仍然返回给
worker，由 parser 在与顺序路径相同的位置报错，不静默跳过。

## Alternatives considered

- **只修问题 1 或只修问题 2**：两者独立（各自打补丁时对方仍失败，已在源码副本上
  验证），但都属于同一次几何审查发现，且修复互不干扰；分两次改同一函数只会增加
  后续维护成本。
- **放大对齐窗口**：问题不是窗口太短，而是窗口内行分类/残段判据错误；放大窗口
  不改变 `p==0` 的判定缺口，还把"7 行 = 3 残段 + 4 记录"的紧上界破坏掉。
- **`p==0` 也按"前一行内容"判定，回读整行**：能达到同样效果，但为判定一个字节
  分配并读取整行（质量行可长达上限）是浪费；只读 `'+'` 与行界字节即可。
- **CRLF 只按 LF 几何检查**：parser 接受并规范化 CRLF（README 无损边界已注明），
  只查 `start-2 == '+'` 会让 CRLF 文件上的同一 bug 继续存在；因此判定同时接受
  `+\r\n`。
- **把 `[seq, '+']` 直接加入可行形状**：那等于放松残段约束，会让"缺 `+'` 行后
  紧跟合法 header"的畸形输入被当作合法残段，重新打开静默丢弃的口子。

## Consequences

- **收益**：块边界正好落在 Q31 质量行或 header 内部时，合法 FASTQ 不再被整批
  拒绝；`--parse-workers > 1` 与顺序路径在这些几何上行为一致。归档格式、
  分帧策略与顺序路径的字节一致性门禁不变。
- **代价/已知极限**：`p==0` 的新判定与既有的 `p>=1` 判定共享同一信息论歧义——
  若上一条记录的 quality 行内容恰好是单字符 `'+'`（1 bp read、Q10），它与裸 `'+'`
  分隔行在字节流上不可区分；此时紧邻的畸形记录头可能被误判为质量行跳过。该歧义
  在[并行对齐静默丢弃笔记](2026-10-01-parallel-parse-alignment-silent-drop.md)
  中已作为既有取舍记录，本次没有扩大发生面（仍要求单碱基 read + Q10 + 畸形下一条
  记录 + 窗口内还能找到合法候选）。CRLF 输入仍按既有边界规范化，不提供逐字节往返。
- 既有 `skipped` 掩码仅保留"本轮扫描已判定过"的语义，不再影响残段可行性；这与
  上述笔记中"后续候选的残段排除 skipped 行"的表述相反，按新事实更新了源码注释；
  那篇笔记是历史决策记录，已在其正文标注本次修正而不改写原决定。

## Verification

- 新增回归（修复前均失败，修复后全绿）：
  - `FindFirstRecordStartTest.BoundaryAtQualityLineStartSkipsIt`：70 B 文件的
    质量行首及其前导 `\n` 各测一次，并覆盖 CRLF 变体（`+\r\n`）。
  - `FindFirstRecordStartTest.BoundaryInsideHeaderWithAtQualityResidue`：
    header 内部边界 + `@I` 质量行残段，断言对齐到下一条 header。
  - `ParallelParsePipelineTest.ThreeWorkersBoundaryAtQualityLineRoundTrips`：
    5×14 B、K=3，断言解码记录等于 `parseAllFastq`；修复前报
    `invalid FASTQ at line 3: expected '+'`。
  - `ParallelParsePipelineTest.FourWorkersBoundaryInsideHeaderRoundTrips`：
    6×13 B、K=4，修复前报 `no valid FASTQ record starts ... byte offset 40`。
- `export CMAKE_BUILD_PARALLEL_LEVEL=4 && ./scripts/build.sh clang-debug 4`：
  clang 18.1.3 / libc++ 构建通过。
- `./scripts/test.sh clang-debug`：**19/19 测试通过**（18 unit + 1 e2e，
  约 3.7 s）。
- `./scripts/notes.sh verify`：笔记树与相对链接校验通过。
- `clang-format --dry-run --Werror` 对两个改动文件仅报告既有的
  clang-format 21 与 CI 固定 18 的参数换行差异（[版本偏差笔记](../process/2026-07-24-ci-local-clang-version-divergence.md)
  中记录的已知偏差），本次新增行无告警。
- 未运行：ASan/TSan preset（本次改动不涉及并发原语，仅对齐扫描的纯函数逻辑）。
