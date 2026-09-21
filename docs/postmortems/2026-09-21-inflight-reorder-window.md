# reorder 等待区无界与并行 credit 环形等待

- 日期：2026-09-21
- 严重度：high
- 状态：closed
- 引入点：v2 多帧并行流水线
- 相关：`include/fqc/pipeline/reorder_buffer.h`、`include/fqc/pipeline/chunk_orderer.h`、`include/fqc/pipeline/in_flight_limiter.h`

## 症状

流水线文档把两条深度为 4 的队列和 encoder 数量相加，作为在途帧上界。但 writer 会先从第二条队列
取出乱序帧，再放入 `ReorderBuffer` 或 `ChunkOrderer` 的 `std::map`；因此队列为空时，保序等待区仍可
持有大量帧。最早帧长时间未完成时，这个等待区没有独立的背压边界，内存上界不成立。

此外，直接在并行解析器前加一个共享总 credit 也不充分：后续 chunk 可以先占满窗口，最前面的 chunk
在提交首帧前阻塞，而 writer 必须先看到最前面的 chunk 才能归还 credit，形成环形等待。

## 复现

原始无界问题可由以下调度形态确定性构造：让 chunk 0 的首帧延迟完成，同时让 chunk 1 及之后的帧
连续完成。writer 会持续消费后续帧并插入 `ChunkOrderer::pending_`，上游队列不会因此保持满载。

共享 credit 的中间修复曾在以下现有用例中稳定挂起，说明并行解析还需要按 owner 保留 credit：

```bash
timeout 15s ./build/clang-debug/tests/parallel_parse_pipeline_test \
  --gtest_filter=ParallelParsePipelineTest.MultiWorkerPreservesRecordOrderAndContent
```

## 调查

1. 先对照 `ReorderBuffer`/`ChunkOrderer` 的消费路径检查队列深度假设，确认 writer 的 `pop()` 会把
   乱序条目移出队列并长期保存在 map 中。
2. 检查 ring buffer 的实现后确认模板 `Capacity` 还保留一个空/满判别槽位，可用容量实际是
   `Capacity - 1`，不能直接按模板参数核算窗口。
3. 首版共享 credit 修复通过 Debug 编译，但多 worker 测试挂起；调试线程栈显示所有 parser 在
   credit 条件变量上等待，而 encoder/writer 已分别等待空队列。
4. 检查 limiter 状态发现专属槽位已空闲，但 `notify_one()` 可能反复唤醒不满足 owner 谓词的其他
   parser，真正可继续的 parser 没有被唤醒。

## 根因

队列容量只约束“仍在队列中的条目”，不约束已经被 writer 取出但尚未按序提交的条目；文档把局部
队列背压误当成了全链路在途窗口。并行解析又有多个独立 source owner，单一总 credit 无法保证
排序链最前面的 owner 至少能提交一个帧。修复中的 owner-specific 条件变量还不能使用无条件的
`notify_one()`，否则唤醒对象可能与刚释放的专属槽位不匹配。

## 修复

- 新增 `InFlightLimiter`，源端在第一条队列入队前获取 credit，writer 完成有序提交后归还；窗口
  大小按两条队列的可用容量加 worker 数计算，覆盖 reorder 等待区。
- 并行解析为每个 chunk 保留一个专属 credit，其余容量共享；专属 credit 归还时 `notify_all()`，
  共享 credit 仍使用 `notify_one()`。
- `MpmcQueue` 明确暴露 `kUsableCapacity`，并拒绝 `Capacity == 1` 这种实际不可用的实例化。
- `PipelineStats`/日志增加 in-flight 高水位，更新架构与路线图中错误的内存模型描述。

## 验证

- `./scripts/lint.sh format-check` 通过。
- `./scripts/test.sh clang-debug`：17/17 通过。
- `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0 ./scripts/test.sh clang-asan`：17/17 通过。
- `./scripts/test.sh clang-tsan`：17/17 通过。
- 多 worker 并行解析、质量行以 `@` 起始、取消和 writer/sink 失败路径均通过现有测试。

## 后续与教训

- 在途内存必须按“源提交到有序提交”的全生命周期核算，不能只看某一条队列的深度。
- 多 source 的保序流水线除了总窗口，还需要 owner 级公平性或预留；否则提高窗口大小只能推迟而
  不能消除环形等待。
- 现有高水位是观测值，不替代每帧编码/解码前的内存预检；后续真实语料基准应同时记录 credit
  高水位、两个队列高水位和 reorder 等待量。
