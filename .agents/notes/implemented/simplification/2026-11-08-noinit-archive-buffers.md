# Agent Note: 归档载荷缓冲免零初始化

Status: implemented

## Problem

`RawFrame::ids/sequences/qualities` 与 `CompressedStream` 的解压输出缓冲都是
`std::vector<std::uint8_t>`，读取/解压路径统一是「`resize(n)` → 整块写入」：

- `readRawFrame`：`frame.ids.resize(idsSize)` 后立刻 `readExact` 覆盖全部字节；
- `decompressStream`：`output.resize(result)` 后由 `ZSTD_decompress` 写满；
- `CompressedStream::inputBuffer_`：同上。

`std::vector::resize(n)` 对新增元素做值初始化，对字节就是一次整块 memset。
实测成本约 8 ms/GiB（64 KiB 块）～30 ms/GiB（4 MiB 块），相对解码吞吐折算
约 0.2%–0.8%。C++23 的 `resize_and_overwrite` 只属于 `std::string`，
`std::vector` 没有等价接口。

## Decision

在 `include/fqc/common/types.h` 新增：

- `NoInitAllocator<T>`：无参 `construct` 为空操作（`uint8_t` 属隐式生存期类型，
  取得存储即创建对象，见 [basic.life]），带参 `construct` 仍执行 placement new，
  保证 `assign`/`insert`/`push_back`/`resize(n, v)` 的复制语义不变；
- `using ByteBuffer = std::vector<std::uint8_t, NoInitAllocator<std::uint8_t>>;`

`RawFrame`、`CompressedFrame`、`DecodedBuffer` 的载荷字段，`CompressedStream` 的
`inputBuffer_`，以及 `archive.cpp` 内部的 `Bytes` 别名统一改用 `ByteBuffer`。

## Alternatives considered

- **改 `std::string` + `resize_and_overwrite`**：标准且更「正统」，但载荷到处按
  `std::span<const std::uint8_t>` 传递，且 zstd/xxhash 接口都是裸指针 + 长度，
  换成 string 只会让类型与接口注释变复杂。
- **在 `readExact` 里先读进固定 scratch 再拷贝**：多一次 memcpy，与省下的
  memset 同量级甚至更贵。
- **什么都不做**：可接受（<1%），但改动被 `Bytes`/字段类型收口后成本很低。

## Consequences

- 每 GiB 载荷省下 8–30 ms 的 memset；端到端提升在噪声量级。
- `RawFrame` / `CompressedFrame` 的字段类型从 `std::vector<std::uint8_t>` 变为
  `ByteBuffer`：外部代码若把它绑定到 `std::vector<std::uint8_t>&` 会编译失败，
  其余 `.data()` / `.size()` / `std::span` 用法不变。归档字节格式零变化。
- 「resize 后、写入前不得读取」的约束写在分配器注释里；误用到依赖零值的缓冲
  （例如以 0 表示缺失的标志位数组）会得到不确定行为。

## Verification

- `./scripts/build.sh clang-debug` + `./scripts/test.sh clang-debug`：19/19 通过。
- `./scripts/test.sh clang-asan`：19/19 通过，无内存错误。
- `./scripts/lint.sh format-check` 通过。
- 端到端：30.9 MB 合成 FASTQ（120k reads，含 150/100 bp 混合）compress →
  decompress 与原始文件 `cmp` 逐字节一致，`verify` 通过。
- 微基准（同机 `clang++ -std=c++23 -stdlib=libc++ -O2`，1 GiB 累计 resize）：
  `vector<char>` 7.8–8.7 ms/GiB（64 KiB）、21–31 ms/GiB（4 MiB）；
  `NoInitAllocator<char>` 两侧均 ≈0 ms/GiB。
