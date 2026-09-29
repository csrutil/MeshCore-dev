#include "TxPolicy.h"

#define MIN_TX_BUDGET_RESERVE_MS   100    // min budget (ms) required before allowing next TX
#define MIN_TX_BUDGET_AIRTIME_DIV  2      // require at least 1/N of estimated airtime as budget before TX

namespace mesh {

void AirtimeBudgetTxPolicy::begin(uint32_t now) {
  float duty_cycle = 1.0f / (1.0f + _env->getAirtimeBudgetFactor());
  tx_budget_ms = (uint32_t)(_env->getDutyCycleWindowMs() * duty_cycle);
  last_budget_update = now;
}

void AirtimeBudgetTxPolicy::updateBudget(uint32_t now) {
  uint32_t elapsed = now - last_budget_update;

  float duty_cycle = 1.0f / (1.0f + _env->getAirtimeBudgetFactor());
  uint32_t max_budget = (uint32_t)(_env->getDutyCycleWindowMs() * duty_cycle);
  uint32_t refill = (uint32_t)(elapsed * duty_cycle);

  if (refill > 0) {
    tx_budget_ms += refill;
    if (tx_budget_ms > max_budget) {
      tx_budget_ms = max_budget;
    }
    last_budget_update = now;
  }
}

bool AirtimeBudgetTxPolicy::canStart(uint32_t now, uint32_t pkt_airtime, uint32_t mtu_airtime,
                                     uint32_t& retry_at) {
  (void)pkt_airtime;
  updateBudget(now);

  if (tx_budget_ms < mtu_airtime / MIN_TX_BUDGET_AIRTIME_DIV) {
    float duty_cycle = 1.0f / (1.0f + _env->getAirtimeBudgetFactor());
    uint32_t needed = mtu_airtime / MIN_TX_BUDGET_AIRTIME_DIV - tx_budget_ms;
    retry_at = now + (uint32_t)(needed / duty_cycle);
    return false;
  }
  retry_at = now;
  return true;
}

void AirtimeBudgetTxPolicy::onTxDone(uint32_t now, uint32_t est_airtime, uint32_t actual_ms,
                                     unsigned long& next_tx_time) {
  (void)est_airtime;
  updateBudget(now);

  if (actual_ms > tx_budget_ms) {
    tx_budget_ms = 0;
  } else {
    tx_budget_ms -= actual_ms;
  }

  if (tx_budget_ms < MIN_TX_BUDGET_RESERVE_MS) {
    float duty_cycle = 1.0f / (1.0f + _env->getAirtimeBudgetFactor());
    uint32_t needed = MIN_TX_BUDGET_RESERVE_MS - tx_budget_ms;
    next_tx_time = now + (uint32_t)(needed / duty_cycle);
  } else {
    next_tx_time = now;
  }
}

uint32_t AirtimeBudgetTxPolicy::remainingTxMs(uint32_t now) const {
  (void)now;
  return tx_budget_ms;
}

void BurstTxPolicy::begin(uint32_t now) {
  burst_used_ms = 0;
  last_tx_end = now;
}

bool BurstTxPolicy::canStart(uint32_t now, uint32_t pkt_airtime, uint32_t mtu_airtime,
                             uint32_t& retry_at) {
  (void)mtu_airtime;
  if ((uint32_t)(now - last_tx_end) >= quiet_ms) {
    burst_used_ms = 0;   // quiet period elapsed: start a new burst
  }

  if (burst_used_ms + pkt_airtime <= max_ms) {
    retry_at = now;
    return true;
  }
  retry_at = last_tx_end + quiet_ms;   // wait out the quiet period
  return false;
}

void BurstTxPolicy::onTxDone(uint32_t now, uint32_t est_airtime, uint32_t actual_ms,
                             unsigned long& next_tx_time) {
  (void)actual_ms;
  burst_used_ms += est_airtime;
  last_tx_end = now;
  next_tx_time = now;
}

void BurstTxPolicy::onTxAborted(uint32_t now) {
  burst_used_ms = max_ms;   // treat the burst as fully used
  last_tx_end = now;        // quiet period starts now
}

bool BurstTxPolicy::canChain(uint32_t pkt_airtime) const {
  return burst_used_ms + pkt_airtime <= max_ms;
}

uint32_t BurstTxPolicy::remainingTxMs(uint32_t now) const {
  (void)now;
  return (burst_used_ms >= max_ms) ? 0 : (max_ms - burst_used_ms);
}

}
