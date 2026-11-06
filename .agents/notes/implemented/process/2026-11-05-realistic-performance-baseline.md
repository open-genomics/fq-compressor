# Agent Note: 性能基准默认改为 realistic 合成数据

Status: implemented

## Problem

`tests/e2e/test_performance.sh` 默认 `FQC_PERF_DATA=structured`：序列只在 6 个
固定 pattern 间循环，质量值固定为 `"I" * read_len`。入库 baseline
`perf-baselines/2026-08-30-baseline/` 因此出现 1051×/1204×/11419× 的压缩比。
这类数据压缩后输出极小，吞吐测量主要反映“极低熵 zstd + 极少 I/O”，不能代表
真实 FASTQ；用它判断优化会系统性失真。

## Decision

- `FQC_PERF_DATA` 默认改为 `realistic`；按读长选择：
  - 短读：Illumina-like，坐标型 ID、位置相关质量分布、少量低质量离群；
  - 长读：ONT-like，`runid=`/channel 头、较宽质量分布。
- 保留 `structured` 与 `random` 作为压力/回归 fixture，不再作为默认 baseline。
- 保留 `illumina-realistic`/`ont-realistic`/`novaseq-realistic` 显式模式，便于
  单独复现某类数据。
- 生成器的固定种子、短/长读各自的 header 字节估算、数据模式写入 JSONL 的 `data` 字段；
  旧 baseline 继续保留，但在台账中标注“structured 压力数据，不可与真实语料横比”。
- Illumina header 使用标准 7 段；未知 `FQC_PERF_DATA` 直接报错；小尺寸 JSON 数字
  补前导零，`/proc/uptime` 的 10ms 精度下限为 0.01s，避免 quick 模式除零。
- `bench.sh` 首次归档时自动记录 CPU/编译器，源码 dirty 时标记 `+ 工作区`；重跑保留
  手工 README，避免覆盖溯源信息。

## Alternatives considered

- **直接改用真实语料切片**：最有代表性，但 100 MiB 级切片不适合每次本地 benchmark；
  真实语料继续由 `docs/real-corpus.md` 和 `scripts/fetch_real_corpus.sh` 承担，
  合成数据只做可重复的回归/吞吐排序。
- **继续用 structured，只加注释**：无法阻止后续优化在 1000× 数据上得出错误结论；
  默认值必须改。
- **用 random 模式作为默认**：比 structured 好，但质量值仍是 10 符号均匀随机，
  缺少位置相关性和真实 ID 结构；不作为默认。
- **在 benchmark 里直接调用外部压缩器生成 gz**：会增加依赖；gz 路径留作后续
  独立 benchmark，不在本轮改生成器时混入。

## Consequences

- 收益：默认 benchmark 的压缩比回到 3×/2.6× 量级（64 MiB realistic 实测），
  与 README/真实语料的量级一致；后续优化不会再被 1000× 数据误导。
- 代价：realistic 生成器比 structured 慢（逐碱基质量采样），大尺寸 benchmark
  的准备时间变长；这是可重复性与真实性的取舍。CI 的 quick/小尺寸不受影响。
- 旧 `perf-baselines/2026-08-30-baseline` 仍是合法历史数据，但只能作为
  structured 压力基线，不能和 realistic/真实语料直接比较。

## Verification

- `FQC_PERF_DATA=realistic FQC_PERF_SIZES=1 FQC_PERF_REPEATS=1 ./tests/e2e/test_performance.sh`
  通过；1 MiB Illumina ratio 3.37×、ONT ratio 2.62×，round-trip `cmp` 通过。
- 64 MiB realistic 本地 A/B 通过（旧/新二进制归档 `cmp` 一致；解压输出 `cmp` 一致）。
- 1 MiB realistic 复测（生成器修正后）：Illumina 实际 0.98 MiB、ONT 0.99 MiB；
  Illumina header 为 7 段；JSONL 逐行可被 Python `json.loads` 解析；
  `FQC_PERF_DATA=bogus` 返回非零。
- 512/1024 MiB realistic 正式 baseline 已归档：
  `perf-baselines/2026-11-05-realistic/`。原始 JSONL 头部记录 HEAD commit、运行参数与
  二进制；编译器、工作区改动见归档 README。`docs/benchmarks.md` 摘要按实际输入尺寸
  （470.51/941.02/1023.53 MiB）记录中位数、极差、压缩比与 RSS。
- 旧 `2026-08-30-baseline` 继续保留，但只能作 structured 压力回归；`docs/benchmarks.md`
  已明确标注不可与 realistic/真实语料直接比较。
- 2026-11-05 归档使用旧版 header 估算（Illumina 实际输入 470.51/941.02 MiB，比目标小
  约 8%）；已在归档 README 与台账标注，未重跑该归档。
