//SPDX-License-Identifier: GPL-2.0-only

#ifndef TCPBBRV3_H
#define TCPBBRV3_H

#include "tcp-congestion-ops.h"

#include "ns3/data-rate.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traced-value.h"

#include <limits>

namespace ns3
{

/**
 * @ingroup congestionOps
 *
 * @brief BBRv3 congestion control algorithm
 *
 * ns-3 port of Google's BBRv3 (net/ipv4/tcp_bbr.c). This is a separate
 * class from ns-3's existing TcpBbr (BBRv1); its structure mirrors
 * tcp-bbr.h where the underlying logic is unchanged, and adds:
 * - a loss/ECN-bounded bandwidth-inflight model (m_bwLo/m_bwHi/m_inflightLo/
 *   m_inflightHi and the adaptation logic that drives them), and
 * - a 4-phase PROBE_BW sub-state machine (UP/DOWN/CRUISE/REFILL), replacing
 *   v1's flat 8-phase pacing-gain cycle.
 *
 * ECN-based adaptation is not yet implemented: m_ecnEligible is never set to
 * true, so the ECN branches throughout this class are currently safe no-ops.
 * Restart-from-idle handling (CA_EVENT_TX_START) is likewise not ported, so
 * m_idleRestart never becomes true.
 */
class TcpBbrV3 : public TcpCongestionOps
{
  public:
    /**
     * @brief Number of phases in the BBRv3 PROBE_BW gain cycle.
     *
     * v1 used 8 phases; v3 restructures this into 4 explicit sub-states.
     * Corresponds to Linux's bbr_pacing_gain[] and enum bbr_pacing_gain_phase.
     */
    static const uint8_t GAIN_CYCLE_LENGTH = 4;

    /** @brief PROBE_BW pacing gain per sub-state. Linux: bbr_pacing_gain[]. */
    const static double PACING_GAIN_CYCLE[];

    /**
     * @brief BBR high-level modes. Same 4 states as v1.
     * Linux: enum bbr_mode.
     */
    enum BbrMode_t
    {
        BBR_STARTUP,
        BBR_DRAIN,
        BBR_PROBE_BW,
        BBR_PROBE_RTT,
    };

    /**
     * @brief PROBE_BW sub-states.
     * Linux: enum bbr_pacing_gain_phase.
     */
    enum BbrProbeBwPhase_t
    {
        BBR_BW_PROBE_UP = 0,     //!< push inflight up to probe for more bw
        BBR_BW_PROBE_DOWN = 1,   //!< drain excess inflight from the queue
        BBR_BW_PROBE_CRUISE = 2, //!< use pipe, with headroom in queue/pipe
        BBR_BW_PROBE_REFILL = 3, //!< refill the pipe back to ~100%
    };

    /**
     * @brief How incoming ACKs relate to bandwidth probing.
     * Linux: enum bbr_ack_phase.
     */
    enum BbrAckPhase_t
    {
        BBR_ACKS_INIT,
        BBR_ACKS_REFILLING,
        BBR_ACKS_PROBE_STARTING,
        BBR_ACKS_PROBE_FEEDBACK,
        BBR_ACKS_PROBE_STOPPING,
    };

    static TypeId GetTypeId();

    TcpBbrV3();
    TcpBbrV3(const TcpBbrV3& sock);
    ~TcpBbrV3() override;

    std::string GetName() const override;
    Ptr<TcpCongestionOps> Fork() override;

    void Init(Ptr<TcpSocketState> tcb) override;
    void CongestionStateSet(Ptr<TcpSocketState> tcb,
                            const TcpSocketState::TcpCongState_t newState) override;
    void CwndEvent(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCAEvent_t event) override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    bool HasCongControl() const override;
    void CongControl(Ptr<TcpSocketState> tcb,
                      const TcpRateOps::TcpRateConnection& rc,
                      const TcpRateOps::TcpRateSample& rs) override;

    /**
     * @brief Accumulates bytes-acked vs. bytes-acked-while-ECN-marked.
     *
     * Same integration point DCTCP uses (see TcpDctcp::PktsAcked in
     * tcp-dctcp.cc) -- segmentsAcked is only available on this hook, not
     * inside CongControl(). Feeds m_ackedBytesEcn/m_ackedBytesTotal, which
     * UpdateEcnAlpha() consumes and resets once per round.
     */
    void PktsAcked(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked, const Time& rtt) override;

    /**
     * @brief Set the random number stream used for randomized bw-probe timing.
     * @param stream the stream number.
     */
    virtual void SetStream(uint32_t stream);

    /**
     * @brief Get the current value of pacing gain.
     * @return the current pacing gain.
     */
    double GetPacingGain() const;

    /**
     * @brief Get the current value of cwnd gain.
     * @return the current cwnd gain.
     */
    double GetCwndGain() const;

    /**
     * @brief Get the current BBR high-level mode.
     * @return the current mode.
     */
    BbrMode_t GetBbrState() const;

    /**
     * @brief Get the current PROBE_BW sub-state.
     * @return the current sub-state.
     */
    BbrProbeBwPhase_t GetProbeBwPhase() const;

    /**
     * @brief Enter startup phase.
     */
    void EnterStartup();

    /**
     * @brief Enter drain phase.
     */
    void EnterDrain();

    /**
     * @brief Enter probe BW phase.
     */
    void EnterProbeBW();

    /**
     * @brief Enter probe RTT phase.
     */
    void EnterProbeRTT();

    /**
     * @brief Enter the PROBE_BW UP sub-state.
     * @param tcb the transmission control block.
     * @param sampleBw the latest bandwidth sample.
     */
    void StartBwProbeUp(Ptr<TcpSocketState> tcb, DataRate sampleBw);

    /**
     * @brief Enter the PROBE_BW DOWN sub-state.
     */
    void StartBwProbeDown();

    /**
     * @brief Enter the PROBE_BW CRUISE sub-state.
     */
    void StartBwProbeCruise();

    /**
     * @brief Enter the PROBE_BW REFILL sub-state.
     * @param bwProbeUpRounds the initial value of the bw-probe-up round counter.
     */
    void StartBwProbeRefill(uint32_t bwProbeUpRounds);

  private:
    // State shared with v1's STARTUP/DRAIN/PROBE_BW/PROBE_RTT skeleton

    BbrMode_t m_state{BBR_STARTUP}; //!< Current top-level state

    Time m_minRtt{Time::Max()};          //!< RTprop: the min RTT observed
    Time m_minRttStamp{Time::Min()};     //!< Time m_minRtt was last updated
    bool m_minRttExpired{false};         //!< Whether BBR.RTprop has expired
    Time m_minRttFilterLen{Seconds(10)}; //!< Length of the RTprop min-filter window (Linux: min_rtt_win_sec)

    /**
     * @brief Two-stage min-RTT filter, matching Linux's bbr_update_min_rtt():
     * m_probeRttMin tracks the min RTT within a short (default 5s) window,
     * and m_minRtt/m_minRttStamp above are refreshed from it once per short
     * window (or immediately when a new short-window low is found). This
     * makes m_minRtt track more responsively than a single 10s window would.
     */
    Time m_probeRttMin{Time::Max()};           //!< Min RTT observed within the short probe-RTT window
    Time m_probeRttMinStamp{Time::Min()};      //!< Time m_probeRttMin was last updated
    Time m_probeRttWinLen{MilliSeconds(5000)}; //!< Length of the short probe-RTT min-filter window (Linux: probe_rtt_win_ms)
    bool m_probeRttExpired{false}; //!< Whether the short probe-RTT window has expired, which schedules PROBE_RTT

    uint32_t m_roundCount{0};            //!< Count of packet-timed round trips
    bool m_roundStart{false};            //!< True if a round-trip boundary was reached
    uint64_t m_nextRoundDelivered{0};    //!< m_delivered value marking the next round trip
    bool m_isPipeFilled{false};          //!< True once STARTUP has found the bottleneck's capacity
    TcpSocketState::TcpCongState_t m_prevCaState{
        TcpSocketState::CA_OPEN}; //!< CA state on the previous ACK. Linux: prev_ca_state.

    /**
     * @brief Current best bandwidth estimate: max of the two bw_hi[] slots.
     * Replaces v1's m_maxBwFilter.GetBest(). Linux: bbr_max_bw().
     */
    DataRate MaxBw() const;

    /**
     * @brief The bandwidth estimate actually used to drive pacing rate and
     * target inflight: MaxBw() capped by the loss/ECN lower bound m_bwLo.
     * Linux: bbr_bw().
     */
    DataRate Bw() const;

    void UpdateRound(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    uint32_t m_fullBwCount{0}; //!< Number of round trips w/o large bw growth
    DataRate m_fullBw{0};      //!< Last bw growth-plateau baseline
    bool m_fullBwNow{false};   //!< True if the bw plateau was reached this round
    double m_pacingGain{1};    //!< Current pacing gain
    double m_cWndGain{1};      //!< Current cwnd gain
    uint32_t m_cycleIndex{0};  //!< Unused: retained for parity with v1's field layout
    Time m_cycleStamp{Seconds(0)}; //!< Time the current PROBE_BW sub-state was entered
    bool m_isInitialized{false};   //!< False until the first CA_OPEN CongestionStateSet() call
    Ptr<UniformRandomVariable> m_uv; //!< Rand, used for randomized bw-probe timing
    double m_highGain{0};            //!< STARTUP pacing gain (HighGain attribute)
    double m_startupCwndGain{2.0};   //!< STARTUP/DRAIN cwnd gain. Linux: bbr_startup_cwnd_gain.
    double m_drainGain{1.0 / 2.885}; //!< DRAIN pacing gain. Linux: bbr_drain_gain.

    static const char* const BbrModeName[BBR_PROBE_RTT + 1]; //!< Names for BbrMode_t, for logging

    void SetCycleIdx(BbrProbeBwPhase_t phase); //!< Linux: bbr_set_cycle_idx()

    // StartBwProbeRefill()/StartBwProbeUp()/StartBwProbeDown()/StartBwProbeCruise()
    // are declared in the public section above.

    void ResetCongestionSignals(); //!< Linux: bbr_reset_congestion_signals()
    void ResetLowerBounds();       //!< Linux: bbr_reset_lower_bounds()
    void PickProbeWait();          //!< Linux: bbr_pick_probe_wait()
    void ResetFullBw();            //!< Linux: bbr_reset_full_bw()

    /**
     * @brief True while in a phase that intentionally pushes inflight up to
     * probe for bw (STARTUP, or PROBE_BW's REFILL/UP sub-states) -- lower-bound
     * adaptation is skipped then. Linux: bbr_is_probing_bandwidth().
     */
    bool IsProbingBandwidth() const;

    /**
     * @brief Seed bw_lo/inflight_lo the first time they're needed, since they
     * start "unset" (sentinel). Linux: bbr_init_lower_bounds().
     */
    void InitLowerBounds(Ptr<TcpSocketState> tcb, bool initBw);

    /**
     * @brief Reduce bw and inflight to (1 - beta) of their current lower
     * bound, floored by this round's actually-observed latest bw/inflight.
     * Linux: bbr_loss_lower_bounds().
     */
    void LossLowerBounds(DataRate& bw, uint32_t& inflight);

    /**
     * @brief Per-round-trip adaptation of bw_lo/inflight_lo in response to
     * loss observed this round. Linux: bbr_adapt_lower_bounds().
     */
    void AdaptLowerBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /**
     * @brief Did this rate sample show loss (or ECN) severe enough to
     * suggest we've overfilled a buffer? Shared by the STARTUP exit check
     * below and the PROBE_BW upper-bound adaptation.
     * Linux: bbr_is_inflight_too_high().
     */
    bool IsInflightTooHigh(const TcpRateOps::TcpRateSample& rs) const;

    /**
     * @brief Common STARTUP-exit action for both the loss and ECN checks:
     * latch full_bw_reached and raise inflight_hi to the current BDP.
     * Linux: bbr_handle_queue_too_high_in_startup().
     */
    void HandleQueueTooHighInStartup(Ptr<TcpSocketState> tcb);

    uint64_t m_delivered{0}; //!< Mirrors rc.m_delivered on each CongControl() call

    Time m_probeRttDoneStamp{Seconds(0)}; //!< Time to exit from BBR_PROBE_RTT state
    bool m_idleRestart{false};            //!< When restarting from idle, set it true
    uint32_t m_priorCwnd{0};              //!< The last-known good congestion window
    uint32_t m_targetCWnd{0};             //!< Target cwnd, adapted to the estimated BDP
    Time m_probeRttDuration{MilliSeconds(200)}; //!< Minimum duration to stay in PROBE_RTT
    bool m_probeRttRoundDone{false};            //!< True when it is time to exit BBR_PROBE_RTT
    uint32_t m_minPipeCwnd{0}; //!< General cwnd floor (4 * segment size). Linux: cwnd_min_target.
    double m_probeRttCwndGain{0.5}; //!< Gain applied to BDP for the PROBE_RTT cwnd cap. Linux: probe_rtt_cwnd_gain.
    Ptr<TcpRateOps> m_rateOps; //!< Rate operations, used by HandleProbeRTT() to mark app-limited

    /**
     * @brief cwnd cap while in BBR_PROBE_RTT: BDP scaled by m_probeRttCwndGain
     * (0.5), floored at m_minPipeCwnd. Unlike a fixed floor, this scales up
     * on high-BDP paths so PROBE_RTT doesn't starve them more than needed to
     * measure RTprop. Linux: bbr_probe_rtt_cwnd().
     */
    uint32_t ProbeRttCwnd(Ptr<TcpSocketState> tcb);

    void SaveCwnd(Ptr<const TcpSocketState> tcb);
    void RestoreCwnd(Ptr<TcpSocketState> tcb);
    void HandleProbeRTT(Ptr<TcpSocketState> tcb);

    uint32_t m_extraAckedGain{1};                 //!< Gain factor for adding extra ack to cwnd
    uint32_t m_extraAcked[2]{0, 0};               //!< Maximum excess data acked in epoch
    uint32_t m_extraAckedWinRtt{0};               //!< Age of extra acked in rtt
    uint32_t m_extraAckedWinRttLength{5};         //!< Window length of extra acked window
    uint32_t m_ackEpochAckedResetThresh{1 << 17}; //!< Max allowed m_ackEpochAcked, in bytes
    uint32_t m_extraAckedIdx{0};                  //!< Current index in extra acked array
    Time m_ackEpochTime;                          //!< Starting of ACK sampling epoch time
    uint32_t m_ackEpochAcked{0};                  //!< Bytes ACKed in sampling epoch
    uint32_t m_sendQuantum{0}; //!< Max size of a data aggregate scheduled and transmitted together

    uint32_t InFlight(Ptr<TcpSocketState> tcb, DataRate bw, double gain);
    void InitPacingRate(Ptr<TcpSocketState> tcb);

    bool m_hasSeenRtt{false};    //!< True once a valid RTT sample has been seen
    double m_pacingMargin{0.01}; //!< Fractional safety margin subtracted from the pacing rate

    void ModulateCwndForProbeRTT(Ptr<TcpSocketState> tcb);
    uint32_t AckAggregationCwnd();
    void UpdateTargetCwnd(Ptr<TcpSocketState> tcb);
    void InitRoundCounting();
    void InitFullPipe();
    void SetRateOps(Ptr<TcpRateOps> rateOps) override;

    TracedCallback<std::vector<double>> m_traceInternalState; //!< Internal state trace, for logging

    // ---- Loss/ECN round-tracking (new in v3) ----

    bool m_lossInRound{false};        //!< True if loss was observed this round
    bool m_lossInCycle{false};        //!< True if loss was observed this PROBE_BW cycle
    uint32_t m_lossRoundDelivered{0}; //!< m_delivered value marking the start of the current round
    bool m_lossRoundStart{false};     //!< True on the round trip in which a new round starts
    uint8_t m_lossEventsInRound{0};   //!< Count of loss events this round (STARTUP only)

    bool m_ecnInRound{false};      //!< True if an ECN mark was observed this round
    bool m_ecnInCycle{false};      //!< True if an ECN mark was observed this PROBE_BW cycle
    bool m_ecnEligible{false};     //!< Whether ECN marks should influence the model (never set true)
    double m_ecnAlpha{0.0};        //!< EWMA of ecn-marked-bytes/total-bytes, in [0, 1]
    uint8_t m_startupEcnRounds{0}; //!< Consecutive high-ECN STARTUP rounds seen so far
    uint32_t m_ecnThresh{50};      //!< CE-mark ratio threshold (0-100 scale) for "ECN too high"
    uint8_t m_fullEcnCnt{2};       //!< Consecutive high-ECN STARTUP rounds needed to exit

    // Byte counters for the alpha EWMA, same accounting pattern as
    // TcpDctcp::m_ackedBytesEcn / m_ackedBytesTotal (tcp-dctcp.h). Filled in
    // PktsAcked(), consumed and reset in UpdateEcnAlpha() at each round start.
    uint32_t m_ackedBytesEcn{0};
    uint32_t m_ackedBytesTotal{0};

    // ---- Loss/ECN-bounded bandwidth/inflight model (the core new feature in v3) ----

    //!< Conservative short-term bandwidth lower bound. The sentinel value
    //!< DataRate(UINT64_MAX) means "unset" (no bound yet), matching Linux's
    //!< bw_lo == ~0U check in bbr_init_lower_bounds().
    DataRate m_bwLo{std::numeric_limits<uint64_t>::max()};
    DataRate m_bwHi[2]; //!< Windowed max-bandwidth filter (2-slot, replaces v1's single filter)
    uint32_t m_inflightLo{0xFFFFFFFF}; //!< Inflight lower bound (~0U sentinel means "unset")
    uint32_t m_inflightHi{0xFFFFFFFF}; //!< Inflight upper bound (~0U sentinel means "unset")
    double m_beta{0.3};                //!< Multiplicative loss cut factor (30%)
    double m_lossThresh{0.02};         //!< Loss ratio threshold (2%) for "inflight too high"
    uint8_t m_fullLossCnt{6}; //!< Consecutive high-loss rounds needed to exit STARTUP

    DataRate m_undoBwLo;         //!< Rollback value for m_bwLo (not yet wired up)
    uint32_t m_undoInflightLo{0}; //!< Rollback value for m_inflightLo (not yet wired up)
    uint32_t m_undoInflightHi{0}; //!< Rollback value for m_inflightHi (not yet wired up)

    DataRate m_bwLatest{0};       //!< Max delivered bw in the *last* round trip (non-windowed)
    uint32_t m_inflightLatest{0}; //!< Max delivered inflight in the *last* round trip

    // ---- PROBE_BW sub-state machine (new in v3) ----

    BbrProbeBwPhase_t m_probeBwPhase{BBR_BW_PROBE_CRUISE}; //!< Current PROBE_BW sub-state
    BbrAckPhase_t m_ackPhase{BBR_ACKS_INIT};                //!< Current bw-probe ACK phase
    uint8_t m_bwProbeUpRounds{0};    //!< Round count used by RaiseInflightHiSlope()
    uint32_t m_bwProbeUpCnt{0};      //!< ACKed-packets-per-inflight_hi-increment counter
    uint32_t m_bwProbeUpAcks{0};     //!< ACKed packets accumulated toward m_bwProbeUpCnt
    Time m_probeWait{Seconds(0)};    //!< Time to wait before the next bw probe
    uint8_t m_roundsSincProbe{0};    //!< Rounds elapsed since the last bw probe
    bool m_prevProbeTooHigh{false};  //!< True if the last bw probe saw excessive loss/ECN
    bool m_stoppedRiskyProbe{false}; //!< True if the last probe stopped before it looked risky
    bool m_bwProbeSamples{false};    //!< True while rate samples reflect an active bw probe

    double m_bwProbeRandRounds{2};      //!< Random round-count bound before the next bw probe
    Time m_bwProbeBaseWait{Seconds(2)}; //!< Base wall-clock wait before the next bw probe
    Time m_bwProbeRandWait{Seconds(1)}; //!< Random wall-clock wait added to m_bwProbeBaseWait
    uint32_t m_bwProbeMaxRounds{63};    //!< Max rounds between bw probes (Reno-coexistence bound)
    double m_inflightHeadroom{0.15};    //!< Headroom fraction kept below inflight_hi while cruising

    /** @brief BDP at Bw(), clamped by current cwnd. In bytes. Linux: bbr_target_inflight(). */
    uint32_t TargetInflight(Ptr<TcpSocketState> tcb);

    /** @brief Each round trip of PROBE_UP, double the volume of probing data.
     * Linux: bbr_raise_inflight_hi_slope(). */
    void RaiseInflightHiSlope(Ptr<TcpSocketState> tcb);

    /** @brief In PROBE_UP, not seeing high loss/ECN/queue, so raise inflight_hi.
     * Linux: bbr_probe_inflight_hi_upward(). */
    void ProbeInflightHiUpward(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /** @brief Loss/ECN too high while probing: cut inflight_hi (once per probe)
     * and restart the cycle. Linux: bbr_handle_inflight_too_high(). */
    void HandleInflightTooHigh(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /** @brief Grow/shrink inflight_hi based on bw-probe feedback.
     * @return true if a state transition was already decided.
     * Linux: bbr_adapt_upper_bounds(). */
    bool AdaptUpperBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /** @brief Flip the 2-slot max-bw filter window once per PROBE_BW cycle.
     * Linux: bbr_advance_max_bw_filter(). */
    void AdvanceMaxBwFilter();

    /** @brief Is it time to kick off the next bw probe?
     * Linux: bbr_check_time_to_probe_bw(). */
    bool CheckTimeToProbeBw(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /** @brief Is it time to transition from PROBE_DOWN to PROBE_CRUISE?
     * Linux: bbr_check_time_to_cruise(). */
    bool CheckTimeToCruise(Ptr<TcpSocketState> tcb, uint32_t inflight, DataRate bw);

    /** @brief Reno-coexistence bound on inter-probe spacing.
     * Linux: bbr_is_reno_coexistence_probe_time(). */
    bool IsRenoCoexistenceProbeTime(Ptr<TcpSocketState> tcb);

    /** @brief Has the given interval elapsed since the current cycle phase started?
     * Linux: bbr_has_elapsed_in_phase(). */
    bool HasElapsedInPhase(Time interval) const;

    /** @brief inflight_hi minus headroom, floored at the min cwnd target.
     * Linux: bbr_inflight_with_headroom(). */
    uint32_t InflightWithHeadroom() const;

    /**
     * @brief Update per-round loss/ECN signals and adjust bw_lo/inflight_lo/hi.
     * Linux: bbr_update_congestion_signals(), plus its callees
     * bbr_loss_lower_bounds(), bbr_ecn_lower_bounds(), bbr_adapt_lower_bounds(),
     * bbr_adapt_upper_bounds(), bbr_handle_inflight_too_high().
     */
    void UpdateCongestionSignals(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /**
     * @brief Loss-based STARTUP exit condition (v3's addition to v1's
     * bandwidth-plateau-only check).
     * Linux: bbr_check_loss_too_high_in_startup().
     */
    void CheckLossTooHighInStartup(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /**
     * @brief ECN-based STARTUP exit condition.
     * Linux: bbr_check_ecn_too_high_in_startup().
     */
    void CheckEcnTooHighInStartup(Ptr<TcpSocketState> tcb, uint32_t ceRatio);

    /**
     * @brief STARTUP exit check -- same role as v1's CheckFullPipe(), but also
     * short-circuits on loss/ECN via the two functions above.
     * Linux: bbr_check_full_bw_reached().
     */
    void CheckFullBwReached(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /**
     * @brief Drives the 4-phase PROBE_BW sub-state machine
     * (UP -> DOWN -> CRUISE -> REFILL -> UP ...). Replaces v1's
     * CheckCyclePhase()/AdvanceCyclePhase().
     * Linux: bbr_update_cycle_phase(), plus callees
     * bbr_start_bw_probe_refill/up/down/cruise(), bbr_check_time_to_probe_bw(),
     * bbr_check_time_to_cruise().
     */
    void UpdateCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /**
     * @brief Per-round EWMA of ECN CE-mark ratio.
     * Linux: bbr_update_ecn_alpha().
     */
    int UpdateEcnAlpha(Ptr<TcpSocketState> tcb);

    /**
     * @brief Tracks max delivered bw/inflight in the *current* round
     * (non-windowed -- feeds the loss/ECN bound logic, distinct from the
     * windowed m_bwHi[] max filter).
     * Linux: bbr_update_latest_delivery_signals() and
     * bbr_advance_latest_delivery_signals(), called at the start and end of
     * CongControl()/bbr_main() respectively.
     */
    void UpdateLatestDeliverySignals(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void AdvanceLatestDeliverySignals(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    /**
     * @brief Final cwnd clamp against the inflight_hi/lo bounds. Has no v1
     * equivalent. Linux: bbr_bound_cwnd_for_inflight_model().
     */
    void BoundCwndForInflightModel(Ptr<TcpSocketState> tcb);

    void UpdateModelAndState(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void UpdateControlParameters(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void UpdateAckAggregation(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void CheckDrain(Ptr<TcpSocketState> tcb);
    void UpdateRTprop(Ptr<TcpSocketState> tcb);
    void CheckProbeRTT(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void SetPacingRate(Ptr<TcpSocketState> tcb, double gain);
    void SetSendQuantum(Ptr<TcpSocketState> tcb);
    void SetCwnd(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    // EnterStartup()/EnterDrain()/EnterProbeBW()/EnterProbeRTT() are declared
    // in the public section above.
    void SetBbrState(BbrMode_t state);
    void ExitProbeRTT();
};

} // namespace ns3

#endif // TCPBBRV3_H
