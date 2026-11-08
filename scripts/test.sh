#!/usr/bin/env bash
# scripts/test.sh - 构建并运行测试
#
# Usage: ./scripts/test.sh [preset] [filter]
#        ./scripts/test.sh -p clang-asan -f 'ArchiveTest.*' -v
#
# [preset] 缺省 clang-debug；位置参数只有在该名称是有效 preset 时才按 preset
#          处理，否则按 [filter] 处理（例如只传过滤器）。
# [filter] 是 GoogleTest 用例过滤器（GTEST_FILTER 语法），不是 ctest -R 的
#          测试名正则；按测试二进制筛选请直接用 ctest -R。
#
# Options:
#   -p, --preset <name>    CMake preset (default: clang-debug)
#   -f, --filter <pattern> GoogleTest case filter (GTEST_FILTER syntax)
#   -l, --label <label>    Run only ctest tests carrying the label
#   -v, --verbose          Verbose test output
#   -b, --build-only       Build test targets without running
#   -h, --help             Show this help

set -euo pipefail

PRESET="clang-debug"
PRESET_SET=false
FILTER=""
LABEL=""
VERBOSE=""
BUILD_ONLY=false

# 与 scripts/build.sh 的 VALID_PRESETS 保持一致（CMakePresets.json 的 configure presets）
VALID_PRESETS="clang-debug clang-release clang-release-portable clang-fuzz clang-asan clang-tsan clang-coverage"

while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--preset) PRESET="$2"; PRESET_SET=true; shift 2 ;;
        -f|--filter) FILTER="$2"; shift 2 ;;
        -l|--label) LABEL="$2"; shift 2 ;;
        -v|--verbose) VERBOSE="--verbose"; shift ;;
        -b|--build-only) BUILD_ONLY=true; shift ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *)
            # 位置参数只有匹配已知 preset 时才占用 preset 槽，避免把 filter 当
            # preset（旧逻辑在 preset 为默认值 clang-debug 时会把过滤 pattern
            # 覆盖到 PRESET 上）。
            if [[ "$PRESET_SET" == false && " $VALID_PRESETS " == *" $1 "* ]]; then
                PRESET="$1"
                PRESET_SET=true
            elif [[ -z "$FILTER" && "$1" != clang-* && "$1" != gcc-* ]]; then
                FILTER="$1"
            else
                # 近似 preset 的名字（clang-*/gcc-*）拼错时直接报错，避免误当
                # GTest 过滤器匹配不到用例而静默全绿。
                echo "Error: Unrecognized argument: $1" >&2
                exit 1
            fi
            shift ;;
    esac
done

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build/$PRESET"

cd "$PROJECT_DIR"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cmake --preset "$PRESET"
fi

echo "=== Building ($PRESET) ==="
cmake --build --preset "$PRESET" 2>&1 | tail -5

if [[ "$BUILD_ONLY" == true ]]; then
    echo "Build-only mode: done."
    exit 0
fi

CTEST_ARGS=(--test-dir "$BUILD_DIR" --output-on-failure)
[[ -n "$VERBOSE" ]] && CTEST_ARGS+=("$VERBOSE")
[[ -n "$LABEL" ]] && CTEST_ARGS+=(-L "$LABEL")

echo ""
echo "=== Running tests ($PRESET) ==="
if [[ -n "$FILTER" ]]; then
    # tests/CMakeLists.txt 用 add_test 为每个测试二进制注册一个 ctest 测试，
    # ctest -R 只能匹配二进制名而匹配不到具体 GTest 用例；过滤器改由 GTest
    # 自己（GTEST_FILTER 环境变量）执行，ctest 子进程会继承。
    echo "GTest filter: $FILTER"

    # GTest 对零匹配过滤器返回成功（某个二进制一条用例都没跑也是 0 退出），
    # 拼错 pattern 会静默全绿。先用 --gtest_list_tests 确认至少命中一条。
    matched=false
    while IFS= read -r test_bin; do
        list_output="$("$test_bin" --gtest_list_tests --gtest_filter="$FILTER" 2>/dev/null || true)"
        if grep -qE '^  [^ ]' <<<"$list_output"; then
            matched=true
            break
        fi
    done < <(find "$BUILD_DIR" -maxdepth 4 -type f -executable -name '*_test' | sort)
    if [[ "$matched" == false ]]; then
        echo "Error: GTest filter '$FILTER' 没有匹配任何用例（检查 pattern 拼写）" >&2
        exit 1
    fi

    export GTEST_FILTER="$FILTER"
fi
ctest "${CTEST_ARGS[@]}"
