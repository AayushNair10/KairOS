#!/usr/bin/env python3
"""Figures for the report, from the final evaluation (F).

  tradeoff.pdf  latency against batch throughput, one point per scheduler
                (median over reps, whiskers = interquartile range): the
                burst sweep (busy p50) and the Azure replay (p50)
  byburst.pdf   the burst sweep per burst length L: busy p50 and the busy
                share of requests over 5 ms

Per-run metrics come from stats.py's own functions (burst_metrics,
level_metrics), so the figures show the numbers the tests use. They are
cached as CSV in <out>/data/ (one row per run and metric), which is also
the figures' table view; --refresh recomputes them (~2 min for F).

Needs matplotlib, which the rest of analysis/ doesn't; e.g.
  python3 -m venv ~/kairos-data/venv && ~/kairos-data/venv/bin/pip install matplotlib
  ~/kairos-data/venv/bin/python analysis/plots.py

Usage: plots.py [--burst DIR] [--azure DIR] [--out DIR] [--refresh] [--png]
"""
import argparse
import csv
import glob
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stats  # noqa: E402

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    raise SystemExit("plots.py needs matplotlib (see the top of this file)")

# Names in the figures, by result label. Variants of kairos are written
# kairos-<variant>.
NAMES = {
    "kairos": "kairos",
    "pw1000r100": "kairos-pw1000r100",
    "fixed3": "kairos-fixed3",
    "never3": "kairos-never3",
    "bpfland": "bpfland",
    "lavd": "lavd",
    "lavdperf": "lavd (performance)",
    "eevdf": "EEVDF",
    "eevdfidle": "EEVDF, batch idle",
}
# Left out of the figures: fixed3 ran the same 3-CPU pool as kairos and
# lands on it (RESEARCH_LOG 6.32); lavdperf lands on lavd.
OMIT = {"fixed3", "lavdperf"}

# Colors: the dataviz reference palette's first three categorical slots
# (validated for all pairs), the rest in the muted gray, so kairos and its
# closest baseline carry the hue. Ink and chrome from the same palette.
HUE = {"kairos": "#2a78d6", "pw1000r100": "#eb6834", "bpfland": "#1baf7a"}
MARKER = {"kairos": "o", "pw1000r100": "s", "bpfland": "D"}
GRAY = "#898781"
INK, INK2, MUTED = "#0b0b0b", "#52514e", "#898781"
GRID, AXIS = "#e1e0d9", "#c3c2b7"

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.size": 8,
    "axes.labelsize": 8,
    "axes.labelcolor": INK2,
    "axes.edgecolor": AXIS,
    "axes.linewidth": 0.6,
    "axes.titlesize": 8,
    "axes.titleweight": "bold",
    "axes.titlecolor": INK,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "xtick.labelcolor": INK2,
    "ytick.labelcolor": INK2,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "xtick.minor.width": 0.4,
    "ytick.minor.width": 0.4,
    "grid.color": GRID,
    "grid.linewidth": 0.5,
    "grid.linestyle": "-",
    "legend.frameon": False,
    "legend.fontsize": 7,
    # TrueType fonts in the PDF: paper templates reject Type 3 fonts.
    "pdf.fonttype": 42,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
})


def load(results_dir, kind, cache, refresh):
    """{config: {rep: {metric: value}}} for every run, cached as CSV."""
    if os.path.exists(cache) and not refresh:
        vals = {}
        with open(cache) as f:
            for r in csv.DictReader(f):
                vals.setdefault(r["config"], {}).setdefault(int(r["rep"]), {})[r["metric"]] = float(r["value"])
        return vals
    vals = {}
    for d in sorted(glob.glob(os.path.join(results_dir, "*-r*"))):
        m = re.fullmatch(r"(.+)-r(\d+)", os.path.basename(d))
        if not m or not os.path.exists(os.path.join(d, "loadgen.json")):
            continue
        ms = stats.burst_metrics(d) if kind == "burst" else stats.level_metrics(d, [1000, 2000])
        vals.setdefault(m.group(1), {})[int(m.group(2))] = {n: v for n, _, _, v in ms}
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "rep", "metric", "value"])
        for c in sorted(vals):
            for rep in sorted(vals[c]):
                for n, v in vals[c][rep].items():
                    w.writerow([c, rep, n, v])
    return vals


def summary(vals, config, metric):
    """(median, first quartile, third quartile) over the reps."""
    x = [vals[config][r][metric] for r in sorted(vals[config])]
    q1, q2, q3 = statistics.quantiles(x, n=4, method="inclusive")
    return statistics.median(x), q1, q3


def style(ax, xticks=None, yticks=None):
    """Recessive chrome; xticks/yticks: a log axis with these major ticks."""
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(True, which="major")
    ax.set_axisbelow(True)
    for axis, ticks, scale in ((ax.xaxis, xticks, ax.set_xscale), (ax.yaxis, yticks, ax.set_yscale)):
        if ticks is None:
            continue
        scale("log")
        axis.set_major_locator(matplotlib.ticker.FixedLocator(ticks))
        axis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
        axis.set_minor_locator(matplotlib.ticker.NullLocator())


# Label offsets in points from each point, alignment, and whether a leader
# line joins label and point, per panel; set by looking at the figure.
TRADEOFF_LABELS = {
    "burst": {
        "kairos": (6, -2, "left", "top"),
        "pw1000r100": (-50, 21, "left", "bottom", True),
        "bpfland": (7, 3, "left", "bottom"),
        "eevdfidle": (7, -2, "left", "top"),
        "eevdf": (7, -2, "left", "top"),
        "never3": (6, 2, "left", "bottom"),
        "lavd": (-6, -2, "right", "top"),
    },
    "azure": {
        "kairos": (6, -2, "left", "top"),
        "pw1000r100": (-6, 2, "right", "bottom"),
        "bpfland": (6, -2, "left", "top"),
        "eevdfidle": (0, -7, "center", "top"),
        "eevdf": (6, -2, "left", "top"),
        "never3": (0, 6, "center", "bottom"),
        "lavd": (6, 2, "left", "bottom"),
    },
}


def tradeoff(panels, path):
    """panels: [(title, vals, latency metric, x label, x ticks, x limits, label key)]."""
    fig, axes = plt.subplots(1, len(panels), figsize=(7.0, 2.5))
    for ax, (title, vals, lat, xlabel, xticks, xlim, key) in zip(axes, panels):
        style(ax, xticks=xticks)
        ax.set_xlim(*xlim)
        configs = [c for c in vals if c not in OMIT]
        # gray points first, so the highlighted ones sit on top
        for c in sorted(configs, key=lambda c: c in HUE):
            x, x1, x3 = summary(vals, c, lat)
            y, y1, y3 = summary(vals, c, "batch, whole load")
            color = HUE.get(c, GRAY)
            hi = c in HUE
            ax.plot([x1, x3], [y, y], color=color, lw=0.8, alpha=0.8, zorder=2)
            ax.plot([x, x], [y1, y3], color=color, lw=0.8, alpha=0.8, zorder=2)
            # the white edge is the surface ring that keeps markers legible
            ax.plot(x, y, MARKER.get(c, "o"), ms=6 if hi else 4.5, color=color,
                    mec="white", mew=1.0, zorder=4 if hi else 3)
            dx, dy, ha, va, *leader = TRADEOFF_LABELS[key].get(c, (6, 0, "left", "center"))
            arrow = dict(arrowstyle="-", color=AXIS, lw=0.5, shrinkA=1, shrinkB=4) if leader else None
            ax.annotate(NAMES.get(c, c), (x, y), xytext=(dx, dy), textcoords="offset points",
                        arrowprops=arrow,
                        ha=ha, va=va, fontsize=7, color=INK if hi else INK2,
                        fontweight="bold" if hi else "normal", zorder=5)
        ax.set_title(title, loc="left")
        ax.set_xlabel(xlabel)
        ax.set_ylabel("batch throughput (MB/s)")
        ax.annotate("better", xy=(0.03, 0.97), xytext=(0.17, 0.83), xycoords="axes fraction",
                    textcoords="axes fraction", color=MUTED, fontsize=7, ha="left", va="top",
                    arrowprops=dict(arrowstyle="->", color=MUTED, lw=0.6))
    fig.tight_layout(w_pad=2.0)
    fig.savefig(path)
    return fig


def byburst(vals, path):
    lengths = sorted({int(m.group(1)) for c in vals for r in vals[c] for n in vals[c][r]
                      if (m := re.fullmatch(r"busy p50, L (\d+) ms", n))})
    configs = [c for c in vals if c not in OMIT]
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.5))
    for ax, (fmt, ylabel, yticks) in zip(axes, [
            ("busy p50, L {} ms", "busy p50 (ms, log scale)", [1, 2, 5, 10, 20]),
            ("busy >5 ms, L {} ms", "busy requests over 5 ms (%)", None)]):
        logy = yticks is not None
        style(ax, xticks=lengths, yticks=yticks)
        ends = []
        for c in sorted(configs, key=lambda c: c in HUE):
            med = [summary(vals, c, fmt.format(L))[0] for L in lengths]
            hi = c in HUE
            color = HUE.get(c, GRAY)
            ax.plot(lengths, med, color=color, lw=1.5 if hi else 1.0,
                    solid_capstyle="round", solid_joinstyle="round", zorder=3 if hi else 2)
            ax.plot(lengths, med, MARKER.get(c, "o"), ms=4.5 if hi else 3, color=color,
                    mec="white", mew=0.8, zorder=4 if hi else 2)
            ends.append((med[-1], c))
        # end labels, spread apart where lines end close together
        ends.sort()
        placed = []
        for v, c in ends:
            y = v
            if placed:
                prev = placed[-1]
                # minimum gap, about a line of 7 pt text at this figure size
                y = max(y, prev * 1.3) if logy else max(y, prev + 6)
            placed.append(y)
            hi = c in HUE
            ax.annotate(NAMES.get(c, c), (lengths[-1], v), xytext=(lengths[-1] * 1.25, y),
                        textcoords="data", fontsize=7, va="center", ha="left",
                        color=INK if hi else INK2, fontweight="bold" if hi else "normal",
                        arrowprops=dict(arrowstyle="-", color=AXIS, lw=0.5) if y != v else None)
        ax.set_xlim(lengths[0] / 1.4, lengths[-1] * 1.4)
        ax.set_xlabel("burst length L")
        ax.set_ylabel(ylabel)
        # a tick at every L, a label at every decade (all of them don't fit)
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(
            lambda v, _: "" if f"{v:g}"[0] != "1" else f"{v / 1000:g} s" if v >= 1000 else f"{v:g} ms"))
    fig.tight_layout(w_pad=4.0)
    fig.savefig(path)
    return fig


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--burst", default="results/2026-11-05-final-burst")
    ap.add_argument("--azure", default="results/2026-11-05-final-azure")
    ap.add_argument("--out", default="docs/report/figures")
    ap.add_argument("--refresh", action="store_true", help="recompute the cached per-run metrics")
    ap.add_argument("--png", action="store_true", help="also write PNGs (for a quick look)")
    a = ap.parse_args()
    data = os.path.join(a.out, "data")
    burst = load(a.burst, "burst", os.path.join(data, os.path.basename(a.burst) + ".csv"), a.refresh)
    azure = load(a.azure, "level", os.path.join(data, os.path.basename(a.azure) + ".csv"), a.refresh)
    figs = {
        "tradeoff": tradeoff([
            ("Burst sweep (10 ms - 10 s bursts)", burst, "busy p50, all L",
             "busy p50 (ms, log scale)", [1, 2, 5, 10], (0.75, 12), "burst"),
            ("Azure Functions replay", azure, "p50, all", "p50 (ms, log scale)",
             [0.8, 1, 1.2, 1.4, 1.6], (0.7, 1.8), "azure"),
        ], os.path.join(a.out, "tradeoff.pdf")),
        "byburst": byburst(burst, os.path.join(a.out, "byburst.pdf")),
    }
    for name, fig in figs.items():
        if a.png:
            fig.savefig(os.path.join(a.out, name + ".png"), dpi=200)
        print(os.path.join(a.out, name + ".pdf"))


if __name__ == "__main__":
    main()
