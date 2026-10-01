#!/usr/bin/env bash
# scripts/lint.sh - 代码质量检查脚本

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

resolve_cmake_build_dir() {
    local build_dir="$PROJECT_DIR/build/$2"
    if [ -f "$build_dir/$3" ]; then echo "$build_dir"
    elif [ -f "$build_dir/build/Debug/$3" ]; then echo "$build_dir/build/Debug"
    elif [ -f "$build_dir/build/Release/$3" ]; then echo "$build_dir/build/Release"
    else echo "$build_dir"; fi
}

ACTION="${1:-lint}"
PRESET="${2:-clang-debug}"

# 查找所有源文件（含测试，用于格式检查）
find_all_sources() {
    find "$PROJECT_DIR/src" "$PROJECT_DIR/include" "$PROJECT_DIR/tests" \
        \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
        -not -path "*/build/*" 2>/dev/null || true
}

# 查找生产 translation units（项目头文件通过 header filter 检查）
find_lint_sources() {
    find "$PROJECT_DIR/src" -name "*.cpp" -not -path "*/build/*" 2>/dev/null || true
}

# 检测 clang-format 版本
# 固定使用 clang-format-18 以与 CI（.github/workflows/ci.yml 装的是 clang-format-18）
# 保持一致；18 与 21 在部分返回类型换行策略上输出不同，混用会导致 CI 反复失败。
detect_clang_format() {
    if command -v clang-format-18 &> /dev/null; then
        echo "clang-format-18"
    elif command -v clang-format &> /dev/null; then
        echo "clang-format"
    else
        echo "Error: clang-format not found" >&2
        exit 1
    fi
}

# 检测 clang-tidy 版本
# 固定使用 clang-tidy-18 以与 CI（ci.yml 的 clang-tidy job 装 clang-tidy-18）
# 保持一致：不同版本的检查集与告警存在差异，"本地干净 ≠ CI 干净"会让门禁
# 失去可预期性（clang-format 已同理由 18 锚定）。
detect_clang_tidy() {
    if command -v clang-tidy-18 &> /dev/null; then
        echo "clang-tidy-18"
    elif command -v clang-tidy &> /dev/null; then
        echo "clang-tidy"
    else
        echo "Error: clang-tidy not found" >&2
        exit 1
    fi
}

CLANG_FORMAT=""
CLANG_TIDY=""

case $ACTION in
    format)
        CLANG_FORMAT=$(detect_clang_format)
        echo "Formatting code with $CLANG_FORMAT..."
        mapfile -t sources < <(find_all_sources)
        if [ ${#sources[@]} -gt 0 ]; then
            "$CLANG_FORMAT" -i "${sources[@]}"
            echo "Done. Formatted ${#sources[@]} files."
        else
            echo "No source files found."
        fi
        ;;
    format-check)
        CLANG_FORMAT=$(detect_clang_format)
        echo "Checking format with $CLANG_FORMAT..."
        mapfile -t sources < <(find_all_sources)
        if [ ${#sources[@]} -gt 0 ]; then
            "$CLANG_FORMAT" --dry-run --Werror "${sources[@]}"
            echo "Format check passed."
        else
            echo "No source files found."
        fi
        ;;
    lint)
        CLANG_TIDY=$(detect_clang_tidy)
        echo "Running $CLANG_TIDY..."
        BUILD_DIR=$(resolve_cmake_build_dir "$PROJECT_DIR" "$PRESET" compile_commands.json)
        if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
            echo "Error: compile_commands.json not found in $BUILD_DIR"
            echo "Please run './scripts/build.sh $PRESET' first."
            exit 1
        fi
        mapfile -t sources < <(find_lint_sources)
        if [ ${#sources[@]} -gt 0 ]; then
            "$CLANG_TIDY" \
                -p "$BUILD_DIR" \
                --quiet \
                --header-filter="^${PROJECT_DIR}/(include|src)/" \
                --exclude-header-filter='(^|/)(build|\.conan2|vcpkg_installed)/' \
                --system-headers=false \
                "${sources[@]}" \
                2>&1 | sed -E \
                    -e '/^[0-9]+ warnings generated\.$/d' \
                    -e '/^Suppressed [0-9]+ warnings/d'
            echo "Lint check passed."
        else
            echo "No source files found."
        fi
        ;;
    all)
        "$0" format-check "$PRESET"
        "$0" lint "$PRESET"
        ;;
    *)
        echo "Usage: $0 {format|format-check|lint|all} [preset]"
        echo ""
        echo "Commands:"
        echo "  format       - Format all source files in place"
        echo "  format-check - Check formatting without modifying files"
        echo "  lint         - Run clang-tidy static analysis"
        echo "  all          - Run format-check and lint"
        echo ""
        echo "Preset:"
        echo "  Optional for lint/all, defaults to clang-debug"
        exit 1
        ;;
esac
