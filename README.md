# fq-compressor


> **Format family**: `fqc-sequential/v2` — command `fqc`, extension `.fqc`,
> magic `46 51 43 56 32 0D 0A 1A`. This is distinct from the Rust implementation's
> `fqc-indexed/v2` (magic `89 46 51 43 0D 0A 1A 0A`). Readers reject the other
> family's magic with an explicit unsupported-format-family error; extension
> alone cannot select a decoder.

**把 FASTQ 压成小而可校验的归档，内存可控，管道友好。**

[![CI 状态](https://github.com/open-genomics/fq-compressor/actions/workflows/ci.yml/badge.svg)](https://github.com/open-genomics/fq-compressor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![CMake 3.28+](https://img.shields.io/badge/CMake-3.28%2B-blue.svg)
![Conan 2.x](https://img.shields.io/badge/Conan-2.x-blue.svg)

[快速开始](#快速开始) · [架构](ARCHITECTURE.md) · [算法](ALGORITHM.md)

## 解决什么问题

FASTQ 文件大、传输贵、存档要防静默损坏。fq-compressor 针对这三点：

* **体积小** — 2-bit 打包碱基 + Zstd，随机合成数据压缩比约 2.8–2.9×。
* **可校验** — 全局头、逻辑帧、footer 三层 XXH64。`verify` **完整解码**并验证全部结构、逻辑内容与 footer，但不写 FASTQ；CPU/内存成本与 `decompress` 同量级，不是常数时间元数据检查。XXH64 用于发现随机损坏，不提供对恶意篡改的密码学认证。
* **内存有界** — 默认 16 GiB 预算，最低 64 MiB；逐帧保守估算峰值，不会 OOM。

不支持随机访问、按区间提取、有损压缩、非 FASTQ 输入。详见 [已知限制](#已知限制)。

## 快速开始

### 源码构建

```bash
git clone https://github.com/open-genomics/fq-compressor.git
cd fq-compressor
./scripts/build.sh clang-release
```

下文示例用 `$fqc` 代指可执行文件路径：

```bash
fqc=./build/clang-release/src/fqc
```

### 用法

```bash
$fqc compress   -i reads.fastq.gz -o reads.fqc    # 压缩
$fqc decompress -i reads.fqc        -o out.fastq   # 解压
$fqc verify     reads.fqc                          # 完整解码校验，不写 FASTQ
```

双端测序：

```bash
$fqc compress -i R1.fastq.gz -2 R2.fastq.gz -o paired.fqc
```

完整参数：`$fqc --help`。字节布局：[ARCHITECTURE.md](ARCHITECTURE.md)。

## 性能

合成数据（阶段 H A/B 同窗口，64 MiB 内存预算，随机 FASTQ ×5 次中位数）：

| 数据 | 压缩（并行解析） | 相对顺序解析 | 压缩比 |
|---|---:|---:|---:|
| Illumina-like 150 bp | 148.84 MiB/s | +47.7% | 2.96× |
| ONT-like 20 kbp | 与顺序路径持平（−1.3%，噪声内） | — | 2.84× |

WSL2 下 wall-clock 波动较大，同机重跑结果可能低于上表，仅供参考。真实生物语料的 round-trip、压缩比与质量流门槛复测见 [docs/real-corpus.md](docs/real-corpus.md)。

## 设计

* **顺序帧** — 独立自校验帧，压缩/解压/校验共用一个引擎。
* **紧凑编码** — 大写 A/C/G/T 打包为 2 bit；其余 IUPAC 字符和小写碱基按原始位置精确保留。
* **内存有界** — 逐帧保守峰值估算后再分配。
* **管道友好** — 支持 stdin/stdout；普通文件先写临时文件，成功后原子替换。
* **双端相邻** — R1/R2 成对存储，帧边界不拆开配对。
* **多层校验** — XXH64 覆盖全局头、每个逻辑帧、结尾 footer（完整性检测，非密码学认证）。

机制细节见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 技术栈

C++23（GCC 14+ / Clang 18+）· CMake 3.28+ + Ninja · Conan 2.x · Zstd · xxHash · GoogleTest

## 已知限制

* 不支持随机访问、按区间提取 reads。
* 不支持有损压缩、原始顺序重排。
* 仅支持 FASTQ 格式。
* 合成数据不能代表真实压缩比：短读质量集中时更高，长读质量近满字母表时可以更低。公开切片实测见 [docs/real-corpus.md](docs/real-corpus.md)。

## 构建

* C++23 编译器：**GCC 14+** 或 **Clang 18+**
* **CMake 3.28+**
* **Conan 2.x**
* Linux / macOS；Windows 用 WSL 或 Docker

## 质量

CI（`.github/workflows/ci.yml`，ubuntu-24.04 + clang-18）覆盖：clang-debug 构建、全部测试（单元 + 集成 + 端到端）、clang-format 检查，以及 `clang-asan`（ASan+UBSan）构建与测试门禁。校验失败即报错，exit code 约定见 [AGENTS.md](AGENTS.md)。

本地另有 `clang-release` / `clang-asan` / `clang-tsan` preset。注意环境限制：LeakSanitizer 在部分受限环境不可用，CI 与本地均以 `ASAN_OPTIONS=detect_leaks=0` 运行（泄漏检测保留为发布机检查项）；ASan 下系统 libc++18 未插桩，异常对象释放会触发 alloc-dealloc-mismatch 误报，CI 以 `alloc_dealloc_mismatch=0` 关闭该子检查（其余 ASan/UBSan 检查保持）；ASan preset 的 GTest 需与项目同工具链从源码构建（CI 用 `--build=gtest*`），避免预编译包混链在 gtest 静态注册阶段触发 libc++ 容器注解误报（heap-buffer-overflow）。详见 `docs/postmortems/2026-07-13-sanitizer-env-limitations.md`。

## 文档

| 目的 | 位置 |
|---|---|
| 构建与首次运行 | 本文件 |
| 命令参数 | `$fqc --help` |
| 压缩算法与原理 | [ALGORITHM.md](ALGORITHM.md) |
| 架构与字节布局 | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 变更记录 | [CHANGELOG.md](CHANGELOG.md) |
| 并发路线图（A–H 已收束） | [docs/roadmap.md](docs/roadmap.md) |
| 真实语料验收 | [docs/real-corpus.md](docs/real-corpus.md) |

## 许可证

[MIT](LICENSE)。
