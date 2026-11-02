#!/usr/bin/env bash
# E7, scheduler overhead, all parts in a row: batch throughput (e7-batch),
# light-load latency (e7-light), context-switch cost (e7_pipe.sh), and the
# BPF callbacks' own run time (e7-bpfstats, with kernel.bpf_stats_enabled=1,
# reset to 0 on exit however the script ends).
# Usage: experiments/e7.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# A part that fails doesn't stop the others.
./experiments/run.sh experiments/configs/e7-batch.conf || echo "e7-batch failed"
./experiments/run.sh experiments/configs/e7-light.conf || echo "e7-light failed"
./experiments/e7_pipe.sh 5 || echo "e7-pipe failed"
trap 'sudo sysctl -q kernel.bpf_stats_enabled=0' EXIT
sudo sysctl -q kernel.bpf_stats_enabled=1
./experiments/run.sh experiments/configs/e7-bpfstats.conf || echo "e7-bpfstats failed"
