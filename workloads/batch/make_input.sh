#!/usr/bin/env bash
# Create the batch job's input: the first <MB> MB of a tar of the Linux
# source tree (realistic, compressible text). `git archive` output depends
# only on the commit, so the input is reproducible. It is kept outside the
# repository and is small enough to stay in the page cache, so the batch job
# is CPU-bound rather than disk-bound.
#
# Usage: ./workloads/batch/make_input.sh [MB] [output]
set -euo pipefail

MB=${1:-64}
OUT=${2:-$HOME/kairos-data/zstd-input-${MB}M.tar}
SRC=${KSRC:-$HOME/linux-src}

mkdir -p "$(dirname "$OUT")"
# head stops reading after MB megabytes, which kills git archive with
# SIGPIPE; that is expected, so don't let pipefail treat it as an error.
( set +o pipefail; git -C "$SRC" archive --format=tar HEAD | head -c $((MB * 1024 * 1024)) > "$OUT" )
echo "$OUT: $(stat -c %s "$OUT") bytes from $SRC at $(git -C "$SRC" rev-parse --short HEAD)"
