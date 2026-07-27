# FASTQ 压缩算法业界调研与 FQC v2 对比

- 用途：弄清业界顶级 FASTQ 压缩器的算法设计，定位 FQC v2 的谱系与差距，为 `docs/roadmap.md` 阶段 E–I 提供取舍依据。
- 调研日期：2026-07-27。外部 benchmark 数据随数据集、工具版本、硬件变化，本文数字非本地复测，只表征量级。
- 来源标注约定：**【本地】** 本仓库代码/文档事实（给 `文件:行号`）；**【文献】** 论文/官方文档；**【外部实测】** 第三方 benchmark（注明数据集）；**【推断】** 作者分析。

## 1. 问题背景：FASTQ 压缩的特殊性

FASTQ 每条记录含标识符（ID + 注释）、序列、`+` 分隔行、质量值四个字段，三者的统计特性迥异【本地 ALGORITHM.md:21-31】：

- **ID** 共享仪器名、泳道号、坐标前缀，前缀重复率极高——字典匹配（LZ 类）的主场。
- **序列** 对随机化数据接近 2 bit/碱基的理论熵，但真实生物数据含大量重复/低复杂度区域，且**同一基因组的 read 彼此高度相似**——跨 read 冗余是最大的一块，但只有重排序或组装式编码才吃得到。
- **质量值** 字母表约 94 个可打印 ASCII，分布由测序化学决定，沿 read 位置和前序质量值强相关——上下文建模（context modeling）的主场。

无损是硬约束：质量值承载变异检测的统计权重，有损变换会改变下游分析结果【本地 ALGORITHM.md:115-116】。

**结论句：质量值是压缩比的主战场，"质量值分布决定一切专用工具的成败"**（§6 给出正反论证）。

## 2. 业界工具分类全景

| 类别 | 工具 | 一句话定位 |
|---|---|---|
| 通用层 | gzip / zstd | 无结构感知；zstd 是本项目底座【本地 conanfile】 |
| 无参专用 | DSRC2、fqzcomp5、MFCompress、Quip、SPRING、repaq | 利用 FASTQ 字段结构，不需参考基因组 |
| 有参 | CRAM / uCRAM | 依赖参考基因组对齐，SAM/BAM 生态 |
| 商业 | Petagene | 压缩比追平 SPRING、速度高一个数量级（§5） |

划分维度：**是否用参考基因组**（有参/无参）、**是否全局重排 read**（流式/非流式）、**熵编码器**（Huffman 类 / 算术编码类）。FQC v2 是纯无参、无重排、熵编码外包给 zstd 的流式设计【本地 ARCHITECTURE.md:3-4】。

## 3. 代表算法深潜

### 3.1 SPRING（2019，无参压缩比标杆）【文献】

**核心思想**：三件套——
1. **min-hash 相似聚类重排序**：为每条 read 计算 min-hash 签名，把彼此相似的 read 排到相邻位置（可选保留原序的重排映射）。
2. **组装式序列编码（HARC 风格）**：重排后，read 与前序 read 做近似对齐，存"锚 read + 位置 + 差异编辑脚本"而非碱基本身。
3. **质量值 order-1 有限上下文模型 + 算术编码**；ID 流重排后交给通用压缩器。

**为什么有效**：同基因组 read 高度相似，但它们在原始文件中相距遥远，任何滑动窗口压缩器（窗口几十~几百 MB）都看不到这份冗余。重排把相似 read 聚邻后，序列流退化为"少量锚序列 + 大量短差异列表"，跨 read 冗余被显式消除。这是业界压缩比第一梯队（≈91–95% 减少，§5）的核心引擎。

**代价账单**：全局重排 → 非流式（或需两趟）、内存随数据规模增长、实现复杂度高（min-hash + 比对 + 编辑脚本 + 重排映射）。本项目的 v1 坟场正是这条路线：v1 曾实现 ABC（Spring/Mincom 风格）+ ReorderMap，最终因"重排序、编码器状态与内存核算耦合在错误层级，内存上限无法强制执行"整体删除 27,835 行【本地 docs/postmortems/2026-07-13-legacy-architecture-debt.md:30-40】。

### 3.2 fqzcomp（2013，质量值上下文建模派）【文献】

**核心思想**：质量值 **order-N 上下文建模 + range coder**——用同一 read 前几个碱基的质量值（及 read 内位置）作为上下文，逐符号预测当前质量的条件分布，交给 range coder（算术编码的变体）编码；N 可配置（典型 1–5）。序列侧 2-bit 打包加每位置上下文。

**为什么有效**：质量值沿 read 位置和前序质量强相关（测序化学的连续性），order-N 建模把码长压到接近**条件熵**而非零阶熵——这是通用压缩器（零阶/字典模型）结构上吃不到的部分。

**代价账单**：每个 worker 一张稠密上下文表（内存 × worker 数）、每个符号一次全字母表累积更新（CPU 密集）。本项目 v1 的 SCM（Quality Order-2）付过同款账单："为每个 worker 分配一张稠密上下文表，并对每个符号执行全字母表累积更新"【本地 docs/postmortems/2026-07-13-legacy-architecture-debt.md:15】。另一个结构性软肋：对已经分箱的质量值（如 NovaSeq 的 2-bit 质量档）无肉可吃——外部实测中 fqzcomp 在 NovaSeq WGS 数据上反输 gzip 默认档（§5 脚注 1）。

### 3.3 MFCompress（2014，轻量 Markov 派）【文献】

**核心思想**：多个**有限上下文模型（finite-context / Markov）**分别建模序列流与质量流，逐符号算概率后接**算术编码**。无参、单趟、不需要重排。

**为什么有效**：与 fqzcomp 同源（上下文 → 条件熵），但模型更轻、按字段独立建模，实现比重排序派简单一个量级。

**代价账单**：模型内存与更新 CPU 仍在；文献口径中压缩时间通常为 gzip 的数倍【推断】；无重排意味着序列侧只吃得到 k-mer 阶内的冗余，跨 read 大冗余吃不到。

### 3.4 DSRC / DSRC2（2011/2016，FQC 的设计最近邻）【文献】

**核心思想**：**分字段列式**——title、quality、sequence 各自聚合成独立块，序列用自研 LZ77 变体 + Huffman，质量用 Huffman；块独立、无参、流式、以速度为目标。

**为什么有效**：与 FQC v2 完全相同的洞察——同一字段在不同记录间的统计相关性远高于字段间，列式让通用熵编码器的字典/频率模型命中率最大化【本地 ALGORITHM.md:21-31】。

**代价账单**：压缩比低于上下文建模派与重排序派；DSRC2 未进入 §5 的第三方对比，文献口径其压缩比介于 gzip 与 fqzcomp 之间【推断】。

**FQC v2 与 DSRC2 的关系**：同一象限（分字段列式 + 通用熵编码后端、无参、流式、块/帧独立），区别是 FQC 用 zstd（LZ77 + Huffman + FSE/tANS）替代其自研 LZ + Huffman，并额外做了 2-bit 打包前置变换与三层校验【本地 src/format/archive.cpp:337-347, ALGORITHM.md:135-150】。

## 4. FQC v2 设计速写【本地】

```text
FASTQ → 列式分离三条流（ALGORITHM.md:8-19）
  ├─ ID 流:      varint 长度 + 原始字节（前缀冗余留给 zstd 字典匹配）
  ├─ 序列流:     2-bit 打包（4 碱基/字节，zstd 之前先 4:1）+ 异常列表（N/简并/小写，
  │              位置 delta 编码）（ALGORITHM.md:43-89）
  └─ 质量流:     varint 长度 + 原始 ASCII，零变换（ALGORITHM.md:104-121）
       ↓ 每帧 × 每流各一次 ZSTD_compress(level=1)（archive.cpp:337-347, 701-703）
       ↓ 三层 XXH64：全局头 / 帧逻辑（未压缩流链式哈希）/ footer 滚动（ALGORITHM.md:135-150）
```

关键架构性质：帧间完全独立（并行编码的切分点，ARCHITECTURE.md:97）；编解码状态帧内局部；内存有界三道防线（ARCHITECTURE.md:69-80）；无自研熵编码、无上下文建模、无重排序、无参考基因组。

## 5. 对比表

外部工具（真实人类数据）【外部实测】¹ ²：

| 工具 | 年份 | 类型 | read 重排 | 序列编码 | 质量值编码 | 熵编码器 | 随机访问 | WES¹ | WGS² | 速度特征 |
|---|---|---|---|---|---|---|---|---|---|---|
| gzip -6（锚点） | — | 通用 | 无 | — | — | LZ77+Huffman | 无 | 10.3 GB（3.9×） | 33 GB（8.6×） | 慢（WGS 压缩 2h59m），解压快 |
| DSRC2 | 2016 | 无参·流式 | 无 | 自研 LZ | Huffman | Huffman | 无 | 未测³ | 未测³ | 快（设计目标） |
| fqzcomp5 | 2013 | 无参·流式 | 无 | 2-bit+上下文 | order-N 上下文建模 | range coder | 无 | 4.7 GB（8.5×） | 37 GB（7.7×）⁴ | 慢（WGS 2h15m / 解压 ~2h10m） |
| MFCompress | 2014 | 无参·流式 | 无 | 有限上下文模型 | 有限上下文模型 | 算术编码 | 无 | 未测³ | 未测³ | 文献口径数倍于 gzip【推断】 |
| Quip | 2012 | 无参（可选有参） | 无 | de Bruijn 组装式 | 统计建模 | 算术编码 | 无 | 未测³ | 未测³ | 慢 |
| SPRING | 2019 | 无参·重排 | min-hash 聚类 | 组装式（锚+编辑脚本） | order-1 上下文 | 算术编码 | 无 | **3.5 GB（11.4×）** | **15 GB（18.9×）** | 很慢（WGS 压缩 3h3m / 解压 53m） |
| repaq | 2019 | 无参·流式 | 无 | 轻量变换 | 轻量变换 | gzip/xz 后端 | 无 | 12 GB（3.3×）/ xz 5.4 GB | 77 GB（3.7×）/ xz 21 GB | 快（WGS 47m），比 gzip 快 3-4× |
| uCRAM | 2011+ | 有参生态 | 按 name 排序⁵ | CRAM 列式编码 | CRAM 列式编码 | rANS/外部 codecs | 有 | 6.6 GB（6.1×） | 22 GB（12.9×） | 中等（WGS 2h35m） |
| Petagene | 商业 | 商业（无参） | 有⁶ | 未公开 | 未公开 | 未公开 | 有 | 3.6 GB（11.1×） | 15.2 GB（18.7×） | **最快（WGS 压缩 17m / 解压 12m）** |

FQC v2（本仓库，单独列出，**不可与上表直接横比**，见脚注 7）【本地】：

| 工具 | 类型 | read 重排 | 序列编码 | 质量值编码 | 熵编码器 | 随机访问 | 压缩比⁷ | 吞吐⁸ |
|---|---|---|---|---|---|---|---|---|
| FQC v2 | 无参·流式 | 无（README.md:84） | 2-bit 打包+异常列表 | 原样字节 | zstd 内部（LZ77+Huffman+FSE） | 无 | 2.96×（Illumina-like）/ 2.84×（ONT-like） | 压缩 53–56 / 解压 182–215 MiB/s |

**脚注（缺一不可）：**

1. 【外部实测】数据源：`github.com/godotgildor/fastq_compression_comparison`（2026-07 检索）。WES = SRR2962693 人类外显子组（HiSeq 2500，原始 FASTQ 40 GB）；WGS = SRR8861483 人类全基因组（NovaSeq 6000，原始 FASTQ 284 GB）。括号内为相对原始 FASTQ 的倍率（原始/压缩后）。不同数据集结论会反转，见脚注 4。
2. gzip -6 在 WGS 上达 8.6× 的反常高值，原因是 NovaSeq 质量值已做 2-bit 分箱（约 8 档），ASCII 质量流本身冗余极高——再次印证"质量值分布决定论"。
3. 该对比仓库未评估 DSRC2/MFCompress/Quip；描述来自各自论文【文献】，无同条件数字。
4. **反转案例**：fqzcomp 在 HiSeq WES 上 8.5× 表现正常，但在 NovaSeq WGS 上 7.7× 反输 gzip -6 的 8.6×——质量值已 2-bit 分箱后，order-N 上下文建模无冗余可吃，模型开销反而拖累。另注意 fqzcomp 会把 N 碱基的质量分置 0（轻微有损）【外部实测】。
5. uBAM/uCRAM 按 read name 排序输出，不保原始顺序；SPRING 可选保留原序【外部实测】。
6. Petagene 为商业闭源，机制未公开；以其保序模式与吞吐特征推断含重排/专用编码【推断】。
7. 【本地】FQC 的 2.9588×/2.8403× 出自 64 MiB **随机合成数据**（`tests/e2e/test_performance.sh`，README.md:57-62，docs/postmortems/2026-07-26-stage-d-parallel-encode-no-speedup.md:85），与外部真实 WES/WGS 不是同一数据集，**只表征量级，不可直接横比**。真实生物语料压缩比尚未测量（README.md:86）。
8. 【本地】吞吐出自 WSL2（Ryzen 7 5800H），同机波动可达 ±20–85%（docs/postmortems/2026-07-26-stage-d-parallel-encode-no-speedup.md:55）。

## 6. FQC 定位与差距分析

### 6.1 谱系定位

FQC v2 的设计最近邻是 **DSRC 象限**：分字段列式 + 通用熵编码后端、无参、流式、帧独立（§3.4）。

- 与 SPRING 的分界线 = **read 重排序**（重排派吃跨 read 冗余，FQC 明确不做，README.md:84）。
- 与 fqzcomp/MFCompress 的分界线 = **质量值上下文建模**（建模派吃条件熵，FQC 质量流零变换）。
- 与 CRAM 的分界线 = **参考基因组**（有参派，项目定位外）。

### 6.2 差距三来源（按贡献排序）

**① 质量值零变换（主差距）【本地+外部实测】**
质量流占原始数据约 40%，且占 FQC 压缩后体积的 **67–83%**（ALGORITHM.md:160-170 端到端表：366 B → ~120 B 中质量值占 80–100 B）。业界压缩比第一梯队的主引擎正是质量值的 order-N 上下文建模 + 算术编码（§3.2/3.3）。正论证：FQC 把最大的一块原样交给 zstd level 1；反论证：fqzcomp 在 NovaSeq 分箱数据上反输 gzip（§5 脚注 4）证明质量值的可压空间 = 其分布熵，只有上下文建模吃得到，字典编码器（zstd）只能吃到表层重复。

**② 熵编码档位（小差距）【本地】**
zstd level 1 偏吞吐；level 1→3 仅补 2–5%（ALGORITHM.md:37-39）。即便拉满档位，也补不齐 LZ77+Huffman 与 range coder 在字节级熵上的结构差距。

**③ 无 read 重排序（对真实数据是大头，对合成数据无影响）【文献+本地复盘】**
SPRING 的 min-hash 聚类对真实生物数据（重复/低复杂度/同基因组 read 相似）收益巨大，这是 91–95% 减少的核心来源（§3.1）；对随机合成数据收益趋近于零。但这条路是 v1 已经付过一次的学费：重排序/编码器状态/内存核算耦合错位导致内存上限无法强制执行，27,835 行整体删除（docs/postmortems/2026-07-13-legacy-architecture-debt.md:30-40）。

### 6.3 反向论点：压缩比弱 ≠ 设计失败

FQC 的目标函数是 **吞吐 × 内存有界 × 可校验 × 可回退 × 练手价值**，不是压缩比极值（README.md:15-21）。工业界同样用脚投票选吞吐：Petagene 以追平 SPRING 的压缩比 + 约 10 倍速度取胜（WGS 压缩 17 min vs 3h3m，§5）；SPRING 论文级压缩比背后是小时级的运行时间。在"不动质量值编码"的前提下，任何其他改动（序列、ID、熵编码档位）都是小头——这决定了本项目只做 §7 的轻量改进，而非全面开战。

## 7. 可借鉴 vs 应避免

### 可借鉴（轻量、不破坏帧独立性）

1. **per-stream 差异化压缩参数**（DSRC2 分字段思想的最小化）：质量流单独提升 zstd level，ID/序列保持 1。帧内局部、zstd 帧自描述（解码与 level 无关）、codec ID 不动、格式零破坏。→ 已落地为路线图**阶段 I**，见 `docs/roadmap.md`。
2. **准入门槛制度复活**（CODEC_GATES 理念）：原台账文件已随 `benchmark_v2/` 删除，理念留存于 v1 复盘（docs/postmortems/2026-07-13-legacy-architecture-debt.md:50）与 ARCHITECTURE.md:61-62——任何 codec 改动须过"体积 + 吞吐"双门槛才合入默认值。→ 阶段 I 的判定机制。
3. **测量先行的质量流分布实验**：先做分布统计（直方图、相邻差分熵），用数据决定是否值得任何变换；不允许无测量直接上编码器。
4. **SPRING/fqzcomp 的教训本身**：知道压缩比从哪来、账单有多大，正是本文的价值。

### 应避免（每条呼应 v1 删除历史）

1. **read 重排序 / min-hash 全局聚类（SPRING 路线）**：v1 复盘根因——内存上限无法强制执行；破坏流式与帧独立。
2. **自研算术编码 / range coder / 手写 FSE（fqzcomp/Quip 路线）**：复杂度与验证成本超练手预算；v1 SCM（Quality Order-2 稠密上下文表 + 全字母表更新）坟场（v1 复盘 :15）。
3. **有参压缩（CRAM 路线）**：需参考基因组，项目定位外。
4. **跨帧 zstd 字典**：破坏帧独立性——帧独立是阶段 D 并行编码的切分点（ARCHITECTURE.md:97）；且训练集外收益不稳。
5. **有损质量值分箱**：无损是硬约束（ALGORITHM.md:115-116）。

## 8. 参考资料

**文献**
- Chandak et al., "SPRING: a next-generation compressor for FASTQ data", *Bioinformatics* 2019（PMC6662292）。
- Bonfield & Mahoney, "Compression of FASTQ and SAM format sequencing data", *PLoS ONE* 2013（fqzcomp）。
- Pinho & Pratas, "MFCompress: a compression tool for FASTA and multi-FASTA data", *Bioinformatics* 2014。
- Deorowicz & Grabowski, "Compression of DNA sequence reads in FASTQ format"（DSRC, 2011）；Roguski & Ribeca（DSRC2, 2016）。
- Jones et al., "Compression of next-generation sequencing reads aided by highly efficient de novo assembly"（Quip, 2012）。
- Fritz et al., "Efficient storage of high throughput DNA sequencing data using reference-based compression"（CRAM, 2011）。

**外部实测**
- `github.com/godotgildor/fastq_compression_comparison`（SRR2962693 / SRR8861483，2026-07 检索）。

**本地文档**
- `ALGORITHM.md`（算法原理与端到端压缩比分析）
- `ARCHITECTURE.md`（帧布局、内存模型、执行架构）
- `docs/postmortems/2026-07-13-legacy-architecture-debt.md`（v1 ABC/SCM/ReorderMap 删除复盘）
- `docs/postmortems/2026-07-26-stage-d-parallel-encode-no-speedup.md`（阶段 D 瓶颈与 WSL2 波动）
- `docs/roadmap.md`（阶段 E–I 开发路线）
