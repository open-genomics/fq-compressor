# `GzipStreamBuf` move 丢失已解码缓冲

- 日期：2026-07-13
- 严重度：high
- 状态：closed
- 引入点：v2 phase 3（pre-75b7400）
- 相关：`src/io/compressed_stream.cpp`

## 症状

拼接的 gzip 输入在 stream move 之后可能丢失数据：当持有 stream 的对象被 move 之后，第二个 gzip member 产生的输出会被截断或为空。

## 复现

将一个拼接的 `.gz`（两个或更多 member）输入到 `GzipStreamBuf`，并在 member 之间对该 stream 进行 move 赋值。

## 调查

- 首先添加了拼接 gzip 支持，随后在多 member 输入上观察到间歇性的短读（short read）。
- 将输入截断为两个小 member，确认第二个 member 的第一个数据块（chunk）恰好在发生过 move 时缺失。
- 检查 move 赋值运算符：它转移了 zlib 状态，但没有转移已经解码进 streambuf 取区（get area）且尚未被消费的字节。

## 根因

move 赋值运算符在销毁源的 put/get 缓冲时，既没有 flush 也没有转移已解码但尚未读取的字节，因此在 move 发生时任何位于缓冲中的 inflate 输出都会丢失。

## 修复

在 move 赋值过程中保留未读取的 inflate 输出：在释放源缓冲之前，将待处理的取区字节转移到目标对象中。

## 验证

`compressed_stream_test` 覆盖了拼接 gzip 加 move 的场景；`v2_archive_engine_test` 对拼接 gzip 的 stdin 进行往返（round-trip）测试。静态分析（clang-tidy）标记了原始缺陷，并用于确认修复有效。

## 后续与教训

压缩流（compressed-stream）测试现已包含拼接 gzip 加 move 的用例；bugprone clang-tidy 检查在 `./scripts/lint.sh lint` 中运行。
