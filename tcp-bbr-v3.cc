// SPDX-License-Identifier: GPL-2.0-only


#include "tcp-bbr-v3.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpBbrV3");
NS_OBJECT_ENSURE_REGISTERED(TcpBbrV3);

const double TcpBbrV3::PACING_GAIN_CYCLE[] = {1.25, 0.91, 1.0, 1.0}; // Linux: bbr_pacing_gain[]

const char* const TcpBbrV3::BbrModeName[BBR_PROBE_RTT + 1] = {
    "BBR_STARTUP",
    "BBR_DRAIN",
    "BBR_PROBE_BW",
    "BBR_PROBE_RTT",
};

namespace
{
const char* const ProbeBwPhaseName[TcpBbrV3::GAIN_CYCLE_LENGTH] = {
    "BBR_BW_PROBE_UP",
    "BBR_BW_PROBE_DOWN",
    "BBR_BW_PROBE_CRUISE",
    "BBR_BW_PROBE_REFILL",
};
} // namespace

TypeId
TcpBbrV3::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpBbrV3")
            .SetParent<TcpCongestionOps>()
            .AddConstructor<TcpBbrV3>()
            .SetGroupName("Internet")
            .AddAttribute("HighGain",
                          "STARTUP pacing gain (Linux: bbr_startup_pacing_gain)",
                          DoubleValue(2.77),
                          MakeDoubleAccessor(&TcpBbrV3::m_highGain),
                          MakeDoubleChecker<double>())
            .AddAttribute("Stream",
                          "Random number stream (default is set to 4 to align with Linux results)",
                          UintegerValue(4),
                          MakeUintegerAccessor(&TcpBbrV3::SetStream),
                          MakeUintegerChecker<uint32_t>())
            .AddTraceSource("InternalState",
                           "Internal BBRv3 state variables for logging",
                           MakeTraceSourceAccessor(&TcpBbrV3::m_traceInternalState),
                           "ns3::TcpBbrV3::InternalStateTracedCallback")
            // The other v3-specific tunables (loss/ECN thresholds, beta, the
            // bw-probe timing constants, etc.) are plain member defaults
            // rather than ns-3 Attributes, so they aren't runtime-configurable
            // without a code change.
        ;
    return tid;
}

TcpBbrV3::TcpBbrV3()
    : TcpCongestionOps()
{
    NS_LOG_FUNCTION(this);
    m_uv = CreateObject<UniformRandomVariable>();
}

TcpBbrV3::TcpBbrV3(const TcpBbrV3& sock)
    : TcpCongestionOps(sock),
      m_state(sock.m_state),
      m_minRtt(sock.m_minRtt),
      m_minRttStamp(sock.m_minRttStamp),
      m_minRttExpired(sock.m_minRttExpired),
      m_minRttFilterLen(sock.m_minRttFilterLen),
      m_probeRttMin(sock.m_probeRttMin),
      m_probeRttMinStamp(sock.m_probeRttMinStamp),
      m_probeRttWinLen(sock.m_probeRttWinLen),
      m_probeRttExpired(sock.m_probeRttExpired),
      m_roundCount(sock.m_roundCount),
      m_roundStart(sock.m_roundStart),
      m_nextRoundDelivered(sock.m_nextRoundDelivered),
      m_isPipeFilled(sock.m_isPipeFilled),
      m_prevCaState(sock.m_prevCaState),
      m_fullBwCount(sock.m_fullBwCount),
      m_fullBw(sock.m_fullBw),
      m_fullBwNow(sock.m_fullBwNow),
      m_pacingGain(sock.m_pacingGain),
      m_cWndGain(sock.m_cWndGain),
      m_cycleIndex(sock.m_cycleIndex),
      m_cycleStamp(sock.m_cycleStamp),
      m_isInitialized(sock.m_isInitialized),
      m_uv(sock.m_uv),
      m_highGain(sock.m_highGain),
      m_startupCwndGain(sock.m_startupCwndGain),
      m_drainGain(sock.m_drainGain),
      m_delivered(sock.m_delivered),
      m_probeRttDoneStamp(sock.m_probeRttDoneStamp),
      m_idleRestart(sock.m_idleRestart),
      m_priorCwnd(sock.m_priorCwnd),
      m_targetCWnd(sock.m_targetCWnd),
      m_probeRttDuration(sock.m_probeRttDuration),
      m_probeRttRoundDone(sock.m_probeRttRoundDone),
      m_minPipeCwnd(sock.m_minPipeCwnd),
      m_probeRttCwndGain(sock.m_probeRttCwndGain),
      m_rateOps(sock.m_rateOps),
      m_extraAckedGain(sock.m_extraAckedGain),
      m_extraAcked{sock.m_extraAcked[0], sock.m_extraAcked[1]},
      m_extraAckedWinRtt(sock.m_extraAckedWinRtt),
      m_extraAckedWinRttLength(sock.m_extraAckedWinRttLength),
      m_ackEpochAckedResetThresh(sock.m_ackEpochAckedResetThresh),
      m_extraAckedIdx(sock.m_extraAckedIdx),
      m_ackEpochTime(sock.m_ackEpochTime),
      m_ackEpochAcked(sock.m_ackEpochAcked),
      m_sendQuantum(sock.m_sendQuantum),
      m_hasSeenRtt(sock.m_hasSeenRtt),
      m_pacingMargin(sock.m_pacingMargin),
      m_lossInRound(sock.m_lossInRound),
      m_lossInCycle(sock.m_lossInCycle),
      m_lossRoundDelivered(sock.m_lossRoundDelivered),
      m_lossRoundStart(sock.m_lossRoundStart),
      m_lossEventsInRound(sock.m_lossEventsInRound),
      m_ecnInRound(sock.m_ecnInRound),
      m_ecnInCycle(sock.m_ecnInCycle),
      m_ecnEligible(sock.m_ecnEligible),
      m_ecnAlpha(sock.m_ecnAlpha),
      m_startupEcnRounds(sock.m_startupEcnRounds),
      m_ecnThresh(sock.m_ecnThresh),
      m_fullEcnCnt(sock.m_fullEcnCnt),
      m_ackedBytesEcn(sock.m_ackedBytesEcn),
      m_ackedBytesTotal(sock.m_ackedBytesTotal),
      m_bwLo(sock.m_bwLo),
      m_bwHi{sock.m_bwHi[0], sock.m_bwHi[1]},
      m_inflightLo(sock.m_inflightLo),
      m_inflightHi(sock.m_inflightHi),
      m_beta(sock.m_beta),
      m_lossThresh(sock.m_lossThresh),
      m_fullLossCnt(sock.m_fullLossCnt),
      m_undoBwLo(sock.m_undoBwLo),
      m_undoInflightLo(sock.m_undoInflightLo),
      m_undoInflightHi(sock.m_undoInflightHi),
      m_bwLatest(sock.m_bwLatest),
      m_inflightLatest(sock.m_inflightLatest),
      m_probeBwPhase(sock.m_probeBwPhase),
      m_ackPhase(sock.m_ackPhase),
      m_bwProbeUpRounds(sock.m_bwProbeUpRounds),
      m_bwProbeUpCnt(sock.m_bwProbeUpCnt),
      m_bwProbeUpAcks(sock.m_bwProbeUpAcks),
      m_probeWait(sock.m_probeWait),
      m_roundsSincProbe(sock.m_roundsSincProbe),
      m_prevProbeTooHigh(sock.m_prevProbeTooHigh),
      m_stoppedRiskyProbe(sock.m_stoppedRiskyProbe),
      m_bwProbeSamples(sock.m_bwProbeSamples),
      m_bwProbeRandRounds(sock.m_bwProbeRandRounds),
      m_bwProbeBaseWait(sock.m_bwProbeBaseWait),
      m_bwProbeRandWait(sock.m_bwProbeRandWait),
      m_bwProbeMaxRounds(sock.m_bwProbeMaxRounds),
      m_inflightHeadroom(sock.m_inflightHeadroom)
{
    NS_LOG_FUNCTION(this);
}

TcpBbrV3::~TcpBbrV3()
{
    NS_LOG_FUNCTION(this);
}

std::string
TcpBbrV3::GetName() const
{
    return "TcpBbrV3";
}

Ptr<TcpCongestionOps>
TcpBbrV3::Fork()
{
    return CopyObject<TcpBbrV3>(this);
}

double
TcpBbrV3::GetPacingGain() const
{
    return m_pacingGain;
}

double
TcpBbrV3::GetCwndGain() const
{
    return m_cWndGain;
}

TcpBbrV3::BbrMode_t
TcpBbrV3::GetBbrState() const
{
    return m_state;
}

TcpBbrV3::BbrProbeBwPhase_t
TcpBbrV3::GetProbeBwPhase() const
{
    return m_probeBwPhase;
}

void
TcpBbrV3::EnterStartup()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BbrMode_t::BBR_STARTUP);
    // Unlike v1 (2.89 for both), v3 uses separate STARTUP pacing and cwnd gains.
    m_pacingGain = m_highGain;
    m_cWndGain = m_startupCwndGain;
}

void
TcpBbrV3::SetBbrState(BbrMode_t mode)
{
    NS_LOG_FUNCTION(this << mode);
    NS_LOG_DEBUG(Simulator::Now() << " Changing from " << BbrModeName[m_state] << " to "
                                  << BbrModeName[mode]);
    m_state = mode;
}

void
TcpBbrV3::EnterDrain()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BbrMode_t::BBR_DRAIN);
    m_pacingGain = m_drainGain;
    m_cWndGain = m_startupCwndGain;
}

void
TcpBbrV3::SetCycleIdx(BbrProbeBwPhase_t phase)
{
    NS_LOG_FUNCTION(this << phase);
    NS_LOG_DEBUG(Simulator::Now() << " PROBE_BW phase " << ProbeBwPhaseName[m_probeBwPhase]
                                   << " -> " << ProbeBwPhaseName[phase]);
    m_probeBwPhase = phase;
    // The fast-path optimization (Linux's bbr_run_fast_path(), and the
    // try_fast_path flag it clears here) is intentionally not implemented
    // in this port.
}

void
TcpBbrV3::StartBwProbeDown()
{
    NS_LOG_FUNCTION(this);
    ResetCongestionSignals();
    m_bwProbeUpCnt = 0xFFFFFFFF; // "not growing inflight_hi any more" (Linux: ~0U)
    PickProbeWait();
    m_cycleStamp = Simulator::Now();
    m_ackPhase = BBR_ACKS_PROBE_STOPPING;
    m_nextRoundDelivered = m_delivered;
    m_pacingGain = PACING_GAIN_CYCLE[BBR_BW_PROBE_DOWN];
    m_cWndGain = 2;
    SetCycleIdx(BBR_BW_PROBE_DOWN);
}

void
TcpBbrV3::StartBwProbeRefill(uint32_t bwProbeUpRounds)
{
    NS_LOG_FUNCTION(this << bwProbeUpRounds);
    ResetLowerBounds();
    m_bwProbeUpRounds = static_cast<uint8_t>(bwProbeUpRounds);
    m_bwProbeUpAcks = 0;
    m_stoppedRiskyProbe = false;
    m_ackPhase = BBR_ACKS_REFILLING;
    m_nextRoundDelivered = m_delivered;
    m_pacingGain = PACING_GAIN_CYCLE[BBR_BW_PROBE_REFILL];
    m_cWndGain = 2;
    SetCycleIdx(BBR_BW_PROBE_REFILL);
}

void
TcpBbrV3::StartBwProbeUp(Ptr<TcpSocketState> tcb, DataRate sampleBw)
{
    NS_LOG_FUNCTION(this << tcb << sampleBw);
    m_ackPhase = BBR_ACKS_PROBE_STARTING;
    m_nextRoundDelivered = m_delivered;
    m_cycleStamp = Simulator::Now();
    ResetFullBw();
    m_fullBw = sampleBw;
    m_pacingGain = PACING_GAIN_CYCLE[BBR_BW_PROBE_UP];
    // Linux: bbr_cwnd_gain (2) + bw_probe_cwnd_gain(1) * BBR_UNIT/4.
    m_cWndGain = 2.25;
    SetCycleIdx(BBR_BW_PROBE_UP);
    RaiseInflightHiSlope(tcb);
}

void
TcpBbrV3::StartBwProbeCruise()
{
    NS_LOG_FUNCTION(this);
    if (m_inflightLo != 0xFFFFFFFF)
    {
        m_inflightLo = std::min(m_inflightLo, m_inflightHi);
    }
    m_pacingGain = PACING_GAIN_CYCLE[BBR_BW_PROBE_CRUISE];
    m_cWndGain = 2;
    SetCycleIdx(BBR_BW_PROBE_CRUISE);
}

void
TcpBbrV3::ResetCongestionSignals()
{
    NS_LOG_FUNCTION(this);
    m_lossInRound = false;
    m_ecnInRound = false;
    m_lossInCycle = false;
    m_ecnInCycle = false;
    m_bwLatest = DataRate(0);
    m_inflightLatest = 0;
}

void
TcpBbrV3::ResetLowerBounds()
{
    NS_LOG_FUNCTION(this);
    m_bwLo = DataRate(std::numeric_limits<uint64_t>::max());
    m_inflightLo = 0xFFFFFFFF;
}

bool
TcpBbrV3::IsProbingBandwidth() const
{
    NS_LOG_FUNCTION(this);
    return m_state == BbrMode_t::BBR_STARTUP ||
           (m_state == BbrMode_t::BBR_PROBE_BW &&
            (m_probeBwPhase == BBR_BW_PROBE_REFILL || m_probeBwPhase == BBR_BW_PROBE_UP));
}

void
TcpBbrV3::InitLowerBounds(Ptr<TcpSocketState> tcb, bool initBw)
{
    NS_LOG_FUNCTION(this << tcb << initBw);
    if (initBw && m_bwLo == DataRate(std::numeric_limits<uint64_t>::max()))
    {
        m_bwLo = MaxBw();
    }
    if (m_inflightLo == 0xFFFFFFFF)
    {
        m_inflightLo = tcb->m_cWnd;
    }
}

void
TcpBbrV3::LossLowerBounds(DataRate& bw, uint32_t& inflight)
{
    NS_LOG_FUNCTION(this << bw << inflight);
    double lossCut = 1.0 - m_beta;
    bw = DataRate(std::max<uint64_t>(m_bwLatest.GetBitRate(),
                                      static_cast<uint64_t>(bw.GetBitRate() * lossCut)));
    inflight = std::max<uint32_t>(m_inflightLatest, static_cast<uint32_t>(inflight * lossCut));
}

void
TcpBbrV3::AdaptLowerBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // Only use lower-bound estimates when not probing bw; while probing we
    // need to push inflight higher, so a stale lower bound would fight it.
    if (IsProbingBandwidth())
    {
        return;
    }

    // The ECN lower-bound response Linux's bbr_ecn_lower_bounds() adds would
    // hook in here; it's not implemented, since m_ecnInRound is never set.

    if (m_lossInRound)
    {
        InitLowerBounds(tcb, true);
        LossLowerBounds(m_bwLo, m_inflightLo);
    }

    m_bwLo = std::max(DataRate(1), m_bwLo);
}

void
TcpBbrV3::PickProbeWait()
{
    NS_LOG_FUNCTION(this);
    // Decide the random round-trip bound for wait until probe.
    m_roundsSincProbe = static_cast<uint8_t>(m_uv->GetValue(0, m_bwProbeRandRounds));
    // Decide the random wall-clock bound for wait until probe.
    Time randWait =
        NanoSeconds(static_cast<int64_t>(m_uv->GetValue(0, m_bwProbeRandWait.GetNanoSeconds())));
    m_probeWait = m_bwProbeBaseWait + randWait;
}

void
TcpBbrV3::EnterProbeBW()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BbrMode_t::BBR_PROBE_BW);
    StartBwProbeDown();
}

void
TcpBbrV3::EnterProbeRTT()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BbrMode_t::BBR_PROBE_RTT);
    m_pacingGain = 1;
    m_cWndGain = 1;
}

void
TcpBbrV3::ExitProbeRTT()
{
    NS_LOG_FUNCTION(this);
    ResetLowerBounds();
    if (m_isPipeFilled)
    {
        SetBbrState(BbrMode_t::BBR_PROBE_BW);
        // Raising inflight after PROBE_RTT may cause loss, so reset the
        // PROBE_BW clock and schedule the next bandwidth probe for a
        // friendly and randomized future point in time.
        StartBwProbeDown();
        // Since we are exiting PROBE_RTT, we know inflight is below our
        // estimated BDP, so it is reasonable to cruise instead of
        // sitting through the drain sub-state.
        StartBwProbeCruise();
    }
    else
    {
        EnterStartup();
    }
}

void
TcpBbrV3::UpdateRound(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // Linux: bbr_update_round_start() gates only on interval_us > 0 here
    // (unlike UpdateCongestionSignals()/UpdateLatestDeliverySignals(), which
    // also require acked_sacked != 0) -- a zero/negative interval is the only
    // "not a valid observation" case for round-boundary purposes. Right after
    // a loss collapses bytes-in-flight to near zero, samples with a zero
    // interval stop reflecting real round-trip progress; treating them as
    // round boundaries anyway lets the round counter run far ahead of real
    // RTTs, which in turn fires the per-round loss-response cut in
    // AdaptLowerBounds() (and thrashes the PROBE_BW sub-state machine) many
    // times within a single actual RTT, permanently collapsing the bandwidth
    // estimate.
    if (rs.m_interval.IsZero())
    {
        m_roundStart = false;
        return;
    }
    if (rs.m_priorDelivered >= m_nextRoundDelivered)
    {
        m_nextRoundDelivered = m_delivered;
        m_roundCount++;
        m_roundStart = true;
    }
    else
    {
        m_roundStart = false;
    }
}

DataRate
TcpBbrV3::MaxBw() const
{
    NS_LOG_FUNCTION(this);
    return std::max(m_bwHi[0], m_bwHi[1]);
}

DataRate
TcpBbrV3::Bw() const
{
    NS_LOG_FUNCTION(this);
    return std::min(MaxBw(), m_bwLo);
}

void
TcpBbrV3::UpdateLatestDeliverySignals(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    m_lossRoundStart = false;
    if (rs.m_interval.IsZero() || rs.m_ackedSacked == 0)
    {
        return; // not a valid observation
    }

    m_bwLatest = std::max(m_bwLatest, rs.m_deliveryRate);
    m_inflightLatest = std::max<uint32_t>(m_inflightLatest, rs.m_delivered);

    if (rs.m_priorDelivered >= m_lossRoundDelivered)
    {
        m_lossRoundDelivered = static_cast<uint32_t>(m_delivered);
        m_lossRoundStart = true; // mark start of new round trip
    }
}

void
TcpBbrV3::AdvanceLatestDeliverySignals(Ptr<TcpSocketState> tcb,
                                       const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // Linux also checks !rs->is_acking_tlp_retrans_seq here; ns-3's
    // TcpRateSample doesn't track TLP retransmit acks, so that guard is
    // omitted.
    if (m_lossRoundStart)
    {
        m_bwLatest = rs.m_deliveryRate;
        m_inflightLatest = rs.m_delivered;
    }
}

void
TcpBbrV3::PktsAcked(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked, const Time& rtt)
{
    NS_LOG_FUNCTION(this << tcb << segmentsAcked << rtt);
    // Same accounting pattern as TcpDctcp::PktsAcked (tcp-dctcp.cc):
    // accumulate total acked bytes, and separately the bytes acked while the
    // current ECN state indicates the peer echoed a CE mark (ECN_ECE_RCVD).
    // This only counts bytes; it does NOT do DCTCP's receiver-side
    // delayed-ACK/ECE-echo generation -- BBR is sender-only here, it just
    // reads tcb->m_ecnState, it doesn't need to manufacture ECE-carrying
    // ACKs itself.
    m_ackedBytesTotal += segmentsAcked * tcb->m_segmentSize;
    if (tcb->m_ecnState == TcpSocketState::ECN_ECE_RCVD)
    {
        m_ackedBytesEcn += segmentsAcked * tcb->m_segmentSize;
    }
}

int
TcpBbrV3::UpdateEcnAlpha(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Port of Linux's bbr_update_ecn_alpha(), but built on the byte-counter
    // accounting above instead of Linux's rs->delivered_ce (ns-3's
    // TcpRateSample doesn't currently carry a CE-mark count, so it's tracked
    // here the same way DCTCP does). Only called when m_roundStart is true --
    // i.e. once per round trip, same cadence as the Linux version.
    bool hadData = m_ackedBytesTotal > 0;
    double bytesEcn = 0.0;
    if (hadData)
    {
        bytesEcn = static_cast<double>(m_ackedBytesEcn) / static_cast<double>(m_ackedBytesTotal);
    }

    // bbr_ecn_alpha_gain = BBR_UNIT * 1/16 -> g = 1/16 here.
    const double g = 1.0 / 16.0;
    m_ecnAlpha = (1.0 - g) * m_ecnAlpha + g * bytesEcn;

    m_ackedBytesEcn = 0;
    m_ackedBytesTotal = 0;

    // Return ce_ratio as a percentage (0-100) for
    // CheckEcnTooHighInStartup()'s comparison against m_ecnThresh. If there
    // were no acked bytes this round (shouldn't normally happen mid-transfer),
    // signal "no data" the same way Linux returns -1 via ce_ratio when
    // delivered == 0.
    if (!hadData)
    {
        return -1;
    }
    int ceRatio = static_cast<int>(bytesEcn * 100);
    // Linux calls bbr_check_ecn_too_high_in_startup() from here, at the end
    // of bbr_update_ecn_alpha() -- not from bbr_update_model() like
    // CheckLossTooHighInStartup().
    CheckEcnTooHighInStartup(tcb, static_cast<uint32_t>(ceRatio));
    return ceRatio;
}

void
TcpBbrV3::UpdateCongestionSignals(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // AdaptUpperBounds()/HandleInflightTooHigh() -- which grow/shrink
    // m_inflightHi -- are called from UpdateCyclePhase(), not from here,
    // matching Linux's bbr_update_cycle_phase() call site.
    if (rs.m_interval.IsZero() || rs.m_ackedSacked == 0)
    {
        return; // not a valid observation
    }

    // Linux: bbr_take_max_bw_sample(), called from
    // bbr_update_congestion_signals() rather than v1's separate
    // UpdateBottleneckBandwidth().
    if (!rs.m_isAppLimited || rs.m_deliveryRate >= MaxBw())
    {
        m_bwHi[1] = std::max(rs.m_deliveryRate, m_bwHi[1]);
    }

    m_lossInRound |= (rs.m_bytesLoss > 0);

    if (!m_lossRoundStart)
    {
        return; // skip the per-round-trip updates
    }
    AdaptLowerBounds(tcb, rs);

    m_lossInRound = false;
    m_ecnInRound = false;
}

bool
TcpBbrV3::IsInflightTooHigh(const TcpRateOps::TcpRateSample& rs) const
{
    NS_LOG_FUNCTION(this << rs);
    // Loss rate over the flight of the most recently delivered packet, not
    // this ACK's freshly marked losses (Linux: rs->lost vs rs->losses).
    if (rs.m_lost > 0 && rs.m_txInFlight > 0)
    {
        auto lossThreshBytes = static_cast<uint32_t>(rs.m_txInFlight * m_lossThresh);
        if (rs.m_lost > lossThreshBytes)
        {
            return true;
        }
    }
    // The ECN half of this check (Linux: rs->delivered_ce / ecn_eligible) is
    // not implemented -- ns-3's TcpRateSample doesn't carry a CE-mark
    // delivery count, and m_ecnEligible is never set true (see
    // UpdateEcnAlpha()/AdaptLowerBounds()).
    return false;
}

void
TcpBbrV3::HandleQueueTooHighInStartup(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    NS_LOG_DEBUG(Simulator::Now() << " STARTUP exit via loss/ECN short-circuit (not bw plateau)");
    m_isPipeFilled = true;
    uint32_t bdp = InFlight(tcb, MaxBw(), 1);
    m_inflightHi = std::max(bdp, m_inflightLatest);
}

void
TcpBbrV3::CheckLossTooHighInStartup(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (m_isPipeFilled)
    {
        return;
    }

    // For STARTUP exit, check the loss rate at the end of each round trip
    // of Recovery episodes in STARTUP. We check the loss rate at the end
    // of the round trip to filter out noisy/low loss and have a better
    // sense of inflight (extent of loss), so we can drain more accurately.
    if (rs.m_bytesLoss > 0 && m_lossEventsInRound < 0xf)
    {
        m_lossEventsInRound++; // saturating counter
    }
    if (m_fullLossCnt > 0 && m_lossRoundStart &&
        tcb->m_congState == TcpSocketState::CA_RECOVERY &&
        m_lossEventsInRound >= m_fullLossCnt && IsInflightTooHigh(rs))
    {
        HandleQueueTooHighInStartup(tcb);
        return;
    }
    if (m_lossRoundStart)
    {
        m_lossEventsInRound = 0;
    }
}

void
TcpBbrV3::CheckEcnTooHighInStartup(Ptr<TcpSocketState> tcb, uint32_t ceRatio)
{
    NS_LOG_FUNCTION(this << tcb << ceRatio);
    // m_ecnEligible is never set true (ECN-eligibility detection is not
    // implemented), so this is currently a safe no-op.
    if (m_isPipeFilled || !m_ecnEligible || m_fullEcnCnt == 0 || m_ecnThresh == 0)
    {
        return;
    }

    if (ceRatio >= m_ecnThresh)
    {
        m_startupEcnRounds++;
    }
    else
    {
        m_startupEcnRounds = 0;
    }

    if (m_startupEcnRounds >= m_fullEcnCnt)
    {
        HandleQueueTooHighInStartup(tcb);
    }
}

void
TcpBbrV3::CheckFullBwReached(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // The loss/ECN short-circuit that Linux's bbr_check_full_bw_reached()
    // also performs is handled separately, via
    // CheckLossTooHighInStartup()/CheckEcnTooHighInStartup(); this function
    // implements only the bandwidth-plateau check.
    //
    // Unlike v1's CheckFullPipe(), v3 looks for 25% bw growth on every ACK
    // and only counts plateau rounds at round boundaries. Checking growth
    // solely on the round-start sample declares a plateau too readily, which
    // cuts PROBE_UP (and STARTUP) short.
    if (m_fullBwNow || rs.m_isAppLimited)
    {
        return;
    }

    // An invalid sample carries a stale m_deliveryRate; Linux uses bw 0 then.
    DataRate sampleBw = rs.m_interval.IsZero() ? DataRate(0) : rs.m_deliveryRate;
    DataRate bwThresh(static_cast<uint64_t>(m_fullBw.GetBitRate() * 1.25));
    if (sampleBw >= bwThresh)
    {
        ResetFullBw();
        m_fullBw = sampleBw;
        return;
    }
    if (!m_roundStart)
    {
        return;
    }

    m_fullBwCount++;
    m_fullBwNow = m_fullBwCount >= 3;
    m_isPipeFilled |= m_fullBwNow;
    if (m_isPipeFilled)
    {
        NS_LOG_DEBUG("Pipe filled");
    }
}

uint32_t
TcpBbrV3::TargetInflight(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    uint32_t bdp = InFlight(tcb, Bw(), 1);
    return std::min(bdp, tcb->m_cWnd.Get());
}

void
TcpBbrV3::RaiseInflightHiSlope(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Calculate "slope": packets S/ACKed per inflight_hi increment.
    uint32_t growthThisRound = 1u << m_bwProbeUpRounds;
    m_bwProbeUpRounds = std::min<uint8_t>(m_bwProbeUpRounds + 1, 30);
    uint32_t cnt = tcb->m_cWnd.Get() / growthThisRound;
    cnt = std::max<uint32_t>(cnt, 1);
    m_bwProbeUpCnt = cnt;
}

void
TcpBbrV3::ProbeInflightHiUpward(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (!tcb->m_isCwndLimited || tcb->m_cWnd.Get() < m_inflightHi)
    {
        return; // not fully using inflight_hi, so don't grow it
    }

    // For each bw_probe_up_cnt bytes ACKed, increase inflight_hi by one
    // segment. Linux tracks cwnd/acked_sacked/inflight_hi in packets, so its
    // "inflight_hi += delta" means +delta packets; this port tracks all of
    // those in bytes instead, so the same delta has to be scaled by segment
    // size to land back in inflight_hi's units. Without this scaling,
    // inflight_hi grows by mere bytes per probe cycle instead of segments --
    // orders of magnitude too slow for a flow that fell behind (e.g. lost a
    // bandwidth race against a competing flow) to ever climb back out.
    m_bwProbeUpAcks += rs.m_ackedSacked;
    if (m_bwProbeUpAcks >= m_bwProbeUpCnt)
    {
        uint32_t delta = m_bwProbeUpAcks / m_bwProbeUpCnt;
        m_bwProbeUpAcks -= delta * m_bwProbeUpCnt;
        m_inflightHi += delta * tcb->m_segmentSize;
    }

    if (m_roundStart)
    {
        RaiseInflightHiSlope(tcb);
    }
}

void
TcpBbrV3::HandleInflightTooHigh(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    m_prevProbeTooHigh = true;
    m_bwProbeSamples = false; // only react once per probe
    // If we are app-limited then we are not robustly probing the max volume
    // of inflight data we think might be safe.
    if (!rs.m_isAppLimited)
    {
        uint32_t cutTarget = static_cast<uint32_t>(TargetInflight(tcb) * (1.0 - m_beta));
        m_inflightHi = std::max(rs.m_txInFlight, cutTarget);
        NS_LOG_DEBUG(Simulator::Now()
                    << " loss/ECN too high while probing, cutting inflight_hi to "
                    << m_inflightHi);
    }
    if (m_state == BbrMode_t::BBR_PROBE_BW && m_probeBwPhase == BBR_BW_PROBE_UP)
    {
        StartBwProbeDown();
    }
}

void
TcpBbrV3::AdvanceMaxBwFilter()
{
    NS_LOG_FUNCTION(this);
    if (m_bwHi[1] == DataRate(0))
    {
        return; // no samples in this window; remember old window
    }
    m_bwHi[0] = m_bwHi[1];
    m_bwHi[1] = DataRate(0);
}

bool
TcpBbrV3::AdaptUpperBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // Track when we'll see bw/loss samples resulting from our bw probes.
    if (m_ackPhase == BBR_ACKS_PROBE_STARTING && m_roundStart)
    {
        m_ackPhase = BBR_ACKS_PROBE_FEEDBACK;
    }
    if (m_ackPhase == BBR_ACKS_PROBE_STOPPING && m_roundStart)
    {
        // End of samples from the bw probing phase.
        m_bwProbeSamples = false;
        m_ackPhase = BBR_ACKS_INIT;
        // At this point in the cycle, our current bw sample is also our
        // best recent chance at finding the highest available bw for this
        // flow, so now is the best time to forget the previous cycle's bw
        // samples by advancing the window.
        if (m_state == BbrMode_t::BBR_PROBE_BW && !rs.m_isAppLimited)
        {
            AdvanceMaxBwFilter();
        }
        // If we had an inflight_hi, then probed and pushed inflight all the
        // way up to hit it without seeing any high loss/ECN, probe up
        // again, letting inflight persist at inflight_hi for a round trip,
        // then accelerating beyond.
        if (m_state == BbrMode_t::BBR_PROBE_BW && m_stoppedRiskyProbe && !m_prevProbeTooHigh)
        {
            StartBwProbeRefill(0);
            return true; // state transition already decided
        }
    }

    if (IsInflightTooHigh(rs))
    {
        if (m_bwProbeSamples) // is this sample from bw probing?
        {
            HandleInflightTooHigh(tcb, rs);
        }
    }
    else
    {
        // Loss/ECN rate is declared safe. Adjust upper bound upward.
        if (m_inflightHi == 0xFFFFFFFF)
        {
            return false; // no excess queue signals yet
        }

        // To be resilient to random loss, raise bw/inflight_hi if we
        // observe in any phase that a higher level is safe.
        if (rs.m_txInFlight > m_inflightHi)
        {
            m_inflightHi = rs.m_txInFlight;
        }

        if (m_state == BbrMode_t::BBR_PROBE_BW && m_probeBwPhase == BBR_BW_PROBE_UP)
        {
            ProbeInflightHiUpward(tcb, rs);
        }
    }

    return false;
}

bool
TcpBbrV3::HasElapsedInPhase(Time interval) const
{
    NS_LOG_FUNCTION(this << interval);
    return Simulator::Now() > (m_cycleStamp + interval);
}

bool
TcpBbrV3::IsRenoCoexistenceProbeTime(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Random loss can shave some small percentage off of our inflight in
    // each round; to survive this, flows need robust periodic probes.
    // TargetInflight() is byte-valued in this port (unlike Linux's
    // packet-count bbr_target_inflight()), so convert to a packet count
    // before comparing against the packet-scale bw_probe_max_rounds bound.
    uint32_t targetInflightPackets = TargetInflight(tcb) / tcb->m_segmentSize;
    uint32_t rounds = std::min(m_bwProbeMaxRounds, targetInflightPackets);
    return m_roundsSincProbe >= rounds;
}

bool
TcpBbrV3::CheckTimeToProbeBw(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // The ECN reprobe-on-cessation branch that Linux's
    // bbr_check_time_to_probe_bw() also has is not implemented;
    // m_ecnEligible/m_ecnInCycle are never set true, so it would never fire
    // regardless.
    if (HasElapsedInPhase(m_probeWait) || IsRenoCoexistenceProbeTime(tcb))
    {
        StartBwProbeRefill(0);
        return true;
    }
    return false;
}

uint32_t
TcpBbrV3::InflightWithHeadroom() const
{
    NS_LOG_FUNCTION(this);
    if (m_inflightHi == 0xFFFFFFFF)
    {
        return 0xFFFFFFFF;
    }
    uint32_t headroom =
        std::max<uint32_t>(static_cast<uint32_t>(m_inflightHi * m_inflightHeadroom), 1);
    uint32_t withHeadroom = (m_inflightHi > headroom) ? (m_inflightHi - headroom) : 0;
    return std::max(withHeadroom, m_minPipeCwnd);
}

bool
TcpBbrV3::CheckTimeToCruise(Ptr<TcpSocketState> tcb, uint32_t inflight, DataRate bw)
{
    NS_LOG_FUNCTION(this << tcb << inflight << bw);
    if (inflight > InflightWithHeadroom())
    {
        return false; // always need to pull inflight down to leave headroom
    }
    return inflight <= InFlight(tcb, bw, 1);
}

void
TcpBbrV3::UpdateCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (!m_isPipeFilled)
    {
        return;
    }

    // In DRAIN, PROBE_BW, or PROBE_RTT, adjust upper bounds.
    if (AdaptUpperBounds(tcb, rs))
    {
        return; // already decided state transition
    }

    if (m_state != BbrMode_t::BBR_PROBE_BW)
    {
        return;
    }

    // Linux computes `inflight` via bbr_packets_in_net_at_edt(), which
    // adjusts prior_in_flight for data already "baked in" by earlier
    // departure-time pacing decisions (EDT/FQ). ns-3's simpler pacing model
    // doesn't expose that to congestion control, so we use the current
    // bytes-in-flight directly -- the same simplification CheckDrain()
    // already makes.
    uint32_t inflight = tcb->m_bytesInFlight.Get();
    DataRate bw = MaxBw();

    switch (m_probeBwPhase)
    {
    // First we spend most of our time cruising with a pacing_gain of 1.0,
    // which paces at the estimated bw, to try to fully use the pipe
    // without building queue. If we encounter loss/ECN marks, we adapt by
    // slowing down.
    case BBR_BW_PROBE_CRUISE:
        if (CheckTimeToProbeBw(tcb, rs))
        {
            return; // already decided state transition
        }
        break;

    // After cruising, when it's time to probe, we first "refill": send at
    // the estimated bw to fill the pipe, before probing higher and
    // knowingly risking overflowing the bottleneck buffer (causing loss).
    case BBR_BW_PROBE_REFILL:
        if (m_roundStart)
        {
            // After one full round trip of sending in REFILL, we start to
            // see bw samples reflecting our REFILL, which may be putting
            // too much data in flight.
            m_bwProbeSamples = true;
            StartBwProbeUp(tcb, rs.m_deliveryRate);
        }
        break;

    // After refilling the pipe, probe with a pacing_gain > 1.0 to probe for
    // bw. Terminate PROBE_UP upon: (1) hitting the inflight_hi target set
    // by the most recent previous bw probe, (2) inflight_hi hasn't limited
    // bw growth recently and delivered bw has plateaued, (3)/(4) loss/ECN
    // filter says the rate is too high (checked in IsInflightTooHigh(),
    // via AdaptUpperBounds() above).
    case BBR_BW_PROBE_UP:
    {
        bool isBwProbeDone = false;
        if (m_prevProbeTooHigh && inflight >= m_inflightHi)
        {
            m_stoppedRiskyProbe = true;
            isBwProbeDone = true;
        }
        else if (tcb->m_isCwndLimited && tcb->m_cWnd.Get() >= m_inflightHi)
        {
            // inflight_hi is limiting bw growth.
            ResetFullBw();
            m_fullBw = rs.m_deliveryRate;
        }
        else if (m_fullBwNow)
        {
            // Plateau in estimated bw. Pipe looks full.
            isBwProbeDone = true;
        }
        if (isBwProbeDone)
        {
            m_prevProbeTooHigh = false; // no loss/ECN (yet)
            StartBwProbeDown();         // restart w/ down
        }
        break;
    }

    // After probing in PROBE_UP, we have usually accumulated some data in
    // the bottleneck buffer. PROBE_DOWN drains any excess data from the
    // queue via a pacing_gain < 1.0, held until inflight falls to the
    // target cruising point.
    case BBR_BW_PROBE_DOWN:
        if (CheckTimeToProbeBw(tcb, rs))
        {
            return; // already decided state transition
        }
        if (CheckTimeToCruise(tcb, inflight, bw))
        {
            StartBwProbeCruise();
        }
        break;
    }
}

void
TcpBbrV3::BoundCwndForInflightModel(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (!m_isInitialized)
    {
        return;
    }

    uint32_t cap = 0xFFFFFFFF;
    if (m_state == BbrMode_t::BBR_PROBE_BW && m_probeBwPhase != BBR_BW_PROBE_CRUISE)
    {
        // Probe to see if more packets fit in the path.
        cap = m_inflightHi;
    }
    else if (m_state == BbrMode_t::BBR_PROBE_RTT ||
             (m_state == BbrMode_t::BBR_PROBE_BW && m_probeBwPhase == BBR_BW_PROBE_CRUISE))
    {
        cap = InflightWithHeadroom();
    }

    // Adapt to any loss/ECN since our last bw probe.
    cap = std::min(cap, m_inflightLo);
    cap = std::max(cap, m_minPipeCwnd);
    tcb->m_cWnd = std::min(cap, tcb->m_cWnd.Get());
}

void
TcpBbrV3::UpdateModelAndState(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    UpdateCongestionSignals(tcb, rs);
    UpdateAckAggregation(tcb, rs);
    CheckLossTooHighInStartup(tcb, rs);
    CheckFullBwReached(tcb, rs);
    CheckDrain(tcb);
    UpdateCyclePhase(tcb, rs);
    UpdateRTprop(tcb);
    CheckProbeRTT(tcb, rs);
}

void
TcpBbrV3::CongControl(Ptr<TcpSocketState> tcb,
                      const TcpRateOps::TcpRateConnection& rc,
                      const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // Mirrors the call order of Linux's bbr_main(); the fast-path
    // optimization (bbr_run_fast_path()) is intentionally not implemented.

    m_delivered = rc.m_delivered;

    // Linux: bbr_note_loss(), run from the skb_marked_lost hook as segments
    // are marked lost. Restarting the loss round on its first loss makes
    // bw_latest/inflight_latest span a full round trip of loss before
    // AdaptLowerBounds() uses them. The per-lost-segment inflight_hi check
    // that hook also does is not ported; AdaptUpperBounds() checks per ACK.
    if (rs.m_bytesLoss > 0)
    {
        if (!m_lossInRound)
        {
            m_lossRoundDelivered = static_cast<uint32_t>(m_delivered);
        }
        m_lossInRound = true;
        m_lossInCycle = true;
    }

    // Linux: bbr_update_round_start() is the first thing bbr_main() does.
    UpdateRound(tcb, rs);
    if (m_roundStart)
    {
        // Linux: min_t(s32, rounds_since_probe + 1, 0xFF) -- feeds
        // IsRenoCoexistenceProbeTime()'s inter-bw-probe spacing bound.
        if (m_roundsSincProbe < 0xFF)
        {
            m_roundsSincProbe++;
        }
        // UpdateEcnAlpha(tcb) would be called here once ECN is implemented.
    }

    UpdateLatestDeliverySignals(tcb, rs);

    UpdateModelAndState(tcb, rs);

    UpdateControlParameters(tcb, rs); // SetPacingRate()/SetSendQuantum()/SetCwnd()
    BoundCwndForInflightModel(tcb);

    AdvanceLatestDeliverySignals(tcb, rs);
    m_prevCaState = tcb->m_congState;

    // Fields, in order (8 total, to stay under a typical MAX_CC_STATE_VARS
    // budget): last RTT (s), BbrMode_t, BbrProbeBwPhase_t (only meaningful
    // when state == BBR_PROBE_BW), cwnd (bytes), pacing rate (bit/s),
    // delivery rate this sample (bit/s), Bw() -- the loss/ECN-bounded
    // bandwidth estimate that actually drives pacing (bit/s), bytes lost
    // this sample. Sim time is deliberately omitted -- callers typically log
    // it separately (e.g. a "send_time" column) alongside this trace.
    m_traceInternalState({tcb->m_lastRtt.Get().GetSeconds(),
                          static_cast<double>(m_state),
                          static_cast<double>(m_probeBwPhase),
                          static_cast<double>(tcb->m_cWnd.Get()),
                          static_cast<double>(tcb->m_pacingRate.Get().GetBitRate()),
                          static_cast<double>(rs.m_deliveryRate.GetBitRate()),
                          static_cast<double>(Bw().GetBitRate()),
                          static_cast<double>(rs.m_bytesLoss)});
}

void
TcpBbrV3::Init(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Initialization happens in CongestionStateSet()'s CA_OPEN branch below,
    // matching v1's TcpBbr, which likewise leaves Init() empty.
}

void
TcpBbrV3::CongestionStateSet(Ptr<TcpSocketState> tcb,
                             const TcpSocketState::TcpCongState_t newState)
{
    NS_LOG_FUNCTION(this << tcb << newState);
    if (newState == TcpSocketState::CA_OPEN && !m_isInitialized)
    {
        NS_LOG_DEBUG("CongestionStateSet triggered to CA_OPEN :: " << newState);
        m_minRtt = tcb->m_srtt.Get() != Time::Max() ? tcb->m_srtt.Get() : Time::Max();
        m_minRttStamp = Simulator::Now();
        m_probeRttMin = m_minRtt;
        m_probeRttMinStamp = m_minRttStamp;
        m_priorCwnd = tcb->m_cWnd;
        tcb->m_ssThresh = tcb->m_initialSsThresh;
        m_targetCWnd = tcb->m_cWnd;
        m_minPipeCwnd = 4 * tcb->m_segmentSize;
        m_sendQuantum = 1 * tcb->m_segmentSize;
        // Linux's threshold is 2^20 packets; this port counts bytes.
        m_ackEpochAckedResetThresh = (1U << 20) * tcb->m_segmentSize;
        InitRoundCounting();
        InitFullPipe();
        EnterStartup();
        InitPacingRate(tcb);
        m_ackEpochTime = Simulator::Now();
        m_extraAckedWinRtt = 0;
        m_extraAckedIdx = 0;
        m_ackEpochAcked = 0;
        m_extraAcked[0] = 0;
        m_extraAcked[1] = 0;
        m_isInitialized = true;
    }
    else if (newState == TcpSocketState::CA_LOSS)
    {
        // Linux: bbr_set_state(). Unlike v1, v3 does not cut cwnd or use
        // packet conservation on entering fast recovery; loss is handled by
        // the bw_lo/inflight_lo/inflight_hi model instead.
        NS_LOG_DEBUG("CongestionStateSet triggered to CA_LOSS :: " << newState);
        m_prevCaState = TcpSocketState::CA_LOSS;
        ResetFullBw();
        if (!IsProbingBandwidth() && m_inflightLo == 0xFFFFFFFF)
        {
            // AdaptLowerBounds() needs the cwnd from before the RTO.
            m_inflightLo = std::max(tcb->m_cWnd.Get(), m_priorCwnd);
        }
    }
    else if (tcb->m_congState == TcpSocketState::CA_LOSS && newState != TcpSocketState::CA_LOSS)
    {
        // Linux: bbr_exit_loss_recovery().
        RestoreCwnd(tcb);
    }
}

void
TcpBbrV3::CwndEvent(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCAEvent_t event)
{
    NS_LOG_FUNCTION(this << tcb << event);
    // Unlike TcpDctcp::CwndEvent() (tcp-dctcp.cc), this is deliberately
    // thin: DCTCP's version calls CeState0to1()/CeState1to0(), which
    // generate extra delayed-ACK/ECE-echo packets -- that's DCTCP acting as
    // an ECN-aware RECEIVER (RFC 8257 requires the receiver to echo CE marks
    // back promptly). BBR only needs to consume ECN as a SENDER: PktsAcked()
    // above already reads tcb->m_ecnState directly for the alpha accounting,
    // so there's no receiver-side bookkeeping for BBR to do here.
    //
    // Restart-from-idle handling (CA_EVENT_TX_START, which v1's
    // TcpBbr::CwndEvent() uses to set m_idleRestart) is not implemented; as
    // a result m_idleRestart never becomes true.
    switch (event)
    {
    case TcpSocketState::CA_EVENT_ECN_IS_CE:
    case TcpSocketState::CA_EVENT_ECN_NO_CE:
        // No action needed beyond what tcb->m_ecnState already tracks.
        break;
    default:
        break;
    }
}

uint32_t
TcpBbrV3::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    NS_LOG_FUNCTION(this << tcb << bytesInFlight);
    SaveCwnd(tcb);
    return tcb->m_ssThresh;
}

bool
TcpBbrV3::HasCongControl() const
{
    return true;
}

void
TcpBbrV3::UpdateControlParameters(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    SetPacingRate(tcb, m_pacingGain);
    SetSendQuantum(tcb);
    SetCwnd(tcb, rs);
}

uint32_t
TcpBbrV3::AckAggregationCwnd()
{
    NS_LOG_FUNCTION(this);
    uint32_t maxAggrBytes; // MaxBW * 0.1 secs
    uint32_t aggrCwndBytes = 0;
    if (m_extraAckedGain && m_isPipeFilled)
    {
        maxAggrBytes = Bw().GetBitRate() / (10 * 8);
        aggrCwndBytes = m_extraAckedGain * std::max(m_extraAcked[0], m_extraAcked[1]);
        aggrCwndBytes = std::min(aggrCwndBytes, maxAggrBytes);
    }
    return aggrCwndBytes;
}

void
TcpBbrV3::UpdateAckAggregation(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    uint32_t expectedAcked;
    uint32_t extraAck;
    uint32_t epochProp;
    if (!m_extraAckedGain || rs.m_ackedSacked <= 0 || rs.m_delivered < 0)
    {
        return;
    }
    if (m_roundStart)
    {
        m_extraAckedWinRtt = std::min<uint32_t>(31, m_extraAckedWinRtt + 1);
        if (m_extraAckedWinRtt >= m_extraAckedWinRttLength)
        {
            m_extraAckedWinRtt = 0;
            m_extraAckedIdx = m_extraAckedIdx ? 0 : 1;
            m_extraAcked[m_extraAckedIdx] = 0;
        }
    }
    epochProp = Simulator::Now().GetSeconds() - m_ackEpochTime.GetSeconds();
    // Linux's bbr_update_ack_aggregation() uses bbr_bw() (loss-bound
    // capped), not bbr_max_bw() -- same Bw()/MaxBw() split as
    // SetPacingRate().
    expectedAcked = Bw().GetBitRate() * epochProp / 8;
    if (m_ackEpochAcked <= expectedAcked ||
        (m_ackEpochAcked + rs.m_ackedSacked >= m_ackEpochAckedResetThresh))
    {
        m_ackEpochAcked = 0;
        m_ackEpochTime = Simulator::Now();
        expectedAcked = 0;
    }
    m_ackEpochAcked = m_ackEpochAcked + rs.m_ackedSacked;
    extraAck = m_ackEpochAcked - expectedAcked;
    extraAck = std::min(extraAck, tcb->m_cWnd.Get());
    if (extraAck > m_extraAcked[m_extraAckedIdx])
    {
        m_extraAcked[m_extraAckedIdx] = extraAck;
    }
}

uint32_t
TcpBbrV3::InFlight(Ptr<TcpSocketState> tcb, DataRate bw, double gain)
{
    NS_LOG_FUNCTION(this << tcb << bw << gain);
    if (m_minRtt == Time::Max())
    {
        return tcb->m_initialCWnd * tcb->m_segmentSize;
    }
    double estimatedBdp = bw.GetBitRate() * m_minRtt.GetSeconds() / 8.0;
    // Linux: bbr_quantization_budget(). v3 floors the BDP at 3 send quanta
    // (and the min cwnd) rather than adding 3 quanta to it as v1 did.
    double inflight = std::max(gain * estimatedBdp, 3.0 * m_sendQuantum);
    inflight = std::max(inflight, static_cast<double>(m_minPipeCwnd));
    if (m_state == BbrMode_t::BBR_PROBE_BW && m_probeBwPhase == BBR_BW_PROBE_UP)
    {
        inflight += 2 * tcb->m_segmentSize;
    }
    return inflight;
}

uint32_t
TcpBbrV3::ProbeRttCwnd(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Linux calls bbr_bdp() directly here. InFlight() only differs by its
    // 3-quanta floor, which is below m_minPipeCwnd with this port's
    // one-segment send quantum, so the result is the same.
    return std::max(InFlight(tcb, Bw(), m_probeRttCwndGain), m_minPipeCwnd);
}

void
TcpBbrV3::CheckDrain(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_state == BbrMode_t::BBR_STARTUP && m_isPipeFilled)
    {
        EnterDrain();
        // Linux's bbr_check_drain() uses bbr_max_bw() (uncapped) here, not
        // bbr_bw() -- this ssthresh value is exported purely for monitoring
        // and isn't used by BBR's own control loop.
        tcb->m_ssThresh = InFlight(tcb, MaxBw(), 1);
        ResetCongestionSignals();
    }
    if (m_state == BbrMode_t::BBR_DRAIN && tcb->m_bytesInFlight <= InFlight(tcb, MaxBw(), 1))
    {
        EnterProbeBW();
    }
}

void
TcpBbrV3::UpdateRTprop(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Linux: bbr_update_min_rtt(). Real BBRv3 uses a two-stage filter: a
    // short (default 5s) probe_rtt_win_ms window feeds the longer (default
    // 10s) min_rtt_win_sec window that the rest of this class reads via
    // m_minRtt, and its expiry (m_probeRttExpired) schedules PROBE_RTT --
    // unlike v1's simpler single 10s-windowed
    // filter (every RTT sample compared directly against one 10s window),
    // the short inner window lets m_minRtt track a real drop in RTT
    // responsively without waiting for the full 10s window to roll over.
    //
    // The `!rs->is_ack_delayed` condition Linux adds to its forced-refresh
    // branch isn't ported -- ns-3's TcpRateSample doesn't track delayed-ACK
    // status, so a forced refresh here always takes tcb->m_lastRtt.
    m_probeRttExpired = Simulator::Now() > (m_probeRttMinStamp + m_probeRttWinLen);
    if (tcb->m_lastRtt.Get().IsPositive() &&
        (tcb->m_lastRtt < m_probeRttMin || m_probeRttExpired))
    {
        m_probeRttMin = tcb->m_lastRtt;
        m_probeRttMinStamp = Simulator::Now();
    }

    m_minRttExpired = Simulator::Now() > (m_minRttStamp + m_minRttFilterLen);
    if (m_probeRttMin <= m_minRtt || m_minRttExpired)
    {
        m_minRtt = m_probeRttMin;
        m_minRttStamp = m_probeRttMinStamp;
    }
}

void
TcpBbrV3::CheckProbeRTT(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    // Linux: PROBE_RTT is scheduled by the short probe_rtt_win_ms window,
    // not by the 10s min_rtt window expiring.
    if (m_state != BbrMode_t::BBR_PROBE_RTT && m_probeRttExpired && !m_idleRestart)
    {
        EnterProbeRTT();
        SaveCwnd(tcb);
        m_probeRttDoneStamp = Seconds(0);
        m_ackPhase = BBR_ACKS_PROBE_STOPPING;
        m_nextRoundDelivered = m_delivered;
    }
    if (m_state == BbrMode_t::BBR_PROBE_RTT)
    {
        HandleProbeRTT(tcb);
    }
    if (rs.m_delivered)
    {
        m_idleRestart = false;
    }
}

void
TcpBbrV3::SaveCwnd(Ptr<const TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_prevCaState < TcpSocketState::CA_RECOVERY && m_state != BbrMode_t::BBR_PROBE_RTT)
    {
        m_priorCwnd = tcb->m_cWnd;
    }
    else
    {
        m_priorCwnd = std::max(m_priorCwnd, tcb->m_cWnd.Get());
    }
}

void
TcpBbrV3::RestoreCwnd(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    tcb->m_cWnd = std::max(m_priorCwnd, tcb->m_cWnd.Get());
}

void
TcpBbrV3::HandleProbeRTT(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    m_rateOps->SetAppLimited(tcb->m_bytesInFlight.Get());
    if (m_probeRttDoneStamp.IsZero() && tcb->m_bytesInFlight <= ProbeRttCwnd(tcb))
    {
        m_probeRttDoneStamp = Simulator::Now() + m_probeRttDuration;
        m_probeRttRoundDone = false;
        m_nextRoundDelivered = m_delivered;
    }
    else if (!m_probeRttDoneStamp.IsZero())
    {
        if (m_roundStart)
        {
            m_probeRttRoundDone = true;
        }
        if (m_probeRttRoundDone && Simulator::Now() > m_probeRttDoneStamp)
        {
            // Linux: bbr_check_probe_rtt_done() resets probe_rtt_min_stamp
            // (the short-window timer, scheduling the next short-window
            // low-RTT check) here, not min_rtt_stamp directly -- the long
            // window's stamp is kept in sync by UpdateRTprop() itself as new
            // probe_rtt_min values propagate up to it.
            m_probeRttMinStamp = Simulator::Now();
            RestoreCwnd(tcb);
            ExitProbeRTT();
        }
    }
}

void
TcpBbrV3::InitPacingRate(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (!tcb->m_pacing)
    {
        NS_LOG_WARN("BBR must use pacing");
        tcb->m_pacing = true;
    }
    Time rtt;
    if (tcb->m_minRtt != Time::Max())
    {
        rtt = MilliSeconds(std::max<long int>(tcb->m_minRtt.GetMilliSeconds(), 1));
        m_hasSeenRtt = true;
    }
    else
    {
        rtt = MilliSeconds(1);
    }
    DataRate nominalBandwidth(tcb->m_cWnd * 8 / rtt.GetSeconds());
    tcb->m_pacingRate = DataRate(m_pacingGain * nominalBandwidth.GetBitRate());
    // v1 constructed a fresh WindowedFilter here. v3's 2-slot m_bwHi[] has
    // no equivalent "reset/seed" constructor -- just seed both slots with
    // the nominal bandwidth directly.
    m_bwHi[0] = nominalBandwidth;
    m_bwHi[1] = nominalBandwidth;
}

void
TcpBbrV3::SetPacingRate(Ptr<TcpSocketState> tcb, double gain)
{
    NS_LOG_FUNCTION(this << tcb << gain);
    DataRate rate(gain * Bw().GetBitRate());
    rate *= (1.f - m_pacingMargin);
    rate = std::min(rate, tcb->m_maxPacingRate);
    if (!m_hasSeenRtt && tcb->m_minRtt != Time::Max())
    {
        InitPacingRate(tcb);
    }
    if (m_isPipeFilled || rate > tcb->m_pacingRate)
    {
        tcb->m_pacingRate = rate;
        NS_LOG_DEBUG("Pacing rate updated. New value: " << tcb->m_pacingRate);
    }
}

void
TcpBbrV3::ModulateCwndForProbeRTT(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_state == BbrMode_t::BBR_PROBE_RTT)
    {
        tcb->m_cWnd = std::min(tcb->m_cWnd.Get(), ProbeRttCwnd(tcb));
    }
}

void
TcpBbrV3::SetSendQuantum(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    m_sendQuantum = 1 * tcb->m_segmentSize;
}

void
TcpBbrV3::UpdateTargetCwnd(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    // Linux's bbr_target_inflight() uses bbr_bw() (loss-bound capped) --
    // distinct from bbr_check_drain()'s use of bbr_max_bw().
    m_targetCWnd = InFlight(tcb, Bw(), m_cWndGain) + AckAggregationCwnd();
}

void
TcpBbrV3::SetCwnd(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (!rs.m_ackedSacked)
    {
        goto done;
    }
    UpdateTargetCwnd(tcb);
    if (m_isPipeFilled)
    {
        tcb->m_cWnd = std::min(tcb->m_cWnd.Get() + (uint32_t)rs.m_ackedSacked, m_targetCWnd);
    }
    else if (tcb->m_cWnd < m_targetCWnd ||
             tcb->m_cWnd < 2 * tcb->m_initialCWnd * tcb->m_segmentSize)
    {
        tcb->m_cWnd = tcb->m_cWnd.Get() + rs.m_ackedSacked;
    }
    tcb->m_cWnd = std::max(tcb->m_cWnd.Get(), m_minPipeCwnd);
    NS_LOG_DEBUG("Congestion window updated. New value:" << tcb->m_cWnd);
done:
    ModulateCwndForProbeRTT(tcb);
}

void
TcpBbrV3::InitRoundCounting()
{
    NS_LOG_FUNCTION(this);
    m_nextRoundDelivered = 0;
    m_roundStart = false;
    m_roundCount = 0;
}

void
TcpBbrV3::ResetFullBw()
{
    NS_LOG_FUNCTION(this);
    m_fullBw = DataRate(0);
    m_fullBwCount = 0;
    m_fullBwNow = false;
}

void
TcpBbrV3::InitFullPipe()
{
    NS_LOG_FUNCTION(this);
    m_isPipeFilled = false;
    ResetFullBw();
}

void
TcpBbrV3::SetRateOps(Ptr<TcpRateOps> rateOps)
{
    m_rateOps = rateOps;
}

void
TcpBbrV3::SetStream(uint32_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uv->SetStream(stream);
}

} // namespace ns3
