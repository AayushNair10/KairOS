"""Read one run directory written by experiments/run.sh.

Used by summarize_run.py (one run) and aggregate.py (many runs).
"""
import csv
import glob
import json
import os

USER_HZ = 100  # unit of the CPU times in /proc/stat and /proc/<pid>/stat

# Processes that belong to the experiment itself; everything else counts as
# "other" CPU use (noise).
EXPERIMENT_COMMS = {"grpcserver", "loadgen", "zstd", "zstd_loop.sh", "scx_kairos"}


def load_summary(run_dir):
    with open(os.path.join(run_dir, "loadgen.json")) as f:
        return json.load(f)


def batch_mb_per_s(run_dir, start_ns, end_ns):
    """Batch throughput in MB/s of zstd input over [start_ns, end_ns).

    Each completed pass counts in proportion to how much of it overlaps the
    window, assuming zstd works through its input at a steady rate.
    """
    done_bytes = 0.0
    for path in glob.glob(os.path.join(run_dir, "batch_*.csv")):
        with open(path) as f:
            for row in csv.DictReader(f):
                s = int(row["start_unix_us"]) * 1000
                e = int(row["end_unix_us"]) * 1000
                overlap = min(e, end_ns) - max(s, start_ns)
                if overlap > 0:
                    done_bytes += int(row["bytes"]) * overlap / (e - s)
    return done_bytes / 1e6 / ((end_ns - start_ns) / 1e9)


def _read_proc_stat(path):
    """{"cpu0": [user, nice, system, idle, ...], ..., "ctxt": n} from /proc/stat."""
    out = {}
    with open(path) as f:
        for line in f:
            k, *v = line.split()
            if k.startswith("cpu") and k != "cpu":
                out[k] = [int(x) for x in v]
            elif k == "ctxt":
                out[k] = int(v[0])
    return out


def system_stats(run_dir):
    """CPU use and context switches between proc_stat_before and _after.

    The elapsed time comes from /proc/stat itself: every CPU's fields
    together advance by USER_HZ per second.
    """
    a = _read_proc_stat(os.path.join(run_dir, "proc_stat_before"))
    b = _read_proc_stat(os.path.join(run_dir, "proc_stat_after"))
    cpus = sorted((k for k in a if k != "ctxt"), key=lambda k: int(k[3:]))
    ticks = {c: [y - x for x, y in zip(a[c], b[c])] for c in cpus}
    elapsed = sum(sum(t) for t in ticks.values()) / len(cpus) / USER_HZ
    # Fields: user nice system idle iowait irq softirq steal ...
    busy = {c: 1 - (t[3] + t[4]) / sum(t) for c, t in ticks.items()}
    system = sum(t[2] + t[5] + t[6] for t in ticks.values()) / sum(
        sum(t) for t in ticks.values())
    return {
        "elapsed_s": elapsed,
        "ctxt_per_s": (b["ctxt"] - a["ctxt"]) / elapsed,
        "busy": busy,  # per CPU, fraction of time not idle
        "system_frac": system,  # all CPUs: system + irq + softirq time
    }


def _read_procs(path):
    """{pid: (comm, utime + stime in ticks)} from concatenated /proc/<pid>/stat."""
    out = {}
    with open(path, errors="replace") as f:
        for line in f:
            # "pid (comm) state ..." where comm may contain spaces or ')'.
            lp, rp = line.find("("), line.rfind(")")
            if lp < 0 or rp < 0:
                continue
            rest = line[rp + 2:].split()
            # rest[0] is field 3 (state); utime and stime are fields 14, 15.
            out[int(line[:lp])] = (line[lp + 1:rp], int(rest[11]) + int(rest[12]))
    return out


def other_cpu(run_dir):
    """CPU seconds used during the load by processes outside the experiment,
    by command name, largest first. Processes that started and exited between
    the snapshots are missed."""
    before = os.path.join(run_dir, "procs_before")
    if not os.path.exists(before):
        return None
    a = _read_procs(before)
    b = _read_procs(os.path.join(run_dir, "procs_after"))
    by_comm = {}
    for pid, (comm, t) in b.items():
        if comm in EXPERIMENT_COMMS:
            continue
        prev = a[pid][1] if pid in a and a[pid][0] == comm else 0
        if t > prev:
            by_comm[comm] = by_comm.get(comm, 0) + (t - prev) / USER_HZ
    return sorted(by_comm.items(), key=lambda kv: -kv[1])


def sched_stats(run_dir, start_ns, end_ns):
    """Means over [start_ns, end_ns) of the scx_kairos loader's per-second
    stats (sched_stats.csv), or None for runs without them. Each row covers
    the second before its timestamp; rows ending inside the window count."""
    path = os.path.join(run_dir, "sched_stats.csv")
    if not os.path.exists(path):
        return None
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            t = float(row["unix_s"]) * 1e9
            if start_ns + 1e9 <= t <= end_ns:  # the whole second is inside
                rows.append(row)
    if not rows:
        return None
    mean = lambda k: sum(float(r[k]) for r in rows) / len(rows)  # noqa: E731
    return {k: mean(k) for k in ("nr_runnable", "fast_load", "slow_load", "wake_rate",
                                 "wait_avg_us", "wait_max_us", "preempt", "local",
                                 "global", "no_victim")}


def phase_rows(run_dir):
    """One dict per loadgen phase, with that phase's batch throughput and
    (for scx_kairos runs) mean scheduler stats ("sched") added."""
    lg = load_summary(run_dir)
    rows = []
    for p in lg["phases"]:
        r = dict(p)
        r["batch_mb_s"] = batch_mb_per_s(run_dir, p["start_unix_ns"], p["end_unix_ns"])
        r["sched"] = sched_stats(run_dir, p["start_unix_ns"], p["end_unix_ns"])
        rows.append(r)
    return rows
