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
EXPERIMENT_COMMS = {"grpcserver", "redis-server", "loadgen", "zstd", "zstd_loop.sh", "scx_kairos"}


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
    together advance by USER_HZ per second. That is time the VM's CPUs
    ran: when the host doesn't run them (the Mac busy or asleep) it falls
    behind the wall clock, and this hypervisor reports no steal time. So
    vm_share = that time / the wall time between the two snapshots (from
    the files' modification times); ~0.97-0.98 when the host is quiet.
    """
    a = _read_proc_stat(os.path.join(run_dir, "proc_stat_before"))
    b = _read_proc_stat(os.path.join(run_dir, "proc_stat_after"))
    cpus = sorted((k for k in a if k != "ctxt"), key=lambda k: int(k[3:]))
    ticks = {c: [y - x for x, y in zip(a[c], b[c])] for c in cpus}
    wall = (os.stat(os.path.join(run_dir, "proc_stat_after")).st_mtime_ns
            - os.stat(os.path.join(run_dir, "proc_stat_before")).st_mtime_ns) / 1e9
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
        "vm_share": elapsed / wall if wall > 0 else float("nan"),
    }


def memory_stats(run_dir):
    """Memory counters between vmstat_before and _after: pages swapped in
    and out, major page faults (a thread waited for a page from disk),
    direct-reclaim stalls (an allocation waited for memory to be freed)
    and compaction stalls. None for runs from before these files existed.
    In E3 (2026-11-02) the slowest seconds came with swap-in bursts."""
    before = os.path.join(run_dir, "vmstat_before")
    if not os.path.exists(before):
        return None
    vals = []
    for path in (before, os.path.join(run_dir, "vmstat_after")):
        with open(path) as f:
            vals.append(dict((k, int(v)) for k, v in (line.split() for line in f)))
    a, b = vals
    d = {k: b[k] - a[k] for k in a if k in b}
    return {
        "swap_in": d.get("pswpin", 0),
        "swap_out": d.get("pswpout", 0),
        "major_faults": d.get("pgmajfault", 0),
        "reclaim_stalls": sum(v for k, v in d.items() if k.startswith("allocstall_")),
        "compact_stalls": d.get("compact_stall", 0),
    }


def pressure_stats(run_dir):
    """Pressure stall time (PSI) between pressure_before and _after, in ms
    per second of wall time: {"cpu_some", "memory_some", "memory_full",
    "io_some", "io_full"}. "some" = at least one task was stalled on the
    resource, "full" = all non-idle tasks were (cpu full is 0 system-wide).
    None for runs from before these files existed."""
    paths = [os.path.join(run_dir, f"pressure_{w}") for w in ("before", "after")]
    if not all(os.path.exists(p) for p in paths):
        return None
    vals = []
    for path in paths:
        d = {}
        with open(path) as f:
            for line in f:  # "<resource> some|full avg10=... total=<us>"
                r, kind, *fields = line.split()
                d[f"{r}_{kind}"] = int(fields[-1].split("=")[1])
        vals.append(d)
    # wall time between the snapshots from the files' modification times,
    # as in system_stats()
    wall = (os.stat(paths[1]).st_mtime_ns - os.stat(paths[0]).st_mtime_ns) / 1e9 or float("nan")
    a, b = vals
    return {k: (b[k] - a[k]) / 1000 / wall for k in
            ("cpu_some", "memory_some", "memory_full", "io_some", "io_full") if k in a and k in b}


def _read_procs(path):
    """{pid: (comm, utime + stime in ticks, start time in ticks since boot)}
    from concatenated /proc/<pid>/stat."""
    out = {}
    with open(path, errors="replace") as f:
        for line in f:
            # "pid (comm) state ..." where comm may contain spaces or ')'.
            lp, rp = line.find("("), line.rfind(")")
            if lp < 0 or rp < 0:
                continue
            rest = line[rp + 2:].split()
            # rest[0] is field 3 (state); utime and stime are fields 14, 15;
            # starttime is field 22.
            out[int(line[:lp])] = (line[lp + 1:rp], int(rest[11]) + int(rest[12]),
                                   int(rest[19]))
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
    for pid, (comm, t, start) in b.items():
        if comm in EXPERIMENT_COMMS:
            continue
        # Same process if the pid has the same start time (pids get reused;
        # names don't identify it: kworkers rename themselves after the
        # work item they run, e.g. kworker/2:2-events -> -mm_percpu_wq).
        prev = a[pid][1] if pid in a and a[pid][2] == start else 0
        if t > prev:
            by_comm[comm] = by_comm.get(comm, 0) + (t - prev) / USER_HZ
    return sorted(by_comm.items(), key=lambda kv: -kv[1])


def proc_cpu(run_dir, comm):
    """CPU seconds used during the load by the processes named comm that
    ran through both snapshots (all their threads; a process that started
    or exited in between, like each zstd of the batch loop, is missed)."""
    a = _read_procs(os.path.join(run_dir, "procs_before"))
    b = _read_procs(os.path.join(run_dir, "procs_after"))
    return sum(t - a[pid][1] for pid, (c, t, start) in b.items()
               if c == comm and pid in a and a[pid][2] == start) / USER_HZ


def sut_cpus(run_dir):
    """The server's CPU numbers from meta.json's workload string."""
    with open(os.path.join(run_dir, "meta.json")) as f:
        w = json.load(f)["workload"]
    spec = dict(kv.split("=", 1) for kv in w.split() if "=" in kv)["sut_cpus"]
    out = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        out += range(int(lo), int(hi or lo) + 1)
    return out


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
    # Newer columns (preempt_batch, to_batch, from_batch, the latency-target
    # and --split columns) are missing in older runs.
    return {k: mean(k) for k in ("nr_runnable", "fast_load", "slow_load", "wake_rate",
                                 "wait_avg_us", "wait_max_us", "preempt", "preempt_batch",
                                 "to_batch", "from_batch", "local", "global",
                                 "no_victim", "lat_wakeups", "lat_late", "late_frac",
                                 "mode_switches", "latency_ms", "lc_cpus", "lc_demand",
                                 "borrow", "kick", "idle_cpus", "idle_queued", "delayed",
                                 "late_preempt", "sync", "dup_pick", "dup_alt") if k in rows[0]}


def latency_with_failures(run_dir, nphases):
    """Per phase: (p50, p99, p99.9) in µs from requests.csv, counting failed
    requests (timeouts, errors) as infinitely slow. loadgen.json's
    percentiles only cover successful requests, which hides overload."""
    lat = [[] for _ in range(nphases)]
    with open(os.path.join(run_dir, "requests.csv")) as f:
        for r in csv.DictReader(f):
            v = int(r["latency_ns"]) / 1000 if r["code"] == "0" else float("inf")
            lat[int(r["phase"])].append(v)
    out = []
    for xs in lat:
        xs.sort()
        # nearest rank, as in loadgen
        pick = lambda q: xs[max(0, -(-int(q * 1000) * len(xs) // 1000) - 1)] if xs else float("nan")  # noqa: E731
        out.append((pick(0.50), pick(0.99), pick(0.999)))
    return out


def phase_rows(run_dir):
    """One dict per loadgen phase, with that phase's batch throughput,
    percentiles that count failed requests ("all_p50_us", ...) and (for
    scx_kairos runs) mean scheduler stats ("sched") added."""
    lg = load_summary(run_dir)
    allp = latency_with_failures(run_dir, len(lg["phases"]))
    rows = []
    for p in lg["phases"]:
        r = dict(p)
        r["all_p50_us"], r["all_p99_us"], r["all_p999_us"] = allp[p["phase"]]
        r["batch_mb_s"] = batch_mb_per_s(run_dir, p["start_unix_ns"], p["end_unix_ns"])
        r["sched"] = sched_stats(run_dir, p["start_unix_ns"], p["end_unix_ns"])
        rows.append(r)
    return rows
