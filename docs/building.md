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

CI（`.github/workflows/ci.yml`，ubuntu-24.04 + clang-18）覆盖：clang-debug 构建、全部测试
（单元 + 集成 + 端到端）、clang-format 检查，以及 `clang-asan`（ASan+UBSan）构建与测试门禁。
校验失败即报错，exit code 约定见 [AGENTS.md](../AGENTS.md)。

### Sanitizer 环境限制

- LeakSanitizer 在部分受限环境不可用，CI 与本地均以 `ASAN_OPTIONS=detect_leaks=0` 运行
  （泄漏检测保留为发布机检查项）。
- ASan 下系统 libc++18 未插桩，异常对象释放会触发 alloc-dealloc-mismatch 误报，
  CI 以 `alloc_dealloc_mismatch=0` 关闭该子检查（其余 ASan/UBSan 检查保持）。
- ASan preset 的 GTest 需与项目同工具链从源码构建（CI 用 `--build=gtest*`），避免预编译包
  混链在 gtest 静态注册阶段触发 libc++ 容器注解误报（heap-buffer-overflow）。

详见 [postmortems/2026-07-13-sanitizer-env-limitations.md](postmortems/2026-07-13-sanitizer-env-limitations.md)。
