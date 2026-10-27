#!/usr/bin/env bash
# Write meta.json describing an experiment run, so every result can be traced
# back to the exact code, kernel and settings that produced it.
# Usage: ./scripts/meta.sh <results-dir> "<scheduler options>" "<workload description>"
set -euo pipefail

OUT_DIR="${1:?results directory required}"
SCHED_OPTS="${2:-}"
WORKLOAD="${3:-}"
mkdir -p "$OUT_DIR"

json_escape() { printf '%s' "$1" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'; }

cat > "$OUT_DIR/meta.json" <<EOF
{
  "timestamp": "$(date -Iseconds)",
  "git_commit": "$(git rev-parse HEAD 2>/dev/null || echo unknown)",
  "git_dirty": $( [ -n "$(git status --porcelain 2>/dev/null)" ] && echo true || echo false ),
  "kernel": "$(uname -r)",
  "nr_cpus": $(nproc),
  "mem_total_kb": $(awk '/MemTotal/ {print $2}' /proc/meminfo),
  "sched_ext_state": "$(cat /sys/kernel/sched_ext/state 2>/dev/null || echo unavailable)",
  "scheduler_options": $(json_escape "$SCHED_OPTS"),
  "workload": $(json_escape "$WORKLOAD")
}
EOF

echo "Wrote $OUT_DIR/meta.json"
