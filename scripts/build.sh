#!/usr/bin/env bash
# scripts/build.sh - 统一构建脚本

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# 默认值
PRESET="${1:-clang-debug}"
JOBS="${2:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

print_usage() {
    echo "Usage: $0 [preset] [jobs]"
    echo ""
    echo "Available presets:"
    echo "  clang-debug      - Clang Debug build (recommended for development)"
    echo "  clang-release    - Clang Release build"
    echo "  clang-asan       - Clang with AddressSanitizer"
    echo "  clang-tsan       - Clang with ThreadSanitizer"
    echo "  clang-coverage   - Clang build with code coverage instrumentation (llvm-cov)"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build with clang-debug preset"
    echo "  $0 clang-release      # Build with clang-release preset"
    echo "  $0 clang-asan 4       # Build with clang-asan using 4 jobs"
}

# 检查参数
if [[ "$PRESET" == "-h" || "$PRESET" == "--help" ]]; then
    print_usage
    exit 0
fi

# 验证 preset 是否存在
VALID_PRESETS="clang-debug clang-release clang-asan clang-tsan clang-coverage"
if ! echo "$VALID_PRESETS" | grep -qw "$PRESET"; then
    echo "Error: Unknown preset '$PRESET'"
    print_usage
    exit 1
fi

cd "$PROJECT_DIR"

echo "=== Building fq-compressor ==="
echo "Preset: $PRESET"
echo "Jobs: $JOBS"
echo ""

# 检查 Conan 依赖是否已安装
# Conan 2.x 生成的 toolchain 文件位于嵌套路径 build/<preset>/build/<BuildType>/generators/
BUILD_DIR="build/$PRESET"
if ! find "$BUILD_DIR" -name "conan_toolchain.cmake" -print -quit 2>/dev/null | grep -q .; then
    echo "Error: Conan toolchain not found in $BUILD_DIR." >&2
    echo "Run 'conan install . --profile=conan/profiles/clang --build=missing' first." >&2
    exit 1
fi

# 配置
echo "Configuring..."
cmake --preset "$PRESET"

# 构建
echo "Building..."
cmake --build --preset "$PRESET" -j "$JOBS"

echo ""
echo "=== Build complete ==="
echo "Binary location: $BUILD_DIR"
