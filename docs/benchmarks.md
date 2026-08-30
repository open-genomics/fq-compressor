# 性能基准台账

本文件是**入库的基准记录**：每次记录进仓库的性能 baseline 在这里留摘要与结论。
原始数据（JSONL）在 `perf-baselines/YYYY-MM-DD-<slug>/`，两者配套。

## 规范

- 每次新增 baseline：跑 `scripts/bench.sh run --slug <slug>`（或等效环境变量），
  把原始 JSONL 归档进 `perf-baselines/`，再把摘要追加到本文件并提交。
- 摘要必含：commit、环境（CPU/OS/内存预算）、各基准的中位数吞吐与极差、压缩比、
  RSS，以及相对上次 baseline 的变化（`scripts/bench.sh compare` 输出）。
- 环境不同（机器/容器/WSL2）的两次结果**不可直接对比**，须注明环境差异。

## 环境模板

- CPU / 核心数：
- OS（WSL2 需注明）：
- 构建：clang-release（版本见归档 JSONL 头部 `bin_a`）
- 内存预算：16 GiB（`--memory-limit 16384`）
- 数据：结构化合成 FASTQ（`FQC_PERF_DATA=structured`），5 次重复取中位数

---

## baseline — commit 26b2214（2026-08-30）

- 环境：WSL2（AMD Ryzen 7 5800H，12 核），Clang 21 Release，16 GiB 内存预算
- 归档：`perf-baselines/2026-08-30-baseline/`

| 基准 | 压缩 MiB/s（极差） | 解压 MiB/s（极差） | 压缩比 | 压缩 RSS |
|---|---:|---:|---:|---:|
| illumina_512mib | 772.6 (750.5–784.1) | 972.9 (875.6–1030.1) | 1051.5× | 927 376 KiB |
| illumina_1024mib | 848.2 (841.4–855.1) | 1041.3 (982.9–1095.6) | 1204.5× | 1 211 092 KiB |
| ont_1024mib | 361.9 (346.0–363.2) | 1932.5 (1796.9–2276.0) | 11419.0× | 2 098 892 KiB |

- 结论：首个入库 baseline，无历史对比。后续基准改动（并行解析、编码器、zstd 档位）
  应与此对比，压缩/解压吞吐回退或压缩比下降需给出解释。
