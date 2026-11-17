# TcpBbrV3 fidelity fix log

Every change made to bring the ns-3 BBRv3 port (`src/internet/model/tcp-bbr-v3.{cc,h}`)
and the ns-3 TCP code it relies on closer to Linux BBRv3. Each entry says what was
wrong, which Linux code it now matches, why that is the right reference, and the
measured effect. Negative results (tried, no effect, reverted) are kept too, so a
reviewer can see what was ruled out.

**Reference implementation:** Google's Linux BBRv3, `net/ipv4/tcp_bbr.c` on the
v3 branch, as built into the test VMs' kernel (6.13.7-bbrv3). Where Linux and
draft-ietf-ccwg-bbr-06 disagree, ns-3 defaults to the draft (Pedro's request) and
exposes the Linux value through an attribute.

**Ground truth:** nsperf runs on the Google Cloud VMs over Pedro's grid
(50/100/500 Mbps x 5/25/100 ms x 0.1/1/2/4 BDP, flow 2 joining at 12 s, 45 s per
flow). ns-3 runs the same grid with the same rate, RTT, FIFO size, flow timing
and MSS 1448. Scripts: `linux-lab/` (see its README).

**Measures used throughout:**
- *Bad bins:* share of 0.1 s intervals, while both flows run (15-45 s), in which
  a flow gets less than half its own median throughput. This is the "probing
  drop-off" Pedro reported.
- *Stalls:* 3 or more bad bins in a row (300 ms or longer).
- *Distance from Linux:* the bad-bin share's absolute difference from the Linux
  run of the same configuration, averaged over configurations. The yardstick
  is the kernel's own run-to-run distance (two kernel runs of the whole grid).

---

## October 2026: probing drop-off

Symptom (Pedro): when one flow probed, ns-3 throughput fell far more than in the
Linux captures, and the other flow's bandwidth estimate collapsed over a few
round trips (e.g. 36 -> 12 -> 4 -> 3 Mbps at 100 Mbps / 25 ms / 1 BDP).

1. **Loss over-reported to the congestion control (ns-3 TCP core).**
   The socket reported loss as `|lostOut now - lostOut before|`. When an ACK
   repaired lost segments, `lostOut` fell and the fall was counted as new loss.
   Fix: monotonic `TcpTxBuffer::GetTotalLost()`, the equivalent of Linux's
   `tp->lost`, used for the rate sample's loss. Also affects ns-3's BBRv1.
   Why Linux is right here: BBR's loss signal is defined on cumulative lost
   packets (`rs->lost = tp->lost - rs->prior_lost`), so a repair cannot look
   like a loss.

2. **BBRv1 recovery behavior left in the v3 port.**
   Linux v3 does not do packet conservation or cut cwnd on entering fast
   recovery. Removed both; ported Linux's CA_Loss (RTO) entry and exit and the
   "save cwnd" logic keyed on the previous congestion state
   (`bbr_set_state`, `bbr_save_cwnd`, `bbr_undo_cwnd`).

3. **Wrong "inflight too high" test.**
   Linux compares loss over a whole flight with what was in flight when it was
   sent (`bbr_is_inflight_too_high`: `rs->lost` vs `rs->tx_in_flight`). The port
   compared one ACK's fresh losses with current in-flight. Added per-segment
   tx-time lost and in-flight snapshots to `TcpTxItem`/rate samples and used
   them in the three places Linux does; ported `bbr_note_loss`'s round reset.

4. **The socket overwrote BBR's cwnd on recovery exit (ns-3 TCP core).**
   `TcpSocketBase` always set cwnd to ssthresh on leaving recovery, even for a
   congestion control that sets cwnd itself (`HasCongControl`). Now skipped in
   that case, matching the existing CWR exit path. Linux leaves cwnd to the
   module's `cong_control`. Also affects BBRv1.

5. **PROBE_RTT timing and other state machine details.**
   PROBE_RTT on the 5 s min-RTT-probe window (`bbr_probe_rtt_win_ms`), full-bw
   check on every ACK, Linux cwnd quantization, STARTUP gains pacing 2.77 /
   cwnd 2.0, DRAIN 1/2.885 with cwnd 2.0 (the port still had v1's 2.89).

Effect (33 configs, October, local runs): bad bins 5.0% -> 2.5% (Linux 2.5%);
stalls of 300 ms or more 40 -> 4; long-flow share error vs Linux 0.175 -> 0.029.

## November 3 2026: draft-ietf-ccwg-bbr-06 defaults

At Pedro's request the defaults follow the June 2026 draft where it differs from
Linux. All four are attributes so Linux-faithful runs can set them back:

| Attribute | draft-06 (default) | Linux |
|---|---|---|
| DrainGain | 0.5 | 1/2.885 = 0.3466 |
| ProbeBwDownGain | 0.90 | 0.91 |
| DrainMaxRounds | 3 | none (0 disables) |
| ExtraAckedRtts | 10 | 5 |

All other gains and thresholds are the same in the draft and Linux.
Effect over the 36 BBRv3 configs (Nov 10 runs): loss 0.39% (draft) vs 0.40%
(Linux values); distance from Linux in bad bins 2.4 pp vs 1.5 pp, as expected
since the kernel uses the other values.

## November 4 2026: ACK-aggregation epoch truncation

Symptom: ns-3 BBRv3 dropped about 1% of packets with a 2 BDP buffer
(100 Mbps / 100 ms: 4227 drops) where Linux drops none.

Cause: `UpdateAckAggregation` stored the time since the aggregation epoch began
as a `uint32_t` number of whole seconds. For the first second of every epoch the
expected ACKed amount was 0, so every ACK counted as "extra", `extra_acked`
stayed at its cap of one BDP, and cwnd was about 3 BDP instead of about 2.

Fix: compute the epoch in seconds as a double and the expected amount from the
bandwidth estimate, as Linux does in `bbr_update_ack_aggregation`
(`epoch_us = tcp_stamp_us_delta(...)`, `expected_acked = bw * epoch_us`).
Also ported Linux's STARTUP rule: until full bandwidth is reached the
`extra_acked` window is one round, not `ExtraAckedRtts`.

The same truncation exists in upstream ns-3 `TcpBbr` (BBRv1,
`src/internet/model/tcp-bbr.cc`); fixed there too, without the STARTUP rule
(Linux v1 has none). That file is upstream ns-3 and can be submitted separately.

Effect: 2 BDP drops 4227 -> 0 (Linux 0). Mean loss at 2 BDP over the grid
0.07% (Linux 0.08%).

## November 4 2026: tried and reverted (no effect)

Three shortcuts in `TcpTxBuffer` to cut the per-ACK cost of SACK recovery
(start `Update` at the highest-SACK hint, stop `NextSeg` early past the highest
SACK, skip `MarkRetransmittedSegmentsLost` when nothing is retransmitted). Output
was byte-identical but wall time did not improve, because SACK blocks usually
start below the highest SACKed segment. Reverted; a real speedup needs an
indexed scoreboard.

## Harness notes (not port changes)

- ns-3 socket buffers are sized at 4 BDP + 2 bottleneck buffers (at least
  512 KB). A 64 MB buffer let a flow keep sending queued data for about 11 s
  after its application stopped, unlike netperf flows.
- `MinRto` 200 ms (Linux's minimum); ns-3's default 1 s turns a fully lost
  PROBE_RTT window into a 1 s stall.

## November 11 2026: per-ACK rate samples (ns-3 TCP core)

Symptom: a lone ns-3 BBRv3 flow estimated more bandwidth than the link has, by
about two packets per round trip: +8% at 50 Mbps / 5 ms, +4% at 100 Mbps / 5 ms,
+2% at 50 Mbps / 25 ms, +0.2% at 500 Mbps / 25 ms. The Linux flow on the same
path estimated 47.9 Mbps against a 48.2 Mbps goodput ceiling (kernel trace, see
the tooling section below). Pacing at 1.0 x an 8% high estimate keeps a standing
queue, so PROBE_BW DOWN never drains it and a flow joining later measures an
inflated min_rtt from its handshake.

Cause: `TcpRateLinux` kept one `TcpRateSample` for the whole connection.
`SkbDelivered()` only replaces the sample's "prior" packet (`m_priorDelivered`,
`m_priorTime`, the send and ACK elapsed times, `m_isAppLimited`, and the tx-time
`m_priorLost`/`m_txInFlight` added in October) when an ACK delivers a packet with
a strictly larger delivered snapshot. With pacing and delayed ACKs, consecutive
ACKs often deliver packets sent between the same two ACK arrivals, so they share
a snapshot; the sample then kept the previous ACK's prior time and elapsed
times while `delivered` was recomputed against the new total. The interval came
out one ACK short and the sample two packets long. In the ns-3 log, consecutive
samples had the same 7.69 ms interval while delivered grew 32 -> 34 packets.

Linux: `tcp_ack()` starts every ACK with a zeroed `struct rate_sample`, and
`tcp_rate_skb_delivered()` picks the most recently *sent* packet the ACK delivers
(`tcp_skb_sent_after()`: later tx timestamp, ties broken by end sequence).

Fix (`tcp-rate-ops.{h,cc}`, `tcp-socket-base.cc`, `tcp-tx-item.{h,cc}`):
- `TcpRateOps::StartSample()`, called by `TcpSocketBase::ReceivedAck()` before
  the ACK's SACK blocks and cumulative ACK are processed, clears the per-ACK
  fields (Linux's fresh `rs`). An ACK that delivers nothing now gives an invalid
  sample, as `tcp_rate_gen()` does when `rs->prior_mstamp` is 0.
- `SkbDelivered()` chooses the prior packet by send time and end sequence, as
  `tcp_skb_sent_after()` does (`TcpTxItem::GetSeqEnd()` added for the tie-break).
- `m_deliveryRate` is now `delivered / interval` of every valid sample, which is
  what Linux BBR computes (`bbr_calculate_bw_sample()`, v1 `bbr_update_bw()`).
  Before, it was only refreshed when the sample was not app-limited or set a new
  app-limited record (the `tp->rate_*` bookkeeping Linux keeps for `tcp_info`),
  so other samples carried a stale rate into BBR. Invalid samples report 0.
- `tcp-rate-ops-test.cc` asserted the old behaviour (a dup ACK must repeat the
  previous sample); it now asserts the Linux behaviour (the sample is invalid).

Also affects ns-3 BBRv1 (`TcpBbr`), which consumes the same samples.

Effect: lone 50 Mbps / 5 ms flow bw estimate 52.2 -> 48.20 Mbps (ceiling 48.20).
Grid effect: measured together with the other Nov 11 changes, see the Nov 11 status table.

## November 11 2026: PROBE_BW uses in-flight before the ACK

`UpdateCyclePhase()` used the bytes in flight after the ACK for the UP exit
(`inflight >= inflight_hi`) and DOWN -> CRUISE tests. Linux's
`bbr_update_cycle_phase()` uses `bbr_packets_in_net_at_edt(sk, rs->prior_in_flight)`:
in flight before the ACK, less what is delivered before the next paced departure.
Now `rs.m_priorInFlight`; the EDT term is omitted because ns-3 paces one segment
at a time and does not expose the next departure time (it is at most about one
segment). Grid effect: measured together, see the Nov 11 status table.

## November 11 2026: RTT samples from transmit times (ns-3 TCP core)

Symptom: ns-3 min_rtt values were whole milliseconds (5000 us on a 5.25 ms
path; Linux reports 5048 us), and a flow joining a queued 50 Mbps / 5 ms link
kept min_rtt = 8.0 ms for seconds while the Linux flow had 6.0 ms, falling to
5.5 ms. With a BDP estimate 50% too large, the ns-3 late flow held cwnd 65-86
packets against Linux's 34-39 and starved the first flow (7 s at 14-19 Mbps of
50, where Linux shared 25/25 by 15.5 s). ns-3 flows also entered PROBE_RTT every
5 s on every config (6 per flow in 30 s), where Linux skipped about half of them
in small-buffer configs (3 per flow at 100 Mbps / 100 ms / 0.1 BDP), because a
lower RTT sample refreshes the PROBE_RTT window (`bbr_update_min_rtt()`).

Cause: with the TCP timestamp option on (the ns-3 default, and Linux's),
`TcpSocketBase::CalculateRttSample()` took every RTT sample from TS-ECR. ns-3's
timestamp clock is in milliseconds (`TcpOptionTS::NowToTsValue()`), and with
delayed ACKs the echoed value belongs to the older of the two ACKed segments,
so samples were quantized and biased high.

Linux: `tcp_ack_update_rtt()` ("Prefer RTT measured from ACK's timing to
TS-ECR") uses the skb transmit timestamps (us), and TS-ECR only when no clean
sample exists (retransmitted segments, Karn's rule). BBR gets the RTT of the
most recently sent (S)ACKed packet (`rs->rtt_us = ca_rtt_us`).

Fix: `CalculateRttSample()` uses `now - transmit time` for segments that were
not retransmitted, and TS-ECR only for retransmitted ones. Applies to every ns-3
TCP variant (RTO estimation now has transmit-time precision as well).
Grid effect: measured together, see the Nov 11 status table.

## November 11 2026: STARTUP pacing initialization

`InitPacingRate()` used `tcb->m_minRtt` truncated to whole milliseconds and
seeded both bw filter slots with the nominal rate (initial cwnd / RTT). Linux
(`bbr_init()`, `bbr_init_pacing_rate_from_rtt()`) paces at
`startup_pacing_gain * cwnd / srtt` (us), applies the pacing margin, and starts
the filter empty (`bw_hi[0] = bw_hi[1] = 0`), so the first real samples, not a
seed, set the bw estimate. Now the same; the re-initialization on the first RTT
sample keys on srtt, as `bbr_set_pacing_rate()` does.

## November 11 2026: TSO/GSO-sized send bursts (port + ns-3 TCP core)

Symptom: Linux's lone first flow lost packets in STARTUP and in every PROBE_UP
(bottleneck drops at 1.5, 3.6, 5.6, 8.7, 10.7 s at 500 Mbps / 25 ms / 1 BDP), so
it carried an inflight_hi (2122 packets) and cruised at cwnd 1808 with an empty
queue when the second flow joined. The ns-3 flow never lost a packet, had no
inflight_hi, cruised at cwnd 2084, and its PROBE_UP built a 23 ms queue at the
join; the joining flow's RTT doubled, its STARTUP stalled at cwnd 19-27, and it
left STARTUP at 6.6 Mbps where Linux's left at 91 Mbps. Across the traced
configs the ns-3 late flow left STARTUP 20-60% lower than Linux's.

Cause: Linux does not send one packet per pacing interval. BBR sizes each
TSO/GSO skb with its `tso_segs` hook (`bbr_tso_segs()`), about 1 ms of data at
the pacing rate, and the stack paces skb by skb. nsperf turns segmentation
offload off only on the router interfaces, so these bursts reach the bottleneck
back to back. The kernel capture confirms it: at 500 Mbps most data skbs carry
33-44 segments, at 100 Mbps 5-7. ns-3's TCP paced one segment at a time.

Fix:
- `TcpSocketState::m_sendQuantum`: bytes a congestion control lets the socket
  send back to back per pacing interval; `TcpSocketBase::SendPendingData()`
  sends up to that much and then waits the whole burst's transmission time at
  the pacing rate, as Linux paces a TSO skb by its length. The default (0) is
  the old one-segment behaviour, so no other congestion control changes.
- `TcpBbrV3::SetSendQuantum()` is now Linux's `bbr_tso_segs_generic()`:
  `pacing_rate >> 10` bytes, plus 64 KB halved per 512 us of min_rtt, capped at
  64 KB less `MAX_TCP_HEADER`, at least 2 segments. `InFlight()`'s floor of
  three bursts then matches `bbr_quantization_budget()`. Attribute `TsoBursts`
  turns it on; it is **off by default** (see the ablation below).
- `ProbeRttCwnd()` now uses the BDP without that floor, as `bbr_probe_rtt_cwnd()`
  does (with a one-segment quantum the floor never mattered there).
Ablation (35 configs, 500/100/1 left out while it is rerun, see below; distance
is every ns-3 seed against every kernel run; kernel vs kernel 0.65 pp, 12-19
stalls per run):

| Variant | Distance (pp) | Stalls per run | ns-3 seed vs seed (pp) |
|---|---|---|---|
| Fixes above, RFC 6298 RTO, one-segment pacing | 1.29 | 44 | 1.19 |
| + Linux RTO, one-segment pacing | **1.06** | 43 | **0.70** |
| + TSO bursts, RFC 6298 RTO | 1.42 | 38 | 1.66 |
| + both | 1.65 | 39 | 1.44 |

The bursts reproduce Linux's per-probe losses and cut stalls, but they move
the grid further from the kernel and roughly double ns-3's run-to-run spread.
So the burst model is not yet faithful as a whole: what reaches the kernel's
bottleneck is evidently smoother than back-to-back skbs (three forwarding hops
and qdiscs lie between the sender and netem), and that part is not modeled.
`TsoBursts` stays available, off by default, and the comparison runs use the
default.

## November 11 2026: Linux retransmission timer (ns-3 TCP core, opt-in)

Symptom: at 100 Mbps / 100 ms / 1 BDP both flows entered fast recovery when the
second flow's STARTUP overflowed the queue. Linux's second flow recovered in
about 600 ms; ns-3's took an RTO 207 ms after its fast retransmit, while the
retransmission was still waiting behind about 100 ms of queue, and BBR's RTO
response (cwnd to 1, inflight_lo from cwnd) left it at a third of Linux's cwnd
for seconds. This produced most of the excess stalls in the 1 BDP, 100 ms configs.

Cause: ns-3 computes the RTO per RFC 6298, `max(SRTT + 4 RTTVAR, MinRto)`, about
200-250 ms here, and restarts the timer only on new cumulative ACKs. Linux keeps
`rttvar_us >= tcp_rto_min` (`tcp_rtt_estimator()`) and sets `rto = srtt +
rttvar` (`__tcp_set_rto()`), i.e. at least SRTT + 200 ms, and restarts the timer
when it retransmits the head of the queue (`tcp_xmit_retransmit_queue()`).

Fix: attribute `TcpSocketBase::LinuxRto` (default false, so ns-3 keeps RFC 6298
unless asked): RTO = SRTT + max(4 RTTVAR, MinRto), and the timer restarts when
the first unacknowledged segment is retransmitted. The comparison runs set it,
as they already set Linux's 200 ms MinRto (runner `LINUXRTO=1`). Linux's
peak-tracking of the deviation over the last RTT (`mdev_max_us`) is not
modeled. Effect at the join above: no RTO, cwnd after recovery 184 (was 110,
Linux 267). Grid effect: see the ablation table above (1.29 -> 1.06 pp on 35 configs).

## November 11 2026: per-segment loss hook (port + ns-3 TCP core)

Symptom: in the 1 BDP, 100 ms configs most of ns-3's extra stalls were 0.3-1.4 s
stretches of zero goodput for both flows: fast recovery that would not end,
while the first flow stayed in PROBE_UP for 0.8 s or more with the queue full
(100/100/1, seed 2, 30.12-30.94 s). Linux leaves PROBE_UP almost as soon as a
probe loses packets. Late-flow STARTUP was not the cause: both ramps match
within about 10%, and either STARTUP exit leaves cwnd near 260-270 by 14 s.

Cause: Linux calls the congestion control for every segment the moment it is
marked lost (`tcp_notify_skb_loss_event()` -> `.skb_marked_lost` =
`bbr_skb_marked_lost()`). While probing, BBR checks the loss rate over that
segment's own flight (`tx.in_flight`, `tp->lost - tx.lost`), sets inflight_hi
at the point within the flight where loss crossed 2%
(`bbr_inflight_hi_from_lost_skb()`), and starts PROBE_DOWN. ns-3 had no such
hook; the port only checked per ACK, against the flight of the packet the ACK
delivered, which during recovery was sent before the losses, so the check
rarely fired and PROBE_UP kept overfilling the queue.

Fix:
- `TcpTxBuffer::SetSkbLostCallback()`: called for every segment as it is
  marked lost, after the cumulative lost count includes it. A retransmission
  deemed lost again now counts as a new loss, as in Linux's
  `tcp_mark_skb_lost()` (`TCPLostRetransmit`).
- `TcpCongestionOps::SkbMarkedLost()` (default: nothing), called from the socket.
- `TcpBbrV3::SkbMarkedLost()` = `bbr_skb_marked_lost()` with `bbr_note_loss()`,
  and `InflightHiFromLostSkb()` = `bbr_inflight_hi_from_lost_skb()`, in bytes.
  The per-ACK emulation of `bbr_note_loss()` in `CongControl()` is removed.
  Difference: Linux reads `tp->delivered` at the hook; ns-3 uses the value as
  of the previous ACK.
Grid effect (all 36 configs, 3 seeds against 3 kernel runs): no measurable
change. Distance 1.48 -> 1.50 pp, stalls per run 60 -> 59, bad bins 3.31% ->
3.37%, run-to-run spread 1.16 -> 1.17 pp. Kept because it is how Linux reacts
to loss while probing: in the traced 100/100/1 run the hook now cuts
inflight_hi and ends PROBE_UP at the first loss, as Linux does, and recovery
episodes match Linux in number and length (24-27 vs 20, about 0.45 s each). The
remaining long zero-goodput stretches there come from a lost retransmission of
the head-of-line segment, which takes about one RTT to detect in both stacks
(about 1% of retransmissions are lost in each).

## November 11 2026: harness matches nsperf's netem queue limit (not a port change)

nsperf's bottleneck is netem with `limit = one-way BDP (1500 B packets) + buf`
(`nsperf.py netem_limit()`, "needs to hold the packets in emulated pipe and
emulated buffer"). netem counts every packet until it leaves the delay line, so
when the link has not been fully busy for the last one-way delay it accepts more
than `buf` queued packets. The ns-3 harness had a plain FIFO of `buf` packets.
`--netemLimit/--netemDelay` (runner: `NETEM=1`) make the ns-3 queue count packets
from enqueue until one netem delay after they finish transmission, with the same
limit. Effect at 100 Mbps / 100 ms / 0.1 BDP: bad bins 12.2% -> 4.3% (Linux 6.3%,
5.2% in the Nov 3 kernel run). Over the grid, one run: stalls 54 -> 47, distance
1.52 -> 1.74 pp, i.e. within ns-3's own run-to-run swing (50/5/4, which netem
barely changes, went 4.8% -> 11.2%), so from here every ns-3 variant is run with
three RngRun seeds.

## November 11 2026: comparison measures goodput the same way on both sides (not a port change)

The Linux goodput per 0.1 s bin (`pcap2csv.py`) is cumulative-ACK progress in the
sender's capture. The ns-3 harness reported FlowMonitor `rxBytes`, which counts
every packet reaching the receiver, including out-of-order packets past a loss.
A loss recovery therefore shows up in Linux as a 100-300 ms goodput stall (the
cumulative ACK stops until the hole is filled) but not in ns-3. That hid exactly
the dips the bad-bin measure is about: at 100 Mbps / 100 ms / 0.1 BDP, ns-3 and
Linux had the same number of recovery episodes (16 and 17 per flow in 15-45 s,
from the kernel trace and `GetBbrInfo()`), yet ns-3 showed 0.3% bad bins against
Linux's 5-6%, nearly all of Linux's dips falling in `TCP_CA_Recovery`.

The harness now also writes `acked_bytes` (the sender's snd_una - ISN - 1) and
`verify_data.py` uses it when present, so both sides measure in-order goodput
at the sender. All results before this point, including the Nov 10 dashboard,
used `rxBytes` on the ns-3 side; the Nov 10 code was rerun with the new measure
as the baseline below.

The harness also skipped the sample at a flow's stop time, so the first flow's
last bin in the 15-45 s window (44.9-45.0 s) was always empty: one guaranteed
bad bin per ns-3 run. Linux's first flow runs to about 46.1 s. Fixed; on old
runs, dropping that bin on both sides changes the figures by under 0.2 pp.

## Tooling added November 11 2026

- Linux: `vm/bbrtrace.bt`, a bpftrace kprobe on `bbr_main()` that logs each
  sender's `struct bbr` (mode, cycle_idx, gains, min_rtt, bw_hi/bw_lo,
  inflight_hi/lo, extra_acked) and the ACK's rate sample, at most every 1 ms per
  flow and at every mode/phase change (`run_matrix.sh`, `BBRTRACE=1`). The
  6.13.7-bbrv3 kernel is built without `CONFIG_INET_DIAG`, so `ss` cannot show
  `tcp_bbr_info`; offsets come from `struct bbr` in google/bbr v3 `tcp_bbr.c`
  and were checked against known values (min_rtt 5048 us at 5 ms, gains 256 /
  232 / 512 = 1.0 / 0.91 / 2.0).
- ns-3: `TcpBbrV3::GetBbrInfo()` returns the fields of Linux's `tcp_bbr_info`
  (`bbr_get_info()`); the harness writes them every 5 ms (`--bbrInfoInterval`)
  in the same columns and units as the Linux trace, so the two can be compared
  state by state.
- Runner: `RUN=n` sets ns-3's RngRun (random probe timing), the counterpart of
  repeating a kernel run.

## November 11 2026: diagnosis of the remaining 1 BDP / 100 ms stalls (no code change)

Question: are the extra stalls in the 1 BDP, 100 ms configs caused by the test
setup rather than by the port? Measured on 8 focus configs, 3 ns-3 seeds
against 3 kernel runs. Tools: harness `--dropLog` (every bottleneck drop,
marked if it was a retransmission) and per-flow `bottleneck_drops`,
`retx_drops` and a 15-45 s ACK-size histogram in log.txt;
`linux-lab/vm/ack_sizes.py` and `linux-lab/vm/loss_episodes.py` for the kernel pcaps.

Same in both stacks (ruled out):
- Configs: identical (rate, delay, buffer, netem limit, start times), and so is
  the effective buffer: smoothed RTT tops out at 199-202 ms in both at 1 BDP /
  100 ms. (RTTs up to 320 ms in the kernel pcap come from timestamp echoes
  during recovery.) So GSO skbs counting as one packet in netem's limit do
  not enlarge the kernel's buffer.
- Bottleneck drops over all 36 configs: ns-3 0.91x Linux; 0.96x at 100/100/1.
- Loss episodes: similar length (55-400 ms), and the first retransmission
  leaves about 201 ms after the first drop in both.
- Share of an episode's drops taken by the flow in PROBE_UP: 0.38 in both
  (100/100/1), so it is not the deterministic drop-tail phase effect.
- ACK sizes: Linux receivers also ACK every 2 segments (no GRO stretch
  ACKs), but send 15-25% single-segment ACKs where ns-3 sends about 0.1%.
- Recovery cwnd: ns-3 does not apply PRR under BBR (`HasCongControl()`), the
  same as Linux's `tcp_cong_control()`.

Different:
- Lost retransmissions: ns-3 79/58 per run at 100/100/1 and 40 at 50/100/1;
  Linux `TCPLostRetransmit` 9-25 and 7-24. During 15-45 s Linux loses almost
  none and ns-3 about 30. Each lost head-of-line retransmission costs a
  further RTT of zero goodput, i.e. a stall.
- In some ns-3 episodes the probing flow stays in PROBE_UP for about 150-200 ms
  after its own losses start (100/100/1 seed 1, 41.05 s: recovery at 41.30 s,
  PROBE_DOWN at 41.47 s). The port's `SkbMarkedLost()` runs and matches
  `bbr_skb_marked_lost()`; the loss rate over the flight just takes until
  then to exceed 2%. Linux's probing flow left PROBE_UP within 50 ms of
  recovery in the traced run.

Experiment: host timing noise (harness `--hostJitter`: every sender and
receiver holds each packet for a uniform random 0..J, without reordering),
to test whether ns-3's perfectly regular timing causes the difference.

| Variant (8 focus configs, 3 seeds) | Bad bins | Stalls per grid | Distance from Linux |
|---|---|---|---|
| Linux | 5.15% | 9.7 | 1.40 pp (kernel vs kernel) |
| ns-3, no jitter (reproduces the hook grid exactly) | 7.24% | 34.0 | 2.56 pp |
| ns-3, 20 us jitter | 8.18% | 39.0 | 3.35 pp |
| ns-3, 100 us jitter | 7.56% | 36.0 | 2.61 pp |

Jitter lowers bad bins in the 1 BDP / 100 ms configs (100/100/1 13.4% ->
10.9%, 500/100/1 16.3% -> 11.6%; Linux 7.9% and 9.3%), but stalls and lost
retransmissions do not fall, and the 0.1 BDP configs get worse. Result:
negative. Host timing noise does not explain the gap, and the option stays
off (harness only, not part of the port).

## November 11 2026: why ns-3 loses more retransmissions (ns-3 TCP core, TsoBursts only)

Question: at 1 BDP / 100 ms ns-3 loses 3-5x more retransmissions than Linux
although total drops match. Measured with per-10 ms send counts in both
stacks: ns-3 harness `run.csv.sends.csv` (new, retx and head-of-line retx sent,
from the sender's MacTx) joined with the drop log; kernel `vm/retx_bins.py`
from the sender pcap (a segment sent n times had its first n - 1 sends lost).

Finding: in the same 10 ms bins in which a flow loses 11-13% of its new
segments, Linux loses only 2.5-3% of its retransmissions (0-0.4% in 15-45 s)
and no head-of-line retransmission at all (0 of 61); ns-3 loses retransmissions
at the same ~10% as new data, 3-4 of them head-of-line per run.

Cause: where Linux's drops fall. The kernel pcap shows drops at the tail of
TSO/GSO bursts: over 100/100/1, 50/100/1, 500/25/1 and 500/100/0.1, the first
segment of a burst of 3 or more is lost 0.02-0.07% of the time, the last one
0.85-5.1% (20-70x). A burst arrives at netem faster than the link drains, so
its head takes the slots freed since the previous burst and its tail
overflows. A retransmission goes out as its own small skb in a gap and nearly
always fits. With one-segment pacing every ns-3 packet has the same drop
chance, so retransmissions get no such protection. This also settles the
question left open in the TSO bursts entry above: the bursts do reach netem
back to back.

Test (100/100/1 and 50/100/1, 3 seeds; Linux 7.9% / 7.8% bad bins, 2.3 stalls each):

| Variant | 100/100/1 bad / stalls / lost retx / lost HOL retx | 50/100/1 bad / stalls / lost retx / lost HOL retx |
|---|---|---|
| One-segment pacing (default) | 13.4% / 11.0 / 57.7 / 3.7 | 11.0% / 7.0 / 40.3 / 0.7 |
| `TsoBursts` | 9.2% / 7.0 / 38.7 / 0.7 | 9.6% / 8.0 / 24.3 / 0.3 |
| `TsoBursts` + skb boundaries (below) | 11.9% / 7.7 / 43.0 / 0.7 | 8.8% / 7.0 / 17.3 / 0.7 |

Change (ns-3 TCP core, only active with a send quantum, i.e. `TsoBursts`):
`TcpSocketBase::SendPendingData()` ends a paced burst where Linux's skb would
end: retransmitted and new data never share a burst, and a retransmission
burst covers one contiguous range. Linux sends each rtx-queue skb on its own
(`tcp_xmit_retransmit_queue()`, trimmed to the lost range by
`__tcp_retransmit_skb()`) before `tcp_write_xmit()` sends new data. Effect is
within seed noise; kept because it is the Linux behaviour. Default
one-segment pacing is unchanged. All 46 tcp-* suites pass.

Conclusion: the lost head-of-line retransmissions are a burst effect, and
`TsoBursts` removes most of them (3.7 -> 0.7 per run at 100/100/1). Stalls
there remain 7 against Linux's 2.3, so the rest of the gap is not lost
retransmissions; see the PROBE_UP exit timing (next). `TsoBursts` stays off
by default until a grid run shows it helps overall.

## November 11 2026: inflight_hi in whole segments (port)

Question: why does ns-3's probing flow sometimes stay in PROBE_UP long after
its losses start? Traced both stacks with the bw-probe flags (kernel:
`vm/bbrtrace.bt` now logs the struct bbr word at offset 124 as `v3_flags`;
ns-3: `BbrInfo` gained `bwProbeSamples`, `prevProbeTooHigh`,
`stoppedRiskyProbe` and `ackPhase`, written in the same bit layout).

Finding: the delay from loss detection to leaving PROBE_UP is the same in
both (median 23 ms ns-3, 39 ms Linux; both have a long tail). What differs is
the probing pattern. Per run at 100/100/1 (15-45 s), Linux has 18.5 PROBE_UPs,
10.5 of them stopped by the "risky probe" check (`prev_probe_too_high &&
inflight >= inflight_hi`, `bbr_update_cycle_phase()`) and 6.5 quick re-probes
(`bbr_adapt_upper_bounds()`: a risky stop without loss refills one round
later). ns-3 had 12.0, 2.3 and 1.0, with longer probes, even though it starts
most probes with `prev_probe_too_high` set.

Cause: Linux keeps inflight_hi in packets, rounding down
(`bbr_handle_inflight_too_high()`: `target * (BBR_UNIT - beta) >> BBR_SCALE`;
`bbr_inflight_hi_from_lost_skb()`: integer division; STARTUP's BDP), so cwnd
capped at inflight_hi is a whole number of packets that inflight reaches.
The port kept these values in bytes (e.g. 1879445 B = 1297.96 segments). Cwnd
was capped at that fraction, ns-3 sends whole segments, inflight stopped one
segment short, and `inflight >= inflight_hi` was almost never true, so the
risky-probe exit and the re-probe after it rarely happened.

Fix: `TcpBbrV3::FloorSegments()` rounds inflight_hi (STARTUP exit on loss,
`HandleInflightTooHigh()`, the upward reset in `AdaptUpperBounds()`),
inflight_lo (`LossLowerBounds()`) and the headroom
(`InflightWithHeadroom()`, now at least one segment like Linux's at least
one packet) down to whole segments. `m_segmentSize` is taken from the socket
every ACK. All 46 tcp-* suites pass.

Effect (3 seeds; Linux in brackets):

| Per run | 100/100/1 before | 100/100/1 after | 50/100/1 before | 50/100/1 after |
|---|---|---|---|---|
| PROBE_UPs | 12.0 | 16.3 (18.5) | 11.3 | 13.0 (17.5) |
| Risky-probe stops | 2.3 | 8.0 (10.5) | 2.0 | 6.3 (9.5) |
| Quick re-probes | 1.0 | 5.0 (6.5) | 0.7 | 2.7 (5.0) |
| Bad bins | 13.4% | 13.0% (7.4%) | 11.0% | 13.1% (7.8%) |
| Stalls | 11.0 | 8.0 (1.5) | 7.0 | 9.0 (4.0) |

The probing pattern now follows Linux's; the outcome on these two configs
alone is mixed.

Focus grid (8 configs, 3 seeds each, every seed against the 3 kernel runs;
`TsoBursts` rows include the skb-boundary change above; Linux kernel vs kernel
1.40 pp, 5.15% bad bins, 9.7 stalls per grid):

| Variant | Bad bins | Stalls per grid | Distance (pp) | Per seed (pp) | Seed vs seed (pp) |
|---|---|---|---|---|---|
| Before (one-segment pacing, bytes) | 7.24% | 34.0 | 2.56 | 2.42-2.65 | 1.17 |
| Whole segments | 7.67% | 32.0 | 2.82 | 1.64-3.94 | 1.93 |
| `TsoBursts` only (control) | 7.31% | 31.7 | 2.78 | 2.19-3.26 | 1.88 |
| Whole segments + `TsoBursts` | 6.90% | 28.0 | 2.16 | 1.97-2.44 | 1.99 |

Per config, whole segments + `TsoBursts` is closer at 50/100/1 (9.6% vs Linux
7.8%, was 11.0%) and 500/100/1 (12.3% vs 9.3%, was 16.3%; lost
retransmissions 135 vs Linux 194, was 519), but further at the 0.1 BDP
configs (100/100/0.1: 8.8% vs 5.6%, was 6.1%; lost retransmissions 323 vs
Linux 120), where back-to-back bursts overflow a queue of a few packets.
Whole segments alone is within ns-3's seed noise (one seed 1.64 pp, another
3.94 pp).

Control (`TsoBursts` with the byte-valued bounds, built on the VMs with
`FloorSegments()` returning its input, then restored): 2.78 pp, 31.7 stalls,
no better than before. So neither change helps on its own and the pair does:
with bursts, whole-segment bounds let inflight actually reach inflight_hi, so
the risky-probe exit fires. The pair also rescues most of the burst damage
at 100/100/0.1 (8.8% vs 13.5% with `TsoBursts` only; Linux 5.6%). Every seed
of the pair beats the before mean on stalls (27-29 vs 34), but 3 seeds on 8
configs is a small sample, and both burst variants double ns-3's seed spread.

Decision: keep whole segments (it is what Linux does, and costs nothing on
its own). `TsoBursts` stays off by default until a 36-config grid confirms
the pair; the 0.1 BDP configs remain its weak spot (lost retransmissions
323 vs Linux 120 at 100/100/0.1).

Full grid (36 configs, 3 seeds, `fidelity_data.py` basis; results
`ns3out_pair_s1..3`):

| Measure | Linux (kernel vs kernel) | Now (bytes, one-segment pacing) | Whole segments + `TsoBursts` |
|---|---|---|---|
| Bad bins, mean | 2.38% | 3.37% | 3.58% |
| Distance from Linux (pp) | 0.69 | **1.50** (0.94-2.10) | 1.70 (1.40-1.94) |
| Configs more than 3 pp off, per run pair | 1 | 4.7 | 4.9 |
| Stalls per run | 18.7 | 59 | **51** |
| Run-to-run spread (pp) | 0.69 | 1.17 | 1.67 |

By buffer and RTT (distance pp now -> pair; stalls now -> pair, Linux):

| Group | Distance | Stalls |
|---|---|---|
| 0.1 BDP | 0.89 -> 2.12 | 8.0 -> 11.7 (4.3) |
| 1 BDP | 2.65 -> 1.85 | 35.7 -> 24.3 (8.0) |
| 2 BDP | 1.33 -> 1.55 | 7.7 -> 8.0 (2.7) |
| 4 BDP | 1.13 -> 1.27 | 7.7 -> 7.0 (3.7) |
| 5 ms | 0.94 -> 0.72 | 5.3 -> 0.3 (1.7) |
| 25 ms | 1.22 -> 2.03 | 7.3 -> 10.3 (1.3) |
| 100 ms | 2.33 -> 2.35 | 46.3 -> 40.3 (15.7) |

The focus-grid gain does not hold overall. The pair is much closer at
1 BDP (500/100/1 6.94 -> 3.67 pp, 100/5/1 3.00 -> 0.63) but much further at
0.1 BDP and at 500 Mb/s with 25 ms (500/25/0.1: Linux 1.1%, now 1.7%, pair
7.8%; 500/25/2 and /4 also worse). These are the configs where ns-3's burst
(up to about 64 KB, 44 segments at 500 Mb/s) is large next to the free queue
space, so the burst model, not the whole-segment bounds, is the suspect.
Decision unchanged: whole segments stay, `TsoBursts` stays off.

## November 11 2026: burst sizes, Linux vs ns-3 `TsoBursts` (measurement)

Question: are ns-3's bursts larger than Linux's where `TsoBursts` hurts?
Measured the size of each back-to-back send train per flow, 15-45 s, weighted
by segments (the burst a typical segment rides in). Linux: `vm/burst_sizes.py`
on the sender-side pcap (srv.r, before srt.r segments the GSO skb; fq sends
each skb on its own, so trains equal skbs). ns-3: harness `SenderMacTx`, gap
under 30 us on the 10 Gb/s access link, printed as `trains_15_45s` in log.txt.
Kernel grid `out_fresh`; ns-3 seed 1, `TsoBursts` on (`ns3out_burst_s1`).

| Config | Linux flow 0 / 1, weighted mean (p50) | ns-3 flow 0 / 1, weighted mean (p50) |
|---|---|---|
| 500/25/0.1 | 24.1 (26) / 12.1 (12) | 8.3 (2) / 6.6 (4) |
| 500/25/2 | 27.8 (29) / 12.1 (14) | 9.3 (2) / 6.7 (4) |
| 500/100/1 | 30.0 (31) / 11.1 (11) | 17.9 (22) / 8.0 (6) |
| 100/100/0.1 | 5.2 (5) / 2.5 (2) | 3.2 (2) / 2.1 (2) |
| 100/100/1 | 5.4 (5) / 2.6 (2) | 4.3 (5) / 2.0 (2) |
| 100/5/1 | 3.9 (4) / 3.6 (4) | 3.4 (3) / 3.5 (4) |
| 50/100/1 | 2.5 (2) / 2.0 (2) | 2.6 (3) / 2.0 (2) |

The hypothesis is wrong: ns-3's bursts are not larger, they are smaller and
split in two. Linux sends almost nothing in skbs of 1-4 segments at 500 Mb/s
(0-2% of segments); ns-3 sends about half its segments in 2-segment trains
(one per stretch ACK) and the rest in full quanta, and two quanta sometimes
run together (max 66 segments). Linux skb sizes follow
`bbr_tso_segs_generic()` for each flow's own pacing rate, so the quantum
formula is right.

Likely cause: Linux's `tcp_tso_should_defer()` (tcp_output.c, called from
`tcp_write_xmit()` for multi-segment skbs). When cwnd lets only part of a
full TSO skb out, it waits for more ACKs instead of sending a small skb,
unless the free window is at least a third of cwnd (`tcp_tso_win_divisor`)
or the oldest unacked skb is younger than srtt/2. ns-3 has no equivalent, so
when cwnd-limited it sends whatever each ACK frees. Not yet confirmed in
ns-3; next step.

Confirmed in ns-3 (temporary counters in `SendPendingData()`, VM build only,
removed): share of segments, 15-45 s, by why the burst ended.

| Config | Full quantum | Cut short by cwnd | Cut short in recovery |
|---|---|---|---|
| 500/25/0.1 | 24% | 73% (2.6 segments each) | 3% |
| 500/100/1 | 52% | 41% (2.6 each) | 7% |
| 100/100/0.1 | 39% | 56% (2.0 each) | 5% |

## November 11 2026: TSO deferral (ns-3 TCP core, only with a send quantum)

What was wrong: with a send quantum (`TsoBursts`), ns-3 sent a partial burst
whenever an ACK freed some window, so most of its segments went out as
2-3 segment trains between full bursts. Linux holds such a partial skb back.

Linux code matched: `tcp_tso_should_defer()` (net/ipv4/tcp_output.c, v6.13),
called from `tcp_write_xmit()` for new data. It sends now if in recovery, if
the last skb's departure time (`tcp_wstamp_ns`) is more than 1 ms ago, if a
full TSO skb fits (`limit >= max_segs * mss`, max_segs from
`bbr_tso_segs_generic()`), if a third of the window is free
(`tcp_tso_win_divisor` 3), or if the oldest unacked skb was sent less than
srtt/2 ago; otherwise it waits for more ACKs.

Change: `TcpSocketBase::TsoShouldDefer()` makes the same tests;
`SendPendingData()` calls it before starting a burst of new data when the
send quantum is larger than one segment. `m_pacingRelease` records when the
last paced burst's transmission time ends (`tcp_wstamp_ns`), and
`TcpTxBuffer::HeadSentTime()` gives the oldest unacked segment's last send
time (`tcp_rtx_queue_head()->tstamp`). Left out: the skb-tail test (a bulk
sender's queued skb is already full-sized, so it equals the full-skb test).
A pending FIN sends now, as in Linux. Without a send quantum nothing changes.
All 46 tcp-* suites pass.

Effect. Burst sizes (weighted mean, flow 0 / flow 1; Linux, ns-3 before,
ns-3 with deferral; `ns3out_deferburst_s1`, `ns3out_defer_s1`):

| Config | Linux | Before | Deferral |
|---|---|---|---|
| 500/25/0.1 | 24.1 / 12.1 | 8.3 / 6.6 | 26.6 / 13.8 |
| 500/25/2 | 27.8 / 12.1 | 9.3 / 6.7 | 32.4 / 13.4 |
| 500/100/1 | 30.0 / 11.1 | 17.9 / 8.0 | 30.1 / 13.3 |
| 100/100/0.1 | 5.2 / 2.5 | 3.2 / 2.1 | 5.9 / 2.0 |
| 100/100/1 | 5.4 / 2.6 | 4.3 / 2.0 | 6.2 / 1.9 |
| 100/5/1 | 3.9 / 3.6 | 3.4 / 3.5 | 3.7 / 3.9 |
| 50/100/1 | 2.5 / 2.0 | 2.6 / 2.0 | 2.9 / 2.0 |

Focus grid (8 configs x 3 seeds, same basis as above; with whole segments,
`TsoBursts` on):

| Variant | Bad bins | Stalls per grid | Distance (pp) | Per seed (pp) | Seed vs seed (pp) |
|---|---|---|---|---|---|
| Linux (kernel vs kernel) | 5.15% | 9.7 | 1.40 | | |
| Before (one-segment pacing) | 7.24% | 34.0 | 2.56 | 2.42-2.65 | 1.17 |
| Whole segments + `TsoBursts` | 6.90% | 28.0 | 2.16 | 1.97-2.44 | 1.99 |
| + TSO deferral | 5.29% | **17.0** | **1.76** | 1.55-2.08 | 1.15 |

Per config with deferral (Linux in brackets): 100/100/1 8.8% and 3.0 stalls
(7.9%, 2.3; was 13.4%, 11.0); 500/100/1 8.3%, 5.0 (9.3%, 3.3; was 16.3%,
12.3); 500/25/1 2.9% (1.7%; was 3.6%). Still off: lost retransmissions at
0.1 BDP (100/100/0.1: 501 vs Linux 120, although bad bins are 3.1% vs 5.6%).
Full grid (36 configs, 3 seeds, `fidelity_data.py` basis; whole segments,
`TsoBursts` on, deferral; `ns3out_deferfull_s1..3`):

| Measure | Linux (kernel vs kernel) | Now (one-segment pacing) | Whole segments + `TsoBursts` | + TSO deferral |
|---|---|---|---|---|
| Bad bins, mean | 2.38% | 3.37% | 3.58% | 2.81% |
| Distance from Linux (pp) | 0.69 | 1.50 (0.94-2.10) | 1.70 (1.40-1.94) | **1.21** (0.88-1.67) |
| Configs more than 3 pp off, per run pair | 1 | 4.7 | 4.9 | 3.9 |
| Stalls per run | 18.7 | 59 | 51 | **34.3** |
| Run-to-run spread (pp) | 0.69 | 1.17 | 1.67 | **0.99** |

By group (distance pp / stalls; now -> deferral, Linux stalls in brackets):
0.1 BDP 0.89/8.0 -> 1.08/5.7 (4.3); 1 BDP 2.65/35.7 -> 1.47/15.0 (8.0);
2 BDP 1.33/7.7 -> 1.21/7.0 (2.7); 4 BDP 1.13/7.7 -> 1.09/6.7 (3.7);
5 ms 0.94 -> 0.66; 25 ms 1.22 -> 1.29; 100 ms 2.33/46.3 -> 1.69/27.0 (15.7).

Worst configs now: 500/25/2 and 500/25/4 (Linux 0.9% and 1.4% bad bins,
ns-3 5.2% both; were 3.9% and 3.5%), then 100/100/1, 500/100/1, 50/100/1
(about 2.8-2.9 pp each, down from 3.2-6.9). 100/100/0.1 moved from 0.92 to
2.45 pp, with ns-3 now below Linux (3.1% vs 5.6%).

Result: the burst model with deferral is closer to Linux than one-segment
pacing on every summary measure. Recommendation: use `TsoBursts` in the
comparison runs.

## November 12 2026: ACK-aggregation cwnd in STARTUP (port)

Question: why are 500/25/2 and 500/25/4 the worst configs after the TSO
deferral fix (ns-3 5.2% bad bins, Linux about 1%)?

Finding: not dips. The second flow (starts at 12 s) runs at about 40 Mb/s
until about 18.6 s, so in 2 of 3 seeds it has about 40 bad bins against
Linux's 4-7. There is no loss (ns-3's 2 BDP and 4 BDP runs are identical).
It leaves STARTUP after 0.25 s at 7-10 Mb/s with cwnd 35-53 packets: cwnd is
2 x bw x min_rtt while flow 0's queue doubles the RTT, so delivery stalls at
the current estimate and the plateau test fires. It then spends 2.2 s at a
time in PROBE_DOWN (inflight stays above its own BDP behind flow 0's queue)
and gains bw only on each probe.

Kernel trace (`vm/bbrtrace.bt`, 3 runs of each config, `out_deep1..3`): the
joining Linux flow leaves STARTUP after 0.36-0.43 s at 85-91 Mb/s with cwnd
419-451 packets; its extra_acked during STARTUP is 30-54 packets.

Cause: Linux v3's `bbr_ack_aggregation_cwnd()` (tcp_bbr.c, google/bbr v3)
adds `extra_acked_gain * bbr_extra_acked()` to the cwnd target whenever the
gain is set. BBRv1 added it only after `bbr_full_bw_reached()`, and the port
kept that v1 condition (`m_extraAckedGain && m_isPipeFilled`). ns-3 did
compute extra_acked in STARTUP (8-13 packets here, about a third of cwnd) but
dropped it from the cwnd target.

Fix: `TcpBbrV3::AckAggregationCwnd()` no longer checks `m_isPipeFilled`.
All 46 tcp-* suites pass.

Effect (500/25, 3 seeds; joining flow at STARTUP exit, and bad bins):

| | Linux | Before | After |
|---|---|---|---|
| STARTUP exit: time, bw, cwnd | 0.36-0.43 s, 85-91 Mb/s, 419-451 | 0.25 s, 7-10 Mb/s, 35-53 | 0.64-0.69 s, 91-101 Mb/s, 480-506 |
| 500/25/2: flow 1 bad bins per run | 4-7 | 11, 40, 39 | 6, 10, 7 |
| 500/25/2: flow 1 median Mb/s | 147-232 | 121-143 | 175-222 |
| 500/25/4: flow 1 bad bins per run | 1-7 | 11, 40, 39 | 7, 10, 7 |

Full grid (36 configs, 3 seeds, `fidelity_data.py` basis; with `TsoBursts`,
whole segments and TSO deferral; `ns3out_aggrfull_s1..3`):

| Measure | Linux (kernel vs kernel) | Now (uploaded) | + TSO deferral | + this fix |
|---|---|---|---|---|
| Bad bins, mean | 2.38% | 3.37% | 2.81% | 2.51% |
| Distance from Linux (pp) | 0.69 (0.66-0.72) | 1.50 (0.94-2.10) | 1.21 (0.88-1.67) | **0.90** (0.69-1.01) |
| Configs more than 3 pp off, per run pair | 1 | 4.7 | 3.9 | **1.4** |
| Stalls per run | 18.7 | 59 | 34.3 | **26.7** |
| Run-to-run spread (pp) | 0.69 | 1.17 | 0.99 | **0.61** |

By group (distance pp; stalls with Linux in brackets): 0.1 BDP 1.17, 1.7
(4.3); 1 BDP 1.03, 15.3 (8.0); 2 BDP 0.80, 4.7 (2.7); 4 BDP 0.61, 5.0 (3.7);
5 ms 0.70; 25 ms 0.71; 100 ms 1.29, 22.7 (15.7).

Largest remaining distances: 500/100/1 2.63 pp (but Linux's own runs differ
by 4.67 pp there), then the 0.1 BDP configs 100/100/0.1 2.61, 50/100/0.1
2.20, 50/5/0.1 2.05, where ns-3 now has fewer bad bins than Linux (2.9% vs
5.6%, 4.3% vs 6.0%, 1.9% vs 3.9%).

## November 12 2026: 0.1 BDP buffers too smooth (two fixes)

Question: why do the 0.1 BDP configs now show fewer bad bins than Linux
(100/100/0.1 2.9% vs 5.6%, 50/5/0.1 1.9% vs 3.9%)?

Finding: total goodput looks the same. The difference is how the two flows
share it. In Linux the flows trade bandwidth for 0.5-1 s at a time (per-flow
cv 0.25-0.27, flow correlation -0.93 at 50/5/0.1); in ns-3 they stay closer
to equal (cv 0.13-0.14, correlation -0.29 to -0.67). Kernel traces
(`vm/bbrtrace.bt` with the bw-probe flags, `out_s505f1..2`) against ns-3
sampled every 1 ms, 50/5/0.1, per flow, 15-45 s:

| | Linux | ns-3 before |
|---|---|---|
| PROBE_UP endings: risky stop / loss / bw plateau | 332-358 / 258-276 / 1 | 287 / 195 / 113 |
| PROBE_DOWN length, 90th percentile | 1.3-1.4 RTT | 8.6 RTT |

### Fix 1: inflight at the next departure time (port)

Linux code matched: `bbr_packets_in_net_at_edt()` (tcp_bbr.c), used for the
PROBE_BW phase checks (`bbr_update_cycle_phase()`, on rs->prior_in_flight)
and the DRAIN exit (`bbr_check_drain()`). It subtracts what the path
delivers before the next departure time pacing allows (`tcp_wstamp_ns`) and,
with pacing_gain above 1, adds the burst sent then (`bbr_tso_segs_goal()`).

What was wrong: the port used rs.m_priorInFlight (and bytes in flight in
DRAIN) as is; a comment said the EDT term was at most one segment, which no
longer holds with bursts, and the PROBE_UP burst term was missing.

Change: `TcpBbrV3::InflightAtEdt()` and `TcpBbrV3::TsoSegsGoal()` (the
burst size formula, now also used by `SetSendQuantum()`). The socket records
the end of the last paced transmission time in `TcpSocketState::m_pacingRelease`
(was a socket member, added with TSO deferral). 3 seeds, `TsoBursts` on:
100/100/0.1 bad bins 3.7-4.3% (Linux 4.3-6.3%, was 2.0-4.2%); 50/5/0.1
1.8-2.8% (Linux 3.5-4.5%, was 1.7-2.2%).

### Fix 2: cwnd-limited over a window (ns-3 TCP core, read only by BBRv3)

Linux code matched: `tcp_cwnd_validate()` (tcp_output.c), called at the end
of `tcp_write_xmit()`. `tp->is_cwnd_limited`, which BBRv3 reads in PROBE_UP
(to reset the bw plateau check and to raise inflight_hi), stays true from a
cwnd-limited send until that window of data is acknowledged
(`cwnd_usage_seq`), and `tcp_tso_should_defer()` sets it when it waits for
cwnd.

What was wrong: ns-3's `m_isCwndLimited` is recomputed after every segment,
so while pacing it is mostly false. BBRv3 then let the bw plateau end
PROBE_UP early (113 times per flow vs Linux 1) and rarely raised inflight_hi.

Change: `TcpSocketState::m_cwndLimitedInWindow` is kept by
`TcpSocketBase::CwndValidate()` exactly as Linux keeps `is_cwnd_limited`
(including TSO deferral for cwnd); `TcpBbrV3` reads it instead of
`m_isCwndLimited`. Other congestion controls are unaffected. All 46 tcp-*
suites pass.

Effect at 50/5/0.1 (one seed, 1 ms sampling; with fix 1):

| Per flow | Linux | Before | After |
|---|---|---|---|
| PROBE_UP endings: risky / loss / plateau | 332-358 / 258-276 / 1 | 287 / 195 / 113 | 345 / 241 / 2 |
| PROBE_DOWN length, 90th percentile | 1.3-1.4 RTT | 8.6 | 1.2-1.4 |
| Probe cycle (REFILL to REFILL), median | 51.5 ms | | 46.0 ms |
| Bad bins / per-flow cv / flow correlation | 3.5-4.5% / 0.25-0.27 / -0.93 | 2.5% / 0.15 / -0.69 | 3.0% / 0.20 / -0.76 |

Still different: after a risky stop Linux usually sees that probe's loss in
the next round and does not re-probe at once; ns-3 re-probes 2.5x as often
(DOWN -> REFILL 240 vs 94 per flow), with about half Linux's loss rate here
(1.25% vs 2.6% of segments).

Full grid with both fixes (`ns3out_valfull_s1..3`): distance 1.13 pp, 1.9
configs more than 3 pp off, 35 stalls per run, spread 1.16 pp (before:
0.90, 1.4, 26.7, 0.61). Two runs of seed 3 (50/5/0.1 and 50/100/0.1) lost the
joining flow's SYN at the full 0.1 BDP queue, and ns-3 retries a SYN after
`TcpSocket::ConnTimeout` (3 s) where Linux uses TCP_TIMEOUT_INIT (1 s), so
that flow started at 15 s, inside the scored window (Linux: no late start in
any of its 108 runs; ns-3: 2 of 108 here, 0 in every earlier grid). Without
those two configs (34 configs, every variant): kernel 0.65 pp / 16.7 stalls;
before 0.83 pp / 26.3; with both fixes 0.91 pp / 32.7. The fixes help 100/100/0.1
(2.61 -> 0.76 pp), 50/5/2 and 500/25/1, and hurt the 1 BDP / 100 ms configs
(100/100/1 1.55 -> 2.53, 50/100/1 1.04 -> 1.83). There the bw-plateau exits
are gone as in Linux, but ns-3's PROBE_UP still lasts 0.6-3.4 RTTs (median
per flow) against Linux's 0.1-1.0 (`out_flags` traces).

Harness: the comparison runs now set `--ns3::TcpSocket::ConnTimeout=1s`
(Linux TCP_TIMEOUT_INIT). Rerun of this grid with it (`ns3out_val1s_s1..3`, no late starts): distance
1.01 pp, 1.8 configs more than 3 pp off, 34.7 stalls per run, spread 0.91 pp
(before fixes 1-2: 0.90, 1.4, 26.7, 0.61). Kept on the user's decision: both
are direct ports of Linux code that the traces show are needed, and later
fixes build on them.

### Fix 3: inflight_hi growth slope floor (port)

Linux code matched: `bbr_raise_inflight_hi_slope()` sets
`bw_probe_up_cnt = max(cwnd / 2^bw_probe_up_rounds, 1)` packets, and
`bbr_probe_inflight_hi_upward()` adds one packet to inflight_hi per
`bw_probe_up_cnt` packets ACKed. The port counts in bytes but floored the
count at 1 byte, so once a PROBE_UP lasted about log2(cwnd) rounds
inflight_hi grew by one segment per byte ACKed. It happened at 5 ms RTT:
inflight_hi jumped by more than 20% within one 5 ms sample 10-16 times per
run at 500/5/0.1 (`ns3out_valfull_s*`). The floor is now one segment.

### Fix 4: pacing catch-up credit (ns-3 TCP core, only with a send quantum)

Linux code matched: `tcp_update_skb_after_send()` (tcp_output.c): the next
departure time advances by the skb's pacing time less
`min(len_ns / 2, credit)`, where credit is how late this send was relative
to the previous departure time ("take into account OS jitter"). A sender
that waited for an ACK (cwnd-limited) therefore sends its next skb sooner.

What was wrong: ns-3 always waited the full pacing time.

Change: `TcpSocketBase::SchedulePacing()` starts the pacing timer at all
three sites and applies the credit when a send quantum is set (`TsoBursts`);
without one nothing changes.

Effect at 50/5/0.1 (2 seeds, with fixes 1-3 and the 1 s SYN timeout):

| | Linux | Before | After |
|---|---|---|---|
| Loss rate at the bottleneck | 2.5-2.6% | 2.2-2.3% | 2.6-2.7% |
| Per-flow cv / flow correlation | 0.25-0.27 / -0.93 | 0.20 / -0.76, -0.87 | 0.24-0.28 / -0.84, -0.90 |
| Bad bins | 3.5-4.5% | 2.8-3.0% | 2.2-3.7% |

Full grid with fixes 3 and 4 (`ns3out_cred_s1..3`, fixes 1-4, ConnTimeout 1 s):

| Measure | Linux (kernel vs kernel) | Before fixes 1-2 (aggr) | Fixes 1-2 (val1s) | Fixes 1-4 (cred) |
|---|---|---|---|---|
| Distance from Linux | 0.69 pp | 0.90 pp | 1.01 pp | **0.97 pp** (0.72-1.24) |
| Configs more than 3 pp off | 1 | 1.4 | 1.8 | 1.9 |
| Stalls per run | 18.7 | 26.7 | 34.7 | 36.3 |
| Bad bins | 2.38% | 2.51% | 2.98% | 3.00% |
| Run-to-run spread | 0.69 pp | 0.61 pp | 0.91 pp | 0.84 pp |

By buffer (distance pp / stalls per run):

| Buffer | aggr | val1s | cred | Linux stalls |
|---|---|---|---|---|
| 0.1 BDP | 1.17 / 1.7 | 1.12 / 4.3 | **0.99** / 7.3 | 4.3 |
| 1 BDP | 1.03 / 15.3 | 1.57 / 20.3 | 1.35 / 19.0 | 8.0 |
| 2 BDP | 0.80 / 4.7 | 0.72 / 5.0 | 0.85 / 5.0 | 2.7 |
| 4 BDP | 0.61 / 5.0 | 0.61 / 5.0 | 0.68 / 5.0 | 3.7 |

Result: fixes 3-4 recover a little of what fixes 1-2 cost (1.01 -> 0.97 pp)
and give the best 0.1 BDP distance so far (50/5/0.1 now 1.02 pp, was 2.98).
The gap left is the 1 BDP buffers: 19 stalls per run against Linux's 8,
mostly the three 100 ms configs (50/100/1 4.7, 100/100/1 4.7, 500/100/1 8.3;
Linux 2.3, 2.3, 3.3).

### What the extra 1 BDP stalls are (measurement, no code change)

Tools (scratchpad): `stalls.py` (stall episodes with the ns-3 BBR state),
`stalldrops.py` (drops of the flow around each stall), `retxep.py` (loss
episodes from Linux `flows.csv` retx rate and ns-3 `run.csv.sends.csv`).

- At 500/100/1, 19 of 25 ns-3 stalls start right after a loss episode in which
  some retransmissions were dropped too; the flow is in recovery with zero
  goodput for 3-7 bins until the lost retransmission is repaired.
- Loss episodes (15-45 s) match Linux in count and length; packets per episode
  are similar at 1 BDP / 100 ms and 1.5-2x Linux at 500/25/1.
- Drops in 15-45 s grew with fixes 1-2 (per run; Linux from its retransmit
  count): 100/100/1 Linux ~1070, aggr 1054, cred 1548; 500/25/1 Linux ~2340,
  aggr 3151, val1s 4930, cred 5110; 500/100/1 Linux ~7000, aggr 4928, cred 7405.
- Lost retransmissions in 15-45 s, per seed: 500/25/1 aggr 20/55/17, val1s
  143/153/191, cred 221/217/138 (Linux `TCPLostRetransmit` 4/17/43 for the
  whole run); 500/100/1 aggr 42/6/42, cred 70/149/100. Fixes 1-2 raised them
  about 5x at 500/25/1 on every seed.
- Checked and ruled out: v3 reads `tp->is_cwnd_limited` directly (not
  `tcp_is_cwnd_limited()`, whose slow-start clause would always be true under
  BBR), as the port does; `CwndValidate()` matches `tcp_write_xmit()` /
  `tcp_cwnd_validate()` on every path; Linux also stays in PROBE_UP through
  recovery for 0.2 s or more (`out_flags` 100/100/1, 20.28-20.50 s), so long
  ns-3 PROBE_UPs in recovery are not by themselves a mismatch.

### A/B: which of fixes 1-2 raises the drops (measurement, no code change)

Fixes 1-4 with fix 1 removed (`ns3out_nofix1_s*`) or fix 2 removed
(`ns3out_nofix2_s*`); VM-only patches (`vm/ab_driver.sh`), both VMs restored
to the current code afterwards (md5 3402db8d). 3 seeds, 500/100/1 2 seeds.
Scored with scratchpad `abscore.py` (distance = mean |bad bins| gap over all
seed/kernel pairs).

| Config | Linux bad / stalls | aggr | cred (fixes 1-4) | without fix 1 | without fix 2 |
|---|---|---|---|---|---|
| 500/25/1 | 1.72% / 0.0 | 0.83 pp, 3151 drops, lost retx 31 | 0.59, 5110, 192 | 0.54, 4651, 222 | 0.65, 2860, 70 |
| 100/100/1 | 7.89% / 2.3 | 1.55, 1054, 13 | 2.04, 1548, 9 | 2.54, 1477, 11 | **1.07**, 906, 9 |
| 50/100/1 | 7.83% / 2.3 | 1.04, 611, 6 | 2.06, 707, 7 | 1.93, 732, 6 | 3.33, 247, 8 |
| 50/5/0.1 | 3.94% / 1.3 | 2.05, 1558, 40 | **1.02**, 3239, 113 | 0.96, 3337, 120 | 1.94, 301, 2 |
| 500/100/1 | 9.34% / 3.3 | 2.63, 4928, 30 | 3.09, 7405, 106 | 2.42, 6152, 55 | 2.58, 4232, 41 |
| Mean distance | | 1.62 | 1.76 | 1.68 | 1.91 |

Result: fix 2 (sticky cwnd-limited) causes the extra drops and lost
retransmissions; fix 1 changes little. Removing fix 2 is not better overall
(1.91 vs 1.76 pp): it fixes 100/100/1 but makes 50/100/1 and 50/5/0.1 too
smooth again (300 drops at 50/5/0.1 where Linux's loss rate needs about
3000). Both fixes stay.

Rechecked against v6.13 `tcp_tso_should_defer()`: Linux sets
`is_cwnd_limited` on a deferral only when `cong_win < send_win` and
`cong_win <= skb->len`; with bulk data and the port's `limit < quantum`
test this is the same as the port's `cWnd < rWnd` condition.

Where ns-3 differs (scratchpad `fill.py`, 15-45 s, PROBE_BW, ca_state Open):
ns-3 is at the top of Linux's run-to-run range rather than outside it. Median
RTT 100/100/1: Linux 173 / 149-160 ms (two traced runs), ns-3 172 ms in every
seed; 50/100/1: Linux 154 / 174 ms, ns-3 172-173 ms. Median inflight/cwnd:
Linux 0.79-0.996, ns-3 0.94-0.997. ns-3 always keeps the queue near full;
Linux sometimes runs with 15-25 ms less queue. No single Linux mechanism found
for this yet.

### Why ns-3 keeps the queue fuller: bandwidth estimates (measurement, no code change)

Sum of both flows' estimates over the link rate, 15-45 s, 0.1 s bins
(scratchpad `bwsum.py`; Linux `out_flags`, `out_flags2`; ns-3 `ns3out_cred_s1..3`):

| | 100/100/1 Linux | 100/100/1 ns-3 | 50/100/1 Linux | 50/100/1 ns-3 |
|---|---|---|---|---|
| Sum of max_bw, median | 1.036-1.070 | 1.083-1.119 | 1.042-1.131 | 1.076-1.154 |
| Sum of bbr_bw with both flows in CRUISE, median | 0.985-1.044 | 0.996-1.110 | 1.014-1.056 | 1.073-1.121 |

So ns-3 more often paces the two flows above the link in CRUISE, and the queue
fills until cwnd binds. Checked where that could come from:
- Lower bounds: bw_lo / inflight_lo are set equally often (CRUISE 15-19% of
  samples in Linux, 21-27% in ns-3; DOWN 68-76% vs 77-79%; never in REFILL/UP).
- Per-ACK delivery-rate samples: new harness option `--rateLog` writes the
  port's per-ACK delivery rate (`InternalState` trace, 15-45 s) to
  `<out>.rate.csv` (`ns3out_rate_s1..3`, VM b). Same distribution as Linux's
  `rs_delivered / rs_interval_us` (scratchpad `rates.py`), as a share of the
  link: 100/100/1 Linux larger flow p50 0.645-0.713, p99 0.796-0.811, max
  0.819-0.878; ns-3 p50 0.648-0.752, p99 0.779-0.825, max 0.811-0.894. No
  sample above the link rate in either stack.
- Code compared and matching: `tcp_rate_skb_delivered()` / `tcp_rate_gen()`
  (send/ACK interval max, min-RTT validity check, newest-sent tie break),
  `bbr_take_max_bw_sample()` (app-limited rule), and
  `bbr_advance_max_bw_filter()` (only at the end of the probe-feedback round,
  in PROBE_BW, not app-limited).

Result: negative. The samples and the filter match Linux; the higher summed
estimate is within or just above Linux's two traced runs, so it looks like
ns-3 landing at one end of Linux's run-to-run range every time, not a missing
mechanism. No port change.

### 500/25/1 kernel trace: where the extra drops come from (measurement, no code change)

New Linux runs with `bbrtrace.bt` (v3_flags) and the sender pcap, VM b:
`results/out_t525_1..3` (`run_matrix.sh`, 3 runs). Loss episodes from the pcap
with `vm/loss_episodes.py` (now also in `/root/tools` on VM b), saved as
`loss_episodes.txt` per run; ns-3 episodes from the drop log with the same
grouping (gap > 50 ms). Scratchpad `lossep_cmp.py`, `prtt.py`.

Whole run, kernel counters: retransmits 4856 / 4706 / 4567, `TCPLostRetransmit`
12 / 4 / 29.

Loss episodes, 15-45 s:

| | Linux (3 runs) | ns-3 aggr (3 seeds) | ns-3 cred (3 seeds) |
|---|---|---|---|
| Episodes | 14-22 | 17-19 | 17-24 |
| Lost per episode, median / mean | 42-148 / 111-183 | 117-158 / 129-236 | 88-183 / 220-297 |
| Lost in total | 2450-2617 | 2454-4015 | 4423-5273 |
| Lost retransmissions | 2-5 | 17-55 | 138-221 |
| Largest episode | 456 / 405 / 499 | | 1225 / 1972 (s1, s2), at 15.70 s |

One event explains most of the gap. On every ns-3 seed, flow 0 enters
PROBE_RTT at 15.46 s while flow 1 (started at 12 s) is in PROBE_UP.
- The queue drains (RTT 40 -> 25 ms), so flow 1's bandwidth samples rise
  (147 -> 346 Mb/s).
- Flow 1 is cwnd-limited at inflight_hi, so inflight_hi grows each round with
  the doubling slope (472 -> 1290 packets in 0.2 s). Both are v3 behaviours:
  `bbr_probe_inflight_hi_upward()`, `bbr_raise_inflight_hi_slope()`, and the
  PROBE_UP check that resets full_bw while inflight_hi limits growth.
- Flow 0 leaves PROBE_RTT at 15.68 s into CRUISE, with cwnd 1451 and its
  pre-PROBE_RTT estimate of 470 Mb/s (Linux `bbr_exit_probe_rtt()` does the
  same).
- Both flows together then put about 2700 packets in flight on a 2064-packet
  pipe-plus-buffer. Result: 1225-1972 drops, including 169-184 of the seed's
  138-221 lost retransmissions.

Linux never had that overlap. Its flow 0 entered PROBE_RTT at 15.85 s in all
three runs, with flow 1 in CRUISE; in 33 Linux PROBE_RTTs the other flow
never stayed in PROBE_UP throughout.

Why the schedules differ: the first PROBE_RTT is 5 s after
`probe_rtt_min_stamp`, which is refreshed by any RTT sample strictly below
`probe_rtt_min_us` (seeded from `tcp_min_rtt()` in `bbr_init()`; the port
matches).
- Linux: SYN RTT 25044 us (pcap; 25048 in the trace). At 0.421 s, the end of
  DRAIN, one data sample of exactly 25000 us refreshed the stamp, so the first
  PROBE_RTT came at 5.42 s.
- ns-3: SYN RTT 25002 us and no lower data sample, so the first PROBE_RTT
  came at 5.03 s.

The 0.39 s offset persists through every later PROBE_RTT.

Over the other kernel traces this refresh is not systematic:
- Same timing in both stacks: Linux's first PROBE_RTT is at 5.00 s where
  ns-3's is at 5.03-5.11 s (100/100/1, 50/100/1, 500/25/2, 500/25/4), and
  both refresh at the same point at 50/5/0.1 (0.022-0.026 s vs 0.035 s) and
  100/100/0.1 (1.14 s vs 1.25 s).
- 500/25/1 is the only traced config where Linux finds a sample below its
  handshake RTT during STARTUP and ns-3 does not.
- Linux's handshake RTT is consistently 41-48 us above the bare path (host
  time); ns-3's is 2-18 us above.

Result: no port mismatch. The 500/25/1 gap is mostly one PROBE_RTT/PROBE_UP
coincidence set off by a single sub-handshake RTT sample in Linux. Without the
15.70 s episode ns-3 is at about 3200-3300 drops and 37-48 lost retransmissions
per seed (Linux 2450-2617 and 2-5), so a smaller excess remains.

Not tried: emulating Linux's 40-45 us of handshake host time in the harness.
It would only move PROBE_RTT timing, and only in configs where data samples
can undercut the handshake RTT, so its effect would be configuration-specific
rather than a fidelity fix.

## Status on November 11 2026 (36 BBRv3 configs, Google Cloud VMs)

Same harness for every row: netem-style queue limit, goodput from cumulative
ACKs on both sides, Linux gains. Every ns-3 run (RngRun seed) is compared with
every kernel run of the same config, the same basis as kernel vs kernel.
The Nov 10 code was rerun with this measure (two seeds); the "now" row is the
code being uploaded (`TsoBursts` off, `TcpSocketBase::LinuxRto` on, per-segment
loss hook, three seeds). Three kernel grids: Nov 3, Nov 10, Nov 11. Scores:
`linux-lab/compare/fidelity_data.py` -> `verify/fidelity_scores.csv`,
`verify/fidelity_percfg.csv`.

| Measure | Linux (kernel vs kernel) | Nov 10 code | Now (with the loss hook) |
|---|---|---|---|
| Bad bins, mean | 2.38% | 4.42% | 3.37% |
| Distance from Linux (pp) | 0.69 (0.66-0.72) | 2.39 (2.13-2.66) | 1.50 (0.94-2.10) |
| Configs more than 3 pp off, per run pair | 1 | 7.2 | 4.7 |
| Stalls per run | 18.7 | 69 | 59 |
| Run-to-run spread (pp) | 0.69 | 1.82 | 1.17 |

Heavy runs (500/100/1) spike to 5-8 GB of memory during loss episodes, more than
the VMs' 8 GB for some seeds; the VMs now have 8 GB of swap, and these runs are
run one at a time. A run cut short by the out-of-memory killer is left out of
every score (`fidelity_data.py` checks the last sample time).

The Nov 10 dashboard's 1.52 pp / 54 stalls used FlowMonitor arrivals for ns-3;
with the same measure as Linux the Nov 10 code was 2.39 pp / 69 stalls.

Closed: lone-flow bandwidth overestimate (+8% at 50 Mbps / 5 ms), ms-quantized
min_rtt, inflated late-flow min_rtt and the starvation it caused (50/5/4: 11.0%
-> 1.2-1.8%, Linux 1.3-2.2%), 0.1 BDP at 100 ms (100/100/0.1: 9.5% -> 5.7-8.2%,
Linux 5.2-6.3%; 50/100/0.1 15.8% -> 4.8-6.8%, Linux 5.0-7.8%), spurious RTOs
during recovery.

Open:
1. 1 BDP at 100 ms RTT: 50/100/1, 100/100/1 and 500/100/1 still have 8-13
   stalls per run against Linux's 1-6, about 30 of the 41 excess stalls. The
   second flow leaves STARTUP on the bandwidth plateau earlier and lower than
   Linux's, which leaves on loss with an inflight_hi (traced: 100/100/1 Linux
   13.50 s / 18.3 Mbps / inflight_hi 313; ns-3 13.38 s / 16.5 Mbps, none).
2. Seed-dependent slow late-flow start at 500 Mbps with 2-4 BDP (one seed of
   three: 7-10% bad bins against Linux's 0.5-2%), when the first flow's
   PROBE_UP coincides with the join.
3. TSO-sized bursts: the mechanism matches the kernel capture but makes the
   grid less faithful as a whole; what smooths the bursts before netem in the
   nsperf topology is not modeled.

## Status on November 10 2026 (36 BBRv3 configs, Google Cloud VMs)

Linux: fresh kernel grid (Nov 10). ns-3 now: a rerun with exactly the files
being uploaded; it reproduced the Nov 4 grid byte for byte, and the debug and
optimized builds produce identical output.

| Measure | Linux | Aug 31 port | ns-3 now (Linux values) |
|---|---|---|---|
| Distance from Linux, bad bins (pp) | 0.7 (kernel vs kernel) | 3.3 | 1.5 |
| Configs more than 3 pp off | 1 | 11 | 5 |
| Stalls, total | 21 | 46 | 54 |
| Loss at 2 BDP | 0.10% | 0.20% | 0.07% |
| PROBE_RTT dip, lone flow | 46% of link | none (never entered) | 50% |

Open gaps, being worked on next:
1. Distance from Linux (1.5 pp) is still about twice the kernel's own noise.
2. Five configs are more than 3 pp off, mostly 100 ms RTT with small buffers.
3. More long stalls than Linux, in the same configs where Linux stalls.
