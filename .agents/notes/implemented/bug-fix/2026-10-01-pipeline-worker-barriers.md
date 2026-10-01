# Agent Note: 流水线 worker 异常屏障 + 解析器行长上限 + InFlightLimiter 加固

Status: implemented

## Problem

三条流水线（compress / parallel-parse / decompress）的 `std::jthread` 工作函数
都没有异常屏障：任何逃逸异常直接 `std::terminate`，`main` 的 catch-all 看不到
线程内异常。最现实的触发是 `FastqParser` 无行长上限——畸形/敌意输入的一条
多 GB"序列"行会让 `std::string` 增长到 `bad_alloc`；顺序路径被 `main` 优雅接住，
并行路径直接杀进程。

`InFlightLimiter`（`include/fqc/pipeline/in_flight_limiter.h`）的另一面：owner
越界访问 `vector<bool>` 是 UB；owner 不匹配的 `release` 会让 `sharedInFlight_`
下溢回绕、保留槽永久卡死，静默破坏"每 owner 保留额度"语义（数量不配对时
`inFlight_` 也下溢，整个内存界失效）。当前三处调用点都一致，属潜伏问题。

## Decision

- 三个流水线文件各加一个匿名命名空间辅助 `workerCrashError(stage, detail)`，
  所有 worker 体包 `try/catch(...)`：异常转 `kInternalError` 记录到对应阶段错误槽，
  并 `request_stop()`。parallel-parse 的 chunk-end marker 推送移到 try 之外，保证
  异常路径下 writer 的 ChunkOrderer 仍能收到该 chunk 的完成标记、不悬挂。
- `FastqParser` 增加 `maxLineBytes`（默认 64 MiB）：超长行在 `readLine` 中标记，
  `readRecord`/`readRequiredLine` 返回 `kFormatError`（含行号与上限），不再无限
  分配；`bytesConsumed_` 记账保持精确。`findFirstRecordStart` 同步接 `maxLineBytes`。
- `InFlightLimiter` 加 `assert`：`owner` 越界、`sharedInFlight_`/`inFlight_`
  下溢在 debug 构建直接失败；`canAcquireLocked` 对越界 owner 回退共享池（配合
  assert，越界只可能是逻辑 bug，release 构建下不 UB、不破坏计数）。

## Alternatives considered

- **worker 内逐调用点判空** — 无法覆盖 `bad_alloc`、`fmt` 格式化等非 Result 异常；
  屏障是唯一能兜住全部逃逸点的做法。
- **为 `InFlightLimiter` 引入 RAII 额度令牌** — 更彻底，但改动面大、三处调用点
  都要改接口；assert 已把误用变成 debug 可见的硬失败，代价/收益不划算。

## Consequences

- **收益**：恶意/损坏输入不再可能让流水线 `std::terminate`；limiter 误用从
  "静默破坏限额语义"变成"debug 断言失败"。
- **代价**：worker 体多一层 try/catch 嵌套（可读性小幅下降）；64 MiB 行长上限
  是新增的输入约束（真实读长远低于此，超长即畸形）。
- **已知上限**：行长上限检查发生在 `getline` 已分配该行之后（需先读完整行才能
  报"超长"），防的是"无限增长到 OOM"，不是防一次性大分配；`-rss_limit` 之外
  的峰值由上层 `memoryLimitBytes` 信封约束。

## Verification

- `tests/io/fastq_parser_test.cpp`：2 个新用例（超长序列行 `kFormatError`、超长
  头部行）。
- 既有全部测试 debug + ASan + TSan 17/17 全绿；fuzz（parser + archive reader）
  短跑无崩溃。
