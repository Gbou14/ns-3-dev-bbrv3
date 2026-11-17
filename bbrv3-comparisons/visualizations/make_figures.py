#!/usr/bin/env python3
"""Figures comparing each stage of the ns-3 TcpBbrV3 port with Linux BBRv3.

Plots measured data only: per-flow goodput of every Linux kernel run and every
ns-3 run on Pedro's 36-config grid (data/goodput_bins.csv.gz). Nothing here
simulates TCP or BBR.

Usage:  python3 make_figures.py [OUTDIR]      (default: figures/)
Needs:  matplotlib (pip install matplotlib)

The stages match the commits listed in ../README.md: each adds to the one before.
"""
import csv
import gzip
import itertools
import os
import statistics as st
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOW = (151, 450)  # 0.1 s bins 15.0-45.0 s: both flows running

STAGES = ["stage1", "stage2", "stage3", "stage4", "stage5", "stage6"]
NAMES = {
    "linux": "Linux",
    "stage1": "1 Nov 11 upload",
    "stage2": "2 whole segments + TSO bursts",
    "stage3": "3 TSO deferral",
    "stage4": "4 STARTUP ack aggregation",
    "stage5": "5 EDT inflight + cwnd-limited window",
    "stage6": "6 slope floor + pacing credit",
    "stage6_without_fix1": "6 without EDT inflight",
    "stage6_without_fix2": "6 without cwnd-limited window",
}
SHORT = {"stage1": "1", "stage2": "2", "stage3": "3", "stage4": "4", "stage5": "5", "stage6": "6"}
LINUX_C = "#b5462f"
STAGE_C = ["#c9d3df", "#a9bccf", "#86a3bf", "#5f88ad", "#3c6f99", "#1f5f8b"]
BUF_C = {"0.1": "#d08c2e", "1": "#b5462f", "2": "#3c6f99", "4": "#2f7d4f"}
plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False,
                     "axes.titlesize": 10, "axes.titleweight": "bold", "figure.dpi": 100})


# ---------------------------------------------------------------- data

def load():
    """{(series, run, cfg): {flow: [Mb/s per 0.1 s bin, 600 bins]}}."""
    runs = {}
    with gzip.open(os.path.join(HERE, "data", "goodput_bins.csv.gz"), "rt") as fh:
        rd = csv.reader(fh)
        next(rd)
        for row in rd:
            s, run, bw, rtt, buf, flow, unit = row[:7]
            if unit == "bytes":
                vals = [int(x) * 8 / 0.1 / 1e6 if x else 0.0 for x in row[7:]]
            else:
                vals = [float(x) if x else 0.0 for x in row[7:]]
            runs.setdefault((s, int(run), (bw, rtt, buf)), {})[int(flow)] = vals
    return runs


def bad_bins(flows):
    """(bad-bin %, stalls): bins below half the flow's median, runs of 3+."""
    bad = total = stalls = 0
    for f in (0, 1):
        u = flows.get(f, [0.0] * 600)[WINDOW[0] - 1:WINDOW[1]]
        med = st.median(u)
        run = 0
        for x in u:
            total += 1
            if x < 0.5 * med:
                bad += 1
                run += 1
                stalls += run == 3
            else:
                run = 0
    return 100.0 * bad / total, stalls


def ckey(c):
    return tuple(float(x) for x in c)


def cname(c):
    return "%s/%s/%s" % c


class Grid:
    def __init__(self):
        self.raw = load()
        self.score = {k: bad_bins(v) for k, v in self.raw.items()}
        self.runs, self.cfgs = {}, {}
        for s, r, c in self.raw:
            self.runs.setdefault(s, set()).add(r)
            self.cfgs.setdefault(s, set()).add(c)
        self.all36 = sorted(self.cfgs["linux"], key=ckey)

    def runs_of(self, s, c):
        return sorted(r for r in self.runs[s] if (s, r, c) in self.score)

    def bad(self, s, c):
        return st.mean(self.score[(s, r, c)][0] for r in self.runs_of(s, c))

    def stalls(self, s, c):
        return st.mean(self.score[(s, r, c)][1] for r in self.runs_of(s, c))

    def dist(self, s, c):
        """Mean |bad-bin gap| over every (ns-3 run, Linux run) pair of one config."""
        lin = [self.score[("linux", r, c)][0] for r in self.runs_of("linux", c)]
        if s == "linux":
            return st.mean(abs(a - b) for a, b in itertools.combinations(lin, 2))
        return st.mean(abs(self.score[(s, r, c)][0] - b) for r in self.runs_of(s, c) for b in lin)

    def summary(self, s):
        """Grid-level measures over the 36 configs, same basis as FIX-LOG.md."""
        cfgs, runs = self.all36, sorted(self.runs[s])
        lin = sorted(self.runs["linux"])

        def d(a, ra, b, rb):
            x = [abs(self.score[(a, ra, c)][0] - self.score[(b, rb, c)][0]) for c in cfgs]
            return st.mean(x), sum(v > 3 for v in x)

        if s == "linux":
            pairs = [d(s, a, s, b) for a, b in itertools.combinations(lin, 2)]
            spread = st.mean(p[0] for p in pairs)
        else:
            pairs = [d(s, a, "linux", b) for a in runs for b in lin]
            spread = st.mean(d(s, a, s, b)[0] for a, b in itertools.combinations(runs, 2))
        return {"dist": st.mean(p[0] for p in pairs), "dmin": min(p[0] for p in pairs),
                "dmax": max(p[0] for p in pairs), "off3": st.mean(p[1] for p in pairs),
                "stalls": st.mean(sum(self.score[(s, r, c)][1] for c in cfgs) for r in runs),
                "bad": st.mean(st.mean(self.score[(s, r, c)][0] for c in cfgs) for r in runs),
                "spread": spread}


def save(fig, out, name):
    fig.savefig(os.path.join(out, name), dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("  " + name)


# ---------------------------------------------------------------- figures

def fig_summary(g, out):
    sm = {s: g.summary(s) for s in ["linux"] + STAGES}
    fig, axs = plt.subplots(2, 2, figsize=(11, 7))
    panels = [("dist", "Distance from Linux (pp)", True), ("off3", "Configs more than 3 pp off", False),
              ("stalls", "Stalls per run (300 ms or longer)", False), ("spread", "Run-to-run spread (pp)", False)]
    x = range(len(STAGES))
    for ax, (k, title, whisk) in zip(axs.flat, panels):
        v = [sm[s][k] for s in STAGES]
        ax.bar(x, v, color=STAGE_C, edgecolor="#1f5f8b", linewidth=0.6)
        if whisk:
            ax.errorbar(x, v, yerr=[[sm[s]["dist"] - sm[s]["dmin"] for s in STAGES],
                                    [sm[s]["dmax"] - sm[s]["dist"] for s in STAGES]],
                        fmt="none", ecolor="#333", capsize=3, linewidth=1)
        ax.axhline(sm["linux"][k], color=LINUX_C, ls="--", lw=1.4)
        ax.text(len(STAGES) - 0.5, sm["linux"][k], " Linux %.2f" % sm["linux"][k] if k in ("dist", "spread")
                else " Linux %.1f" % sm["linux"][k], color=LINUX_C, va="bottom", ha="right", fontsize=8)
        for i, y in enumerate(v):
            ax.text(i, y, ("%.2f" if k in ("dist", "spread") else "%.1f") % y, ha="center", va="bottom", fontsize=8)
        ax.set_title(title, loc="left")
        ax.set_xticks(list(x))
        ax.set_xticklabels([SHORT[s] for s in STAGES])
        ax.set_xlabel("stage")
    fig.suptitle("Each stage against Linux, 36 configs (3 ns-3 seeds x 3 Linux runs)", fontweight="bold")
    fig.text(0.5, -0.02, "  ".join("%s = %s" % (SHORT[s], NAMES[s][2:]) for s in STAGES), ha="center", fontsize=8)
    fig.tight_layout()
    save(fig, out, "01_summary_by_stage.png")


def groups(g):
    out = []
    for idx, name, unit in ((2, "buffer", "BDP"), (1, "RTT", "ms"), (0, "rate", "Mb/s")):
        vals = sorted({c[idx] for c in g.all36}, key=float)
        out.append((name, unit, [(v, [c for c in g.all36 if c[idx] == v]) for v in vals]))
    return out


def fig_groups(g, out, what):
    fig, axs = plt.subplots(1, 3, figsize=(13, 4), gridspec_kw={"width_ratios": [4, 3, 3]})
    for ax, (name, unit, gs) in zip(axs, groups(g)):
        n = len(STAGES)
        w = 0.8 / (n + 1)
        for j, s in enumerate(STAGES + ["linux"]):
            ys = []
            for v, cs in gs:
                if what == "dist":
                    ys.append(st.mean(g.dist(s, c) for c in cs))
                else:
                    ys.append(sum(g.stalls(s, c) for c in cs))
            xs = [i - 0.4 + w * (j + 0.5) for i in range(len(gs))]
            ax.bar(xs, ys, width=w, color=LINUX_C if s == "linux" else STAGE_C[j],
                   label=NAMES[s] if s != "linux" else ("Linux vs Linux" if what == "dist" else "Linux"))
        ax.set_xticks(range(len(gs)))
        ax.set_xticklabels(["%s %s" % (v, unit) for v, _ in gs])
        ax.set_title("by %s" % name, loc="left")
    axs[0].set_ylabel("distance from Linux (pp)" if what == "dist" else "stalls per run (sum over group)")
    axs[0].legend(fontsize=7, frameon=False, ncol=1)
    fig.suptitle(("Distance from Linux" if what == "dist" else "Stalls") + " by buffer, RTT and rate, per stage",
                 fontweight="bold")
    fig.tight_layout()
    save(fig, out, "02_distance_by_group.png" if what == "dist" else "03_stalls_by_group.png")


def heat_rows(g):
    rows = [(bw, rtt) for bw in ("50", "100", "500") for rtt in ("5", "25", "100")]
    return rows, ["0.1", "1", "2", "4"]


def fig_heatmaps(g, out):
    rows, bufs = heat_rows(g)
    cmap = LinearSegmentedColormap.from_list("d", ["#f4f7fa", "#f1c9bd", "#b5462f"])
    fig, axs = plt.subplots(1, 7, figsize=(17, 5.2), sharey=True)
    for ax, s in zip(axs, STAGES + ["linux"]):
        m = [[g.dist(s, (bw, rtt, b)) for b in bufs] for bw, rtt in rows]
        ax.imshow(m, cmap=cmap, vmin=0, vmax=4, aspect="auto")
        for i in range(len(rows)):
            for j in range(len(bufs)):
                ax.text(j, i, "%.1f" % m[i][j], ha="center", va="center", fontsize=7.5,
                        color="white" if m[i][j] > 2.6 else "#1b2330")
        ax.set_xticks(range(len(bufs)))
        ax.set_xticklabels(bufs)
        ax.set_xlabel("buffer (BDP)")
        ax.set_title("stage %s\n%s" % (SHORT[s], NAMES[s][2:]) if s != "linux" else "Linux\nvs Linux",
                     fontsize=8, loc="left")
        ax.spines[["left", "bottom"]].set_visible(False)
    axs[0].set_yticks(range(len(rows)))
    axs[0].set_yticklabels(["%s Mb/s, %s ms" % r for r in rows])
    fig.suptitle("Distance from Linux per config (pp; 0 = same bad-bin share as Linux)", fontweight="bold")
    fig.tight_layout()
    save(fig, out, "04_heatmap_by_stage.png")


def fig_config_lines(g, out):
    fig, axs = plt.subplots(1, 4, figsize=(15, 4.6), sharey=True)
    x = list(range(1, len(STAGES) + 1))
    for ax, b in zip(axs, ["0.1", "1", "2", "4"]):
        cs = [c for c in g.all36 if c[2] == b]
        for c in cs:
            ys = [g.dist(s, c) for s in STAGES]
            ax.plot(x, ys, marker="o", ms=3, lw=1.1, alpha=0.85)
            ax.text(x[-1] + 0.12, ys[-1], "%s/%s" % c[:2], fontsize=6.5, va="center")
        noise = st.mean(g.dist("linux", c) for c in cs)
        ax.axhspan(0, noise, color=LINUX_C, alpha=0.10, lw=0)
        ax.axhline(noise, color=LINUX_C, ls="--", lw=1)
        ax.set_title("%s BDP buffer" % b, loc="left")
        ax.set_xticks(x)
        ax.set_xlabel("stage")
        ax.set_xlim(0.7, len(STAGES) + 0.9)
    axs[0].set_ylabel("distance from Linux (pp)")
    fig.suptitle("Each config across the stages (shaded: Linux vs Linux mean for that buffer; labels: rate/RTT)",
                 fontweight="bold")
    fig.tight_layout()
    save(fig, out, "05_per_config_across_stages.png")


def fig_steps(g, out):
    fig, axs = plt.subplots(1, len(STAGES) - 1, figsize=(16, 7.5), sharey=True)
    order = sorted(g.all36, key=lambda c: (float(c[2]), float(c[0]), float(c[1])))
    y = range(len(order))
    for ax, a, b in zip(axs, STAGES[:-1], STAGES[1:]):
        dv = [g.dist(b, c) - g.dist(a, c) for c in order]
        ax.barh(list(y), dv, color=["#2f7d4f" if v < 0 else LINUX_C for v in dv], height=0.7)
        ax.axvline(0, color="#333", lw=0.8)
        ax.set_title("%s -> %s\n%s" % (SHORT[a], SHORT[b], NAMES[b][2:]), fontsize=8.5, loc="left")
        tot = g.summary(b)["dist"] - g.summary(a)["dist"]
        ax.set_xlabel("change in distance (pp)\ngrid: %+.2f pp" % tot)
        lim = max(4.5, max(abs(v) for v in dv) * 1.05)
        ax.set_xlim(-lim, lim)
    axs[0].set_yticks(list(y))
    axs[0].set_yticklabels([cname(c) for c in order], fontsize=7)
    axs[0].invert_yaxis()
    fig.suptitle("What each stage changed, per config (green = closer to Linux, red = further)", fontweight="bold")
    fig.tight_layout()
    save(fig, out, "06_step_changes.png")


def fig_scatter(g, out):
    fig, axs = plt.subplots(2, 3, figsize=(12, 8), sharex=True, sharey=True)
    lim = 18
    for ax, s in zip(axs.flat, STAGES):
        for c in g.all36:
            ax.scatter(g.bad("linux", c), g.bad(s, c), s=22, color=BUF_C[c[2]], edgecolor="white", linewidth=0.4)
        ax.plot([0, lim], [0, lim], color="#888", lw=0.8)
        ax.fill_between([0, lim], [-3, lim - 3], [3, lim + 3], color="#888", alpha=0.08, lw=0)
        ax.set_title(NAMES[s], loc="left", fontsize=9)
        ax.set_xlim(0, lim)
        ax.set_ylim(0, lim)
    for b, col in BUF_C.items():
        axs[0, 0].scatter([], [], color=col, label="%s BDP" % b)
    axs[0, 0].legend(frameon=False, fontsize=7, loc="upper left")
    for ax in axs[1]:
        ax.set_xlabel("Linux bad bins (%)")
    for ax in axs[:, 0]:
        ax.set_ylabel("ns-3 bad bins (%)")
    fig.suptitle("Bad-bin share per config, ns-3 against Linux (diagonal = equal; band = within 3 pp)",
                 fontweight="bold")
    fig.tight_layout()
    save(fig, out, "07_ns3_vs_linux_scatter.png")


def fig_cdf(g, out):
    fig, axs = plt.subplots(1, 2, figsize=(12, 4.3))
    for s in ["linux"] + STAGES:
        bad = sorted(g.score[(s, r, c)][0] for c in g.all36 for r in g.runs_of(s, c))
        stl = sorted(g.score[(s, r, c)][1] for c in g.all36 for r in g.runs_of(s, c))
        col = LINUX_C if s == "linux" else STAGE_C[STAGES.index(s)]
        lw = 2.2 if s in ("linux", "stage6") else 1.2
        for ax, v in zip(axs, (bad, stl)):
            ax.step(v, [(i + 1) / len(v) for i in range(len(v))], where="post", color=col, lw=lw, label=NAMES[s])
    axs[0].set_xlabel("bad bins in one run (%)")
    axs[1].set_xlabel("stalls in one run")
    axs[1].set_xlim(0, 15)
    for ax in axs:
        ax.set_ylabel("share of runs (36 configs x 3 runs)")
    axs[0].legend(frameon=False, fontsize=7)
    fig.suptitle("Distribution over all runs", fontweight="bold")
    fig.tight_layout()
    save(fig, out, "08_distributions.png")


def fig_ab(g, out):
    series = ["stage4", "stage6", "stage6_without_fix1", "stage6_without_fix2"]
    cols = ["#86a3bf", "#1f5f8b", "#d08c2e", "#7a5aa6"]
    cfgs = sorted(g.cfgs["stage6_without_fix1"], key=ckey)
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.3))
    w = 0.8 / len(series)
    for j, s in enumerate(series):
        xs = [i - 0.4 + w * (j + 0.5) for i in range(len(cfgs))]
        axs[0].bar(xs, [g.dist(s, c) for c in cfgs], width=w, color=cols[j], label=NAMES[s])
        axs[1].bar(xs, [g.stalls(s, c) for c in cfgs], width=w, color=cols[j])
    for i, c in enumerate(cfgs):
        axs[0].plot([i - 0.42, i + 0.42], [g.dist("linux", c)] * 2, color=LINUX_C, lw=1.6)
        axs[1].plot([i - 0.42, i + 0.42], [g.stalls("linux", c)] * 2, color=LINUX_C, lw=1.6)
    for ax, t in zip(axs, ("distance from Linux (pp); red = Linux vs Linux", "stalls per run; red = Linux")):
        ax.set_xticks(range(len(cfgs)))
        ax.set_xticklabels([cname(c) for c in cfgs])
        ax.set_title(t, loc="left")
    axs[0].legend(frameon=False, fontsize=7)
    fig.suptitle("A/B: stage 6 with each of the two stage-5 changes removed (500/100/1: 2 seeds)", fontweight="bold")
    fig.tight_layout()
    save(fig, out, "09_ab_stage5_changes.png")


def fig_loss(out):
    rows = list(csv.DictReader(open(os.path.join(HERE, "data", "loss_episodes_500_25_1.csv"))))
    series = [("linux", "Linux"), ("stage4", "ns-3 stage 4"), ("stage6", "ns-3 stage 6")]
    cols = [LINUX_C, "#86a3bf", "#1f5f8b"]
    fig, axs = plt.subplots(1, 2, figsize=(12, 4.3))
    for i, (s, lab) in enumerate(series):
        for r in (1, 2, 3):
            ep = [x for x in rows if x["series"] == s and int(x["run"]) == r]
            xs = [float(x["start_s"]) for x in ep]
            axs[0].scatter(xs, [int(x["lost"]) for x in ep], s=14, color=cols[i], alpha=0.75,
                           label=lab if r == 1 else None, marker="os^"[r - 1])
        tot = [sum(int(x["lost_retransmissions"]) for x in rows if x["series"] == s and int(x["run"]) == r)
               for r in (1, 2, 3)]
        lost = [sum(int(x["lost"]) for x in rows if x["series"] == s and int(x["run"]) == r) for r in (1, 2, 3)]
        axs[1].bar([i - 0.2], [st.mean(lost)], width=0.38, color=cols[i])
        axs[1].bar([i + 0.2], [st.mean(tot) * 10], width=0.38, color=cols[i], alpha=0.45, hatch="//")
        axs[1].text(i - 0.2, st.mean(lost), "%d" % st.mean(lost), ha="center", va="bottom", fontsize=8)
        axs[1].text(i + 0.2, st.mean(tot) * 10, "%d" % st.mean(tot), ha="center", va="bottom", fontsize=8)
    axs[0].set_xlabel("episode start (s); markers = run 1, 2, 3")
    axs[0].set_ylabel("packets lost in the episode")
    axs[0].legend(frameon=False)
    axs[0].set_title("Loss episodes at 500/25/1, 15-45 s", loc="left")
    axs[1].set_xticks(range(3))
    axs[1].set_xticklabels([lab for _, lab in series])
    axs[1].set_title("Per run: packets lost (solid), lost retransmissions x10 (hatched)", loc="left")
    fig.tight_layout()
    save(fig, out, "10_loss_episodes_500_25_1.png")


def fig_series(g, out, c):
    rows = ["linux"] + STAGES
    fig, axs = plt.subplots(len(rows), 1, figsize=(12, 1.55 * len(rows)), sharex=True, sharey=True)
    t = [(k + 1) / 10 for k in range(600)]
    for ax, s in zip(axs, rows):
        d = g.raw.get((s, 1, c))
        if not d:
            continue
        ax.plot(t, d[0], lw=0.7, color="#1f5f8b")
        ax.plot(t, d[1], lw=0.7, color="#d08c2e")
        ax.axvspan(15, 45, color="#eef1f5", zorder=0)
        b, sl = g.score[(s, 1, c)]
        ax.set_title("%s (run 1): bad bins %.1f%%, stalls %d" % (NAMES[s] if s != "linux" else "Linux", b, sl),
                     fontsize=8, loc="left")
        ax.set_ylabel("Mb/s", fontsize=7)
    axs[-1].set_xlabel("time (s); flow 0 blue, flow 1 orange (joins at 12 s); shaded = scored window")
    fig.suptitle("Goodput at %s Mb/s / %s ms / %s BDP, Linux and each stage" % c, fontweight="bold")
    fig.tight_layout()
    save(fig, out, "11_series_%s_%s_%s.png" % c)


def write_tables(g, out):
    with open(os.path.join(out, "summary_by_stage.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["series", "distance_pp", "best_pair_pp", "worst_pair_pp", "configs_over_3pp",
                    "stalls_per_run", "bad_bins_pct", "run_to_run_spread_pp"])
        for s in ["linux"] + STAGES:
            m = g.summary(s)
            w.writerow([s] + ["%.3f" % m[k] for k in ("dist", "dmin", "dmax", "off3", "stalls", "bad", "spread")])
    with open(os.path.join(out, "per_config_by_stage.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["config", "linux_bad_pct", "linux_vs_linux_pp", "linux_stalls"] +
                   ["%s_%s" % (s, k) for s in STAGES for k in ("bad_pct", "distance_pp", "stalls")])
        for c in g.all36:
            w.writerow([cname(c), "%.2f" % g.bad("linux", c), "%.2f" % g.dist("linux", c),
                        "%.2f" % g.stalls("linux", c)] +
                       ["%.2f" % f(s, c) for s in STAGES for f in (g.bad, g.dist, g.stalls)])
    print("  summary_by_stage.csv, per_config_by_stage.csv")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "figures")
    os.makedirs(out, exist_ok=True)
    g = Grid()
    print("writing to", out)
    fig_summary(g, out)
    fig_groups(g, out, "dist")
    fig_groups(g, out, "stalls")
    fig_heatmaps(g, out)
    fig_config_lines(g, out)
    fig_steps(g, out)
    fig_scatter(g, out)
    fig_cdf(g, out)
    fig_ab(g, out)
    fig_loss(out)
    for c in [("100", "100", "1"), ("500", "25", "1"), ("50", "5", "0.1"), ("500", "25", "2")]:
        fig_series(g, out, c)
    write_tables(g, out)


if __name__ == "__main__":
    main()
