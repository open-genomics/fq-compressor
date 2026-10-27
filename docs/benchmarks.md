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

---

## baseline — commit b42333c + ARM 适配/打包优化（2026-10-27）

- 环境：Linux 5.15 aarch64，HiSilicon Kunpeng-920，cgroup cpuset 21 核（单 NUMA node），
  cgroup 内存上限 200 GiB；Clang 18.1.8 + libc++ + LTO，`-mcpu=native`（tsv110）
- 归档：`perf-baselines/2026-10-27-kunpeng920-21c/`
- 参数：默认 `--parse-workers 4`、`--frame-mib 64`、`--memory-limit 16384`，5 次重复取中位数
- 说明：本归档已包含 2-bit 打包的查表 + 4 碱基无分支展开优化；解压侧不受该优化影响，
  解压极差主要来自运行噪声。

| 基准 | 压缩 MiB/s（极差） | 解压 MiB/s（极差） | 压缩比 | 压缩 RSS |
|---|---:|---:|---:|---:|
| illumina_256mib | 206.5 (204.9–208.2) | 270.4 (267.6–273.2) | 1118.8× | 631 120 KiB |
| illumina_1024mib | 210.3 (185.2–213.3) | 294.6 (248.6–318.7) | 1204.5× | 1 019 812 KiB |
| ont_1024mib | 95.8 (95.2–96.9) | 592.0 (569.0–665.1) | 11419.0× | 2 074 248 KiB |

- 与 2026-08-30 x86 baseline **不可直接对比**（不同 CPU/架构/容器）：同尺寸下
  illumina_1024 压缩约慢 4.0×、解压约慢 3.5×；ont_1024 压缩约慢 3.8×、解压约慢 3.3×。
  压缩比与 x86 baseline 一致（1204.5× / 11419.0×），说明归档字节层面的算法行为跨架构一致。
- 手工调参（未入归档）：同机 1 GiB illumina 上 `--parse-workers 12` 中位数约
  **419 MiB/s**，默认 4 约 212 MiB/s；打包优化前分别为约 331 / 197 MiB/s（+27% / +8%）。
  `--frame-mib 64` 优于 16/32。解压侧 `--decode-workers 4` 已最快（写盘受限），
  加到 8/12 反而变慢。
- 结论：ARM64 首个入库 baseline。后续 ARM 调优应以本归档为同机对照，不要直接拿 x86
  数字做回归判断。
