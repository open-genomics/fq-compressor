# 性能基准归档

本目录保存每次本地性能基准的完整原始数据（JSONL），**入库**以便跨版本追溯与对比。

## 结构

```
perf-baselines/
└── YYYY-MM-DD-<slug>/            # 一次基准运行，slug 为本次运行标识
    ├── README.md                 # 归档说明：环境、参数、摘要、对比结论
    └── benchmark_results/
        └── benchmarks.jsonl      # 原始测量：中位数 + [min..max] 吞吐、RSS、压缩比
```

## 约定

- 每次运行由 `scripts/bench.sh run --slug <slug>` 生成（或直接调
  `tests/e2e/test_performance.sh` + `FQC_PERF_ARCHIVE`）。
- JSONL 头部注释记录 git commit 与运行配置（尺寸、重复次数、内存预算、二进制路径），
  归档与代码版本一一对应。
- 摘要与跨版本对比结论记入 `docs/benchmarks.md`（人类可读台账）；本目录存原始数据。
- 机器环境（CPU、OS、WSL2/原生）会显著影响吞吐，跨归档对比优先看同一环境的 A/B 模式
  （`FQC_PERF_BIN_B`），或参考各归档 README 的环境说明。

## 归档列表

| 日期 | slug | git commit | 环境 | 摘要见 |
|---|---|---|---|---|
| 2026-08-30 | baseline | 26b2214 | 见本目录 README | docs/benchmarks.md |
