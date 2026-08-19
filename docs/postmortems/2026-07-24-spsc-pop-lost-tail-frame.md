# SPSC `pop()` 在 `close` 后丢失尾帧

- 日期：2026-07-24
- 严重度：high
- 状态：closed
- 引入点：`SpscQueue` 多 atomic 设计自始存在；`CompressPipeline::run` 接入并发主路径后暴露
- 相关：`include/fqc/pipeline/spsc_queue.h`、`tests/pipeline/pipeline_test.cpp:97`

## 症状

CI `pipeline_test` 中 `CompressPipelineTest.PairedRun` 偶发失败：

```
tests/pipeline/pipeline_test.cpp:107: Failure
Expected equality of these values:
  result->recordCount
    Which is: 0
  10U
    Which is: 10
```

特征：
- 仅 CI（GitHub Actions `ubuntu-24.04` runner）出现，本地 x86 反复跑 200 次不复现
- 失败的是**最后一个**测试用例 `PairedRun`，前序全过
- `recordCount=0` 而非部分值——一帧都没消费到
- 无 sanitizer 报错，无崩溃

## 复现

**本地无法稳定复现。** 关键观察：

- `clang-debug` 200 次、`clang-tsan` 50 次 PairedRun 全过
- CI 同一 commit 反复跑，失败率约 10–30%
- 失败窗口极窄：5 条 paired record（10 条总）体积小，全部累积进**同一帧**，靠 `close` 前最后一次 `pushFrame` 落入队列

`PairedRun` 的特殊性在于：输入小到不触发任何 `frameFull()` 切分，整批记录的生命周期是

```
reader: append×10 → pushFrame(1 帧) → close()
writer: pop() → ... → pop() → nullopt
```

若 writer 的 `pop` 在 reader 的 `pushFrame` 与 `close` 之间进入空判分支，且内存序使其看到 `closed_=true` 却没看到 `head_` 已推进，就会直接返回 `nullopt`，丢掉那唯一一帧。

## 调查

1. **先怀疑环境抖动（flaky）/资源问题。** 但失败模式是"数据少了一帧"而非超时/崩溃，且每次都是 `recordCount=0`（全丢），不像资源问题。决定深挖。
2. **本地复现尝试。** `clang-debug` 跑 200 次、`clang-tsan` 跑 50 次 PairedRun，全过。x86 TSO 内存模型下难复现弱序问题，符合"CI 偶发、本地不复现"的竞态特征。
3. **读 `SpscQueue` 实现。** 注意到 `head_`、`closed_`、`aborted_` 是三个**独立 atomic**，无共同同步点。`pop()` 先 acquire 读 `head_`（判空），再 acquire 读 `closed_`（判关）。两个 load 之间无 ordering 保证。
4. **构造竞态场景。** 生产者 `push`（`head_.store(release)`）紧接 `close`（`closed_.store(release)`）。消费者同一轮迭代：先读 `head_` 可能得 stale 值（=tail，判空），再读 `closed_` 得 true。`closed_` 的 acquire 只与 `closed_.store(release)` 建 happens-before，**不保证**之前的 `head_` load 能看到 `push` 的 store。弱内存模型下"新 closed + 旧 head"的撕裂快照合法。
5. **验证假设与 `PairedRun` 的吻合度。** 小输入使全部记录落入一帧，竞态窗口压缩到 `pushFrame→close` 这一对操作上，反而比大输入更易命中——解释了为何偏偏是最小的 `PairedRun` 失败而 `MultipleFrames`（100 条、多帧）不失败。
6. **确认 TSan 不报的原因。** 每次 load/store 都正确 acquire/release 配对，无数据竞争（race）；问题是合法同步下的"可见性顺序"，TSan 检测不到。靠根因推理而非工具定位。

## 根因

`SpscQueue` 用三个独立 atomic 协调状态（`head_`、`tail_`、`closed_`/`aborted_`），`pop()` 先读 `head_` 判空、再读 `closed_` 判关，两个 load 之间无共同同步点。生产者 `push`（`head_.store(release)`）后紧接 `close`（`closed_.store(release)`）时，消费者可能在同一轮迭代里看到 stale `head_`（=tail，判空）却看到 `closed_=true`，于是误判"队列空且已关闭"返回 `nullopt`，丢失刚 push 的帧。`closed_` 的 acquire 只与 `closed_.store(release)` 建 happens-before，不保证更早的 `head_` load 能看到 `push` 的 store。x86 TSO 下 store→store 与 load→load 不重排故本地难复现；CI 的并发时序更易命中窗口。

## 修复

`include/fqc/pipeline/spsc_queue.h`，`pop()` 在观察到 `closed_/aborted_` 后**再 acquire 重读一次 `head_`**：

```cpp
if (aborted_.load(acquire) || closed_.load(acquire)) {
    if (tail != head_.load(acquire)) {   // 终检：close 的 release 已使 push 的 head store 可见
        T item = std::move(storage_[tail]);
        tail_.store((tail + 1) & kMask, release);
        return item;
    }
    return std::nullopt;
}
```

为何这次重读有效：`closed_.load(acquire)` 读到 `true` 时，已与生产者 `closed_.store(release)` 建立 happens-before；而生产者的 `head_.store(release)` 被序列在 `closed_.store` 之前（同一线程 program order + release-release），故其写对消费者此刻可见。第二次 `head_.load(acquire)` 必能看到 `push` 推进的 head。

注：这是最小修复，不引入新同步原语。更彻底的方案是改用单一 atomic 打包 head/closed（如 seqlock 或高位标志位），但会改动整个队列的 push/pop 路径，收益与风险不成比例。修复 commit：`71d62f3`。

## 验证

- `clang-tsan` 构建，PairedRun 单跑 300 次：全过，无 ThreadSanitizer 告警
- `clang-tsan` 全套 8 个测试：100% 通过
- CI（`ubuntu-24.04`，clang-18）修复后 run `30093223771`：全绿

## 后续与教训

1. **多 atomic 状态机需显式同步点。** 各自 acquire/release 只保证单变量可见性，不保证跨变量的 load 顺序。若消费者逻辑依赖"先读 A 再读 B"的隐含顺序，必须有一处同时建立两者的 happens-before，否则弱内存模型下会观察到"新 B + 旧 A"的撕裂快照。
2. **`close` 不是 flush。** `close()` 只声明"不再有新 push"，不等价于"之前的 push 都已可见"。后者需要 `close` 与 `push` 之间有明确的 release-acquire 链。本例中链是存在的（同线程 program order + release store），但消费者必须在**读到 close 之后**重读 head 才能搭上这条链。
3. **CI 偶发失败 ≠ 环境噪声。** 本地不复现、CI 偶发的测试失败，尤其是"数据少了一个/多了"而非"崩溃/超时"，往往指向真实的内存序或并发缺陷。不要用 `--retry` 掩盖。
4. **小输入是竞态的放大器。** `PairedRun` 用 10 条小记录恰好使整批落入一帧，把竞态窗口压缩到 `pushFrame→close` 这一对操作上，反而比大输入更易命中。压力测试用大输入测吞吐，用**最小输入**测竞态边界。
5. **TSan 不万能。** TSan 检测数据竞争，但本 bug 是合法 acquire/release 下的"可见性顺序"问题，无 race（每次 load/store 都正确配对），TSan 不报。最终靠根因推理 + 修复后回归确认。
6. **未加专项回归测试。** 该竞态依赖弱内存模型时序，x86 上无法稳定复现，故未写会 flaky 的专项测试。防护依赖：code review 时审查 SPSC 多 atomic 的 load 顺序，以及任何对 `pop()`/`push()` 的改动须过 tsan 全套。
