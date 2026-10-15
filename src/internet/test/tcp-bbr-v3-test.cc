/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/log.h"
#include "ns3/tcp-bbr-v3.h"
#include "ns3/tcp-congestion-ops.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/test.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpBbrV3TestSuite");

/**
 * @ingroup internet-test
 *
 * @brief Testing whether BBRv3 enables pacing
 */
class TcpBbrV3PacingEnableTest : public TestCase
{
  public:
    /**
     * @brief constructor
     * @param pacing pacing configuration
     * @param name description of the test
     */
    TcpBbrV3PacingEnableTest(bool pacing, const std::string& name);

  private:
    void DoRun() override;
    /**
     * @brief Execute the test.
     */
    void ExecuteTest();
    bool m_pacing; //!< Initial pacing configuration.
};

TcpBbrV3PacingEnableTest::TcpBbrV3PacingEnableTest(bool pacing, const std::string& name)
    : TestCase(name),
      m_pacing(pacing)
{
}

void
TcpBbrV3PacingEnableTest::DoRun()
{
    Simulator::Schedule(Seconds(0), &TcpBbrV3PacingEnableTest::ExecuteTest, this);
    Simulator::Run();
    Simulator::Destroy();
}

void
TcpBbrV3PacingEnableTest::ExecuteTest()
{
    Ptr<TcpSocketState> state = CreateObject<TcpSocketState>();
    state->m_pacing = m_pacing;

    Ptr<TcpBbrV3> cong = CreateObject<TcpBbrV3>();

    cong->CongestionStateSet(state, TcpSocketState::CA_OPEN);

    NS_TEST_ASSERT_MSG_EQ(state->m_pacing, true, "BBRv3 has not turned pacing on");
}

/**
 * @ingroup internet-test
 *
 * @brief Tests whether BBRv3 sets the correct pacing/cwnd gain and top-level
 * mode when entering STARTUP, DRAIN, or PROBE_RTT.
 *
 * PROBE_BW is deliberately not covered here: unlike v1's flat 8-phase gain
 * cycle, v3 restructures PROBE_BW into 4 explicit sub-states, each with its
 * own gain values. Those are covered by TcpBbrV3ProbeBwSubStateTest instead.
 */
class TcpBbrV3CheckGainValuesTest : public TestCase
{
  public:
    /**
     * @brief constructor
     * @param mode BBRv3 top-level mode under test (STARTUP, DRAIN, or PROBE_RTT)
     * @param highGain value of the HighGain attribute to configure
     * @param name description of the test
     */
    TcpBbrV3CheckGainValuesTest(TcpBbrV3::BbrMode_t mode, double highGain, const std::string& name);

  private:
    void DoRun() override;
    /**
     * @brief Execute the test.
     */
    void ExecuteTest();
    TcpBbrV3::BbrMode_t m_mode; //!< BBRv3 mode under test
    double m_highGain;          //!< Value of BBRv3 high gain
};

TcpBbrV3CheckGainValuesTest::TcpBbrV3CheckGainValuesTest(TcpBbrV3::BbrMode_t mode,
                                                         double highGain,
                                                         const std::string& name)
    : TestCase(name),
      m_mode(mode),
      m_highGain(highGain)
{
}

void
TcpBbrV3CheckGainValuesTest::DoRun()
{
    Simulator::Schedule(Seconds(0), &TcpBbrV3CheckGainValuesTest::ExecuteTest, this);
    Simulator::Run();
    Simulator::Destroy();
}

void
TcpBbrV3CheckGainValuesTest::ExecuteTest()
{
    Ptr<TcpBbrV3> cong = CreateObject<TcpBbrV3>();
    cong->SetAttribute("HighGain", DoubleValue(m_highGain));

    double desiredPacingGain = m_highGain;
    double desiredCwndGain = m_highGain;

    switch (m_mode)
    {
    case TcpBbrV3::BBR_STARTUP:
        cong->EnterStartup();
        desiredPacingGain = m_highGain;
        desiredCwndGain = m_highGain;
        break;
    case TcpBbrV3::BBR_DRAIN:
        cong->EnterDrain();
        desiredPacingGain = 1 / m_highGain;
        desiredCwndGain = m_highGain;
        break;
    case TcpBbrV3::BBR_PROBE_RTT:
        cong->EnterProbeRTT();
        desiredPacingGain = 1;
        desiredCwndGain = 1;
        break;
    default:
        NS_ASSERT(false);
    }

    NS_TEST_ASSERT_MSG_EQ(cong->GetBbrState(),
                          m_mode,
                          "BBRv3 has not entered into the desired top-level mode");
    NS_TEST_ASSERT_MSG_EQ(cong->GetPacingGain(),
                          desiredPacingGain,
                          "BBRv3 has not set the desired pacing gain");
    NS_TEST_ASSERT_MSG_EQ(cong->GetCwndGain(),
                          desiredCwndGain,
                          "BBRv3 has not set the desired cwnd gain");
}

/**
 * @ingroup internet-test
 *
 * @brief Tests whether BBRv3's PROBE_BW sub-state machine
 * (UP/DOWN/CRUISE/REFILL) sets the correct pacing/cwnd gain when entered
 * directly, and that EnterProbeBW() lands in DOWN as its entry sub-state.
 */
class TcpBbrV3ProbeBwSubStateTest : public TestCase
{
  public:
    /**
     * @brief constructor
     * @param phase PROBE_BW sub-state under test
     * @param name description of the test
     */
    TcpBbrV3ProbeBwSubStateTest(TcpBbrV3::BbrProbeBwPhase_t phase, const std::string& name);

  private:
    void DoRun() override;
    /**
     * @brief Execute the test.
     */
    void ExecuteTest();
    TcpBbrV3::BbrProbeBwPhase_t m_phase; //!< PROBE_BW sub-state under test
};

TcpBbrV3ProbeBwSubStateTest::TcpBbrV3ProbeBwSubStateTest(TcpBbrV3::BbrProbeBwPhase_t phase,
                                                         const std::string& name)
    : TestCase(name),
      m_phase(phase)
{
}

void
TcpBbrV3ProbeBwSubStateTest::DoRun()
{
    Simulator::Schedule(Seconds(0), &TcpBbrV3ProbeBwSubStateTest::ExecuteTest, this);
    Simulator::Run();
    Simulator::Destroy();
}

void
TcpBbrV3ProbeBwSubStateTest::ExecuteTest()
{
    Ptr<TcpSocketState> state = CreateObject<TcpSocketState>();
    Ptr<TcpBbrV3> cong = CreateObject<TcpBbrV3>();

    double desiredPacingGain = 1.0;
    double desiredCwndGain = 2.0;

    // Every PROBE_BW sub-state is only ever entered while already in the
    // BBR_PROBE_BW top-level mode (in production, UpdateCyclePhase() bails
    // out immediately otherwise); EnterProbeBW() itself lands in DOWN, which
    // the other three sub-states then transition out of.
    cong->EnterProbeBW();

    switch (m_phase)
    {
    case TcpBbrV3::BBR_BW_PROBE_DOWN:
        desiredPacingGain = 0.91;
        break;
    case TcpBbrV3::BBR_BW_PROBE_REFILL:
        cong->StartBwProbeRefill(0);
        desiredPacingGain = 1.0;
        break;
    case TcpBbrV3::BBR_BW_PROBE_UP:
        cong->StartBwProbeUp(state, DataRate("10Mbps"));
        desiredPacingGain = 1.25;
        desiredCwndGain = 2.25;
        break;
    case TcpBbrV3::BBR_BW_PROBE_CRUISE:
        cong->StartBwProbeCruise();
        desiredPacingGain = 1.0;
        break;
    default:
        NS_ASSERT(false);
    }

    NS_TEST_ASSERT_MSG_EQ(cong->GetBbrState(),
                          TcpBbrV3::BBR_PROBE_BW,
                          "Entering a PROBE_BW sub-state must set the top-level mode to PROBE_BW");
    NS_TEST_ASSERT_MSG_EQ(cong->GetProbeBwPhase(),
                          m_phase,
                          "BBRv3 has not entered the desired PROBE_BW sub-state");
    NS_TEST_ASSERT_MSG_EQ(cong->GetPacingGain(),
                          desiredPacingGain,
                          "BBRv3 has not set the desired pacing gain for this sub-state");
    NS_TEST_ASSERT_MSG_EQ(cong->GetCwndGain(),
                          desiredCwndGain,
                          "BBRv3 has not set the desired cwnd gain for this sub-state");
}

/**
 * @ingroup internet-test
 *
 * @brief Tests that Fork() (used whenever an accepted connection clones its
 * listening socket's congestion control instance) preserves attribute
 * configuration set on the source object.
 *
 * This guards against a regression of the bug where TcpBbrV3's copy
 * constructor did not copy any member (including attribute-backed members
 * like HighGain), silently resetting every forked socket to compile-time
 * defaults regardless of what was configured on the prototype.
 */
class TcpBbrV3ForkPreservesAttributesTest : public TestCase
{
  public:
    /**
     * @brief constructor
     */
    TcpBbrV3ForkPreservesAttributesTest();

  private:
    void DoRun() override;
    /**
     * @brief Execute the test.
     */
    void ExecuteTest();
};

TcpBbrV3ForkPreservesAttributesTest::TcpBbrV3ForkPreservesAttributesTest()
    : TestCase("BBRv3 Fork() must preserve attribute-configured state")
{
}

void
TcpBbrV3ForkPreservesAttributesTest::DoRun()
{
    Simulator::Schedule(Seconds(0), &TcpBbrV3ForkPreservesAttributesTest::ExecuteTest, this);
    Simulator::Run();
    Simulator::Destroy();
}

void
TcpBbrV3ForkPreservesAttributesTest::ExecuteTest()
{
    const double nonDefaultHighGain = 5.5;

    Ptr<TcpBbrV3> prototype = CreateObject<TcpBbrV3>();
    prototype->SetAttribute("HighGain", DoubleValue(nonDefaultHighGain));

    Ptr<TcpCongestionOps> forkedBase = prototype->Fork();
    Ptr<TcpBbrV3> forked = DynamicCast<TcpBbrV3>(forkedBase);
    NS_TEST_ASSERT_MSG_NE(forked, nullptr, "Fork() must return a TcpBbrV3 instance");

    DoubleValue forkedHighGain;
    forked->GetAttribute("HighGain", forkedHighGain);
    NS_TEST_ASSERT_MSG_EQ(forkedHighGain.Get(),
                          nonDefaultHighGain,
                          "Fork() must preserve the HighGain attribute of the source object");

    // Cross-check via observable behavior: entering STARTUP on the forked
    // object should reflect the forked (not default) HighGain.
    forked->EnterStartup();
    NS_TEST_ASSERT_MSG_EQ(forked->GetPacingGain(),
                          nonDefaultHighGain,
                          "Forked BBRv3 instance did not carry over the configured HighGain "
                          "into its own state transitions");
}

/**
 * @ingroup internet-test
 *
 * @brief TCP BBRv3 TestSuite - state machine tests
 */
class TcpBbrV3TestSuite : public TestSuite
{
  public:
    /**
     * @brief constructor
     */
    TcpBbrV3TestSuite()
        : TestSuite("tcp-bbr-v3-test", Type::UNIT)
    {
        AddTestCase(new TcpBbrV3PacingEnableTest(true, "BBRv3 must keep pacing feature on"),
                    TestCase::Duration::QUICK);
        AddTestCase(new TcpBbrV3PacingEnableTest(false, "BBRv3 must turn on pacing feature"),
                    TestCase::Duration::QUICK);

        AddTestCase(
            new TcpBbrV3CheckGainValuesTest(
                TcpBbrV3::BBR_STARTUP,
                2.89,
                "BBRv3 should enter STARTUP and set cwnd and pacing gain to HighGain"),
            TestCase::Duration::QUICK);
        AddTestCase(
            new TcpBbrV3CheckGainValuesTest(
                TcpBbrV3::BBR_DRAIN,
                2.89,
                "BBRv3 should enter DRAIN and set pacing gain to the inverse of HighGain"),
            TestCase::Duration::QUICK);
        AddTestCase(
            new TcpBbrV3CheckGainValuesTest(TcpBbrV3::BBR_PROBE_RTT,
                                            2.89,
                                            "BBRv3 should enter PROBE_RTT with unity gains"),
            TestCase::Duration::QUICK);

        AddTestCase(new TcpBbrV3ProbeBwSubStateTest(
                        TcpBbrV3::BBR_BW_PROBE_DOWN,
                        "EnterProbeBW() should start the cycle in the DOWN sub-state"),
                    TestCase::Duration::QUICK);
        AddTestCase(
            new TcpBbrV3ProbeBwSubStateTest(TcpBbrV3::BBR_BW_PROBE_REFILL,
                                            "BBRv3 should set REFILL sub-state gains correctly"),
            TestCase::Duration::QUICK);
        AddTestCase(
            new TcpBbrV3ProbeBwSubStateTest(TcpBbrV3::BBR_BW_PROBE_UP,
                                            "BBRv3 should set UP sub-state gains correctly"),
            TestCase::Duration::QUICK);
        AddTestCase(
            new TcpBbrV3ProbeBwSubStateTest(TcpBbrV3::BBR_BW_PROBE_CRUISE,
                                            "BBRv3 should set CRUISE sub-state gains correctly"),
            TestCase::Duration::QUICK);

        AddTestCase(new TcpBbrV3ForkPreservesAttributesTest(), TestCase::Duration::QUICK);
    }
};

static TcpBbrV3TestSuite g_tcpBbrV3Test; //!< static variable for test initialization
} // namespace ns3
