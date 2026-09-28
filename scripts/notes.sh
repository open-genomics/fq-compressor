#!/usr/bin/env bash
# scripts/notes.sh - 决策笔记（.agents/notes/）校验与维护入口
#
# 笔记体系由 write-notes-like-deepseek skill 提供（.agents/skills/），
# 脚本为独立 tsx，经 npx 运行、零新增依赖。判定红线与分工见 AGENTS.md。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SKILL_SCRIPTS="$PROJECT_DIR/.agents/skills/write-notes-like-deepseek/scripts"

print_usage() {
    echo "Usage: $0 <command> [args]"
    echo ""
    echo "Commands:"
    echo "  verify                    - 全部校验（tree + format + archived），CI 门禁用"
    echo "  tree                      - 目录结构/分类/文件名/笔记间相对链接"
    echo "  format                    - 头块、Status、必备节、备选方案"
    echo "  archived                  - 归档封印（manifest 哈希、只增不改）"
    echo "  archive <path> [opts]     - 归档一篇笔记（可跟 --superseded-by <新笔记>）"
    echo "  anchors                   - 代码中 // Note: 锚点双向体检（软报告）"
    echo "  board [out.html]          - 生成决策看板（默认 ./board.html）"
}

if [[ ! -d "$SKILL_SCRIPTS" ]]; then
    echo "error: skill scripts not found at $SKILL_SCRIPTS" >&2
    echo "       reinstall with: npx skills add czm15053/write-notes-like-deepseek" >&2
    exit 1
fi

cd "$PROJECT_DIR"

run_tsx() {
    npx --yes tsx "$@"
}

CMD="${1:-verify}"
case "$CMD" in
    verify)
        run_tsx "$SKILL_SCRIPTS/verify-agent-note-tree.ts"
        run_tsx "$SKILL_SCRIPTS/verify-agent-note-format.ts"
        run_tsx "$SKILL_SCRIPTS/verify-archived-agent-notes.ts"
        ;;
    tree)    run_tsx "$SKILL_SCRIPTS/verify-agent-note-tree.ts" ;;
    format)  run_tsx "$SKILL_SCRIPTS/verify-agent-note-format.ts" ;;
    archived) run_tsx "$SKILL_SCRIPTS/verify-archived-agent-notes.ts" ;;
    archive)
        shift
        run_tsx "$SKILL_SCRIPTS/archive-agent-note.ts" "$@"
        ;;
    anchors) run_tsx "$SKILL_SCRIPTS/check-note-anchors.ts" ;;
    board)
        OUT="${2:-$PROJECT_DIR/board.html}"
        run_tsx "$SKILL_SCRIPTS/build-board.ts" --bundle "$PROJECT_DIR/.agents/notes" "$OUT" "fq-compressor 决策看板"
        echo "board: $OUT"
        ;;
    -h|--help|help)
        print_usage
        ;;
    *)
        echo "error: unknown command '$CMD'" >&2
        print_usage
        exit 1
        ;;
esac
