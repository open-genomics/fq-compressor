# Agent Note: 当前预算公式下的 realistic 性能基线

Status: implemented

## Problem

`perf-baselines/2026-11-05-realistic/` 是 realistic 口径的首个正式 baseline，但它
生成于聚合内存预算修复之前（`627fa7d` 加未入库的工作区改动）。2026-11-06 的预算
修复改变了 64 MiB 档的帧大小，README 仍以“旧帧预算公式、不可与当前二进制横比”
标注阶段 H 的 148.84 MiB/s，导致文档没有一组能代表当前 HEAD 的绝对吞吐数字。
后续任何优化都缺少同版本锚点。

## Decision

用 `513b9f9` 的 clang-release 二进制重跑默认 realistic 基准：

```bash
./scripts/bench.sh run --sizes "512 1024" --repeats 5 --slug realistic-64mib
```

- 参数保持默认：16 GiB 内存预算、64 MiB 目标帧、4 parser、quality zstd level 1。
- 原始 JSONL 与归档说明写入 `perf-baselines/2026-11-06-realistic-64mib/`。
- `docs/benchmarks.md` 追加当前 baseline 章节；README 性能表改为这组数字，并明确
  它是绝对 baseline，不是优化 A/B。
- 2026-11-05 与 2026-08-30 归档继续保留，但不作为当前版本的对比基准。

## Results

| 基准 | 实际输入 | 压缩 MiB/s（极差） | 解压 MiB/s（极差） | 压缩比 |
|---|---:|---:|---:|---:|
| illumina_512mib | 502.61 MiB | 341.91 (330.66–361.58) | 353.95 (324.26–408.62) | 3.3313× |
| illumina_1024mib | 1005.22 MiB | 331.75 (324.26–343.07) | 410.29 (391.13–413.67) | 3.3312× |
| ont_1024mib | 1024.03 MiB | 317.03 (253.47–323.03) | 616.88 (530.58–758.54) | 2.6257× |

压缩 RSS 分别为 946 392 / 1 259 292 / 1 686 512 KiB。ONT 的压缩极差明显宽于
Illumina，说明长读路径对运行窗口更敏感；中位数仍保留为锚点。

## Alternatives considered

- **只改 README 标注，不重跑**：最省时间，但后续优化仍没有同版本锚点；把旧数字
  反复解释成“不可横比”不是可维护的文档状态。
- **把 2026-11-05 的数字当当前基线**：它早于预算修复，且包含未入库的工作区改动，
  无法对应任何可复现的 commit。
- **构建旧二进制做同窗口 A/B**：旧 baseline 的工作区改动不可复现，且本轮没有代码
  性能改动；同窗口 A/B 适合验证某个具体优化，不适合建立绝对基线。
- **用 64 MiB 预算重跑**：能覆盖低内存档，但 README 性能主表一直使用 16 GiB 默认档；
  64 MiB 的帧缩小与 RSS 已由聚合内存预算笔记单列，混在一起反而难读。

## Consequences

- 收益：当前 HEAD 有一组可复现的绝对基线；README 不再把“旧公式”数字放在主表。
- 代价：一次约 15 分钟的基准；数字受 WSL2 运行窗口影响，只能作为后续同窗口 A/B
  的锚点，不能跨日比较。
- 与 2026-11-05 的差异（尤其 ONT 压缩 +134%）不归因于代码。两者虽然同机同编译器，
  但运行窗口不同、旧归档含未入库改动、生成器 header 估算已修正；要判断代码影响，
  必须重新做同窗口 A/B。

## Verification

- JSONL 头部记录 commit、运行参数与二进制；归档 README 记录 CPU、编译器与 WSL2 版本。
- `test_performance.sh` 内置 round-trip `cmp` 与 `verify`，三组基准均通过。
- `scripts/bench.sh compare 2026-11-05-realistic 2026-11-06-realistic-64mib` 仅作
  参考输出，不写进结论作为收益声明。
- 本轮未改代码，未重跑单测；文档改动由 `scripts/notes.sh verify` 与 CI 校验。
