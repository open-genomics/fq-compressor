#!/usr/bin/env bash
# scripts/bench.sh - 本地性能基准封装（fq-compressor）
# =============================================================================
# 一键跑 test_performance.sh 并自动归档到 perf-baselines/，支持历史归档对比。
#
# 用法：
#   ./scripts/bench.sh run                 # 正式基准（默认 512 1024 MiB，5 次重复，自动归档）
#   ./scripts/bench.sh run --sizes "64 128"   # 自定义尺寸
#   ./scripts/bench.sh run --repeats 3        # 自定义重复次数
#   ./scripts/bench.sh run --quick            # 冒烟：1 MiB、1 次重复、不查 SLA
#   ./scripts/bench.sh list                # 列出全部历史归档
#   ./scripts/bench.sh compare <归档A> <归档B>  # 对比两个归档（中位数速度 + 差值）
#
# 环境变量透传给 test_performance.sh（FQC_PERF_* 均可覆盖），默认值：
#   sizes=512 1024  repeats=5  enforce_sla=0  memory=16384  archive_slug=local
# 正式基准需要 clang-release 二进制；缺失时自动构建。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PERF_SCRIPT="$PROJECT_ROOT/tests/e2e/test_performance.sh"
BASELINE_ROOT="$PROJECT_ROOT/perf-baselines"
RELEASE_BIN="$PROJECT_ROOT/build/clang-release/src/fqc"

GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

# 从归档目录的 JSONL 中提取一条基准的字段；不存在则返回空。
# 用法: jsonl_field <field> <archive> <bench_name>
jsonl_field() {
    local field="$1" archive="$2" name="$3"
    local file="$BASELINE_ROOT/$archive/benchmark_results/benchmarks.jsonl"
    # grep 无匹配时返回非零，pipefail 下需兜底避免整个脚本退出
    grep "\"name\":\"$name\"" "$file" 2>/dev/null | head -1 |
        sed -n "s/.*\"$field\":\([0-9.]*\).*/\1/p" || true
}

run_bench() {
    local sizes="512 1024" repeats=5 enforce_sla=0 slug="local" memory=16384

    while [[ $# -gt 0 ]]; do
        case $1 in
            --sizes) sizes="$2"; shift 2 ;;
            --repeats) repeats="$2"; shift 2 ;;
            --memory) memory="$2"; shift 2 ;;
            --sla) enforce_sla=1; shift ;;
            --slug) slug="$2"; shift 2 ;;
            --quick) sizes="1"; repeats=1; shift ;;
            -h|--help) usage ;;
            *) log_error "未知参数: $1"; usage ;;
        esac
    done

    if [[ ! -x "$PERF_SCRIPT" ]]; then
        log_error "找不到性能脚本: $PERF_SCRIPT"
        exit 1
    fi

    # 正式基准需要 release 二进制；冒烟模式可用任意已构建二进制。
    if [[ ! -x "$RELEASE_BIN" ]]; then
        if [[ "$sizes" == "1" && "$repeats" == "1" ]]; then
            log_warn "未找到 release 二进制，冒烟模式使用默认路径（可能失败）"
        else
            log_info "未找到 release 二进制，自动构建 clang-release..."
            "$SCRIPT_DIR/build.sh" clang-release
        fi
    fi

    log_info "基准参数: sizes=[$sizes] repeats=$repeats sla=$enforce_sla memory=${memory}MiB"
    FQC_PERF_SIZES="$sizes" \
    FQC_PERF_REPEATS="$repeats" \
    FQC_PERF_ENFORCE_SLA="$enforce_sla" \
    FQC_PERF_MEMORY_MIB="$memory" \
    FQC_PERF_ARCHIVE="$slug" \
        "$PERF_SCRIPT"

    # 归档 README：记录环境与参数，便于跨归档对比时溯源。
    local archive_dir
    archive_dir="$(ls -dt "$BASELINE_ROOT"/"$(date +%F)"-* 2>/dev/null | head -1)"
    if [[ -n "$archive_dir" && -f "$archive_dir/benchmark_results/benchmarks.jsonl" ]]; then
        local commit
        commit="$(grep '^# git:' "$archive_dir/benchmark_results/benchmarks.jsonl" | head -1 | awk '{print $3}')"
        cat > "$archive_dir/README.md" <<EOF
# 基准归档 $(basename "$archive_dir")

- git commit: ${commit:-unknown}
- 参数: sizes=[$sizes] repeats=$repeats sla=$enforce_sla memory=${memory}MiB
- 环境: $(uname -srm) $(nproc 2>/dev/null || echo "?") 核
- 二进制: ${RELEASE_BIN:-unknown}

原始数据: benchmark_results/benchmarks.jsonl
摘要与对比: docs/benchmarks.md
EOF
        log_info "归档说明已写入: $archive_dir/README.md"
    fi
}

list_archives() {
    if [[ ! -d "$BASELINE_ROOT" ]]; then
        log_info "还没有归档（perf-baselines/ 不存在）"
        return 0
    fi
    log_info "历史归档（perf-baselines/）："
    for archive in "$BASELINE_ROOT"/*/; do
        [[ -d "$archive" ]] || continue
        local name
        name="$(basename "$archive")"
        local git_commit jsonl
        jsonl="$archive/benchmark_results/benchmarks.jsonl"
        git_commit="$(grep '^# git:' "$jsonl" 2>/dev/null | head -1 | awk '{print $3}')"
        echo "  $name  (git $git_commit)"
    done
}

compare_archives() {
    [[ $# -eq 2 ]] || { log_error "compare 需要两个归档名"; usage; }

    local archive_a="$1" archive_b="$2"
    local file_a="$BASELINE_ROOT/$archive_a/benchmark_results/benchmarks.jsonl"
    local file_b="$BASELINE_ROOT/$archive_b/benchmark_results/benchmarks.jsonl"

    for f in "$file_a" "$file_b"; do
        [[ -f "$f" ]] || { log_error "归档不存在: ${f%/*}"; exit 1; }
    done

    local commit_a commit_b
    commit_a="$(grep '^# git:' "$file_a" | head -1 | awk '{print $3}')"
    commit_b="$(grep '^# git:' "$file_b" | head -1 | awk '{print $3}')"
    log_info "对比: $archive_a (git $commit_a)  vs  $archive_b (git $commit_b)"

    # 以归档 B 中的基准名为准（两个归档由同一脚本生成，基准名一致）。
    local names
    names="$(grep -o '"name":"[^"]*"' "$file_b" | sed 's/"name":"//;s/"//' | sort -u)"

    printf '  %-22s %12s %12s %8s | %12s %12s %8s\n' \
        "benchmark" "c_speed A" "c_speed B" "Δc%" "d_speed A" "d_speed B" "Δd%"
    printf '  %s\n' "----------------------------------------------------------------------"

    local name c_a c_b d_a d_b
    for name in $names; do
        c_a="$(jsonl_field compress_speed_mib_s "$archive_a" "$name")"
        c_b="$(jsonl_field compress_speed_mib_s "$archive_b" "$name")"
        d_a="$(jsonl_field decompress_speed_mib_s "$archive_a" "$name")"
        d_b="$(jsonl_field decompress_speed_mib_s "$archive_b" "$name")"

        # 冒烟/小尺寸下计时可能为 0 导致字段为空，显示 n/a 而非跳过整行；
        # 两个归档都完全没有该基准（如尺寸集不同）才跳过。
        if [[ -z "$c_a" && -z "$c_b" && -z "$d_a" && -z "$d_b" ]]; then
            continue
        fi

        local dc="n/a" dd="n/a"
        if [[ -n "$c_a" && -n "$c_b" ]]; then
            dc="$(echo "scale=1; ($c_b - $c_a) * 100 / $c_a" | bc 2>/dev/null || echo "n/a")"
        fi
        if [[ -n "$d_a" && -n "$d_b" ]]; then
            dd="$(echo "scale=1; ($d_b - $d_a) * 100 / $d_a" | bc 2>/dev/null || echo "n/a")"
        fi

        printf '  %-22s %12s %12s %7s%% | %12s %12s %7s%%\n' \
            "$name" "${c_a:-n/a}" "${c_b:-n/a}" "$dc" "${d_a:-n/a}" "${d_b:-n/a}" "$dd"
    done
}

case "${1:-}" in
    run) shift; run_bench "$@" ;;
    list) list_archives ;;
    compare) shift; compare_archives "$@" ;;
    -h|--help) usage ;;
    *) usage ;;
esac
