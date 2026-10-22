/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/boolean.h"
#include "ns3/bulk-send-helper.h"
#include "ns3/config.h"
#include "ns3/data-rate.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/error-model.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/log.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/packet-sink.h"
#include "ns3/point-to-point-dumbbell.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/pointer.h"
#include "ns3/queue-size.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/test.h"
#include "ns3/traffic-control-helper.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <numeric>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpBbrV3TopologyTestSuite");

namespace
{

/**
 * @brief Force every new TCP socket created on a node to use a given
 * congestion control algorithm.
 *
 * @param node the node whose future sockets should use \p typeName
 * @param typeName fully-qualified TypeId name (e.g. "ns3::TcpBbrV3")
 */
void
SetCongestionControl(Ptr<Node> node, const std::string& typeName)
{
    std::ostringstream path;
    path << "/NodeList/" << node->GetId() << "/$ns3::TcpL4Protocol/SocketType";
    Config::Set(path.str(), StringValue(typeName));
}

/**
 * @brief Install a saturated (MaxBytes=0, i.e. run until stopped) bulk sender
 * on \p src and a matching sink on \p dst.
 *
 * @param src sender node (its congestion control should already be set via
 *            SetCongestionControl() before calling this)
 * @param dst receiver node
 * @param dstAddress IPv4 address of \p dst reachable from \p src
 * @param port TCP port to use (must be unique per concurrent flow)
 * @param start when the flow should start
 * @param stop when the flow should stop
 * @return the installed PacketSink, whose GetTotalRx() gives bytes received
 */
Ptr<PacketSink>
InstallBulkFlow(Ptr<Node> src,
               Ptr<Node> dst,
               Ipv4Address dstAddress,
               uint16_t port,
               Time start,
               Time stop)
{
    BulkSendHelper source("ns3::TcpSocketFactory", InetSocketAddress(dstAddress, port));
    source.SetAttribute("MaxBytes", UintegerValue(0));
    ApplicationContainer sourceApp = source.Install(src);
    sourceApp.Start(start);
    sourceApp.Stop(stop);

    PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sink.Install(dst);
    sinkApp.Start(start);
    sinkApp.Stop(stop);

    return DynamicCast<PacketSink>(sinkApp.Get(0));
}

/**
 * @brief A single sender - two routers - single receiver topology, with a
 * traffic-control FIFO queue disc (of configurable size) on the router
 * interface facing the bottleneck link, in the sender-to-receiver direction.
 */
struct SingleBottleneckTopology
{
    NodeContainer sender;              //!< Sender leaf node
    NodeContainer receiver;            //!< Receiver leaf node
    NodeContainer routers;             //!< The two bottleneck routers
    Ptr<PointToPointNetDevice> bottleneckDevice; //!< R1's device on the R1->R2 link
    Ipv4Address receiverAddress;       //!< Receiver's IPv4 address
};

/**
 * @brief Build a SingleBottleneckTopology.
 *
 * @param bottleneckRate bottleneck link data rate
 * @param bottleneckDelay bottleneck link one-way propagation delay
 * @param bottleneckQueuePackets bottleneck queue disc capacity, in packets
 * @param accessRate access (sender/receiver-to-router) link data rate
 * @param accessDelay access link one-way propagation delay
 * @return the constructed topology
 */
SingleBottleneckTopology
BuildSingleBottleneckTopology(DataRate bottleneckRate,
                              Time bottleneckDelay,
                              uint32_t bottleneckQueuePackets,
                              DataRate accessRate = DataRate("100Mbps"),
                              Time accessDelay = MilliSeconds(1))
{
    SingleBottleneckTopology topo;
    topo.sender.Create(1);
    topo.receiver.Create(1);
    topo.routers.Create(2);

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", DataRateValue(bottleneckRate));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(bottleneckDelay));
    // The device queue is kept minimal; the FIFO queue disc installed below is
    // the buffer that actually governs bottleneck queueing behavior.
    bottleneckLink.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize("2p")));

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", DataRateValue(accessRate));
    accessLink.SetChannelAttribute("Delay", TimeValue(accessDelay));

    NetDeviceContainer senderEdge = accessLink.Install(topo.sender.Get(0), topo.routers.Get(0));
    NetDeviceContainer bottleneck = bottleneckLink.Install(topo.routers.Get(0), topo.routers.Get(1));
    NetDeviceContainer receiverEdge = accessLink.Install(topo.routers.Get(1), topo.receiver.Get(0));

    InternetStackHelper internet;
    internet.Install(topo.sender);
    internet.Install(topo.receiver);
    internet.Install(topo.routers);

    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FifoQueueDisc",
                         "MaxSize",
                         QueueSizeValue(QueueSize(std::to_string(bottleneckQueuePackets) + "p")));
    tch.Install(bottleneck.Get(0));

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    ipv4.Assign(senderEdge);
    ipv4.NewNetwork();
    ipv4.Assign(bottleneck);
    ipv4.NewNetwork();
    Ipv4InterfaceContainer receiverIfaces = ipv4.Assign(receiverEdge);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    topo.receiverAddress = receiverIfaces.GetAddress(1);
    topo.bottleneckDevice = DynamicCast<PointToPointNetDevice>(bottleneck.Get(0));

    return topo;
}

} // namespace

/**
 * @ingroup internet-test
 *
 * @brief Coexistence/fairness: a BBRv3 flow and a Cubic flow share one
 * bottleneck. BBRv3 is known to be able to starve loss-based competitors if
 * its inflight model isn't properly bounded by the loss/ECN signals it
 * observes; this checks that neither flow is left with a negligible share.
 */
class TcpBbrV3CoexistenceTest : public TestCase
{
  public:
    TcpBbrV3CoexistenceTest();

  private:
    void DoRun() override;
};

TcpBbrV3CoexistenceTest::TcpBbrV3CoexistenceTest()
    : TestCase("BBRv3 must coexist with a competing Cubic flow on a shared bottleneck")
{
}

void
TcpBbrV3CoexistenceTest::DoRun()
{
    const DataRate bottleneckRate("10Mbps");
    const Time simDuration = Seconds(45);

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", DataRateValue(bottleneckRate));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(20)));
    bottleneckLink.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize("2p")));

    PointToPointHelper edgeLink;
    edgeLink.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Mbps")));
    edgeLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(1)));

    PointToPointDumbbellHelper dumbbell(2, edgeLink, 2, edgeLink, bottleneckLink);

    InternetStackHelper stack;
    dumbbell.InstallStack(stack);

    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FifoQueueDisc", "MaxSize", QueueSizeValue(QueueSize("50p")));
    tch.Install(dumbbell.GetLeft()->GetDevice(0));

    Ipv4AddressHelper leftIp("10.1.1.0", "255.255.255.0");
    Ipv4AddressHelper rightIp("10.2.1.0", "255.255.255.0");
    Ipv4AddressHelper routerIp("10.3.1.0", "255.255.255.0");
    dumbbell.AssignIpv4Addresses(leftIp, rightIp, routerIp);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    SetCongestionControl(dumbbell.GetLeft(0), "ns3::TcpBbrV3");
    SetCongestionControl(dumbbell.GetLeft(1), "ns3::TcpCubic");

    Ptr<PacketSink> bbrSink = InstallBulkFlow(dumbbell.GetLeft(0),
                                              dumbbell.GetRight(0),
                                              dumbbell.GetRightIpv4Address(0),
                                              5000,
                                              Seconds(0),
                                              simDuration);
    Ptr<PacketSink> cubicSink = InstallBulkFlow(dumbbell.GetLeft(1),
                                                dumbbell.GetRight(1),
                                                dumbbell.GetRightIpv4Address(1),
                                                5001,
                                                Seconds(0),
                                                simDuration);

    Simulator::Stop(simDuration + Seconds(1));
    Simulator::Run();

    uint64_t bbrBytes = bbrSink->GetTotalRx();
    uint64_t cubicBytes = cubicSink->GetTotalRx();

    Simulator::Destroy();

    // Neither flow should be left with a negligible share of the bottleneck:
    // require each to have delivered at least 5% of what the bottleneck could
    // carry alone over the run. This is a deliberately loose bound -- it is
    // not a fairness-index check -- aimed only at catching a flow being
    // starved outright.
    double maxPossibleBytes = bottleneckRate.GetBitRate() * simDuration.GetSeconds() / 8.0;
    double minAcceptableShare = 0.05 * maxPossibleBytes;

    NS_TEST_ASSERT_MSG_GT(static_cast<double>(bbrBytes),
                          minAcceptableShare,
                          "BBRv3 flow received a negligible share of the bottleneck");
    NS_TEST_ASSERT_MSG_GT(static_cast<double>(cubicBytes),
                          minAcceptableShare,
                          "Competing Cubic flow was starved by the BBRv3 flow");
}

/**
 * @ingroup internet-test
 *
 * @brief Dynamic bottleneck: available bandwidth steps up mid-flow. BBRv3
 * should re-probe and increase its delivery rate accordingly, rather than
 * staying capped at its original bandwidth estimate.
 */
class TcpBbrV3DynamicBottleneckTest : public TestCase
{
  public:
    TcpBbrV3DynamicBottleneckTest();

  private:
    void DoRun() override;
    /**
     * @brief Record a (time, totalRx) sample.
     */
    void Sample();

    Ptr<PacketSink> m_sink;                       //!< The receiving sink
    std::vector<std::pair<Time, uint64_t>> m_samples; //!< (time, cumulative bytes) samples
};

TcpBbrV3DynamicBottleneckTest::TcpBbrV3DynamicBottleneckTest()
    : TestCase("BBRv3 must adapt to a mid-flow bottleneck bandwidth increase")
{
}

void
TcpBbrV3DynamicBottleneckTest::Sample()
{
    m_samples.emplace_back(Simulator::Now(), m_sink->GetTotalRx());
    if (Simulator::Now() < Seconds(59))
    {
        Simulator::Schedule(Seconds(1), &TcpBbrV3DynamicBottleneckTest::Sample, this);
    }
}

void
TcpBbrV3DynamicBottleneckTest::DoRun()
{
    const DataRate initialRate("2Mbps");
    const DataRate steppedUpRate("8Mbps");
    const Time stepTime = Seconds(30);
    const Time simDuration = Seconds(60);

    SingleBottleneckTopology topo =
        BuildSingleBottleneckTopology(initialRate, MilliSeconds(20), 50);

    SetCongestionControl(topo.sender.Get(0), "ns3::TcpBbrV3");
    m_sink = InstallBulkFlow(topo.sender.Get(0),
                             topo.receiver.Get(0),
                             topo.receiverAddress,
                             6000,
                             Seconds(0),
                             simDuration);

    Simulator::Schedule(stepTime, [dev = topo.bottleneckDevice, steppedUpRate]() {
        dev->SetAttribute("DataRate", DataRateValue(steppedUpRate));
    });

    Simulator::Schedule(Seconds(1), &TcpBbrV3DynamicBottleneckTest::Sample, this);

    Simulator::Stop(simDuration + Seconds(1));
    Simulator::Run();
    Simulator::Destroy();

    // Average throughput just before the step, vs. in the last 10s of the run
    // (well after the step, giving BBRv3 time to re-probe and converge).
    uint64_t bytesAt25 = 0;
    uint64_t bytesAt29 = 0;
    uint64_t bytesAt50 = 0;
    uint64_t bytesAt59 = 0;
    for (const auto& [t, bytes] : m_samples)
    {
        if (t == Seconds(25))
        {
            bytesAt25 = bytes;
        }
        if (t == Seconds(29))
        {
            bytesAt29 = bytes;
        }
        if (t == Seconds(50))
        {
            bytesAt50 = bytes;
        }
        if (t == Seconds(59))
        {
            bytesAt59 = bytes;
        }
    }

    double preStepThroughput = (bytesAt29 - bytesAt25) / 4.0;   // bytes/s
    double postStepThroughput = (bytesAt59 - bytesAt50) / 9.0; // bytes/s

    NS_TEST_ASSERT_MSG_GT(postStepThroughput,
                          preStepThroughput * 2.0,
                          "BBRv3 did not meaningfully increase its rate after the bottleneck "
                          "bandwidth quadrupled");
}

/**
 * @ingroup internet-test
 *
 * @brief Long-fat network: a high-BDP path (large bottleneck bandwidth,
 * large RTT). BBRv3 should still achieve good utilization, which is the
 * core problem BBR-style congestion control targets relative to
 * traditional loss-based algorithms.
 */
class TcpBbrV3LongFatNetworkTest : public TestCase
{
  public:
    TcpBbrV3LongFatNetworkTest();

  private:
    void DoRun() override;
};

TcpBbrV3LongFatNetworkTest::TcpBbrV3LongFatNetworkTest()
    : TestCase("BBRv3 must achieve good utilization on a long-fat (high BDP) network")
{
}

void
TcpBbrV3LongFatNetworkTest::DoRun()
{
    const DataRate bottleneckRate("20Mbps");
    const Time simDuration = Seconds(50);

    // Bump the socket buffers so the window itself isn't the bottleneck: BDP
    // here is roughly 20Mbps * 0.2s / 8 =~ 0.5MB. These match the values used
    // by examples/tcp/tcp-bbr-example.cc.
    //
    // A higher bottleneckRate here (50Mbps was tried originally) reliably
    // aborts the process a few seconds in, deep inside
    // PacketMetadata::IsStateOk() via Packet::AddAtEnd() in
    // TcpRxBuffer::Extract() -- Socket::RecvFrom(Address&) (which
    // PacketSink::HandleRead() uses) always requests an uncapped read, and
    // enough packets end up merged into one Packet via repeated AddAtEnd()
    // to overflow ns-3's packet-metadata serialization. This reproduced
    // identically regardless of the SndBufSize/RcvBufSize values above, so
    // it's a core ns-3 scaling limitation independent of this test's buffer
    // sizing, not a BBRv3 bug -- keeping bottleneckRate here modest avoids it.
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(4194304));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(6291456));

    SingleBottleneckTopology topo =
        BuildSingleBottleneckTopology(bottleneckRate, MilliSeconds(100), 250);

    SetCongestionControl(topo.sender.Get(0), "ns3::TcpBbrV3");
    Ptr<PacketSink> sink = InstallBulkFlow(topo.sender.Get(0),
                                           topo.receiver.Get(0),
                                           topo.receiverAddress,
                                           6100,
                                           Seconds(0),
                                           simDuration);

    Simulator::Stop(simDuration + Seconds(1));
    Simulator::Run();
    uint64_t totalBytes = sink->GetTotalRx();
    Simulator::Destroy();

    double achievedRate = 8.0 * totalBytes / simDuration.GetSeconds();
    double utilization = achievedRate / bottleneckRate.GetBitRate();

    NS_TEST_ASSERT_MSG_GT(utilization,
                          0.5,
                          "BBRv3 achieved less than 50% utilization of a high-BDP bottleneck");
}

/**
 * @ingroup internet-test
 *
 * @brief Lossy/wireless-style random loss: uncorrelated packet loss on the
 * last hop, unrelated to queue occupancy. BBRv3's model-based approach
 * should be far less sensitive to this than a purely loss-reactive
 * algorithm, which is one of BBR's headline advantages.
 */
class TcpBbrV3RandomLossTest : public TestCase
{
  public:
    TcpBbrV3RandomLossTest();

  private:
    void DoRun() override;
    /**
     * @brief Run one trial with the given congestion control and random loss
     * rate, and return total bytes delivered.
     * @param congestionControl fully-qualified TypeId name
     * @param lossRate per-packet loss probability
     * @return bytes delivered to the sink over the trial
     */
    uint64_t RunTrial(const std::string& congestionControl, double lossRate);

    Time m_simDuration{Seconds(30)}; //!< Duration of each trial
};

TcpBbrV3RandomLossTest::TcpBbrV3RandomLossTest()
    : TestCase("BBRv3 must substantially outperform a loss-based CCA under random loss")
{
}

uint64_t
TcpBbrV3RandomLossTest::RunTrial(const std::string& congestionControl, double lossRate)
{
    SingleBottleneckTopology topo =
        BuildSingleBottleneckTopology(DataRate("20Mbps"), MilliSeconds(20), 50);

    Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
    em->SetAttribute("ErrorRate", DoubleValue(lossRate));
    em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
    topo.receiver.Get(0)->GetDevice(0)->SetAttribute("ReceiveErrorModel", PointerValue(em));

    SetCongestionControl(topo.sender.Get(0), congestionControl);
    Ptr<PacketSink> sink = InstallBulkFlow(topo.sender.Get(0),
                                           topo.receiver.Get(0),
                                           topo.receiverAddress,
                                           6200,
                                           Seconds(0),
                                           m_simDuration);

    Simulator::Stop(m_simDuration + Seconds(1));
    Simulator::Run();
    uint64_t totalBytes = sink->GetTotalRx();
    Simulator::Destroy();
    return totalBytes;
}

void
TcpBbrV3RandomLossTest::DoRun()
{
    const double lossRate = 0.01; // 1% random loss, unrelated to congestion

    uint64_t bbrBytes = RunTrial("ns3::TcpBbrV3", lossRate);
    uint64_t cubicBytes = RunTrial("ns3::TcpCubic", lossRate);

    NS_TEST_ASSERT_MSG_GT(static_cast<double>(bbrBytes),
                          1.5 * static_cast<double>(cubicBytes),
                          "BBRv3 did not substantially outperform Cubic under 1% random loss");
}

/**
 * @ingroup internet-test
 *
 * @brief Bufferbloat check: a single BBRv3 flow through a bottleneck with a
 * deliberately oversized queue. Unlike a purely loss-reactive algorithm,
 * BBRv3 should keep queueing delay low instead of filling the buffer.
 */
class TcpBbrV3BufferbloatTest : public TestCase
{
  public:
    TcpBbrV3BufferbloatTest();

  private:
    void DoRun() override;
    /**
     * @brief Record an RTT sample once the connection has had time to settle.
     * @param oldRtt previous RTT (unused)
     * @param newRtt newly measured RTT
     */
    void RttTrace(Time oldRtt, Time newRtt);

    std::vector<Time> m_rttSamples; //!< Steady-state RTT samples
};

TcpBbrV3BufferbloatTest::TcpBbrV3BufferbloatTest()
    : TestCase("BBRv3 must keep queueing delay low even with a deliberately oversized buffer")
{
}

void
TcpBbrV3BufferbloatTest::RttTrace(Time oldRtt [[maybe_unused]], Time newRtt)
{
    // Only collect steady-state samples, well after STARTUP/DRAIN.
    if (Simulator::Now() > Seconds(15))
    {
        m_rttSamples.push_back(newRtt);
    }
}

void
TcpBbrV3BufferbloatTest::DoRun()
{
    const Time baseRtt = MilliSeconds(42); // ~2 * 20ms bottleneck delay + access delays
    const Time simDuration = Seconds(35);

    // A deliberately huge buffer: at 10Mbps, 1000 x 1500B packets is over a
    // second of queueing delay if it ever fills -- roughly 25x the base RTT.
    SingleBottleneckTopology topo =
        BuildSingleBottleneckTopology(DataRate("10Mbps"), MilliSeconds(20), 1000);

    SetCongestionControl(topo.sender.Get(0), "ns3::TcpBbrV3");
    InstallBulkFlow(topo.sender.Get(0),
                    topo.receiver.Get(0),
                    topo.receiverAddress,
                    6300,
                    Seconds(0),
                    simDuration);

    Simulator::Schedule(MilliSeconds(1), [this]() {
        Config::ConnectWithoutContext(
            "/NodeList/0/$ns3::TcpL4Protocol/SocketList/0/RTT",
            MakeCallback(&TcpBbrV3BufferbloatTest::RttTrace, this));
    });

    Simulator::Stop(simDuration + Seconds(1));
    Simulator::Run();
    Simulator::Destroy();

    NS_TEST_ASSERT_MSG_GT(m_rttSamples.size(), 0u, "No RTT samples were collected");

    Time sum = Seconds(0);
    for (Time t : m_rttSamples)
    {
        sum += t;
    }
    Time averageRtt = sum / static_cast<double>(m_rttSamples.size());

    NS_TEST_ASSERT_MSG_LT(averageRtt.GetMilliSeconds(),
                          3 * baseRtt.GetMilliSeconds(),
                          "BBRv3's steady-state RTT grew far beyond the base RTT, indicating it "
                          "let the oversized buffer fill instead of controlling queueing delay");
}

/**
 * @ingroup internet-test
 *
 * @brief Multi-flow intra-protocol fairness: several BBRv3 flows sharing one
 * bottleneck should converge to a reasonably fair split of capacity.
 */
class TcpBbrV3MultiFlowFairnessTest : public TestCase
{
  public:
    TcpBbrV3MultiFlowFairnessTest();

  private:
    void DoRun() override;
};

TcpBbrV3MultiFlowFairnessTest::TcpBbrV3MultiFlowFairnessTest()
    : TestCase("Multiple BBRv3 flows sharing a bottleneck must split capacity reasonably fairly")
{
}

void
TcpBbrV3MultiFlowFairnessTest::DoRun()
{
    const uint32_t nFlows = 4;
    const Time simDuration = Seconds(50);

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", DataRateValue(DataRate("20Mbps")));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(20)));
    bottleneckLink.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize("2p")));

    PointToPointHelper edgeLink;
    edgeLink.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Mbps")));
    edgeLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(1)));

    PointToPointDumbbellHelper dumbbell(nFlows, edgeLink, nFlows, edgeLink, bottleneckLink);

    InternetStackHelper stack;
    dumbbell.InstallStack(stack);

    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FifoQueueDisc", "MaxSize", QueueSizeValue(QueueSize("100p")));
    tch.Install(dumbbell.GetLeft()->GetDevice(0));

    Ipv4AddressHelper leftIp("10.1.1.0", "255.255.255.0");
    Ipv4AddressHelper rightIp("10.2.1.0", "255.255.255.0");
    Ipv4AddressHelper routerIp("10.3.1.0", "255.255.255.0");
    dumbbell.AssignIpv4Addresses(leftIp, rightIp, routerIp);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    std::vector<Ptr<PacketSink>> sinks;
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        SetCongestionControl(dumbbell.GetLeft(i), "ns3::TcpBbrV3");
        sinks.push_back(InstallBulkFlow(dumbbell.GetLeft(i),
                                        dumbbell.GetRight(i),
                                        dumbbell.GetRightIpv4Address(i),
                                        static_cast<uint16_t>(7000 + i),
                                        Seconds(0),
                                        simDuration));
    }

    Simulator::Stop(simDuration + Seconds(1));
    Simulator::Run();

    std::vector<double> bytes;
    for (const auto& sink : sinks)
    {
        bytes.push_back(static_cast<double>(sink->GetTotalRx()));
    }

    Simulator::Destroy();

    double sum = std::accumulate(bytes.begin(), bytes.end(), 0.0);
    double sumOfSquares = 0.0;
    for (double b : bytes)
    {
        sumOfSquares += b * b;
    }

    // Jain's fairness index: (sum xi)^2 / (n * sum xi^2), in (0, 1].
    double fairnessIndex = (sum * sum) / (nFlows * sumOfSquares);

    NS_TEST_ASSERT_MSG_GT(fairnessIndex,
                          0.7,
                          "BBRv3 flows sharing a bottleneck did not converge to a reasonably "
                          "fair split of capacity");

    double minAcceptableShare = 0.02 * sum;
    for (double b : bytes)
    {
        NS_TEST_ASSERT_MSG_GT(b,
                              minAcceptableShare,
                              "At least one BBRv3 flow was left with a negligible share of the "
                              "bottleneck while sharing it with same-protocol flows");
    }
}

/**
 * @ingroup internet-test
 *
 * @brief TCP BBRv3 TestSuite - full-topology behavioral tests
 */
class TcpBbrV3TopologyTestSuite : public TestSuite
{
  public:
    TcpBbrV3TopologyTestSuite()
        : TestSuite("tcp-bbr-v3-topology-test", Type::SYSTEM)
    {
        AddTestCase(new TcpBbrV3CoexistenceTest(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TcpBbrV3DynamicBottleneckTest(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TcpBbrV3LongFatNetworkTest(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TcpBbrV3RandomLossTest(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TcpBbrV3BufferbloatTest(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TcpBbrV3MultiFlowFairnessTest(), TestCase::Duration::EXTENSIVE);
    }
};

static TcpBbrV3TopologyTestSuite g_tcpBbrV3TopologyTest; //!< static variable for test initialization

} // namespace ns3
