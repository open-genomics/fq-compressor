# 构建、工具链与质量

## 工具链

* C++23 编译器：**GCC 14+** 或 **Clang 18+**
* **CMake 3.28+**
* **Conan 2.x**
* Linux / macOS；Windows 用 WSL 或 Docker

技术栈：C++23 · CMake 3.28+ + Ninja · Conan 2.x · Zstd · xxHash · GoogleTest

## 源码构建

```bash
git clone https://github.com/open-genomics/fq-compressor.git
cd fq-compressor
./scripts/build.sh clang-release
```

本地 preset：`clang-release` / `clang-debug` / `clang-asan` / `clang-tsan`。

```bash
./scripts/test.sh clang-debug      # 运行全部测试
./scripts/test.sh clang-asan       # sanitizer 构建 + 测试
./scripts/lint.sh format-check     # 检查格式
./scripts/lint.sh format           # 自动格式化
```

> **同名二进制 `PATH` 覆盖风险**：本仓库与 [fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust)
> 都安装名为 `fqc` 的二进制。若两者同时进入 `PATH`，后安装者（或 `PATH` 中更靠前的目录）会覆盖
> 另一个，请用 `which fqc` 确认实际调用的实现。

## ARM64 / AArch64（Linux）

源码与架构无关，但 aarch64 上需要按下面的方式准备工具链（以 Ubuntu 22.04 + 鲲鹏为例）：

1. 安装 Clang 18、libc++ 与 LLVM 18 归档工具：

```bash
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
    | gpg --dearmor -o /etc/apt/keyrings/apt.llvm.org.gpg
echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] \
https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" \
    > /etc/apt/sources.list.d/llvm-18.list
apt-get update
apt-get install -y --no-install-recommends \
    clang-18 clang-format-18 clang-tidy-18 llvm-18 \
    libc++-18-dev libc++abi-18-dev libunwind-18-dev libclang-rt-18-dev
```

2. 生成 armv8 Conan profile（不要复用写死 `x86_64` 的 `conan/profiles/clang`）：

```bash
export CONAN_HOME=/path/to/conan-cache   # 可选，建议放在大容量卷
CC=clang CXX=clang++ conan profile detect --force
conan install . --profile=conan/profiles/clang-armv8 \
    --output-folder=build/clang-debug --build=missing --lockfile=conan.lock \
    -s build_type=Debug -c tools.build:jobs=20
./scripts/build.sh clang-debug 21
./scripts/test.sh clang-debug
```

`tools.build:jobs` 要按容器可用核数（如 `nproc`）设置，不能依赖宿主的
`os.cpu_count()`。Release 构建把 `Debug` 换成 `Release`，输出目录换成
`build/clang-release`。若本机默认 `clang` 是 21 而 profile 写的是 18，请把
profile 的 `compiler.version` 改成 21，或用 `CC=/usr/bin/clang-18
CXX=/usr/bin/clang++-18` 固定编译器，避免 Conan 标错 ABI 版本。

3. **LTO 归档工具**：若 `PATH` 中存在其他 LLVM（例如 CANN 自带的 LLVM 15），CMake
   可能选中旧版 `llvm-ar`，归档 clang-18 的 LTO bitcode 时报
   `Not an int attribute (Producer: 'LLVM18' Reader: 'LLVM 15')`。用
   `clang++ -print-prog-name=llvm-ar` 确认它指向 LLVM 18；必要时显式传
   `-DCMAKE_CXX_COMPILER_AR=/usr/lib/llvm-18/bin/llvm-ar` 与
   `-DCMAKE_CXX_COMPILER_RANLIB=/usr/lib/llvm-18/bin/llvm-ranlib`。

4. **native 调优**：Release 在 aarch64 使用 `-mcpu=native`（clang 的
   `-march=native` 在 ARM 上只选架构特性、不选宿主 CPU）；需要可分发二进制时用
   `clang-release-portable`。

5. **多核参数**：21 核鲲鹏上 1 GiB 合成 illumina 语料实测 `--parse-workers 12`
   约 419 MiB/s，默认 4 约 212 MiB/s；`--frame-mib 64` 最优。解压/校验可用
   `--decode-workers` 调整 decoder 池（0 = 默认 4），实际收益受写盘与 I/O 限制，
   建议用真实语料复测。

## 质量与 CI

CI（`.github/workflows/ci.yml`，ubuntu-24.04 + clang-18）覆盖：clang-debug 构建与全部测试
（单元 + 集成 + 端到端）、clang-format 检查、`clang-asan`（ASan+UBSan）、`clang-tsan`
（TSan）、`clang-release-portable`（Release/IPO 配置编译 + 测试）、libFuzzer 短跑
（parser + archive reader）、coverage、clang-tidy 与决策笔记（notes）门禁。
错误码到退出码的映射见 `include/fqc/common/error.h`（`toExitCode`）。

### Sanitizer 环境限制

- LeakSanitizer 在部分受限环境不可用，CI 与本地均以 `ASAN_OPTIONS=detect_leaks=0` 运行
  （泄漏检测保留为发布机检查项）。
- ASan 下系统 libc++18 未插桩，异常对象释放会触发 alloc-dealloc-mismatch 误报，
  CI 以 `alloc_dealloc_mismatch=0` 关闭该子检查（其余 ASan/UBSan 检查保持）。
- TSan 在 Ubuntu 24.04 runner 上需把 `vm.mmap_rnd_bits` 降到 28 才可用
  （CI job 内已处理）。
- libFuzzer 运行时按 libstdc++ 构建，与 libc++ 不兼容：fuzz 构建整体走 libstdc++
  工具链（`clang-fuzz` 预设继承 `base` 而非 `clang-base`）。
- ASan preset 的 GTest 需与项目同工具链从源码构建（CI 用 `--build=gtest*`），避免预编译包
  混链在 gtest 静态注册阶段触发 libc++ 容器注解误报（heap-buffer-overflow）。

详见 [postmortems/2026-07-13-sanitizer-env-limitations.md](postmortems/2026-07-13-sanitizer-env-limitations.md)。
