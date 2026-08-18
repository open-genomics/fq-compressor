#!/usr/bin/env bash
# 从 ENA 拉两份公开 FASTQ 的前缀切片，供 docs/real-corpus.md 复测。
# 切片不入库（.gitignore: corpus/）。head 截断 gzip 流时 curl 会报 23，属预期。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${FQC_CORPUS_DIR:-$ROOT/corpus}"
mkdir -p "$OUT"

fetch_head() {
    local url="$1"
    local dest="$2"
    local lines="$3"
    if [[ -s "$dest" ]]; then
        echo "exists: $dest"
        return 0
    fi
    echo "fetch: $url -> $dest ($lines lines)"
    curl -L --fail --retry 3 --retry-delay 2 "$url" | gzip -dc | head -n "$lines" >"$dest"
    local status=${PIPESTATUS[0]}
    if [[ "$status" -ne 0 && "$status" -ne 23 ]]; then
        echo "download failed: curl exit $status" >&2
        return 1
    fi
}

# Illumina WXS，R1 前 200k 条（800k 行）。约 54 MiB。
fetch_head \
    "https://ftp.sra.ebi.ac.uk/vol1/fastq/SRR296/003/SRR2962693/SRR2962693_1.fastq.gz" \
    "$OUT/SRR2962693_1.head200k.fastq" \
    800000

# 人类 MinION，前 4k 条（16k 行）。约 125 MiB。ENA 头无 runid=，靠长读 + DRR accession 识别。
fetch_head \
    "https://ftp.sra.ebi.ac.uk/vol1/fastq/DRR171/DRR171398/DRR171398_1.fastq.gz" \
    "$OUT/DRR171398_1.head4k.fastq" \
    16000

echo
wc -l -c "$OUT"/*.fastq
sha256sum "$OUT"/*.fastq
