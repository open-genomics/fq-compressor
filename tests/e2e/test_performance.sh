#!/usr/bin/env bash
# =============================================================================
# fq-compressor - Performance Tests
# =============================================================================
# Benchmarks compression/decompression performance with various file sizes.
#
# Requirements tested: 4.1
#
# Stage-E hardening (docs/roadmap.md stage E):
# - One warmup pass before timed repeats (FQC_PERF_WARMUP, default 1).
# - Spread report: median plus [min..max] -- WSL2 swings +-20-85%, a lone
#   median hides the noise floor (2026-07-26 stage-D postmortem).
# - FQC_PERF_REPEATS default raised 3 -> 5.
# - A/B mode: FQC_PERF_BIN_B=<path> benchmarks a second binary interleaved
#   inside the repeat loop, so both binaries share the same machine-state
#   window -- the only trustworthy comparison (cross-time baselines get
#   polluted by machine drift). SLA enforcement applies to binary A only.
# - FQC_PERF_ARCHIVE=<slug> copies the results to
#   perf-baselines/YYYY-MM-DD-<slug>/ (gitignored, local archaeology).
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEST_DIR="${FQC_PERF_TEST_DIR:-$(mktemp -d)}"
FQC_BIN="${FQC_BIN:-$PROJECT_ROOT/build/clang-release/src/fqc}"
FQC_BIN_B="${FQC_PERF_BIN_B:-}"
RESULTS_DIR="${RESULTS_DIR:-$TEST_DIR/benchmark_results}"
FQC_PERF_SIZES="${FQC_PERF_SIZES:-1 2}"
FQC_PERF_MEMORY_MIB="${FQC_PERF_MEMORY_MIB:-16384}"
FQC_PERF_DATA="${FQC_PERF_DATA:-structured}"
FQC_PERF_KEEP_TEMP="${FQC_PERF_KEEP_TEMP:-0}"
FQC_PERF_ENFORCE_SLA="${FQC_PERF_ENFORCE_SLA:-0}"
FQC_PERF_REPEATS="${FQC_PERF_REPEATS:-5}"
FQC_PERF_WARMUP="${FQC_PERF_WARMUP:-1}"
FQC_PERF_ARCHIVE="${FQC_PERF_ARCHIVE:-}"
FQC_PERF_MIN_COMPRESS_MIB_S="${FQC_PERF_MIN_COMPRESS_MIB_S:-50}"
FQC_PERF_MIN_DECOMPRESS_MIB_S="${FQC_PERF_MIN_DECOMPRESS_MIB_S:-100}"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

cleanup() {
    if [[ "$FQC_PERF_KEEP_TEMP" != "1" ]]; then
        rm -rf "$TEST_DIR"
    fi
}
trap cleanup EXIT

log_info() {
    echo -e "${GREEN}[INFO]${NC} $*"
}

log_bench() {
    echo -e "${YELLOW}[BENCH]${NC} $*"
}

median_value() {
    printf '%s\n' "$@" | sort -n | awk '
        { values[NR] = $1 }
        END {
            middle = int((NR + 1) / 2)
            if (NR % 2 == 1) {
                print values[middle]
            } else {
                print (values[middle] + values[middle + 1]) / 2
            }
        }
    '
}

# awk-only implementations: piping `sort -n` into `head`/`tail` would SIGPIPE
# under `set -o pipefail`.
min_value() {
    printf '%s\n' "$@" | awk 'NR == 1 || $1 < min { min = $1 } END { print min }'
}

max_value() {
    printf '%s\n' "$@" | awk 'NR == 1 || $1 > max { max = $1 } END { print max }'
}

# Generate test FASTQ with specific size
generate_fastq() {
    local file="$1"
    local target_mb="$2"
    local read_len="${3:-150}"

    local target_bytes=$((target_mb * 1024 * 1024))
    local bytes_per_read=$((read_len * 2 + 20))  # seq + qual + headers
    local num_reads=$((target_bytes / bytes_per_read))

    log_info "Generating ${target_mb}MB FASTQ with $num_reads reads..."
    python3 - "$file" "$num_reads" "$read_len" <<'PY'
from pathlib import Path
import os
import random
import sys

path = Path(sys.argv[1])
num_reads = int(sys.argv[2])
read_len = int(sys.argv[3])
patterns = ("ACGT", "TGCA", "GATC", "CTAG", "AGTC", "TCAG")
mode = os.environ.get("FQC_PERF_DATA", "structured")
rng = random.Random(0xF0C2026)
structured_quality = "I" * read_len

with path.open("w", encoding="ascii") as fh:
    for i in range(1, num_reads + 1):
        if mode == "random":
            seq = "".join(rng.choices("ACGT", k=read_len))
            qual = "".join(rng.choices("?@ABCDEFGHI", k=read_len))
        else:
            pattern = patterns[i % len(patterns)]
            seq = (pattern * ((read_len + len(pattern) - 1) // len(pattern)))[:read_len]
            qual = structured_quality
        fh.write(f"@read_{i} length={read_len}\n{seq}\n+\n{qual}\n")
PY
}

# Monotonic wall clock in seconds (centisecond precision). `date +%s.%N` is
# CLOCK_REALTIME: on WSL2 the host resync can jump it backwards mid-run,
# producing impossible *negative* samples that a median silently absorbs but
# that corrupt the min/max spread report. /proc/uptime (CLOCK_BOOTTIME) never
# moves backwards.
now_seconds() {
    local up _
    read -r up _ < /proc/uptime
    echo "$up"
}

# Echo wall seconds of one compress run; /usr/bin/time writes max RSS (KiB)
# to the rss file. Untimed callers may discard stdout.
time_compress() {
    local bin="$1" input="$2" output="$3" profile="$4" rss_file="$5"
    local start_time end_time
    start_time=$(now_seconds)
    /usr/bin/time -f '%M' -o "$rss_file" \
        "$bin" -q --memory-limit "$FQC_PERF_MEMORY_MIB" compress \
        -i "$input" -o "$output" --profile "$profile" -f 2>/dev/null
    end_time=$(now_seconds)
    echo "$end_time - $start_time" | bc
}

time_decompress() {
    local bin="$1" input="$2" output="$3" rss_file="$4"
    local start_time end_time
    start_time=$(now_seconds)
    /usr/bin/time -f '%M' -o "$rss_file" \
        "$bin" -q --memory-limit "$FQC_PERF_MEMORY_MIB" decompress \
        -i "$input" -o "$output" -f 2>/dev/null
    end_time=$(now_seconds)
    echo "$end_time - $start_time" | bc
}

# Warmup: one untimed compress + decompress with binary A, so page cache and
# binary code pages are warm before the first timed iteration.
warmup() {
    local input="$1" profile="$2"
    local warm_fqc="$TEST_DIR/.warmup.fqc"
    local warm_out="$TEST_DIR/.warmup.fastq"
    local warm_rss="$TEST_DIR/.warmup_rss.txt"
    time_compress "$FQC_BIN" "$input" "$warm_fqc" "$profile" "$warm_rss" >/dev/null
    time_decompress "$FQC_BIN" "$warm_fqc" "$warm_out" "$warm_rss" >/dev/null
    rm -f "$warm_fqc" "$warm_out" "$warm_rss"
}

# Aggregate pre-collected timing samples of one binary: median + spread,
# round-trip cmp, print, jsonl. Exports LAST_COMPRESS_SPEED /
# LAST_DECOMPRESS_SPEED for the caller's delta/SLA logic.
# Args: label input_mb input_size compressed decompressed original_input
#       rss_c_kib rss_d_kib name profile times_c... -- times_d...
report_binary() {
    local label="$1" input_mb="$2" input_size="$3" compressed="$4" decompressed="$5"
    local original="$6" rss_c_kib="$7" rss_d_kib="$8" name="$9" profile="${10}"
    shift 10
    local -a compress_times=() decompress_times=()
    local section=0
    for arg in "$@"; do
        if [[ "$arg" == "--" ]]; then
            section=$((section + 1))
            continue
        fi
        if ((section == 0)); then
            compress_times+=("$arg")
        else
            decompress_times+=("$arg")
        fi
    done

    local compress_time=$(median_value "${compress_times[@]}")
    local compress_time_min=$(min_value "${compress_times[@]}")
    local compress_time_max=$(max_value "${compress_times[@]}")
    local decompress_time=$(median_value "${decompress_times[@]}")
    local decompress_time_min=$(min_value "${decompress_times[@]}")
    local decompress_time_max=$(max_value "${decompress_times[@]}")

    local compressed_size=$(stat -c%s "$compressed" 2>/dev/null || stat -f%z "$compressed")
    local ratio=$(echo "scale=4; $input_size / $compressed_size" | bc)
    # Fastest run = shortest time, and vice versa.
    local compress_speed=$(echo "scale=2; $input_mb / $compress_time" | bc)
    local compress_speed_max=$(echo "scale=2; $input_mb / $compress_time_min" | bc)
    local compress_speed_min=$(echo "scale=2; $input_mb / $compress_time_max" | bc)
    local decompress_speed=$(echo "scale=2; $input_mb / $decompress_time" | bc)
    local decompress_speed_max=$(echo "scale=2; $input_mb / $decompress_time_min" | bc)
    local decompress_speed_min=$(echo "scale=2; $input_mb / $decompress_time_max" | bc)

    if ! cmp -s "$original" "$decompressed"; then
        echo "Error: round-trip mismatch for benchmark '$name' (binary $label)" >&2
        return 1
    fi

    printf "  %-2s %-18s %8.2f MiB  ratio: %.4f  compress: %6.2f [%6.2f..%6.2f] MiB/s (%s KiB)  decompress: %6.2f [%6.2f..%6.2f] MiB/s (%s KiB)\n" \
           "$label" "$name" "$input_mb" "$ratio" \
           "$compress_speed" "$compress_speed_min" "$compress_speed_max" "$rss_c_kib" \
           "$decompress_speed" "$decompress_speed_min" "$decompress_speed_max" "$rss_d_kib"

    echo "{\"name\":\"$name\",\"profile\":\"$profile\",\"data\":\"$FQC_PERF_DATA\",\"binary\":\"$label\",\"input_mib\":$input_mb,\"ratio\":$ratio,\"compress_speed_mib_s\":$compress_speed,\"compress_speed_min\":$compress_speed_min,\"compress_speed_max\":$compress_speed_max,\"decompress_speed_mib_s\":$decompress_speed,\"decompress_speed_min\":$decompress_speed_min,\"decompress_speed_max\":$decompress_speed_max,\"compress_max_rss_kib\":$rss_c_kib,\"decompress_max_rss_kib\":$rss_d_kib}" >> "$RESULTS_DIR/benchmarks.jsonl"

    LAST_COMPRESS_SPEED="$compress_speed"
    LAST_DECOMPRESS_SPEED="$decompress_speed"
}

# Time one binary's compress+decompress loops, accumulating wall samples into
# the named arrays and tracking max RSS into the named scalars.
# Args: bin input compressed decompressed profile rss_c_file rss_d_file
#       times_c_name times_d_name rss_c_kib_name rss_d_kib_name
measure_binary() {
    local bin="$1" input="$2" compressed="$3" decompressed="$4" profile="$5"
    local rss_c_file="$6" rss_d_file="$7"
    local -n times_c_ref="$8" times_d_ref="$9" rss_c_ref="${10}" rss_d_ref="${11}"
    local current_rss
    for ((iteration = 1; iteration <= FQC_PERF_REPEATS; ++iteration)); do
        times_c_ref+=("$(time_compress "$bin" "$input" "$compressed" "$profile" "$rss_c_file")")
        current_rss=$(<"$rss_c_file")
        if ((current_rss > rss_c_ref)); then
            rss_c_ref=$current_rss
        fi
        times_d_ref+=("$(time_decompress "$bin" "$compressed" "$decompressed" "$rss_d_file")")
        current_rss=$(<"$rss_d_file")
        if ((current_rss > rss_d_ref)); then
            rss_d_ref=$current_rss
        fi
    done
}

run_benchmark() {
    local name="$1"
    local input="$2"
    local profile="$3"

    local input_size=$(stat -c%s "$input" 2>/dev/null || stat -f%z "$input")
    local input_mb=$(echo "scale=2; $input_size / 1048576" | bc)

    log_bench "Benchmark: $name (${input_mb} MiB, profile=${profile}, repeats=${FQC_PERF_REPEATS})"

    if [[ "$FQC_PERF_WARMUP" == "1" ]]; then
        warmup "$input" "$profile"
    fi

    local compressed_a="$TEST_DIR/${name}A.fqc"
    local decompressed_a="$TEST_DIR/${name}A_out.fastq"
    local rss_ca="$TEST_DIR/${name}A_compress_rss.txt"
    local rss_da="$TEST_DIR/${name}A_decompress_rss.txt"
    local -a times_ca=() times_da=()
    local rss_ca_kib=0 rss_da_kib=0

    if [[ -z "$FQC_BIN_B" ]]; then
        measure_binary "$FQC_BIN" "$input" "$compressed_a" "$decompressed_a" "$profile" \
            "$rss_ca" "$rss_da" times_ca times_da rss_ca_kib rss_da_kib
        report_binary "A" "$input_mb" "$input_size" "$compressed_a" "$decompressed_a" "$input" \
            "$rss_ca_kib" "$rss_da_kib" "$name" "$profile" "${times_ca[@]}" -- "${times_da[@]}"

        if [[ "$FQC_PERF_ENFORCE_SLA" == "1" ]]; then
            if (( $(echo "$LAST_COMPRESS_SPEED < $FQC_PERF_MIN_COMPRESS_MIB_S" | bc -l) )); then
                echo "Error: compression SLA failed for '$name': $LAST_COMPRESS_SPEED < $FQC_PERF_MIN_COMPRESS_MIB_S MiB/s" >&2
                return 1
            fi
            if (( $(echo "$LAST_DECOMPRESS_SPEED < $FQC_PERF_MIN_DECOMPRESS_MIB_S" | bc -l) )); then
                echo "Error: decompression SLA failed for '$name': $LAST_DECOMPRESS_SPEED < $FQC_PERF_MIN_DECOMPRESS_MIB_S MiB/s" >&2
                return 1
            fi
        fi
        if [[ "$FQC_PERF_KEEP_TEMP" != "1" ]]; then
            rm -f "$compressed_a" "$decompressed_a"
        fi
        return
    fi

    # A/B mode: interleave the two binaries per iteration so both share the
    # same machine-state window -- cross-time comparisons get polluted by
    # machine drift (2026-07-26 stage-D postmortem, lesson three).
    local compressed_b="$TEST_DIR/${name}B.fqc"
    local decompressed_b="$TEST_DIR/${name}B_out.fastq"
    local rss_cb="$TEST_DIR/${name}B_compress_rss.txt"
    local rss_db="$TEST_DIR/${name}B_decompress_rss.txt"
    local -a times_cb=() times_db=()
    local rss_cb_kib=0 rss_db_kib=0

    local current_rss
    for ((iteration = 1; iteration <= FQC_PERF_REPEATS; ++iteration)); do
        times_ca+=("$(time_compress "$FQC_BIN" "$input" "$compressed_a" "$profile" "$rss_ca")")
        current_rss=$(<"$rss_ca")
        if ((current_rss > rss_ca_kib)); then rss_ca_kib=$current_rss; fi
        times_cb+=("$(time_compress "$FQC_BIN_B" "$input" "$compressed_b" "$profile" "$rss_cb")")
        current_rss=$(<"$rss_cb")
        if ((current_rss > rss_cb_kib)); then rss_cb_kib=$current_rss; fi
        times_da+=("$(time_decompress "$FQC_BIN" "$compressed_a" "$decompressed_a" "$rss_da")")
        current_rss=$(<"$rss_da")
        if ((current_rss > rss_da_kib)); then rss_da_kib=$current_rss; fi
        times_db+=("$(time_decompress "$FQC_BIN_B" "$compressed_b" "$decompressed_b" "$rss_db")")
        current_rss=$(<"$rss_db")
        if ((current_rss > rss_db_kib)); then rss_db_kib=$current_rss; fi
    done

    report_binary "A" "$input_mb" "$input_size" "$compressed_a" "$decompressed_a" "$input" \
        "$rss_ca_kib" "$rss_da_kib" "$name" "$profile" "${times_ca[@]}" -- "${times_da[@]}"
    local a_c="$LAST_COMPRESS_SPEED" a_d="$LAST_DECOMPRESS_SPEED"
    report_binary "B" "$input_mb" "$input_size" "$compressed_b" "$decompressed_b" "$input" \
        "$rss_cb_kib" "$rss_db_kib" "$name" "$profile" "${times_cb[@]}" -- "${times_db[@]}"

    local delta_c delta_d
    delta_c=$(echo "scale=1; ($LAST_COMPRESS_SPEED - $a_c) * 100 / $a_c" | bc)
    delta_d=$(echo "scale=1; ($LAST_DECOMPRESS_SPEED - $a_d) * 100 / $a_d" | bc)
    printf "  delta B vs A: compress %+s%%  decompress %+s%%\n" "$delta_c" "$delta_d"

    # SLA gates the binary under test (A); B is a reference and may be an
    # older/slower build.
    if [[ "$FQC_PERF_ENFORCE_SLA" == "1" ]]; then
        if (( $(echo "$a_c < $FQC_PERF_MIN_COMPRESS_MIB_S" | bc -l) )); then
            echo "Error: compression SLA failed for '$name': $a_c < $FQC_PERF_MIN_COMPRESS_MIB_S MiB/s" >&2
            return 1
        fi
        if (( $(echo "$a_d < $FQC_PERF_MIN_DECOMPRESS_MIB_S" | bc -l) )); then
            echo "Error: decompression SLA failed for '$name': $a_d < $FQC_PERF_MIN_DECOMPRESS_MIB_S MiB/s" >&2
            return 1
        fi
    fi

    if [[ "$FQC_PERF_KEEP_TEMP" != "1" ]]; then
        rm -f "$compressed_a" "$decompressed_a" "$compressed_b" "$decompressed_b"
    fi
}

main() {
    mkdir -p "$TEST_DIR"
    if ! [[ "$FQC_PERF_REPEATS" =~ ^[1-9][0-9]*$ ]]; then
        echo "Error: FQC_PERF_REPEATS must be a positive integer" >&2
        exit 1
    fi
    echo "========================================"
    echo "fq-compressor Performance Tests"
    echo "========================================"
    echo ""

    # Check binaries
    if [[ ! -x "$FQC_BIN" ]]; then
        echo "Error: fqc binary not found at $FQC_BIN"
        exit 1
    fi
    if [[ -n "$FQC_BIN_B" && ! -x "$FQC_BIN_B" ]]; then
        echo "Error: A/B binary not found at $FQC_BIN_B"
        exit 1
    fi

    mkdir -p "$RESULTS_DIR"
    echo "# Benchmark results $(date -Iseconds)" > "$RESULTS_DIR/benchmarks.jsonl"
    {
        echo "# git: $(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "# config: sizes=[$FQC_PERF_SIZES] data=$FQC_PERF_DATA repeats=$FQC_PERF_REPEATS warmup=$FQC_PERF_WARMUP memory_mib=$FQC_PERF_MEMORY_MIB bin_a=$FQC_BIN bin_b=${FQC_BIN_B:-none}"
    } >> "$RESULTS_DIR/benchmarks.jsonl"

    # Test different file sizes
    echo ""
    echo "=== File Size Scaling ==="
    for size_mb in $FQC_PERF_SIZES; do
        local input="$TEST_DIR/test_${size_mb}mb.fastq"
        generate_fastq "$input" "$size_mb"
        run_benchmark "illumina_${size_mb}mib" "$input" illumina
        if [[ "$FQC_PERF_KEEP_TEMP" != "1" ]]; then
            rm -f "$input"
        fi
    done

    # Exercise the distinct long-read framing/profile path at the largest size.
    echo ""
    echo "=== Long-read Profile (${FQC_PERF_SIZES##* } MiB) ==="
    local profile_size="${FQC_PERF_SIZES##* }"
    local long_input="$TEST_DIR/ont_${profile_size}mib.fastq"
    generate_fastq "$long_input" "$profile_size" 20000
    run_benchmark "ont_${profile_size}mib" "$long_input" ont
    if [[ "$FQC_PERF_KEEP_TEMP" != "1" ]]; then
        rm -f "$long_input"
    fi

    echo ""
    echo "Results saved to: $RESULTS_DIR/benchmarks.jsonl"

    if [[ -n "$FQC_PERF_ARCHIVE" ]]; then
        local archive_dir="$PROJECT_ROOT/perf-baselines/$(date +%F)-${FQC_PERF_ARCHIVE}"
        mkdir -p "$archive_dir"
        cp -r "$RESULTS_DIR" "$archive_dir/"
        log_info "Baseline archived to: $archive_dir"
    fi

    echo ""
    log_info "Performance tests complete"
}

main "$@"
