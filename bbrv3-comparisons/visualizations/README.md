# Each stage of the ns-3 TcpBbrV3 port against Linux BBRv3

Figures comparing the six stages of the port (one commit each in this repo's
history, see `../README.md`) with the Linux kernel,
on Pedro's 36-config grid. Everything here is measured data: the per-flow
goodput of every Linux run and every ns-3 run. There is no BBR or TCP code in
this folder; the Python script only computes the comparison measures and
draws them.

```
pip install matplotlib
python3 make_figures.py          # rewrites figures/
```

## The data

- **Grid:** 50 / 100 / 500 Mb/s x 5 / 25 / 100 ms RTT x 0.1 / 1 / 2 / 4 BDP
  buffer. Two BBRv3 flows; the second joins at 12 s; 45 s per flow; MSS 1448.
  Config names are `rate/RTT/buffer`.
- **Linux:** kernel 6.13.7 with Google's BBRv3, run with nsperf on Google
  Cloud. Three complete grid runs.
- **ns-3:** each stage run with RngRun seeds 1, 2 and 3.
- **Goodput:** cumulative-ACK progress at the sender, in 0.1 s bins, measured
  the same way on both sides (`data/goodput_bins.csv.gz`; `tools/` rebuilds it
  from the raw results).

## The measures

- **Bad bin:** a 0.1 s bin in 15-45 s (both flows running) in which a flow gets
  less than half its own median goodput. This is the probing drop-off.
- **Stall:** 3 or more bad bins in a row (300 ms or longer).
- **Distance:** |bad-bin % of an ns-3 run - bad-bin % of a Linux run| for the
  same config. Every ns-3 seed is compared with every Linux run, and the result
  is averaged over configs.
- **Linux vs Linux:** the same distance between Linux's own runs. This is the
  noise floor: closer than this cannot be told apart from Linux.

## The stages

| Stage | Adds |
|---|---|
| 1 | the Nov 11 upload |
| 2 | inflight bounds in whole segments; TSO-sized bursts at skb boundaries |
| 3 | TSO deferral (`tcp_tso_should_defer()`) |
| 4 | ACK-aggregation cwnd in STARTUP |
| 5 | inflight at the next departure time; cwnd-limited tracked per window |
| 6 | inflight_hi growth floor; pacing catch-up credit (final) |

## Figures

### 1. Summary measures per stage
![summary](figures/01_summary_by_stage.png)
Distance, configs more than 3 pp off, stalls and run-to-run spread for each
stage. The dashed line is Linux against itself. Whiskers on the distance bars
are the best and worst of the 9 seed/kernel pairs.

### 2. Distance by buffer, RTT and rate
![groups](figures/02_distance_by_group.png)
Where each stage gained or lost. Red bars: Linux against itself for that group.

### 3. Stalls by buffer, RTT and rate
![stalls](figures/03_stalls_by_group.png)
The 1 BDP and 100 ms groups hold most of the remaining excess stalls.

### 4. Every config, every stage
![heatmap](figures/04_heatmap_by_stage.png)
Distance from Linux for each config. The last panel is Linux against itself;
500/100/1 is noisy in Linux too.

### 5. Each config across the stages
![lines](figures/05_per_config_across_stages.png)
One line per config, one panel per buffer size. The shaded band is Linux's own
noise for that buffer.

### 6. What each stage changed
![steps](figures/06_step_changes.png)
Per-config change in distance from one stage to the next: green moved closer
to Linux, red moved away. The grid-level change is under each panel.

### 7. ns-3 against Linux, config by config
![scatter](figures/07_ns3_vs_linux_scatter.png)
Bad-bin share of each config, Linux on x and ns-3 on y. On the diagonal = the
same as Linux; the band is within 3 pp.

### 8. Distribution over all runs
![cdf](figures/08_distributions.png)
Bad bins and stalls of every single run (36 configs x 3 runs).

### 9. A/B of the two stage-5 changes
![ab](figures/09_ab_stage5_changes.png)
Stage 6 with each stage-5 change removed, 4 configs x 3 seeds (500/100/1:
2 seeds). Without the per-window cwnd-limited flag, 100/100/1 improves but
50/100/1 and 50/5/0.1 get worse, and the mean is worse (1.91 vs 1.76 pp).

### 10. Loss episodes at 500/25/1
![loss](figures/10_loss_episodes_500_25_1.png)
Linux from the sender pcap, ns-3 from the bottleneck drop log, both grouped
the same way (gaps over 50 ms). The large ns-3 episode at 15.7 s is the
PROBE_RTT / PROBE_UP overlap described in [`../FIX-LOG.md`](../FIX-LOG.md)
(`data/loss_episodes_500_25_1.csv`).

### 11. Goodput over time, Linux and each stage
![100/100/1](figures/11_series_100_100_1.png)
![500/25/1](figures/11_series_500_25_1.png)
![50/5/0.1](figures/11_series_50_5_0.1.png)
![500/25/2](figures/11_series_500_25_2.png)
Run 1 of Linux and seed 1 of each stage. The shaded window is what is scored.

## Tables

- `figures/summary_by_stage.csv`: the grid-level measures per stage.
- `figures/per_config_by_stage.csv`: bad bins, distance and stalls per config
  per stage.
