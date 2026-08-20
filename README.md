# fq-compressor

把 FASTQ 压成**小而可校验**的归档：内存可控、管道友好、压缩比约 2.8–2.9×。

> **格式族 `fqc-sequential/v2`**：与 [fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust)
> （Rust，`fqc-indexed/v2`）同名但**格式不兼容**——magic 与字节布局不同，不能互相解码；
> `.fqc` 扩展名不能判定格式，reader 必须检查 archive magic。

[![CI 状态](https://github.com/open-genomics/fq-compressor/actions/workflows/ci.yml/badge.svg)](https://github.com/open-genomics/fq-compressor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![CMake 3.28+](https://img.shields.io/badge/CMake-3.28%2B-blue.svg)
![Conan 2.x](https://img.shields.io/badge/Conan-2.x-blue.svg)

[快速开始](#快速开始) · [核心算法](#核心算法) · [高性能架构](#高性能架构) · [性能](#性能)

## 快速开始

```bash
git clone https://github.com/open-genomics/fq-compressor.git
cd fq-compressor && ./scripts/build.sh clang-release
fqc=./build/clang-release/src/fqc

$fqc compress   -i reads.fastq.gz -o reads.fqc     # 压缩
$fqc decompress -i reads.fqc        -o out.fastq   # 解压
$fqc verify     reads.fqc                          # 完整解码校验，不写 FASTQ
$fqc compress   -i R1.fastq.gz -2 R2.fastq.gz -o paired.fqc   # 双端
```

完整参数 `$fqc --help`；工具链要求与质量门禁见 [docs/building.md](docs/building.md)。

## 核心算法

FASTQ 记录的信息字段——ID、序列、质量值——按**列式分离**编码为三条独立字节流，
各自 Zstd level 1 压缩后写入**顺序自校验帧**。

* **2-bit 打包** — 大写 A/C/G/T 各占 2 bit，序列部分直接 4:1 压缩（压缩比最大单一贡献者）；
  其余 IUPAC 符号与小写碱基按原始字节 + 增量位置记录，大小写精确保留。
* **质量值直通** — 原始 Phred+33 ASCII 直接交给 Zstd（无损是硬约束，不做有损变换）。
* **三层 XXH64** — 全局头、帧逻辑（**未压缩流**链式哈希，能发现 Zstd 静默错误）、
  footer 滚动累积（发现丢帧/重排/篡改）。完整性检测，非密码学认证。
* **varint 编码** — 所有长度、计数、位置增量用 LEB128。

每步在压什么、端到端压缩比拆解、设计取舍：见 [ALGORITHM.md](ALGORITHM.md)。

## 高性能架构

压缩（未压缩普通文件）是**多帧并行编码流水线**，解压是其镜像；三条命令共用同一引擎。

```text
压缩: parser×K 字节块切分+边界对齐 →[有界 MPMC]→ encoder×N (2-bit+校验和+zstd×3, 乱序) →[MPMC]→ ChunkOrderer 保序 → 写盘
解压: reader 读帧+内存预检 →[MPMC]→ decoder×N (zstd+校验和+解码, 乱序) →[MPMC]→ reorder 保序 → 滚动校验和 → 写出
```

* **内存有界** — 默认 16 GiB 预算、最低 64 MiB。压缩三道防线（采样上限、帧累积目标、
  编码前保守峰值预检）；解压侧同样聚合校验，不会 OOM。
* **故障边界清晰** — 截断、未知格式/版本、校验和不匹配、内存超限一律 fail closed；
  覆盖已有输出需 `--force`。
* **管道友好** — 支持 stdin/stdout；普通文件先写临时文件，成功后原子替换。
* **单引擎复用** — `verify` 走与 `decompress` 完全相同的解码校验路径，只换空 sink
  （因此是完整解码，不是常数时间元数据检查）。

模块划分、内存模型、归档字节布局、并行化收束过程：见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 性能

合成数据（阶段 H A/B 同窗口，64 MiB 内存预算，随机 FASTQ ×5 次中位数）：

| 数据 | 压缩（并行解析） | 相对顺序解析 | 压缩比 |
|---|---:|---:|---:|
| Illumina-like 150 bp | **148.84 MiB/s** | +47.7% | 2.96× |
| ONT-like 20 kbp | 与顺序路径持平 | — | 2.84× |

真实生物语料实测见 [docs/real-corpus.md](docs/real-corpus.md)。

## 已知限制

* 不支持随机访问、按区间提取、有损压缩、原始顺序重排；仅支持 FASTQ。
* 合成数据不能代表真实压缩比：短读质量集中时更高，长读质量近满字母表时可以更低
  （实测见 [docs/real-corpus.md](docs/real-corpus.md)）。

## 文档

| 目的 | 位置 |
|---|---|
| 构建、工具链、质量与 CI | [docs/building.md](docs/building.md) |
| 压缩算法与原理 | [ALGORITHM.md](ALGORITHM.md) |
| 架构与字节布局 | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 真实语料验收 | [docs/real-corpus.md](docs/real-corpus.md) |
| 并发路线图与开发历程 | [docs/roadmap.md](docs/roadmap.md) |
| 变更记录 | [CHANGELOG.md](CHANGELOG.md) |

## 许可证

[MIT](LICENSE)。
