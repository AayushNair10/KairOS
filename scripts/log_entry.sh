#!/usr/bin/env bash
# Append a dated session-entry skeleton to the research log.
# Usage: ./scripts/log_entry.sh "Short title"
set -euo pipefail

LOG="$(git rev-parse --show-toplevel 2>/dev/null || pwd)/docs/RESEARCH_LOG.md"
TITLE="${1:-Session}"
DATE="$(date +%Y-%m-%d)"
COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo 'no-commit')"

cat >> "$LOG" <<EOF

### ${DATE} — ${TITLE}
**Commit:** ${COMMIT}
**Did:**
**Found:**
**Interpretation:**
**Decisions:**
**Problems:**
**Next:**
EOF

echo "Added entry '${TITLE}' to ${LOG}"
