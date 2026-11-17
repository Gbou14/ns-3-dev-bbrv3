// SPDX-License-Identifier: GPL-2.0-only
//
// Copy of bbrv3-comparison-harness.cc that adds three columns to each CSV row:
// wall_s (process CPU seconds so far), events (scheduler events executed so
// far) and sent_list_pkts (sender TcpTxBuffer sent-list length), to find
// where simulation time goes.
//
// Multi-flow BBRv3-vs-baseline comparison harness, modeled on the packet/
// periodic-level CSV logging scheme Pedro described for his own simulation
// script (per-flow congestion-control internal state via the TcpBbrV3
// InternalState trace, plus bottleneck queueing delay, active-flow count,
// ideal fair share, and per-flow throughput/loss via FlowMonitor).
//
// Topology: N independent sender/receiver pairs, each with its own access
// link (so per-flow RTT can differ), sharing one bottleneck link (R1->R2)
// with a FIFO queue disc. Each flow can run a different congestion control
// algorithm, start/stop at a different time, and see a different access
// delay -- enough to drive the comparison battery (utilization, fairness,
// coexistence, RTT fairness, dynamic bottleneck, random loss, bufferbloat).
//
// Output is a periodic (not strictly per-packet) CSV snapshot, sampled every
// --sampleInterval: one row per active flow per sample, columns matching
// Pedro's convention:
//   sample_time,flow_id,cc_algo,state0..state7,bot_arrival_time,
//   bot_waiting_time,num_active_flows,fair_bandwidth,
//   tx_bytes,rx_bytes,dropped_bytes

#include "ns3/bulk-send-helper.h"
#include "ns3/config.h"
#include "ns3/core-module.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/error-model.h"
#include "ns3/fifo-queue-disc.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/ipv4-header.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/packet-sink.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/ppp-header.h"
#include "ns3/queue-disc.h"
#include "ns3/queue-size.h"
#include "ns3/tcp-bbr-v3.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/tcp-tx-buffer.h"
#include "ns3/traffic-control-helper.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("BbrV3ComparisonHarness");

namespace ns3
{

/**
 * FIFO whose limit counts packets on the emulated wire as well as in the queue,
 * like the netem qdisc nsperf uses as its bottleneck: netem holds each packet
 * until its departure time, i.e. through queueing, transmission and the
 * one-way delay, and `limit` (one-way BDP + buffer, in packets) applies to all
 * of them. So when the link has been less than fully busy for the last
 * one-way delay, netem accepts more than `buffer` queued packets.
 */
class NetemLimitQueueDisc : public FifoQueueDisc
{
  public:
    /**
     * Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    static uint32_t s_limit;   //!< netem limit, in packets
    static uint32_t s_inNetem; //!< packets accepted and not yet past the delay line

  private:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
};

uint32_t NetemLimitQueueDisc::s_limit = 0;
uint32_t NetemLimitQueueDisc::s_inNetem = 0;

TypeId
NetemLimitQueueDisc::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NetemLimitQueueDisc")
                            .SetParent<FifoQueueDisc>()
                            .SetGroupName("TrafficControl")
                            .AddConstructor<NetemLimitQueueDisc>();
    return tid;
}

bool
NetemLimitQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    if (s_inNetem >= s_limit)
    {
        DropBeforeEnqueue(item, LIMIT_EXCEEDED_DROP);
        return false;
    }
    bool ok = GetInternalQueue(0)->Enqueue(item);
    if (ok)
    {
        ++s_inNetem;
    }
    return ok;
}

NS_OBJECT_ENSURE_REGISTERED(NetemLimitQueueDisc);

/**
 * Host-side send jitter: holds each packet for a uniform random 0..s_jitter
 * before it may leave, without reordering (a packet never leaves before the
 * one ahead of it). Stands in for the timer, softirq and scheduling noise a
 * real Linux host adds to paced transmissions and ACKs, which ns-3 otherwise
 * models as perfectly regular.
 */
class JitterQueueDisc : public QueueDisc
{
  public:
    /**
     * Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    JitterQueueDisc();

    static Time s_jitter; //!< maximum extra hold time per packet

  private:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    bool CheckConfig() override;
    void InitializeParams() override;

    std::deque<Time> m_ready;        //!< release time of each queued packet, in order
    Time m_lastReady;                //!< release time of the last packet enqueued
    Ptr<UniformRandomVariable> m_rv; //!< hold-time draw
    EventId m_wake;                  //!< pending Run() at the head's release time
};

Time JitterQueueDisc::s_jitter = Time(0);

TypeId
JitterQueueDisc::GetTypeId()
{
    static TypeId tid = TypeId("ns3::JitterQueueDisc")
                            .SetParent<QueueDisc>()
                            .SetGroupName("TrafficControl")
                            .AddConstructor<JitterQueueDisc>();
    return tid;
}

JitterQueueDisc::JitterQueueDisc()
    : QueueDisc(QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE, QueueSizeUnit::PACKETS),
      m_rv(CreateObject<UniformRandomVariable>())
{
}

bool
JitterQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    if (!GetInternalQueue(0)->Enqueue(item))
    {
        return false;
    }
    Time ready = Simulator::Now() +
                 NanoSeconds(static_cast<int64_t>(m_rv->GetValue(0, s_jitter.GetNanoSeconds())));
    m_lastReady = std::max(ready, m_lastReady);
    m_ready.push_back(m_lastReady);
    return true;
}

Ptr<QueueDiscItem>
JitterQueueDisc::DoDequeue()
{
    if (m_ready.empty())
    {
        return nullptr;
    }
    if (m_ready.front() > Simulator::Now())
    {
        if (!m_wake.IsPending())
        {
            m_wake = Simulator::Schedule(m_ready.front() - Simulator::Now(), &QueueDisc::Run, this);
        }
        return nullptr;
    }
    m_ready.pop_front();
    return GetInternalQueue(0)->Dequeue();
}

bool
JitterQueueDisc::CheckConfig()
{
    if (GetNInternalQueues() == 0)
    {
        AddInternalQueue(CreateObjectWithAttributes<DropTailQueue<QueueDiscItem>>(
            "MaxSize",
            QueueSizeValue(QueueSize("100000p"))));
    }
    return true;
}

void
JitterQueueDisc::InitializeParams()
{
}

NS_OBJECT_ENSURE_REGISTERED(JitterQueueDisc);

} // namespace ns3

namespace
{

std::vector<std::string> g_ccAlgo; //!< per-flow CC TypeId
std::vector<double> g_startTime;   //!< per-flow start (s)
std::vector<double> g_stopTime;    //!< per-flow stop (s)
std::vector<std::vector<double>>
    g_lastState;                         //!< per-flow last InternalState (8 fields, 0 if none yet)
std::vector<double> g_reportedLossBytes; //!< per-flow sum of rs.m_bytesLoss seen by CongControl
uint32_t g_numActiveFlows = 0;
Time g_lastBotArrival{Seconds(0)};
Time g_lastBotWaitingTime{Seconds(0)};
DataRate g_bottleneckRate;
bool g_randomizeStreams = false;
std::ofstream g_csv;

Ptr<FlowMonitor> g_monitor;
Ptr<Ipv4FlowClassifier> g_classifier;
std::vector<Ptr<TcpSocketBase>> g_sock; //!< per-flow sender socket (resolved lazily)
NodeContainer g_senders;
std::vector<SequenceNumber32> g_highTx; //!< per-flow highest sequence sent

/// Per-flow statistics over the scoring window, comparable with the kernel runs.
struct WindowStats
{
    SequenceNumber32 maxSeen{0};       //!< highest end sequence seen at the bottleneck
    bool seen{false};                  //!< maxSeen is valid
    uint64_t drops{0};                 //!< bottleneck drops (whole run)
    uint64_t retxDrops{0};             //!< drops of retransmitted segments (whole run)
    SequenceNumber32 lastAck{0};       //!< last cumulative ACK seen by the sender
    std::array<uint64_t, 8> ackHist{}; //!< ACKs by newly acked segments: 0,1,2,3,4,5-8,9-16,17+
    SequenceNumber32 maxSent{0};       //!< highest end sequence the sender put on the wire
    std::set<uint32_t> holRetx;        //!< retransmissions sent as the head of line, not yet seen
    /// Per 10 ms send bin: new segments, retransmissions, head-of-line retransmissions.
    std::map<int64_t, std::array<uint32_t, 3>> sendBins;
    Time lastTx{Seconds(-1)};     //!< when the previous data segment went out
    uint32_t train{0};            //!< segments in the current back-to-back train
    std::vector<uint32_t> trains; //!< finished trains in the ACK window, in segments
};

std::vector<WindowStats> g_ws; //!< per-flow window statistics
std::ofstream g_dropCsv;       //!< t,flow,seq,retx for every bottleneck drop
std::ofstream g_rateCsv;       //!< t,flow,delivery_bps,lost_bytes for every BBRv3 ACK, 15-45 s
uint32_t g_segSize = 1448;     //!< segment size, for ACK sizes
double g_winLo = 15.0;         //!< ACK histogram window start (s)
double g_winHi = 45.0;         //!< ACK histogram window end (s)

/**
 * A data packet reached the bottleneck queue. A segment that starts below the
 * highest sequence already seen there is a retransmission (the path does not
 * reorder).
 * @param dropped true if the queue dropped it
 * @param item the packet
 */
void
BottleneckArrival(bool dropped, Ptr<const QueueDiscItem> item)
{
    Ptr<Packet> p = item->GetPacket()->Copy();
    TcpHeader h;
    if (p->RemoveHeader(h) == 0 || p->GetSize() == 0)
    {
        return;
    }
    uint32_t i = h.GetDestinationPort() - 9000;
    if (i >= g_ws.size())
    {
        return;
    }
    WindowStats& w = g_ws[i];
    SequenceNumber32 seq = h.GetSequenceNumber();
    bool retx = w.seen && seq < w.maxSeen;
    SequenceNumber32 end = seq + p->GetSize();
    if (!w.seen || end > w.maxSeen)
    {
        w.maxSeen = end;
        w.seen = true;
    }
    bool hol = retx && w.holRetx.erase(seq.GetValue()) > 0;
    if (dropped)
    {
        ++w.drops;
        w.retxDrops += retx;
        if (g_dropCsv.is_open())
        {
            g_dropCsv << Simulator::Now().GetSeconds() << "," << i << "," << seq.GetValue() << ","
                      << retx << "," << hol << "\n";
        }
    }
}

/**
 * A sender put a packet on its access link: count new segments and
 * retransmissions per 10 ms, and remember head-of-line retransmissions
 * (sequence == snd_una) so a drop of one can be flagged.
 * @param i flow index
 * @param pkt the packet, with PPP, IP and TCP headers
 */
void
SenderMacTx(uint32_t i, Ptr<const Packet> pkt)
{
    Ptr<Packet> p = pkt->Copy();
    PppHeader ppp;
    Ipv4Header ip;
    TcpHeader h;
    if (p->RemoveHeader(ppp) == 0 || p->RemoveHeader(ip) == 0 || ip.GetProtocol() != 6 ||
        p->RemoveHeader(h) == 0 || p->GetSize() == 0 || i >= g_sock.size() || !g_sock[i])
    {
        return;
    }
    WindowStats& w = g_ws[i];
    SequenceNumber32 seq = h.GetSequenceNumber();
    SequenceNumber32 end = seq + p->GetSize();
    auto& bin = w.sendBins[static_cast<int64_t>(Simulator::Now().GetSeconds() * 100)];
    if (seq < w.maxSent)
    {
        ++bin[1];
        PointerValue buf;
        g_sock[i]->GetAttribute("TxBuffer", buf);
        if (seq == buf.Get<TcpTxBuffer>()->HeadSequence())
        {
            ++bin[2];
            w.holRetx.insert(seq.GetValue());
        }
    }
    else
    {
        ++bin[0];
    }
    if (end > w.maxSent)
    {
        w.maxSent = end;
    }
    // Back-to-back trains (gap under 30 us), as burst_sizes.py measures the
    // kernel's skbs on the sender's veth.
    Time now = Simulator::Now();
    if (w.train > 0 && now - w.lastTx < MicroSeconds(30))
    {
        ++w.train;
    }
    else
    {
        if (w.train > 0 && w.lastTx.GetSeconds() >= g_winLo && w.lastTx.GetSeconds() < g_winHi)
        {
            w.trains.push_back(w.train);
        }
        w.train = 1;
    }
    w.lastTx = now;
}

/**
 * The sender received a segment: bin it by how many segments it newly
 * acknowledges (cumulatively), as linux-lab/compare/ack_sizes.py does for the
 * kernel's sender-side capture.
 * @param i flow index
 * @param h TCP header
 */
void
SenderRx(uint32_t i, Ptr<const Packet>, const TcpHeader& h, Ptr<const TcpSocketBase>)
{
    if (!(h.GetFlags() & TcpHeader::ACK))
    {
        return;
    }
    WindowStats& w = g_ws[i];
    SequenceNumber32 ack = h.GetAckNumber();
    double now = Simulator::Now().GetSeconds();
    if (now >= g_winLo && now < g_winHi)
    {
        uint32_t segs = ack > w.lastAck ? (ack - w.lastAck + g_segSize / 2) / g_segSize : 0;
        uint32_t b = segs <= 4 ? segs : segs <= 8 ? 5 : segs <= 16 ? 6 : 7;
        ++w.ackHist[b];
    }
    if (ack > w.lastAck)
    {
        w.lastAck = ack;
    }
}

void
TraceHighTx(uint32_t i, SequenceNumber32, SequenceNumber32 now)
{
    g_highTx[i] = now;
}

std::vector<FlowId>
    g_flowIdForFlow; //!< maps our flow index -> FlowMonitor's FlowId (0 = unassigned)

/**
 * @brief Split a comma-separated string into a vector, cycling the last
 * token if fewer tokens than nFlows were given (so a single value applies
 * to every flow, matching the CLI ergonomics used elsewhere in this repo's
 * scratch tools).
 */
std::vector<std::string>
SplitAndCycle(const std::string& s, uint32_t nFlows)
{
    std::vector<std::string> tokens;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        tokens.push_back(tok);
    }
    if (tokens.empty())
    {
        tokens.push_back("");
    }
    std::vector<std::string> out(nFlows);
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        out[i] = tokens[std::min<std::size_t>(i, tokens.size() - 1)];
    }
    return out;
}

void
TraceInternalState(uint32_t flowIdx, std::vector<double> fields)
{
    fields.resize(8, 0.0);
    g_reportedLossBytes[flowIdx] += fields[7];
    g_lastState[flowIdx] = fields;
}

/**
 * A packet reached the far end of the bottleneck: stop counting it against
 * the netem limit once it would have left netem's delay line.
 * @param extra netem delay minus the bottleneck link delay
 */
uint32_t SentListPkts(uint32_t i);

/**
 * Cumulatively ACKed payload bytes of a flow's sender (snd_una - ISN - 1):
 * the same goodput the Linux side measures from ACK numbers in the sender's
 * packet capture (linux-lab/vm/pcap2csv.py). FlowMonitor's rxBytes instead
 * counts every packet reaching the receiver, including out-of-order ones past
 * a loss, so it does not show the goodput stall of a loss recovery.
 * @param i flow index
 * @return bytes ACKed so far
 */
uint64_t
AckedBytes(uint32_t i)
{
    SentListPkts(i); // resolves g_sock[i]
    if (!g_sock[i])
    {
        return 0;
    }
    PointerValue buf;
    g_sock[i]->GetAttribute("TxBuffer", buf);
    auto tx = buf.Get<TcpTxBuffer>();
    if (!tx || tx->HeadSequence().GetValue() == 0)
    {
        return 0;
    }
    // ns-3's ISN is 0, so data starts at sequence 1. Payload never exceeds
    // 4 GB in these runs, so no wrap handling is needed.
    return tx->HeadSequence().GetValue() - 1;
}

void
NetemLeave(Time extra, Ptr<const Packet>)
{
    if (extra.IsZero())
    {
        --NetemLimitQueueDisc::s_inNetem;
        return;
    }
    Simulator::Schedule(extra, [] { --NetemLimitQueueDisc::s_inNetem; });
}

/// Per-flow socket values kept from traces, for the BBR info sampler.
struct SockVals
{
    bool connected{false};
    uint32_t cwnd{0};
    uint32_t inflight{0};
    uint32_t caState{0};
    Time srtt;
};

std::vector<SockVals> g_sv;
std::ofstream g_bbrCsv;

/**
 * Write one row per active BBRv3 flow with the fields Linux exports in
 * struct tcp_bbr_info, in the same columns and units (packets, bit/s, us)
 * as the Linux-side sock_diag poller (linux-lab/vm/bbrpoll.py).
 * @param interval sampling interval
 */
void
BbrInfoSample(Time interval)
{
    for (uint32_t i = 0; i < g_ccAlgo.size(); ++i)
    {
        double now = Simulator::Now().GetSeconds();
        if (now < g_startTime[i] || now >= g_stopTime[i])
        {
            continue;
        }
        SentListPkts(i); // resolves g_sock[i]
        if (!g_sock[i])
        {
            continue;
        }
        SockVals& v = g_sv[i];
        if (!v.connected)
        {
            v.connected = true;
            g_sock[i]->TraceConnectWithoutContext(
                "CongestionWindow",
                MakeBoundCallback(+[](SockVals* p, uint32_t, uint32_t n) { p->cwnd = n; }, &v));
            g_sock[i]->TraceConnectWithoutContext(
                "BytesInFlight",
                MakeBoundCallback(+[](SockVals* p, uint32_t, uint32_t n) { p->inflight = n; }, &v));
            g_sock[i]->TraceConnectWithoutContext(
                "CongState",
                MakeBoundCallback(
                    +[](SockVals* p,
                        TcpSocketState::TcpCongState_t,
                        TcpSocketState::TcpCongState_t n) { p->caState = n; },
                    &v));
            g_sock[i]->TraceConnectWithoutContext(
                "RTT",
                MakeBoundCallback(+[](SockVals* p, Time, Time n) { p->srtt = n; }, &v));
            if (g_rateCsv.is_open())
            {
                PointerValue ops;
                g_sock[i]->GetAttribute("CongestionOps", ops);
                ops.Get<TcpCongestionOps>()->TraceConnectWithoutContext(
                    "InternalState",
                    MakeBoundCallback(
                        +[](uint32_t flow, std::vector<double> st) {
                            double t = Simulator::Now().GetSeconds();
                            if (t >= 15 && t < 45)
                            {
                                g_rateCsv << t << "," << flow << "," << st[5] << "," << st[7]
                                          << "\n";
                            }
                        },
                        i));
            }
        }
        PointerValue cc;
        g_sock[i]->GetAttribute("CongestionOps", cc);
        auto bbr = DynamicCast<TcpBbrV3>(cc.Get<TcpCongestionOps>());
        if (!bbr)
        {
            continue;
        }
        TcpBbrV3::BbrInfo b = bbr->GetBbrInfo();
        // Linux's enum tcp_bbr_phase: STARTUP 1, DRAIN 2, PROBE_RTT 3, then
        // PROBE_BW UP 4, DOWN 5, CRUISE 6, REFILL 7.
        static const int bwPhase[] = {4, 5, 6, 7};
        int phase = b.mode == TcpBbrV3::BBR_STARTUP     ? 1
                    : b.mode == TcpBbrV3::BBR_DRAIN     ? 2
                    : b.mode == TcpBbrV3::BBR_PROBE_RTT ? 3
                                                        : bwPhase[b.phase];
        const double mss = 1448;
        auto pk = [mss](uint32_t bytes) {
            return bytes == 0xFFFFFFFF ? std::string("4294967295")
                                       : std::to_string(static_cast<uint32_t>(bytes / mss));
        };
        bool loUnset = b.bwLo.GetBitRate() == std::numeric_limits<uint64_t>::max();
        g_bbrCsv << now << "," << i << "," << v.caState << ","
                 << static_cast<uint32_t>(v.cwnd / mss) << ","
                 << static_cast<uint32_t>(v.inflight / mss) << ",0,0," << v.srtt.GetMicroSeconds()
                 << "," << b.mode << "," << phase << "," << b.bw.GetBitRate() << ","
                 << b.bwHi.GetBitRate() << ","
                 << (loUnset ? std::string("inf") : std::to_string(b.bwLo.GetBitRate())) << ","
                 << (b.minRtt == Time::Max() ? 0 : b.minRtt.GetMicroSeconds()) << ","
                 << b.pacingGain << "," << b.cwndGain << "," << pk(b.inflightLo) << ","
                 << pk(b.inflightHi) << "," << static_cast<uint32_t>(b.extraAcked / mss) << ","
                 << ((b.bwProbeSamples << 10) | (b.prevProbeTooHigh << 11) |
                     (b.stoppedRiskyProbe << 12) | (static_cast<uint32_t>(b.ackPhase) << 24))
                 << "\n";
    }
    Simulator::Schedule(interval, &BbrInfoSample, interval);
}

void
TraceBotSojourn(Time sojourn)
{
    g_lastBotWaitingTime = sojourn;
    g_lastBotArrival = Simulator::Now() - sojourn;
}

void
FlowStarted(uint32_t flowIdx [[maybe_unused]])
{
    g_numActiveFlows++;
}

void
FlowStopped(uint32_t flowIdx [[maybe_unused]])
{
    if (g_numActiveFlows > 0)
    {
        g_numActiveFlows--;
    }
}

/**
 * @brief Resolve each of our flow indices to FlowMonitor's FlowId, by
 * matching source port. Must run after the first packet of each flow has
 * been classified (i.e. after some simulated traffic), so this is called
 * lazily from Sample().
 */
void
ResolveFlowIds(const std::vector<uint16_t>& ports)
{
    FlowMonitor::FlowStatsContainer stats = g_monitor->GetFlowStats();
    for (const auto& [id, flowStats] : stats)
    {
        Ipv4FlowClassifier::FiveTuple tuple = g_classifier->FindFlow(id);
        for (uint32_t i = 0; i < ports.size(); ++i)
        {
            // Match the forward (sender -> receiver) data flow specifically:
            // its destination port is our well-known listening port. The
            // reverse ACK flow has that same port as its *source* port, so
            // matching on sourcePort here would silently track ACK-traffic
            // volume instead of real data throughput.
            if (tuple.destinationPort == ports[i] && g_flowIdForFlow[i] == 0)
            {
                g_flowIdForFlow[i] = id;
            }
        }
    }
}

uint32_t
SentListPkts(uint32_t i)
{
    if (!g_sock[i])
    {
        auto l4 = g_senders.Get(i)->GetObject<TcpL4Protocol>();
        ObjectVectorValue socks;
        l4->GetAttribute("SocketList", socks);
        if (socks.GetN() > 0)
        {
            g_sock[i] = DynamicCast<TcpSocketBase>(socks.Get(0));
            g_sock[i]->TraceConnectWithoutContext("HighestSequence",
                                                  MakeBoundCallback(&TraceHighTx, i));
            g_sock[i]->TraceConnectWithoutContext("Rx", MakeBoundCallback(&SenderRx, i));
        }
    }
    if (!g_sock[i])
    {
        return 0;
    }
    PointerValue buf;
    g_sock[i]->GetAttribute("TxBuffer", buf);
    auto tx = buf.Get<TcpTxBuffer>();
    // Sent-but-unacked bytes, expressed in full-size segments.
    if (!tx || g_highTx[i] < tx->HeadSequence())
    {
        return 0;
    }
    return (g_highTx[i] - tx->HeadSequence()) / 1448;
}

void
Sample(Time interval, Time simEnd, std::vector<uint16_t> ports)
{
    ResolveFlowIds(ports);
    g_monitor->CheckForLostPackets();
    FlowMonitor::FlowStatsContainer stats = g_monitor->GetFlowStats();

    double fairBandwidth =
        g_numActiveFlows > 0 ? g_bottleneckRate.GetBitRate() / static_cast<double>(g_numActiveFlows)
                             : 0.0;

    for (uint32_t i = 0; i < g_ccAlgo.size(); ++i)
    {
        // Include the sample taken at the stop time: it covers the flow's last
        // interval (nsperf's flows also deliver data up to their nominal end).
        bool active = Simulator::Now().GetSeconds() >= g_startTime[i] &&
                      Simulator::Now().GetSeconds() <= g_stopTime[i] + 1e-9;
        if (!active)
        {
            continue;
        }
        uint64_t txBytes = 0;
        uint64_t rxBytes = 0;
        uint64_t droppedBytes = 0;
        if (g_flowIdForFlow[i] != 0 && stats.count(g_flowIdForFlow[i]))
        {
            const auto& fs = stats.at(g_flowIdForFlow[i]);
            txBytes = fs.txBytes;
            rxBytes = fs.rxBytes;
            droppedBytes = std::accumulate(fs.bytesDropped.begin(), fs.bytesDropped.end(), 0ULL);
        }

        g_csv << Simulator::Now().GetSeconds() << "," << i << "," << g_ccAlgo[i];
        for (double v : g_lastState[i])
        {
            g_csv << "," << v;
        }
        g_csv << "," << g_lastBotArrival.GetSeconds() << "," << g_lastBotWaitingTime.GetSeconds()
              << "," << g_numActiveFlows << "," << fairBandwidth << "," << txBytes << "," << rxBytes
              << "," << droppedBytes << "," << static_cast<double>(std::clock()) / CLOCKS_PER_SEC
              << "," << Simulator::GetEventCount() << "," << SentListPkts(i) << "," << AckedBytes(i)
              << "\n";
    }

    if (Simulator::Now() + interval <= simEnd)
    {
        Simulator::Schedule(interval, &Sample, interval, simEnd, ports);
    }
}

} // namespace

int
main(int argc, char* argv[])
{
    uint32_t nFlows = 1;
    std::string ccAlgoArg = "ns3::TcpBbrV3";
    std::string accessDelayArg = "1ms";
    std::string startTimeArg = "0";
    std::string stopTimeArg;
    DataRate bottleneckRate("20Mbps");
    Time bottleneckDelay = MilliSeconds(20);
    uint32_t bottleneckQueuePackets = 100;
    DataRate accessRate("100Mbps");
    Time duration = Seconds(50);
    Time sampleInterval = MilliSeconds(100);
    double lossRate = 0.0;
    Time stepAtTime = Seconds(0);
    DataRate steppedRate("0bps");
    std::string outFile = "bbrv3-harness.csv";
    uint32_t segmentSize = 1448;
    uint32_t sockBufBytes = 64 * 1024 * 1024;
    uint32_t netemLimit = 0;
    Time netemDelay = MilliSeconds(0);
    Time logFrom = Seconds(0);
    Time bbrInfoInterval = Seconds(0);
    std::string logComponents = "TcpBbrV3";
    Time hostJitter = Seconds(0);
    bool dropLog = false;
    bool rateLog = false;

    CommandLine cmd;
    cmd.AddValue("nFlows", "Number of flows sharing the bottleneck", nFlows);
    cmd.AddValue("ccAlgo",
                 "Comma-separated per-flow congestion control TypeId "
                 "(cycles if fewer than nFlows given)",
                 ccAlgoArg);
    cmd.AddValue("accessDelay", "Comma-separated per-flow one-way access delay", accessDelayArg);
    cmd.AddValue("startTime", "Comma-separated per-flow start time (s)", startTimeArg);
    cmd.AddValue("stopTime",
                 "Comma-separated per-flow stop time (s); defaults to --duration for all",
                 stopTimeArg);
    cmd.AddValue("bottleneckRate", "Bottleneck link data rate", bottleneckRate);
    cmd.AddValue("bottleneckDelay", "Bottleneck link one-way delay", bottleneckDelay);
    cmd.AddValue("bottleneckQueuePackets",
                 "Bottleneck queue disc capacity, in packets",
                 bottleneckQueuePackets);
    cmd.AddValue("accessRate", "Access link data rate", accessRate);
    cmd.AddValue("duration", "Simulation duration", duration);
    cmd.AddValue("sampleInterval", "CSV sampling interval", sampleInterval);
    cmd.AddValue("lossRate", "Uniform per-packet random loss rate at each receiver", lossRate);
    cmd.AddValue("stepAtTime", "Time to step the bottleneck rate (0 = no step)", stepAtTime);
    cmd.AddValue("steppedRate", "Bottleneck rate to step to at --stepAtTime", steppedRate);
    cmd.AddValue("outFile", "CSV output file path", outFile);
    cmd.AddValue("segmentSize", "TCP segment size (bytes)", segmentSize);
    cmd.AddValue("sockBufBytes", "TCP send/receive buffer size (bytes)", sockBufBytes);
    cmd.AddValue("randomizeStreams",
                 "Give each BBRv3 flow's random-probe-timing RNG stream a distinct value "
                 "(100+flow_id) instead of the shared TcpBbrV3::Stream default",
                 g_randomizeStreams);
    cmd.AddValue("netemLimit",
                 "If > 0, bottleneck queue limit in packets counting the packets on the "
                 "wire too, as netem does (nsperf: one-way BDP + buffer); 0 = plain FIFO",
                 netemLimit);
    cmd.AddValue("netemDelay",
                 "With --netemLimit, how long after entering the queue (excluding queueing "
                 "and transmission) a packet stops counting; nsperf's netem delay is RTT/2",
                 netemDelay);
    cmd.AddValue("logFrom",
                 "If > 0, enable LOG_LEVEL_ALL for --logComponents from this time on "
                 "(debug builds only)",
                 logFrom);
    cmd.AddValue(
        "bbrInfoInterval",
        "If > 0, also write <outFile>.bbr.csv with TcpBbrV3::GetBbrInfo() of every "
        "flow at this interval (same columns as linux-lab/vm/bbrtrace.bt; v3_flags has the "
        "bit layout of the Linux struct bbr word holding ecn_eligible)",
        bbrInfoInterval);
    cmd.AddValue("hostJitter",
                 "If > 0, every sender and receiver holds each outgoing packet for a uniform "
                 "random 0..hostJitter (in order), like real host timing noise",
                 hostJitter);
    cmd.AddValue("dropLog",
                 "Write <outFile>.drops.csv: t,flow,seq,retx per bottleneck drop",
                 dropLog);
    cmd.AddValue("rateLog",
                 "Write <outFile>.rate.csv: t,flow,delivery_bps,lost_bytes per BBRv3 ACK, "
                 "15-45 s (needs --bbrInfoInterval)",
                 rateLog);
    cmd.AddValue("logComponents", "Comma-separated log components for --logFrom", logComponents);
    cmd.Parse(argc, argv);
    if (logFrom.IsStrictlyPositive())
    {
        Simulator::Schedule(logFrom, [logComponents]() {
            std::stringstream ss(logComponents);
            std::string c;
            while (std::getline(ss, c, ','))
            {
                LogComponentEnable(c.c_str(), LogLevel(LOG_LEVEL_ALL | LOG_PREFIX_ALL));
            }
        });
    }

    if (stopTimeArg.empty())
    {
        stopTimeArg = std::to_string(duration.GetSeconds());
    }

    g_bottleneckRate = bottleneckRate;
    g_ccAlgo = SplitAndCycle(ccAlgoArg, nFlows);
    std::vector<std::string> accessDelayStr = SplitAndCycle(accessDelayArg, nFlows);
    std::vector<std::string> startTimeStr = SplitAndCycle(startTimeArg, nFlows);
    std::vector<std::string> stopTimeStr = SplitAndCycle(stopTimeArg, nFlows);
    g_startTime.resize(nFlows);
    g_stopTime.resize(nFlows);
    g_lastState.assign(nFlows, std::vector<double>(8, 0.0));
    g_reportedLossBytes.assign(nFlows, 0.0);
    g_flowIdForFlow.assign(nFlows, 0);
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        g_startTime[i] = std::stod(startTimeStr[i]);
        g_stopTime[i] = std::stod(stopTimeStr[i]);
    }

    // Match a Linux sender on a 1500-byte MTU path (MSS 1448 with timestamps),
    // and keep socket buffers large enough that high-BDP, deep-buffer
    // configurations are never sender/receiver-window limited.
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(segmentSize));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(sockBufBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(sockBufBytes));

    NodeContainer senders;
    NodeContainer receivers;
    NodeContainer routers;
    senders.Create(nFlows);
    g_senders = senders;
    g_sock.assign(nFlows, nullptr);
    g_ws.assign(nFlows, WindowStats{});
    g_segSize = segmentSize;
    JitterQueueDisc::s_jitter = hostJitter;
    g_highTx.assign(nFlows, SequenceNumber32(0));
    receivers.Create(nFlows);
    routers.Create(2);

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(receivers);
    internet.Install(routers);

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", DataRateValue(bottleneckRate));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(bottleneckDelay));
    bottleneckLink.SetQueue("ns3::DropTailQueue<Packet>",
                            "MaxSize",
                            QueueSizeValue(QueueSize("2p")));
    NetDeviceContainer bottleneck = bottleneckLink.Install(routers.Get(0), routers.Get(1));

    TrafficControlHelper tch;
    if (netemLimit > 0)
    {
        NetemLimitQueueDisc::s_limit = netemLimit;
        tch.SetRootQueueDisc("ns3::NetemLimitQueueDisc",
                             "MaxSize",
                             QueueSizeValue(QueueSize(std::to_string(netemLimit) + "p")));
        // A packet leaves netem one netem delay after it finished transmission;
        // here it reaches the far router after bottleneckDelay, so count it for
        // the remainder.
        Time extra = netemDelay > bottleneckDelay ? netemDelay - bottleneckDelay : Time(0);
        bottleneck.Get(1)->TraceConnectWithoutContext("MacRx",
                                                      MakeBoundCallback(&NetemLeave, extra));
    }
    else
    {
        tch.SetRootQueueDisc(
            "ns3::FifoQueueDisc",
            "MaxSize",
            QueueSizeValue(QueueSize(std::to_string(bottleneckQueuePackets) + "p")));
    }
    QueueDiscContainer qdiscs = tch.Install(bottleneck.Get(0));
    qdiscs.Get(0)->TraceConnectWithoutContext("SojournTime", MakeCallback(&TraceBotSojourn));
    qdiscs.Get(0)->TraceConnectWithoutContext("Enqueue",
                                              MakeBoundCallback(&BottleneckArrival, false));
    qdiscs.Get(0)->TraceConnectWithoutContext("Drop", MakeBoundCallback(&BottleneckArrival, true));
    if (rateLog)
    {
        g_rateCsv.open(outFile + ".rate.csv");
        g_rateCsv << "t,flow,delivery_bps,lost_bytes\n";
    }
    if (dropLog)
    {
        g_dropCsv.open(outFile + ".drops.csv");
        g_dropCsv << "t,flow,seq,retx,hol\n";
    }

    Ipv4AddressHelper routerIp("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer routerIfaces = routerIp.Assign(bottleneck);

    std::vector<uint16_t> ports(nFlows);
    std::vector<Ptr<PacketSink>> sinks(nFlows);

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        PointToPointHelper accessLink;
        accessLink.SetDeviceAttribute("DataRate", DataRateValue(accessRate));
        accessLink.SetChannelAttribute("Delay", TimeValue(Time(accessDelayStr[i])));

        NetDeviceContainer senderEdge = accessLink.Install(senders.Get(i), routers.Get(0));
        NetDeviceContainer receiverEdge = accessLink.Install(routers.Get(1), receivers.Get(i));
        if (dropLog)
        {
            senderEdge.Get(0)->TraceConnectWithoutContext("MacTx",
                                                          MakeBoundCallback(&SenderMacTx, i));
        }
        if (hostJitter.IsStrictlyPositive())
        {
            // Before address assignment, which would otherwise install fq_codel.
            TrafficControlHelper jt;
            jt.SetRootQueueDisc("ns3::JitterQueueDisc");
            jt.Install(senderEdge.Get(0));
            jt.Install(receiverEdge.Get(1));
        }

        Ipv4AddressHelper senderIp;
        senderIp.SetBase(Ipv4Address(("10." + std::to_string(10 + i) + ".1.0").c_str()),
                         Ipv4Mask("255.255.255.0"));
        senderIp.Assign(senderEdge);
        Ipv4AddressHelper receiverIp;
        receiverIp.SetBase(Ipv4Address(("10." + std::to_string(60 + i) + ".1.0").c_str()),
                           Ipv4Mask("255.255.255.0"));
        Ipv4InterfaceContainer receiverIfaces = receiverIp.Assign(receiverEdge);

        if (lossRate > 0.0)
        {
            Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
            em->SetAttribute("ErrorRate", DoubleValue(lossRate));
            em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
            receivers.Get(i)->GetDevice(1)->SetAttribute("ReceiveErrorModel", PointerValue(em));
        }

        std::ostringstream path;
        path << "/NodeList/" << senders.Get(i)->GetId() << "/$ns3::TcpL4Protocol/SocketType";
        Config::Set(path.str(), StringValue(g_ccAlgo[i]));

        ports[i] = static_cast<uint16_t>(9000 + i);
        BulkSendHelper source("ns3::TcpSocketFactory",
                              InetSocketAddress(receiverIfaces.GetAddress(1), ports[i]));
        source.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer sourceApp = source.Install(senders.Get(i));
        sourceApp.Start(Seconds(g_startTime[i]));
        sourceApp.Stop(Seconds(g_stopTime[i]));

        PacketSinkHelper sink("ns3::TcpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), ports[i]));
        ApplicationContainer sinkApp = sink.Install(receivers.Get(i));
        sinkApp.Start(Seconds(g_startTime[i]));
        sinkApp.Stop(Seconds(g_stopTime[i]));
        sinks[i] = DynamicCast<PacketSink>(sinkApp.Get(0));

        Simulator::Schedule(Seconds(g_startTime[i]) + MilliSeconds(1), &FlowStarted, i);
        Simulator::Schedule(Seconds(g_stopTime[i]), &FlowStopped, i);

        if (g_randomizeStreams)
        {
            Simulator::Schedule(Seconds(g_startTime[i]) + MilliSeconds(1), [i, senders]() {
                std::ostringstream streamPath;
                streamPath << "/NodeList/" << senders.Get(i)->GetId()
                           << "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpBbrV3/"
                              "Stream";
                Config::SetFailSafe(streamPath.str(), UintegerValue(100 + i));
            });
        }

        Simulator::Schedule(Seconds(g_startTime[i]) + MilliSeconds(2), [i, senders]() {
            std::ostringstream tracePath;
            tracePath << "/NodeList/" << senders.Get(i)->GetId()
                      << "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpBbrV3/"
                         "InternalState";
            Config::ConnectWithoutContextFailSafe(tracePath.str(),
                                                  MakeBoundCallback(&TraceInternalState, i));
        });
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    if (stepAtTime.IsStrictlyPositive() && steppedRate.GetBitRate() > 0)
    {
        Ptr<PointToPointNetDevice> dev = DynamicCast<PointToPointNetDevice>(bottleneck.Get(0));
        Simulator::Schedule(stepAtTime, [dev, steppedRate]() {
            dev->SetAttribute("DataRate", DataRateValue(steppedRate));
            g_bottleneckRate = steppedRate;
        });
    }

    FlowMonitorHelper flowmonHelper;
    g_monitor = flowmonHelper.InstallAll();
    g_classifier = DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());

    g_csv.open(outFile);
    g_csv << "sample_time,flow_id,cc_algo,state0,state1,state2,state3,state4,state5,state6,"
             "state7,bot_arrival_time,bot_waiting_time,num_active_flows,fair_bandwidth_bps,"
             "tx_bytes,rx_bytes,dropped_bytes,wall_s,events,sent_list_pkts,acked_bytes\n";

    Simulator::Schedule(sampleInterval, &Sample, sampleInterval, duration, ports);
    g_sv.assign(nFlows, SockVals{});
    if (bbrInfoInterval.IsStrictlyPositive())
    {
        g_bbrCsv.open(outFile + ".bbr.csv");
        g_bbrCsv << "t,flow,ca_state,cwnd,unacked,lost,retrans,rtt_us,mode,phase,bw_bps,"
                    "bw_hi_bps,bw_lo_bps,min_rtt_us,pacing_gain,cwnd_gain,inflight_lo,"
                    "inflight_hi,extra_acked,v3_flags\n";
        Simulator::Schedule(bbrInfoInterval, &BbrInfoSample, bbrInfoInterval);
    }

    Simulator::Stop(duration + Seconds(1));
    Simulator::Run();

    g_monitor->CheckForLostPackets();
    for (const auto& [id, fs] : g_monitor->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = g_classifier->FindFlow(id);
        std::cout << "flow " << id << " " << t.sourceAddress << ":" << t.sourcePort << " -> "
                  << t.destinationAddress << ":" << t.destinationPort
                  << " txPackets=" << fs.txPackets << " rxPackets=" << fs.rxPackets
                  << " lostPackets=" << fs.lostPackets << " txBytes=" << fs.txBytes
                  << " rxBytes=" << fs.rxBytes << " forwardedPkts=" << fs.timesForwarded
                  << std::endl;
    }

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        std::cout << "flow index " << i
                  << " loss bytes reported to CongControl=" << g_reportedLossBytes[i] << std::endl;
        const WindowStats& w = g_ws[i];
        std::cout << "flow index " << i << " bottleneck_drops=" << w.drops
                  << " retx_drops=" << w.retxDrops << " ack_hist_15_45s(0,1,2,3,4,5-8,9-16,17+)=";
        for (std::size_t b = 0; b < w.ackHist.size(); ++b)
        {
            std::cout << (b ? "," : "") << w.ackHist[b];
        }
        std::cout << std::endl;
        if (!w.trains.empty())
        {
            std::vector<uint32_t> t = w.trains;
            std::sort(t.begin(), t.end());
            uint64_t tot = 0;
            uint64_t sq = 0;
            for (uint32_t x : t)
            {
                tot += x;
                sq += static_cast<uint64_t>(x) * x;
            }
            auto pct = [&](double q) {
                uint64_t acc = 0;
                for (uint32_t x : t)
                {
                    acc += x;
                    if (acc >= q * tot)
                    {
                        return x;
                    }
                }
                return t.back();
            };
            std::cout << "flow index " << i
                      << " trains_15_45s wmean=" << static_cast<double>(sq) / tot
                      << " p50=" << pct(0.5) << " p90=" << pct(0.9) << " max=" << t.back()
                      << std::endl;
        }
    }

    if (dropLog)
    {
        std::ofstream sends(outFile + ".sends.csv");
        sends << "t,flow,new,retx,hol_retx\n";
        for (uint32_t i = 0; i < nFlows; ++i)
        {
            for (const auto& [b, c] : g_ws[i].sendBins)
            {
                sends << b / 100.0 << "," << i << "," << c[0] << "," << c[1] << "," << c[2] << "\n";
            }
        }
    }
    Simulator::Destroy();

    g_csv.close();
    std::cout << "Wrote " << outFile << std::endl;

    return 0;
}
