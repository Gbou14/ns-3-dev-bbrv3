#!/usr/bin/env python3
"""Rebuild data/goodput_bins.csv.gz from the raw lab results (linux-lab/results).

Not needed to make the figures: the CSV is included. Kept so the data can be
traced back to the raw runs.

Goodput is cumulative-ACK progress at the sender, per flow, per 0.1 s bin, on
both sides. Linux values come from the sender pcap (pcap2csv.py -> flows.csv,
goodput_mbps, copied as written, unit "mbps"). ns-3 values come from the
harness's acked_bytes column (run.csv.gz): bytes ACKed in the bin, exact, unit
"bytes".

Usage: extract_data.py <linux-lab/results dir> <output csv.gz>
"""
import csv
import gzip
import os
import sys

# (series id, result folders). The stage ids match the commits listed in ../../README.md.
SERIES = [
    ("linux", ["out", "out_fresh", "out_k3"]),
    ("stage1", ["ns3out_hook_s1", "ns3out_hook_s2", "ns3out_hook_s3"]),
    ("stage2", ["ns3out_pair_s1", "ns3out_pair_s2", "ns3out_pair_s3"]),
    ("stage3", ["ns3out_deferfull_s1", "ns3out_deferfull_s2", "ns3out_deferfull_s3"]),
    ("stage4", ["ns3out_aggrfull_s1", "ns3out_aggrfull_s2", "ns3out_aggrfull_s3"]),
    ("stage5", ["ns3out_val1s_s1", "ns3out_val1s_s2", "ns3out_val1s_s3"]),
    ("stage6", ["ns3out_cred_s1", "ns3out_cred_s2", "ns3out_cred_s3"]),
    ("stage6_without_fix1", ["ns3out_nofix1_s1", "ns3out_nofix1_s2", "ns3out_nofix1_s3"]),
    ("stage6_without_fix2", ["ns3out_nofix2_s1", "ns3out_nofix2_s2", "ns3out_nofix2_s3"]),
    # Earlier versions, run Nov 17 with today's harness (README: "Earlier versions").
    ("aug31", ["ns3out_aug31_s1", "ns3out_aug31_s2", "ns3out_aug31_s3"]),
    ("oct21", ["ns3out_oct21_s1", "ns3out_oct21_s2", "ns3out_oct21_s3"]),
    ("nov10", ["ns3out_nov10h_s1", "ns3out_nov10h_s2", "ns3out_nov10h_s3"]),
    # Stages 4 and 6 with seeds 4-6 added (runs 1-3 are the grids above).
    ("stage4_6seeds", ["ns3out_aggrfull_s%d" % s for s in range(1, 7)]),
    ("stage6_6seeds", ["ns3out_cred_s%d" % s for s in range(1, 7)]),
]
NBINS = 600  # 0-60 s


def complete(d):
    if not os.path.exists(os.path.join(d, "done")):
        return False
    if os.path.exists(os.path.join(d, "flows.csv")):
        return True
    last = None
    with gzip.open(os.path.join(d, "run.csv.gz"), "rt") as fh:
        for last in fh:
            pass
    return last is not None and float(last.split(",")[0]) > 56


def linux_bins(d):
    out = {}
    for r in csv.DictReader(open(os.path.join(d, "flows.csv"))):
        out.setdefault(int(r["flow"]), {})[int(round(float(r["t"]) * 10))] = r["goodput_mbps"]
    return out


def ns3_bins(d):
    out, prev = {}, {}
    with gzip.open(os.path.join(d, "run.csv.gz"), "rt") as fh:
        for r in csv.DictReader(fh):
            if None in r.values():
                continue
            f = int(r["flow_id"])
            ack = r.get("acked_bytes")
            rx = int(ack) if ack not in (None, "") else int(r["rx_bytes"])
            out.setdefault(f, {})[int(round(float(r["sample_time"]) * 10))] = str(rx - prev.get(f, 0))
            prev[f] = rx
    return out


def main(res, dst):
    with gzip.open(dst, "wt", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["series", "run", "bw_mbps", "rtt_ms", "buf_bdp", "flow", "unit"] +
                   ["b%d" % k for k in range(1, NBINS + 1)])
        for sid, folders in SERIES:
            for run, folder in enumerate(folders, 1):
                base = os.path.join(res, folder)
                if not os.path.isdir(base):
                    continue
                for cfg in sorted(os.listdir(base)):
                    d = os.path.join(base, cfg)
                    if not cfg.startswith("pedro_v3_") or not complete(d):
                        continue
                    bw, rtt, buf = cfg[len("pedro_v3_bw"):].replace("rtt", "").replace("buf", "").split("_")
                    linux = folder.startswith("out")
                    b = linux_bins(d) if linux else ns3_bins(d)
                    for f in (0, 1):
                        row = b.get(f, {})
                        w.writerow([sid, run, bw, rtt, buf, f, "mbps" if linux else "bytes"] +
                                   [row.get(k, "") for k in range(1, NBINS + 1)])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
