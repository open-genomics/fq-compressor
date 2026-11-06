# Agent Note: 抽取共享 worker stage

Status: implemented

## Problem

三条流水线（普通文件压缩、并行解析压缩、解压/verify）各自手写了一份 worker 循环：

```text
while (!stop_requested)
    pop -> 处理 -> push
    + 记录首个错误 + request_stop
    + std::exception / ... 异常屏障
    + worker 本地计时累计后合并
```

三份实现的控制流、错误优先级和计时口径几乎逐行相同，但任何一处修正——例如错误只保留
第一次失败、异常也要 request_stop、计时必须在 pop 返回后停止——都要同时改三处。解压侧
刚补上阶段遥测后，这种重复第一次变得明显：三个 worker 循环的差异只剩下 handler 本身。

这仍然符合仓库“第三个重复模式再抽”的原则：不是为未来可能的流水线预先设计框架，而是
在第三份重复已经存在、且差异已被遥测统一压缩到 handler 内之后抽取。

## Decision

新增 `include/fqc/pipeline/worker_stage.h`：

- `WorkerStageTimings`：worker 本地累计 `popNs` / `computeNs` / `secondaryNs` / `pushNs`；
- `WorkerStageTimingSink`：原子汇总，worker 退出时合并一次，热路径不做原子写；
- `startWorkerStage(...)`：创建 N 个 `std::jthread`，每个线程执行
  `pop -> handler -> push`，并统一处理错误记录、取消请求、异常屏障和计时合并。
  输入队列和输出队列的容量分别作为模板参数推导，helper 不假设两端容量相同。

helper 只接管 worker 执行循环。以下仍由每条流水线自己掌握：

- 队列何时关闭；
- reader / writer 线程的创建顺序与 join 顺序；
- 线程拓扑和 worker 数量；
- handler 如何构造输出、如何解释 marker / 帧 id / chunk 顺序；
- 队列、stop source、错误槽和计时 sink 的生命周期。

handler 返回 `Result<Output>`：错误由 helper 记录到调用方提供的
`std::optional<Error>& firstError`，只在首次失败时写入，然后 `request_stop()` 并结束
当前 worker。异常被视为 worker crash，同样走首个错误槽和取消路径，不让异常穿过线程边界。

`parallel_parse_pipeline` 的 chunk-end marker 留在 handler 内直接返回 `OrderedItem`；
helper 不知道 marker 的存在，也不参与顺序语义。

## 抽取后发现的隐藏假设

第一版 `startWorkerStage` 只接受一个 `Capacity`，要求输入和输出队列容量相同。三条现有
流水线恰好都使用 `kDefaultQueueDepth`，所以生产代码编译通过；新增的直接测试使用 8 槽输入
和 64 槽输出，立刻暴露了这个假设。

这不是测试写错，而是 helper 的契约过窄：worker stage 只关心“从输入取、向输出放”，两端
容量没有理由绑定。修复是把 `InputCapacity` / `OutputCapacity` 分开推导，调用方仍无需显式
写模板参数。结论是：**通用组件的测试应该故意使用与生产不同的形状**，否则只能证明“当前
调用点能编译”，不能证明契约本身成立。

## Alternatives considered

- **保留三份手写循环**：改动最小，但三处错误处理和计时口径会继续漂移；新增观测项或
  修正取消语义仍需三点同步，正是这次抽取要消除的成本。
- **抽完整 pipeline / DAG 调度框架**：可以进一步统一 reader、worker、writer 的创建、
  关闭与 join，但会引入调度节点、生命周期协议和新的同步面。当前拓扑固定为
  reader -> worker -> writer，收益不足以抵消概念成本，也与
  [统一结果模型](2026-11-06-unified-pipeline-results-and-telemetry.md) 中“不引入通用 DAG”
  的决定冲突。
- **让 helper 接管队列关闭和线程 join**：调用方会失去对关闭时序的控制。压缩路径必须在
  reader 结束后关闭 queue1、在 encoder join 后关闭 queue2，解压路径还要在 writer 之后
  检查 footer；把生命周期塞进 helper 会把简单的线性时序变成回调或状态机。
- **用 `std::function` 抹平 handler 类型**：减少模板实例化，但会引入间接调用和类型擦除，
  而 handler 本来就是编译期已知的 lambda；模板参数更直接，也没有运行时成本。

## Consequences

- 三条流水线共减少约 130 行重复控制流，worker 循环只剩 handler 和启动调用；
- 输入/输出队列容量独立推导，helper 不依赖“两条队列深度相同”的隐含前提；
- 新增 `tests/pipeline/worker_stage_test.cpp` 直接覆盖成功路径、错误传播、异常屏障、
  stop token 唤醒和计时合并；这些路径中的异常与 stop 语义很难由端到端测试稳定触发；
- 错误优先级、异常屏障、取消语义和阶段计时只有一份实现，后续修正不会漏掉某条路径；
- helper 是纯头文件模板，不新增编译单元，也不改变线程拓扑或队列生命周期；
- handler 必须可复制、不得抛异常（异常仍会被屏障捕获，但视为 bug）；`stageName` 必须是
  生命周期覆盖线程的字符串（当前均为字面量）；
- worker 计时仍按线程本地累计、退出时合并，保持原先的低开销口径；
- 归档字节布局不变：这次只改控制流，不触碰编码器、zstd 参数或帧顺序。

## Verification

- `clang-debug` / `clang-release` / `clang-asan` / `clang-tsan`：18/18 通过；
- 同一输入分别用改动前、改动后的 release 二进制压缩，单帧与 8 帧归档均逐字节一致
  （`cmp`），解压 round-trip 一致；
- 8 帧 4 MiB 输入确认 worker pop / compute / secondary / push 计时均非零；
- `clang-format-18` 与 `scripts/notes.sh verify` 通过；
- 1 MiB realistic 端到端 benchmark 通过，未改写既有 baseline；
- 64 MiB realistic A/B（A = 本提交，B = 改动前 `4fa17f7`，3 次交错）：
  - Illumina：压缩 A 169.78 / B 179.48 MiB/s，解压 A 251.28 / B 241.61 MiB/s；
  - ONT：压缩与解压中位数完全相同（125.43 / 266.54 MiB/s）；
  - 两条方向不一致，差异落在 WSL2 噪声内；RSS 基本一致（202.5 vs 202.8 MiB、
    136.0 vs 135.9 MiB）。没有可归因于 worker stage 的稳定性能变化。
