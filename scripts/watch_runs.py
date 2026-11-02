#!/usr/bin/env python3
"""Watch an experiment running in tmux and print only the runs that look
disturbed: VM time share below 0.95, other processes above 30% of a CPU,
or more than 1,000 pages swapped in (analysis/summarize_run.py's checks).
Exits when the tmux session ends. Prints nothing for clean runs, so a
watcher started with run_in_background stays quiet during the run.

Usage: scripts/watch_runs.py TMUX_SESSION results/<date>-<name>
"""
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "analysis"))
import runlib  # noqa: E402

session, out_dir = sys.argv[1], sys.argv[2]
seen = set()
while True:
    alive = subprocess.run(["tmux", "has-session", "-t", session],
                           capture_output=True).returncode == 0
    for d in sorted(glob.glob(os.path.join(out_dir, "*-r*"))):
        if d in seen or not os.path.exists(os.path.join(d, "procs_after")):
            continue
        seen.add(d)
        try:
            st = runlib.system_stats(d)
            oth = 100 * sum(s for _, s in runlib.other_cpu(d) or []) / st["elapsed_s"]
            mem = runlib.memory_stats(d) or {"swap_in": 0}
        except Exception as e:  # noqa: BLE001
            print(f"{d}: could not read ({e})", flush=True)
            continue
        if st["vm_share"] < 0.95 or oth > 30 or mem["swap_in"] > 1000:
            print(f"{os.path.basename(d)}: VM share {st['vm_share']:.2f}, other %CPU {oth:.0f}, "
                  f"swapped in {mem['swap_in']}", flush=True)
    if not alive:
        print(f"tmux session ended; {len(seen)} runs checked", flush=True)
        break
    time.sleep(20)
