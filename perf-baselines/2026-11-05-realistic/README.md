# 基准归档 2026-11-05-realistic

- git commit: 627fa7d（另含工作区改动：parser 行缓冲、realistic 默认、worker_error、stdio 同步；
  工作区 diff 未记录哈希，精确复现需先提交这些改动）
- 参数: sizes=[512 1024] repeats=5 warmup=1 sla=0 memory=16384MiB
- 数据: `FQC_PERF_DATA=realistic`，短读 150 bp、长读 20 kbp，固定随机种子
- 构建: clang-release / Clang 18.1.3 / libc++
- 环境: WSL2，AMD Ryzen 7 8845HS，12 线程（WSL 分配，6 核 × 2 线程），16 GiB 内存预算
- 二进制: build/clang-release/src/fqc
- 实际输入: illumina_512mib 470.51 MiB；illumina_1024mib 941.02 MiB；
  ont_1024mib 1023.53 MiB（基准名沿用目标尺寸；本归档使用旧版 header 估算，
  Illumina 实际输入偏小约 8%，生成器已在后续修正）

原始数据: benchmark_results/benchmarks.jsonl
摘要与对比: docs/benchmarks.md
