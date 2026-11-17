# TcpBbrV3 against Linux BBRv3

How close the ns-3 BBRv3 port (`src/internet/model/tcp-bbr-v3.{cc,h}`) is to
Google's Linux BBRv3, and how each round of changes moved it.

- [`visualizations/`](visualizations/README.md): figures comparing every stage
  of the port, and the earlier versions, with the Linux kernel, from the
  measured data. Start here. `visualizations/figures/slides/` has 16:9
  versions for presentations.
- [`FIX-LOG.md`](FIX-LOG.md): every change and experiment, with the Linux code
  it follows and before/after numbers, including negative results.

## Result

36 configs (50/100/500 Mb/s x 5/25/100 ms x 0.1/1/2/4 BDP, two flows), 3 ns-3
seeds against 3 Linux kernel runs:

| Stage | Commit | Adds | Distance from Linux | Configs > 3 pp off | Stalls / run |
|---|---|---|---|---|---|
| 1 | in `0bacc13` | the Nov 11 version (per-ACK rate samples, RTT from transmit times, loss hook, Linux RTO) | 1.50 pp | 4.7 | 59 |
| 2 | `0bacc13` | inflight bounds in whole segments; TSO-sized bursts | 1.70 | 4.9 | 51 |
| 3 | `eebec47` | TSO deferral | 1.21 | 3.8 | 34 |
| 4 | `d497c9e` | ACK-aggregation cwnd in STARTUP | **0.90** | **1.3** | **27** |
| 5 | `543cb8d` | inflight at the next departure time; cwnd-limited per window | 1.01 | 1.9 | 35 |
| 6 | `bb1cbbd` | inflight_hi growth floor; pacing credit (current) | 0.97 | 1.8 | 36 |
| | | Linux against itself | 0.69 | 0.7 | 19 |

Stages 1 and 2 were committed together in `0bacc13`; the commits before it
(`618cb94` to `5371dfa`) hold the October versions of the port.

Earlier versions, rebuilt and run on Nov 17 through the same harness (details
in [`visualizations/`](visualizations/README.md#earlier-versions)):

| Version | Distance from Linux | Configs > 3 pp off | Stalls / run |
|---|---|---|---|
| Aug 31, first port | 3.20 pp | 11.3 | 62 |
| Oct 21, probing fixes (`548a189`) | 3.72 | 13.4 | 111 |
| Nov 10, draft-06 defaults + epoch fix (`5371dfa`) | 2.23 | 7.2 | 69 |

With 6 seeds instead of 3, stages 4 and 6 are equally close to Linux (0.90
and 0.92 pp); stage 6 is closer at 0.1 BDP and has more stalls (35.5 vs 26.3
per run).

Distance is the bad-bin share's difference from Linux for the same config
(a bad bin is a 0.1 s bin in which a flow gets under half its median goodput),
averaged over configs. Linux against itself is the noise floor.

Every stage builds and passes all 46 `tcp-*` test suites, and re-running each
stage reproduces the measured runs exactly. This repository as committed builds
with examples and tests enabled and passes all 398 test suites.

## Settings used for the Linux comparison

```
--ns3::TcpBbrV3::DrainGain=0.3466 --ns3::TcpBbrV3::ProbeBwDownGain=0.91
--ns3::TcpBbrV3::DrainMaxRounds=0 --ns3::TcpBbrV3::ExtraAckedRtts=5
--ns3::TcpBbrV3::TsoBursts=1
--ns3::TcpSocketBase::LinuxRto=1 --ns3::TcpSocketBase::MinRto=200ms
--ns3::TcpSocket::ConnTimeout=1s
```

The defaults follow draft-ietf-ccwg-bbr-06 and keep ns-3's existing TCP
behaviour. The comparison harness is `scratch/bbrv3-wallclock-probe.cc`. The
kernel lab scripts that `FIX-LOG.md` mentions (`linux-lab/`) are not in this
repository and are available on request.
