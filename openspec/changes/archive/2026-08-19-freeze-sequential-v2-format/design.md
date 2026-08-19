# Design: freeze-sequential-v2-format

## Evidence

- `src/format/archive.cpp` 定义 8-byte `FQCV2\r\n\x1A` magic、32-byte header、72-byte frame header 和 40-byte footer；
- reader 在 body 前验证 version/header size/flags/profile/codecs/header checksum；
- 已有 format tests 较丰富，但仓库没有稳定跨版本 archive fixture。

## Fixture set

- `v2-se-minimal`: 单端、最小合法 FASTQ；
- `v2-pe-minimal`: 配对、可验证 read count/order；
- `v2-profiled`: 覆盖当前稳定 profile/header fields；
- 损坏 cases 从合法 fixture 在测试中确定性派生：magic、header checksum、frame size/checksum、footer checksum、截断、unsupported version/codec/flags。

manifest 记录 generator source SHA/version、build profile、command、input/archive hashes、稳定 metadata 和 expected behavior。fixture 不包含敏感数据。

## Normative boundary

主规格记录 bytes layout/endian/field semantics/error category。压缩 payload 若由 zstd 版本影响而非 canonical，不冻结 current writer 全文件 SHA 作为永恒输出要求；frozen archive 必须持续被 future reader 解码。

## Allowed surface

- `openspec/`
- `tests/`、fixture/manifest/test support
- `docs/`、`ARCHITECTURE.md`、README 格式链接、CHANGELOG
- 仅为暴露/测试既有常量所需的最小 format header 变动；不得改变 bytes

## Risk

RC fixture 的生成器来源可能不明确。必须使用仓库 tag/commit 和可复现命令；无法证明来源的现有文件不能标成 normative fixture。
