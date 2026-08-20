# ADR-0001: 不引重并行框架

- 状态：**已采纳**
- 日期：2026-08-21
- 关联：`docs/roadmap.md`「不推荐」清单、`docs/postmortems/2026-07-13-legacy-architecture-debt.md`

## 背景

压缩/解压管线需要生产者-消费者并发。此前评估过引入框架化方案（TBB flow graph、
通用线程池、异步 I/O）的可行性。

## 决策

显式排除以下技术，管线并发只用手写 `MpmcQueue`（mutex + condition_variable +
`std::stop_token`）+ 固定 worker 数：

- **TBB flow graph / 通用线程池**：调度交给框架，职责边界模糊；本项目 worker 数
  固定（解析器/编码器/解码器各自独立计数），不需要通用调度器。
- **lock-free MPSC / 复杂 CAS 队列**：有界 mutex 队列配合条件变量与 stop_token
  已覆盖全部需求；无锁是另一课题，且内存序正确性风险与收益不匹配。
- **异步 I/O（io_uring / aio）**：引入平台复杂度，偏离并发流水线主题；同步 I/O
  在有界队列背压下吞吐足够。

## 后果

- 优点：依赖面极小（仅标准库 + zstd/xxhash）；故障边界（截断、校验和、内存上限）
  全在自有代码内可控。
- 代价：手写队列需自行保证正确性（队满反压、停止传播、高水位统计）；并行度提升
  依赖 `MpmcQueue` 而非现成调度器。
- v1 曾走"重排序 + 编码器状态与内存核算耦合在错误层级"的路线并整体删除
  （见 postmortem 2026-07-13），本决策是那次重构的固化。

## 验证

`tests/mpmc_queue_test` 覆盖队满/队空/停止传播/高水位；`parallel_parse_pipeline_test`
覆盖多 worker 有序提交不变量。
