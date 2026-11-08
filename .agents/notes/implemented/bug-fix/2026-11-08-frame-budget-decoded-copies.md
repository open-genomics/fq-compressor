# Agent Note: 解压帧预算计入解码后的记录字符串

Status: implemented

## Problem

`estimateDecompressionPeak` 只累计记录数组、三路 raw 流和三路压缩流。
`decodeRawStreams` 实际会把每路 raw 流解码成 `ReadRecord` 的 `std::string`：
id/comment、quality 各约一份 raw 大小，2-bit 序列解码后约为 raw 的 4 倍。
恶意帧可以伪造很小的 raw/压缩尺寸，让预检通过后在实际解码时分配数倍内存。

## Decision

- 在原有 raw/压缩流之外，额外汇入解码副本：
  `rawIds + 4 × rawSequences + rawQualities`（含溢出检查）。
- 估算仍以 `recordCount × sizeof(ReadRecord) × 2` 作为记录数组上界。
- `HigherQualityLevelNeverGrowsArchiveAndRoundTrips` 的 reader 预算从 1 MiB
  调整为 8 MiB：2000 条 150 bp 记录的真实峰值（记录数组 + raw 流 + 解码
  字符串 + 压缩流）已经超过 1 MiB，旧预算只是建立在低估之上。

## Verification

- `ArchiveTest.FrameBudgetCountsDecodedRecordStringCopies`：预算高于旧估算、
  低于新增副本，断言读取失败并返回 `kFormatError`。
- `./scripts/test.sh clang-debug`：19/19 通过。

## Alternatives considered

- **只按 raw 大小估算**：即原实现，低估解码后的字符串副本，恶意帧可绕过。
- **按实际字符串容量精确计算**：需要先完整解码，等于把检查放到分配之后，
  失去预检意义。
- **调低位置/质量统计上限来绕开预算**：与本问题无关，且会改变输出语义。

## Consequences

- 帧预算更接近真实峰值，超预算的帧会更早被拒绝；依赖极小预算读取大帧的
  调用方需要相应放宽（仓库内只有该回归测试受影响）。
