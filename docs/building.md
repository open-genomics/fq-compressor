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
