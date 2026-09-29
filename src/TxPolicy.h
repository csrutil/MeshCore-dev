#pragma once

#include <stdint.h>

#define TX_POLICY_MODE_BUDGET  0
#define TX_POLICY_MODE_BURST   1

namespace mesh {

/**
 * \brief  Runtime values read by the TX policies; implemented by Dispatcher,
 *      since the preferences can change while running.
*/
class TxPolicyEnv {
public:
  virtual float getAirtimeBudgetFactor() const = 0;
  virtual unsigned long getDutyCycleWindowMs() const = 0;
  virtual uint32_t getBurstMaxTxMs() const = 0;
  virtual uint32_t getBurstQuietMs() const = 0;
};

/**
 * \brief  Policy for when a new TX may start, and how much TX airtime budget remains.
 *        Timestamps are millis() values (uint32_t, like on all supported targets);
 *        implementations use wrap-safe arithmetic.
*/
class TxPolicy {
public:
  virtual void begin(uint32_t now) = 0;

  /**
   * \brief  May a new TX of this airtime start now?
   * \param  pkt_airtime  estimated airtime of the packet to be sent
   * \param  mtu_airtime  estimated airtime of a maximum-size packet
   * \param  retry_at     when not allowed, when to ask again
  */
  virtual bool canStart(uint32_t now, uint32_t pkt_airtime, uint32_t mtu_airtime,
                        uint32_t& retry_at) = 0;

  /**
   * \brief  A TX completed; updates the policy state and sets next_tx_time for the next send.
  */
  virtual void onTxDone(uint32_t now, uint32_t est_airtime, uint32_t actual_ms,
                        unsigned long& next_tx_time) = 0;

  /**
   * \brief  The TX did not complete (timed out).
  */
  virtual void onTxAborted(uint32_t now) { }

  /**
   * \returns  true if the next ready packet of this airtime should be sent now,
   *      without switching the radio back to Rx first.
  */
  virtual bool canChain(uint32_t pkt_airtime) const { return false; }

  virtual uint32_t remainingTxMs(uint32_t now) const = 0;
};

/**
 * \brief  The default policy: an airtime token bucket that refills at
 *      1/(1 + airtime budget factor) over the duty cycle window.
*/
class AirtimeBudgetTxPolicy : public TxPolicy {
  TxPolicyEnv* _env;
  uint32_t tx_budget_ms;
  uint32_t last_budget_update;

  void updateBudget(uint32_t now);

public:
  AirtimeBudgetTxPolicy(TxPolicyEnv& env)
    : _env(&env), tx_budget_ms(0), last_budget_update(0) { }

  void begin(uint32_t now) override;
  bool canStart(uint32_t now, uint32_t pkt_airtime, uint32_t mtu_airtime,
                uint32_t& retry_at) override;
  void onTxDone(uint32_t now, uint32_t est_airtime, uint32_t actual_ms,
                unsigned long& next_tx_time) override;
  uint32_t remainingTxMs(uint32_t now) const override;
};

/**
 * \brief  Sends queued packets back-to-back while the total estimated TX airtime of
 *      the burst is <= max_ms. Once the next packet does not fit, TX stays quiet for
 *      quiet_ms (radio in Rx), then the next burst starts.
*/
class BurstTxPolicy : public TxPolicy {
  TxPolicyEnv* _env;
  uint32_t burst_used_ms;   // estimated airtime sent in the current burst
  uint32_t last_tx_end;

  uint32_t maxMs() const { return _env->getBurstMaxTxMs(); }
  uint32_t quietMs() const { return _env->getBurstQuietMs(); }

public:
  BurstTxPolicy(TxPolicyEnv& env)
    : _env(&env)
  {
    burst_used_ms = 0;
    last_tx_end = 0;
  }

  void begin(uint32_t now) override;
  bool canStart(uint32_t now, uint32_t pkt_airtime, uint32_t mtu_airtime,
                uint32_t& retry_at) override;
  void onTxDone(uint32_t now, uint32_t est_airtime, uint32_t actual_ms,
                unsigned long& next_tx_time) override;
  void onTxAborted(uint32_t now) override;
  bool canChain(uint32_t pkt_airtime) const override;
  uint32_t remainingTxMs(uint32_t now) const override;
};

}
