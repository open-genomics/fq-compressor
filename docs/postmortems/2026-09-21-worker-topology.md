# parser 扇出与 encoder 池过度绑定

## 症状

并行解析管线用同一个 `parallelism` 同时创建 parser 和 encoder。提高
`--parse-workers` 会按两倍速度增加工作线程，`K=64` 时还要加上 writer，实际约为
129 个线程；其中 parser 的数据并行需求并不意味着 encoder 也需要同样的并行度。

## 复现

在未压缩普通 FASTQ 上运行：

```text
fqc compress -i input.fastq -o output.fqc --parse-workers 64
```

旧拓扑会创建 64 个 parser、64 个 encoder 和 1 个 writer。小核机器或多个任务并发运行时，
线程调度和上下文切换会吞掉解析并行带来的收益。

## 调查

顺序压缩路径的 encoder 默认就是 4 个；并行解析路径只是复用了 parser 的参数，没有独立的
编码并行度概念。两类工作负载的瓶颈不同：parser 负责文件切分、边界扫描和 FASTQ 解析，
encoder 负责 2-bit 打包、校验和与三路 zstd。

## 根因

数据并行度和 CPU 变换并行度被一个配置变量表达，导致线程拓扑随 parser 扇出机械膨胀。
这不是队列或保序协议的必要条件；encoder 只需要消费 parser 产生的帧，数量超过 parser
通常不会增加可执行工作。

## 修复

保留 `--parse-workers` 对 parser 数量的控制，encoder 数量改为：

```text
N = min(K, kDefaultEncoderParallelism)
```

当前默认编码池大小为 4。因此 `K≤4` 时保持原有拓扑，`K>4` 时 parser 可以继续扩展，
encoder 不再同步扩展。`PipelineStats` 和信息日志同时报告 parser/encoder 数量，便于验证
实际拓扑。若 profile 采样已经消费完整个小文件，命令层直接走顺序 reader，不启动空的
parallel parser。

## 验证

- `K=8` 集成测试确认 parser 为 8、encoder 为 4，记录顺序和内容不变。
- profile 采样覆盖完整输入时，`--parse-workers 64` 仍走顺序路径。
- 单 worker 逐字节归档门禁保持通过。
- Debug、ASan、TSan 测试保持全量通过。

## 后续与教训

如果真实语料 profiling 证明 4 个 encoder 仍是瓶颈，再单独引入 encoder 参数或按 CPU 预算
调节；不要重新把两个阶段绑定。线程数改变必须同时观察阶段计时、队列等待和上下文切换，
不能只看总吞吐的一次运行。
