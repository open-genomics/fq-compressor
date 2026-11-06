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

- CPU / 可用逻辑核数（WSL 注明分配数）：
- OS（WSL2 需注明）：
- 构建：clang-release（编译器版本与工作区改动见对应归档 README）
- 内存预算：16 GiB（`--memory-limit 16384`）
- 数据：`realistic` 合成 FASTQ（`FQC_PERF_DATA=realistic`，默认），5 次重复取中位数。
  `structured`/`random` 只作压力/回归 fixture，不能与真实语料或 `realistic` 横比

---

## baseline — commit 26b2214（2026-08-30，structured 压力基线）

- 环境：WSL2，AMD Ryzen 7 5800H，12 线程（WSL 分配），Clang 21 Release，16 GiB 内存预算
- 归档：`perf-baselines/2026-08-30-baseline/`

| 基准 | 压缩 MiB/s（极差） | 解压 MiB/s（极差） | 压缩比 | 压缩 RSS |
|---|---:|---:|---:|---:|
| illumina_512mib | 772.6 (750.5–784.1) | 972.9 (875.6–1030.1) | 1051.5× | 927 376 KiB |
| illumina_1024mib | 848.2 (841.4–855.1) | 1041.3 (982.9–1095.6) | 1204.5× | 1 211 092 KiB |
| ont_1024mib | 361.9 (346.0–363.2) | 1932.5 (1796.9–2276.0) | 11419.0× | 2 098 892 KiB |

> **不要把这里的 1051×/1204×/11419× 当作真实压缩比。** 本 baseline 使用
> `structured` 数据（6 个固定 4-base pattern 循环 + 固定 Q40 质量串），熵极低，
> 压缩后输出极小，吞吐主要反映 zstd 处理极小载荷与极少 I/O 的成本。它只适合
> 复现历史回归，不能与 `realistic`、真实语料或后续优化横比。

- 结论：首个入库 baseline，无历史对比。后续 **structured** 基准改动（并行解析、
  编码器、zstd 档位）可与此对比；涉及真实数据口径时应使用 `realistic` 或真实语料，
  不要拿本表下结论。

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

---

## baseline — commit 627fa7d（2026-11-05，realistic）

- 环境：WSL2，AMD Ryzen 7 8845HS，12 线程（WSL 分配，6 核 × 2 线程），
  Clang 18.1.3 Release（libc++），16 GiB 内存预算，Linux 6.18.54.1-microsoft-standard-WSL2
- 代码：`627fa7d` + 工作区改动（parser 行缓冲、`FQC_PERF_DATA=realistic` 默认、
  共享 `worker_error.h`、`sync_with_stdio(false)`；不含后续 ARM64 的 `--decode-workers`）
- 命令：`./scripts/bench.sh run --sizes "512 1024" --repeats 5 --slug realistic`
- 数据：`FQC_PERF_DATA=realistic`；短读 150 bp、长读 20 kbp，固定随机种子；
  默认 64 MiB frame、4 parser worker、quality zstd level 1
- 归档：`perf-baselines/2026-11-05-realistic/`（JSONL 头部含 HEAD commit 与运行参数；
  编译器版本、工作区改动见归档 README）
- 说明：基准名沿用目标尺寸 512/1024 MiB；生成器按近似 header 开销估算 read 数，
  实际输入为 470.51 / 941.02 / 1023.53 MiB。下表按实际输入计算吞吐。
  本归档使用旧版 header 估算（Illumina 实际输入偏小约 8%）；生成器已在后续修正，
  未来 baseline 的实际输入会更接近目标尺寸。

| 基准 | 实际输入 | 压缩 MiB/s（极差） | 解压 MiB/s（极差） | 压缩比 | 压缩 RSS | 解压 RSS |
|---|---:|---:|---:|---:|---:|---:|
| illumina_512mib | 470.51 MiB | 247.63 (234.08–261.39) | 361.93 (313.67–370.48) | 3.3775× | 1 056 176 KiB | 756 584 KiB |
| illumina_1024mib | 941.02 MiB | 265.82 (253.64–287.77) | 353.76 (330.18–384.08) | 3.3774× | 1 368 260 KiB | 1 035 716 KiB |
| ont_1024mib | 1023.53 MiB | 135.20 (120.55–150.29) | 605.63 (426.47–747.10) | 2.6257× | 2 091 224 KiB | 1 031 880 KiB |

- 结论：这是 realistic 口径下的**单二进制绝对 baseline**，不是 A/B，也不宣称任何
  优化收益。512/1024 MiB Illumina 的压缩吞吐接近（247.6 vs 265.8 MiB/s），差异可能
  来自输入尺寸、cache 和 WSL2 噪声，不应当作 scaling 结论。
- **不要与 2026-08-30 structured baseline 直接对比**：数据熵、输入尺寸、CPU、编译器和
  运行窗口都不同；structured 的 1051×/1204×/11419× 只用于历史压力回归。因此本轮
  没有运行 `scripts/bench.sh compare`——把它做成百分比只会误导。
- 本轮未做真实语料复测；真实压缩比与吞吐见 `docs/real-corpus.md`。