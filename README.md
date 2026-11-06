# fq-compressor

把 FASTQ 压成**小而可校验**的归档：内存可控、管道友好。压缩比视数据而定——
合成 realistic 数据约 2.6–3.4×，真实语料 1.96–4.15×（详见 [性能](#性能) 与
[docs/real-corpus.md](docs/real-corpus.md)）。

*A columnar streaming FASTQ compressor in C++23 — small and verifiable archives, bounded memory, pipe-friendly I/O.*

> **格式族 `fqc-sequential/v2`**（magic `46 51 43 56 32 0D 0A 1A`，`FQCV2\r\n\x1A`）：
> 与 [fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust)
> （Rust，`fqc-indexed/v2`，magic `89 46 51 43 0D 0A 1A 0A`）同名但**格式不兼容**——
> magic 与字节布局不同，不能互相解码；`.fqc` 扩展名不能判定格式，reader 必须检查 archive magic。

[![CI 状态](https://github.com/open-genomics/fq-compressor/actions/workflows/ci.yml/badge.svg)](https://github.com/open-genomics/fq-compressor/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/open-genomics/fq-compressor/branch/master/graph/badge.svg)](https://codecov.io/gh/open-genomics/fq-compressor)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![CMake 3.28+](https://img.shields.io/badge/CMake-3.28%2B-blue.svg)
![Conan 2.x](https://img.shields.io/badge/Conan-2.x-blue.svg)

[安装与获取](#安装与获取) · [快速开始](#快速开始) · [核心算法](#核心算法) · [高性能架构](#高性能架构) · [性能](#性能) · [同类工具对比](#同类工具对比)

## 安装与获取

**Linux x86_64**：从 [GitHub Releases](https://github.com/open-genomics/fq-compressor/releases)
下载 `fqc` 与 `SHA256SUMS`，核对校验和后再运行：

```bash
curl -LO https://github.com/open-genomics/fq-compressor/releases/latest/download/fqc
curl -LO https://github.com/open-genomics/fq-compressor/releases/latest/download/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
chmod +x fqc && ./fqc --version
```

> Release 在打 `v*` tag 时由 CI 自动构建并附加（见
> [.github/workflows/release.yml](.github/workflows/release.yml)）；发布前会跑一次
> round-trip + verify 回归 sanity。

**其他平台 / 从源码构建**：需 clang-18 + libc++、CMake 3.28+、Conan 2.x、Ninja，
工具链要求与质量门禁见 [docs/building.md](docs/building.md)（ARM64 / AArch64 的额外步骤
与多核参数也在该文档）：

```bash
git clone https://github.com/open-genomics/fq-compressor.git
cd fq-compressor && ./scripts/build.sh clang-release
```

## 快速开始

```bash
fqc compress   -i reads.fastq.gz -o reads.fqc     # 压缩
fqc decompress -i reads.fqc        -o out.fastq   # 解压
fqc verify     reads.fqc                          # 完整解码校验，不写 FASTQ
fqc compress   -i R1.fastq.gz -2 R2.fastq.gz -o paired.fqc   # 双端
```

源码构建的二进制在 `build/clang-release/src/fqc`；完整参数 `fqc --help`。

> 若同时安装了 [fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust) 的 `fqc`，
> 两个同名二进制会互相覆盖，`PATH` 中靠前者生效；用 `which fqc` 确认实际调用的实现。

## 核心算法

FASTQ 记录的信息字段——ID、序列、质量值——按**列式分离**编码为三条独立字节流，
各自 Zstd level 1 压缩后写入**顺序自校验帧**。

* **2-bit 打包** — 大写 A/C/G/T 各占 2 bit，序列部分直接 4:1 压缩（压缩比最大单一贡献者）；
  其余 IUPAC 符号与小写碱基按原始字节 + 增量位置记录，大小写精确保留。
* **质量值直通** — 原始 Phred+33 ASCII 直接交给 Zstd（无损是硬约束，不做有损变换）。
* **三层 XXH64** — 全局头、帧逻辑（**未压缩流**链式哈希，能发现 Zstd 静默错误）、
  footer 滚动累积（发现丢帧/重排/篡改）。完整性检测，非密码学认证。
* **varint 编码** — 所有长度、计数、位置增量用 LEB128。

**无损边界**：字节级无损（`cmp` 逐字节一致）仅对 **LF-only 规范 FASTQ** 成立；CRLF
输入会被静默规范化为 LF（行尾 `\r` 不参与往返），字段内容恰以单个 `\r` 结尾的 LF-only
文件同理——该 `\r` 与 CRLF 行尾在字节流上不可区分。超长单行（> 64 MiB）与结构畸形的
记录（缺 `+` 行、sequence/quality 长度不一致等）会以格式错误拒绝而非静默篡改。

每步在压什么、端到端压缩比拆解、设计取舍：见 [ALGORITHM.md](ALGORITHM.md)。

## 高性能架构

压缩（未压缩普通文件）是**多帧并行编码流水线**，解压是其镜像；三条命令共用同一引擎。

```text
压缩: parser×K 字节块切分+边界对齐 →[有界 MPMC]→ encoder×N（N=min(K,4)，2-bit+校验和+zstd×3, 乱序） →[MPMC]→ ChunkOrderer 保序 → 写盘
解压: reader 读帧+内存预检 →[MPMC]→ decoder×N (zstd+校验和+解码, 乱序) →[MPMC]→ reorder 保序 → 滚动校验和 → 写出
```

* **内存有界** — 默认 16 GiB 预算、最低 64 MiB；逐帧峰值按队列、in-flight 预算窗口
  （并行解析取 `max(K, 4)`）和源端本地帧的最大同存数分摊，64 MiB 实测峰值低于预算。
* **故障边界清晰** — 截断、未知格式/版本、校验和不匹配、内存超限一律 fail closed；
  覆盖已有输出需 `--force`。
* **管道友好** — stdin/stdout 直通；普通文件先写临时文件，成功后原子替换。
* **单引擎复用** — `verify` 与 `decompress` 走同一解码校验路径（完整解码，非元数据检查）。

模块划分、内存模型、归档字节布局、并行化收束过程：见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 性能

> 口径：`tests/e2e/test_performance.sh` 默认 `FQC_PERF_DATA=realistic`（短读位置相关
> 质量、长读宽质量分布）；`structured`/`random` 仅作压力 fixture。下表是当前 HEAD 的
> **绝对 baseline**，不是优化 A/B；不同日期、机器或运行窗口的数字不可横比。

当前 realistic baseline（2026-11-06，commit `513b9f9`，16 GiB 预算、64 MiB 目标帧、
4 parser、5 次中位数）：

| 数据 | 实际输入 | 压缩 MiB/s | 解压 MiB/s | 压缩比 | 压缩 RSS |
|---|---:|---:|---:|---:|---:|
| Illumina 150 bp | 502.61 MiB | 341.91 | 353.95 | 3.33× | 924 MiB |
| Illumina 150 bp | 1005.22 MiB | 331.75 | 410.29 | 3.33× | 1 230 MiB |
| ONT 20 kbp | 1024.03 MiB | 317.03 | 616.88 | 2.63× | 1 647 MiB |

极差、原始 JSONL 与归档说明见 [docs/benchmarks.md](docs/benchmarks.md)。

- 与 2026-11-05 的 247.63 / 265.82 / 135.20 MiB/s 差异较大，但运行窗口、生成器版本
  和内存预算口径不同，不能归因于代码；同窗口 A/B 才能下结论。
- 历史阶段 H 的 148.84 MiB/s（随机数据、旧帧预算公式）仅作历史对照，不能与当前
  二进制或真实语料横比。
- 真实生物语料实测（Illumina WXS 4.15×、ONT 1.96× 等）见
  [docs/real-corpus.md](docs/real-corpus.md)；合成数据不代表真实压缩比——短读质量
  集中时更高，长读质量近满字母表时可以更低。

## 同类工具对比

相对通用 `gzip`/`zstd`（无结构感知，约 2–4× 缩小），FQC v2 吃 FASTQ 字段结构
（2-bit 序列打包、独立质量流），压缩比更高；同为无参专用的 SPRING/fqzcomp 在压缩比
上通常更强，但依赖全局重排或稠密上下文建模——非流式、内存随数据规模增长。本项目
的取舍是**流式、内存有界、快速可校验**，压缩比让位于工程可用性。调研全景见
[docs/fastq-compression-survey.md](docs/fastq-compression-survey.md)。

## 已知限制

* 不支持随机访问、按区间提取、有损压缩、原始顺序重排；仅支持 FASTQ。
* 归档帧大小与压缩时的 `--memory-limit` 相关；用显著更低的预算解码高预算生成的归档
  可能失败（例如新版 16 GiB 归档在 64 MiB 下会被逐帧预算拒绝；旧版 64 MiB 归档在新版
  64 MiB 下约需 101 MiB）。格式不变，提高 `--memory-limit` 即可读取。

## 文档

| 目的 | 位置 |
|---|---|
| 构建、工具链、质量与 CI | [docs/building.md](docs/building.md) |
| 压缩算法与原理 | [ALGORITHM.md](ALGORITHM.md) |
| 架构与字节布局 | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 性能基准台账（合成数据） | [docs/benchmarks.md](docs/benchmarks.md) |
| 真实语料验收 | [docs/real-corpus.md](docs/real-corpus.md) |
| 业界调研与差距分析 | [docs/fastq-compression-survey.md](docs/fastq-compression-survey.md) |
| 路线图与开发历程 | [docs/roadmap.md](docs/roadmap.md) · [docs/development-journey.md](docs/development-journey.md) |
| 问题复盘 | [docs/postmortems/](docs/postmortems/README.md) |
| 决策记录 | [docs/decisions/](docs/decisions/README.md)（ADR）· `.agents/notes/`（改动级笔记） |
| 架构评审快照 | [docs/reviews/2026-07-24-architecture-review.md](docs/reviews/2026-07-24-architecture-review.md) |
| 贡献指南 | [CONTRIBUTING.md](CONTRIBUTING.md) |
| 安全漏洞上报 | [SECURITY.md](SECURITY.md) |
| 变更记录 | [CHANGELOG.md](CHANGELOG.md) |

## 许可证

[MIT](LICENSE)。

## 相关项目

- [fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust) — 同题 Rust 实现（块索引 `.fqc`，同名不同格式族、互不兼容）
- [fastq-tools](https://github.com/open-genomics/fastq-tools) — C++23 FASTQ 质控工具，同属组织 C++ 系统线
- [minibwa-rust](https://github.com/open-genomics/minibwa-rust) — 短读比对器（Rust 线）

<div align="center">

本仓库属于 [**Open Genomics**](https://github.com/open-genomics) · 高性能生物信息学系统软件与中文知识库

</div>
