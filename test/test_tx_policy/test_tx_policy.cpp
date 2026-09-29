#include <gtest/gtest.h>
#include "TxPolicy.h"
#include "Dispatcher.h"
#include "helpers/StaticPoolPacketManager.h"
#include "helpers/CommonRadioPrefs.h"
#include "target.h"

#include <cstring>
#include <string>
#include <vector>

using namespace mesh;

// ---------------------------------------------------------------- mocks

class MockBudgetEnv : public TxPolicyEnv {
public:
  float factor = 1.0f;
  unsigned long window_ms = 3600000;
  uint32_t burst_max_ms = 1000;
  uint32_t burst_quiet_ms = 200;
  float getAirtimeBudgetFactor() const override { return factor; }
  unsigned long getDutyCycleWindowMs() const override { return window_ms; }
  uint32_t getBurstMaxTxMs() const override { return burst_max_ms; }
  uint32_t getBurstQuietMs() const override { return burst_quiet_ms; }
};

class MockClock : public MillisecondClock {
public:
  unsigned long value = 0;
  unsigned long getMillis() override { return value; }
};

class MockRadio : public Radio {
public:
  std::vector<std::string> events;
  int send_count = 0, finished_count = 0, recv_count = 0;
  bool send_complete = false;
  int airtime_per_byte = 10;

  int recvRaw(uint8_t*, int) override { recv_count++; events.push_back("recv"); return 0; }
  uint32_t getEstAirtimeFor(int len_bytes) override { return (uint32_t)len_bytes * airtime_per_byte; }
  float packetScore(float, int) override { return 1.0f; }
  bool startSendRaw(const uint8_t*, int) override { send_count++; events.push_back("send"); return true; }
  bool isSendComplete() override { return send_complete; }
  void onSendFinished() override { finished_count++; events.push_back("finished"); }
  bool isInRecvMode() const override { return true; }
};

class TestDispatcher : public Dispatcher {
public:
  TestDispatcher(Radio& radio, MillisecondClock& ms, PacketManager& mgr)
    : Dispatcher(radio, ms, mgr) { }
  DispatcherAction onRecvPacket(Packet*) override { return ACTION_RELEASE; }

  uint8_t tx_policy_mode = TX_POLICY_MODE_BUDGET;   // runtime-selectable, like the examples read from prefs
protected:
  uint8_t getTxPolicyMode() const override { return tx_policy_mode; }
};

// definition of the radio_driver symbol referenced by CommonRadioPrefs.cpp
MockRadioDriver radio_driver;

// ------------------------------------------------------- airtime budget policy

TEST(TxAirtimeBudget, InitialBudgetIsWindowTimesDutyCycle) {
  MockBudgetEnv env;   // factor 1.0 -> duty cycle 0.5
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(1000);
  EXPECT_EQ(policy.remainingTxMs(1000), 1800000UL);
}

TEST(TxAirtimeBudget, RefillsAtDutyCycleRate) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(1000, 100, 600000, next_tx);   // debit 600000 -> 1200000

  uint32_t retry_at;
  EXPECT_TRUE(policy.canStart(2000, 100, 2550, retry_at));
  EXPECT_EQ(policy.remainingTxMs(2000), 1200500UL);   // +1000ms * 0.5
}

TEST(TxAirtimeBudget, RefillCapsAtWindowMaximum) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(1000, 100, 1800000, next_tx);   // debit the whole budget

  uint32_t retry_at;
  EXPECT_TRUE(policy.canStart(7200000, 100, 2550, retry_at));   // 2 hours later
  EXPECT_EQ(policy.remainingTxMs(7200000), 1800000UL);   // capped, not accumulated
}

TEST(TxAirtimeBudget, MtuAirtimeReserveBlocksAndSetsRetry) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(1000, 100, 1799500, next_tx);   // budget left: 500

  uint32_t retry_at;
  // needs mtu_airtime/2 = 1500ms in the budget (budget is 1000 after refill)
  EXPECT_FALSE(policy.canStart(2000, 100, 3000, retry_at));
  EXPECT_EQ(retry_at, 3000UL);   // 2000 + (1500 - 1000) / 0.5
}

TEST(TxAirtimeBudget, DebitsByMeasuredAirtime) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(1000, 100, 500, next_tx);
  EXPECT_EQ(policy.remainingTxMs(1000), 1800000UL - 500);
  EXPECT_EQ(next_tx, 1000UL);   // still above the minimum reserve
}

TEST(TxAirtimeBudget, ReserveDelayWhenBudgetBelowMinimum) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(1000, 100, 1799600, next_tx);   // refill 500, debit to 400
  EXPECT_EQ(next_tx, 1000UL);

  policy.onTxDone(2000, 100, 1799550, next_tx);   // refill 500 -> 900, debit overflows -> 0
  EXPECT_EQ(next_tx, 2200UL);   // 2000 + 100 / 0.5
  EXPECT_EQ(policy.remainingTxMs(2000), 0UL);
}

TEST(TxAirtimeBudget, RefillHandlesMillisWrap) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0xFFFFFFF0UL);
  unsigned long next_tx;
  policy.onTxDone(0xFFFFFFF0UL, 100, 1800000, next_tx);   // debit the whole budget

  uint32_t retry_at;
  // 0x150 - 0xFFFFFFF0 wraps to 0x160 = 352 elapsed -> refill 176
  EXPECT_TRUE(policy.canStart(0x150UL, 100, 300, retry_at));
  EXPECT_EQ(policy.remainingTxMs(0x150UL), 176UL);
}

TEST(TxAirtimeBudget, DoesNotChain) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  EXPECT_FALSE(policy.canChain(100));
}

// ------------------------------------------------------------ burst policy

class BurstPolicyTest : public ::testing::Test {
protected:
  MockBudgetEnv env;
  BurstTxPolicy policy{env};

  void SetUp() override {
    env.burst_max_ms = 1000;
    env.burst_quiet_ms = 200;
    policy.begin(0);
  }
};

TEST_F(BurstPolicyTest, ChainsUntilBurstMax) {
  uint32_t retry_at;
  EXPECT_TRUE(policy.canStart(0, 300, 2550, retry_at));

  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);
  EXPECT_TRUE(policy.canChain(300));
  policy.onTxDone(20, 300, 10, next_tx);
  EXPECT_TRUE(policy.canChain(300));
  policy.onTxDone(30, 300, 10, next_tx);
  EXPECT_FALSE(policy.canChain(300));   // 900 + 300 > 1000
}

TEST_F(BurstPolicyTest, WaitsQuietPeriodWhenPacketDoesNotFit) {
  unsigned long next_tx;
  policy.onTxDone(10, 1000, 10, next_tx);   // burst fully used

  uint32_t retry_at;
  // gap below the quiet period: no reset
  EXPECT_FALSE(policy.canStart(209, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 210UL);   // last_tx_end + 200

  // gap of exactly the quiet period: new burst
  EXPECT_TRUE(policy.canStart(210, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 210UL);
}

TEST_F(BurstPolicyTest, GapBelowQuietPeriodDoesNotReset) {
  unsigned long next_tx;
  policy.onTxDone(10, 800, 10, next_tx);

  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(100, 300, 2550, retry_at));   // 800 + 300 > 1000, gap < 200
  EXPECT_EQ(retry_at, 210UL);
}

TEST_F(BurstPolicyTest, FittingPacketSendsWithoutWaiting) {
  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);

  uint32_t retry_at;
  EXPECT_TRUE(policy.canStart(50, 700, 2550, retry_at));   // 300 + 700 <= 1000
  EXPECT_EQ(retry_at, 50UL);
}

TEST_F(BurstPolicyTest, RejectsSinglePacketAboveBurstMax) {
  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(0, 2000, 2550, retry_at));
}

TEST_F(BurstPolicyTest, RemainingFallsToZeroAtBurstMax) {
  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);
  EXPECT_EQ(policy.remainingTxMs(10), 700UL);
  policy.onTxDone(20, 1000, 10, next_tx);
  EXPECT_EQ(policy.remainingTxMs(20), 0UL);
}

TEST_F(BurstPolicyTest, TimeoutEndsBurstAndStartsQuietPeriod) {
  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);

  policy.onTxAborted(50);
  EXPECT_FALSE(policy.canChain(100));

  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(249, 100, 2550, retry_at));
  EXPECT_EQ(retry_at, 250UL);
  EXPECT_TRUE(policy.canStart(250, 100, 2550, retry_at));
}

TEST_F(BurstPolicyTest, HandlesMillisWrapAround) {
  unsigned long next_tx;
  policy.onTxDone(0xFFFFFF00UL, 800, 10, next_tx);

  uint32_t retry_at;
  // gap below the quiet period: 0xFFFFFF80 - 0xFFFFFF00 = 0x80 = 128
  EXPECT_FALSE(policy.canStart(0xFFFFFF80UL, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 0xFFFFFFC8UL);   // 0xFFFFFF00 + 200 (no wrap yet)

  // 0x20 - 0xFFFFFF00 wraps to 0x120 = 288: quiet period elapsed -> new burst
  EXPECT_TRUE(policy.canStart(0x20UL, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 0x20UL);
}

TEST_F(BurstPolicyTest, ReadsLimitsFromEnvAtRuntime) {
  unsigned long next_tx;
  policy.onTxDone(10, 800, 10, next_tx);

  env.burst_max_ms = 2000;   // lower limits now fit, higher ones do not
  EXPECT_TRUE(policy.canChain(1000));   // 800 + 1000 <= 2000
  EXPECT_FALSE(policy.canChain(1201));   // 800 + 1201 > 2000

  env.burst_max_ms = 1000;
  env.burst_quiet_ms = 500;   // quiet period now ends at 510, not 210
  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(500, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 510UL);
}

// ------------------------------------------------------------- dispatcher

class DispatcherTxTest : public ::testing::Test {
protected:
  MockClock clock;
  MockRadio radio;
  StaticPoolPacketManager mgr{16};
  TestDispatcher dispatcher{radio, clock, mgr};

  void SetUp() override {
    clock.value = 1000;
    dispatcher.begin();
  }

  Packet* queuePacket(uint16_t payload_len) {
    Packet* pkt = dispatcher.obtainNewPacket();
    pkt->header = ROUTE_TYPE_FLOOD;
    pkt->payload_len = payload_len;
    memset(pkt->payload, 0, payload_len);
    dispatcher.sendPacket(pkt, 0, 0);
    return pkt;
  }
};

TEST_F(DispatcherTxTest, SendsOnePacketThenResumesRx) {
  queuePacket(10);   // raw len 12 -> est airtime 120ms

  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);

  radio.send_complete = true;
  clock.value = 1050;
  dispatcher.loop();
  EXPECT_EQ(radio.finished_count, 1);
  EXPECT_EQ(dispatcher.getNumSentFlood(), 1);

  // budget debited by the measured 50ms (and refilled by the elapsed 50ms)
  EXPECT_EQ(dispatcher.getRemainingTxBudget(), 1800000UL - 50);

  // the radio went back into Rx between packets
  EXPECT_EQ(radio.events, (std::vector<std::string>{"recv", "send", "finished", "recv"}));
}

TEST_F(DispatcherTxTest, TimeoutReleasesPacketWithoutDebitingBudget) {
  queuePacket(10);
  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);

  clock.value = 1000 + 120 * 3 / 2 + 1;   // past the max-airtime expiry
  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);
  EXPECT_EQ(radio.finished_count, 1);
  EXPECT_EQ(dispatcher.getRemainingTxBudget(), 1800000UL);
  EXPECT_EQ(mgr.getOutboundTotal(), 0);
}

TEST_F(DispatcherTxTest, ChainsPacketsWithoutReturningToRx) {
  dispatcher.tx_policy_mode = TX_POLICY_MODE_BURST;
  queuePacket(28);   // raw len 30 -> est airtime 300ms
  queuePacket(28);
  queuePacket(28);

  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);

  radio.send_complete = true;
  clock.value = 1050;
  dispatcher.loop();   // completes send 1, chains send 2
  clock.value = 1100;
  dispatcher.loop();   // completes send 2, chains send 3
  clock.value = 1150;
  dispatcher.loop();   // completes send 3, queue empty -> back to Rx

  EXPECT_EQ(radio.send_count, 3);
  EXPECT_EQ(radio.finished_count, 3);
  EXPECT_EQ(dispatcher.getNumSentFlood(), 3);
  // no recv between the burst packets
  EXPECT_EQ(radio.events, (std::vector<std::string>{
    "recv", "send", "finished", "send", "finished", "send", "finished", "recv"}));
}

TEST_F(DispatcherTxTest, StopsChainingWhenBurstMaxReached) {
  dispatcher.tx_policy_mode = TX_POLICY_MODE_BURST;
  queuePacket(58);   // raw len 60 -> est airtime 600ms
  queuePacket(58);

  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);

  radio.send_complete = true;
  clock.value = 1050;
  dispatcher.loop();   // 600 used; 600 + 600 > 1000 -> no chaining

  EXPECT_EQ(radio.send_count, 1);
  EXPECT_EQ(radio.recv_count, 2);   // went back to Rx after the burst

  clock.value = 1200;
  dispatcher.loop();   // still inside the quiet period (last_tx_end 1050 + 200)
  EXPECT_EQ(radio.send_count, 1);

  clock.value = 1300;
  dispatcher.loop();   // quiet period elapsed -> next burst
  EXPECT_EQ(radio.send_count, 2);
}

TEST_F(DispatcherTxTest, DropsPacketWhoseAirtimeExceedsBurstMax) {
  dispatcher.tx_policy_mode = TX_POLICY_MODE_BURST;
  queuePacket(180);   // raw len 182 -> est airtime 1820ms > 1000

  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 0);
  EXPECT_EQ(mgr.getOutboundTotal(), 0);
  EXPECT_EQ(mgr.getFreeCount(), 16);   // packet returned to the pool
}

TEST_F(DispatcherTxTest, OversizedPacketSentInBudgetMode) {
  queuePacket(180);   // est airtime 1820ms fits the (window-scale) budget

  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);
}

TEST_F(DispatcherTxTest, TxTimeoutEndsBurstAndStartsQuietPeriod) {
  dispatcher.tx_policy_mode = TX_POLICY_MODE_BURST;
  queuePacket(28);   // est airtime 300ms -> expiry 1000 + 450
  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 1);

  clock.value = 1500;
  dispatcher.loop();   // timed out
  EXPECT_EQ(radio.finished_count, 1);

  queuePacket(28);
  clock.value = 1600;
  dispatcher.loop();   // inside the quiet period: nothing sent
  EXPECT_EQ(radio.send_count, 1);

  clock.value = 1750;
  dispatcher.loop();   // quiet period elapsed
  EXPECT_EQ(radio.send_count, 2);
}

TEST_F(DispatcherTxTest, ModeChangeDuringSendAppliesAfterCompletion) {
  queuePacket(28);   // budget mode: first packet sent under the budget policy
  dispatcher.loop();
  ASSERT_EQ(radio.send_count, 1);

  dispatcher.tx_policy_mode = TX_POLICY_MODE_BURST;   // while the send is in flight
  radio.send_complete = true;
  clock.value = 1050;
  dispatcher.loop();
  // the completion path ran under the budget policy (no burst chaining), and the
  // radio went back into Rx before checkSend applied the new policy
  EXPECT_EQ(radio.events, (std::vector<std::string>{"recv", "send", "finished", "recv"}));
  EXPECT_EQ(dispatcher.getRemainingTxBudget(), 1000UL);   // new policy: full burst allowance

  queuePacket(28);
  queuePacket(28);
  clock.value = 1100;
  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 2);

  radio.send_complete = true;
  clock.value = 1150;
  dispatcher.loop();   // burst chaining is now active: no Rx between the sends
  EXPECT_EQ(radio.send_count, 3);
}

TEST_F(DispatcherTxTest, SwitchingBackToBudgetStartsWithFullBudget) {
  dispatcher.tx_policy_mode = TX_POLICY_MODE_BURST;
  queuePacket(28);
  dispatcher.loop();
  ASSERT_EQ(radio.send_count, 1);

  radio.send_complete = true;
  clock.value = 1050;
  dispatcher.loop();
  ASSERT_EQ(dispatcher.getRemainingTxBudget(), 700UL);   // 1000 - 300 burst airtime

  dispatcher.tx_policy_mode = TX_POLICY_MODE_BUDGET;
  clock.value = 1100;
  dispatcher.loop();
  EXPECT_EQ(dispatcher.getRemainingTxBudget(), 1800000UL);   // begin() grants a full window
}

// ------------------------------------------------------------- tx_policy CLI

// implements the CommonRadioPrefs interface on plain members (defaults as in NodePrefs)
class TestRadioPrefs : public CommonRadioPrefs {
  void structure() override { }   // not exercised by handleCommand()
public:
  float freq = 0, bw = 0, airtime_factor = 0, rx_delay_base = 0, tx_delay_factor = 0, direct_tx_delay_factor = 0;
  uint8_t sf = 0, cr = 0, cad_enabled = 0, interference_threshold = 0, rx_boosted_gain = 0;
  uint8_t tx_power_dbm = 0;
  uint16_t agc_reset_interval = 0;
  uint8_t path_hash_mode = 0, multi_acks = 0;
  uint8_t radio_fem_rxgain = 0, radio_fem_txgain = 0;
  uint8_t tx_policy = TX_POLICY_MODE_BUDGET;
  uint32_t burst_max_ms = 1000, burst_quiet_ms = 0;

  float getFreq() const override { return freq; }
  void setFreq(float f) override { freq = f; markDirty(); }
  float getBandwidth() const override { return bw; }
  void setBandwidth(float v) override { bw = v; markDirty(); }
  uint8_t getSpreadFactor() const override { return sf; }
  void setSpreadFactor(uint8_t v) override { sf = v; markDirty(); }
  uint8_t getCodingRate() const override { return cr; }
  void setCodingRate(uint8_t v) override { cr = v; markDirty(); }
  float getAirtimeFactor() const override { return airtime_factor; }
  void setAirtimeFactor(float v) override { airtime_factor = v; markDirty(); }
  bool isCadEnabled() const override { return cad_enabled; }
  void setCadEnabled(bool en) override { cad_enabled = en; markDirty(); }
  uint8_t getIntThresh() const override { return interference_threshold; }
  void setIntThresh(uint8_t t) override { interference_threshold = t; markDirty(); }
  uint8_t getRxGain() const override { return rx_boosted_gain; }
  void setRxGain(uint8_t g) override { rx_boosted_gain = g; markDirty(); }
  uint8_t getTxPower() const override { return tx_power_dbm; }
  void setTxPower(uint8_t dbm) override { tx_power_dbm = dbm; markDirty(); }
  float getRxDelay() const override { return rx_delay_base; }
  void setRxDelay(float d) override { rx_delay_base = d; markDirty(); }
  uint16_t getAgcResetInt() const override { return agc_reset_interval; }
  void setAgcResetInt(uint16_t secs) override { agc_reset_interval = secs; markDirty(); }
  uint8_t getHashMode() const override { return path_hash_mode; }
  void setHashMode(uint8_t m) override { path_hash_mode = m; markDirty(); }
  uint8_t getMultiAcks() const override { return multi_acks; }
  void setMultiAcks(uint8_t m) override { multi_acks = m; markDirty(); }
  float getFloodTxDelay() const override { return tx_delay_factor; }
  void setFloodTxDelay(float d) override { tx_delay_factor = d; markDirty(); }
  float getDirectTxDelay() const override { return direct_tx_delay_factor; }
  void setDirectTxDelay(float d) override { direct_tx_delay_factor = d; markDirty(); }
  uint8_t getFEMRxGain() const override { return radio_fem_rxgain; }
  void setFEMRxGain(uint8_t g) override { radio_fem_rxgain = g; markDirty(); }
  uint8_t getFEMTxGain() const override { return radio_fem_txgain; }
  void setFEMTxGain(uint8_t g) override { radio_fem_txgain = g; markDirty(); }
  uint8_t getTxPolicyMode() const override { return tx_policy; }
  void setTxPolicyMode(uint8_t mode) override { tx_policy = mode; markDirty(); }
  uint32_t getBurstMaxTxMs() const override { return burst_max_ms; }
  void setBurstMaxTxMs(uint32_t ms) override { burst_max_ms = ms; markDirty(); }
  uint32_t getBurstQuietMs() const override { return burst_quiet_ms; }
  void setBurstQuietMs(uint32_t ms) override { burst_quiet_ms = ms; markDirty(); }
};

TEST(TxPolicyCli, GetReportsBudgetByDefault) {
  TestRadioPrefs prefs;
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("get tx_policy", 0, reply));
  EXPECT_STREQ(reply, "> budget");
}

TEST(TxPolicyCli, SetBudgetIsAccepted) {
  TestRadioPrefs prefs;
  prefs.setTxPolicyMode(TX_POLICY_MODE_BURST);
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("set tx_policy budget", 0, reply));
  EXPECT_STREQ(reply, "OK");
  EXPECT_EQ(prefs.getTxPolicyMode(), (uint8_t)TX_POLICY_MODE_BUDGET);
}

TEST(TxPolicyCli, SetBurstStoresLimitsAndMode) {
  TestRadioPrefs prefs;
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("set tx_policy burst 1000 200", 0, reply));
  EXPECT_STREQ(reply, "OK");
  EXPECT_EQ(prefs.getTxPolicyMode(), (uint8_t)TX_POLICY_MODE_BURST);
  EXPECT_EQ(prefs.getBurstMaxTxMs(), 1000UL);
  EXPECT_EQ(prefs.getBurstQuietMs(), 200UL);
  ASSERT_TRUE(prefs.handleCommand("get tx_policy", 0, reply));
  EXPECT_STREQ(reply, "> burst 1000 200");
}

TEST(TxPolicyCli, SetBurstRequiresBothLimits) {
  TestRadioPrefs prefs;
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("set tx_policy burst 1000", 0, reply));
  EXPECT_STREQ(reply, "ERROR: tx_policy burst requires max_ms and quiet_ms");
  EXPECT_EQ(prefs.getTxPolicyMode(), (uint8_t)TX_POLICY_MODE_BUDGET);
}

TEST(TxPolicyCli, SetBurstRejectsMaxAboveWatchdogLimit) {
  TestRadioPrefs prefs;
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("set tx_policy burst 8001 200", 0, reply));
  EXPECT_STREQ(reply, "ERROR: tx_policy max_ms must be 1-8000");
  EXPECT_EQ(prefs.getTxPolicyMode(), (uint8_t)TX_POLICY_MODE_BUDGET);
}

TEST(TxPolicyCli, SetBurstRejectsZeroQuietTime) {
  TestRadioPrefs prefs;
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("set tx_policy burst 1000 0", 0, reply));
  EXPECT_STREQ(reply, "ERROR: tx_policy quiet_ms must be >= 1");
  EXPECT_EQ(prefs.getTxPolicyMode(), (uint8_t)TX_POLICY_MODE_BUDGET);
}

TEST(TxPolicyCli, SetRejectsUnknownPolicy) {
  TestRadioPrefs prefs;
  char reply[256];
  ASSERT_TRUE(prefs.handleCommand("set tx_policy foo", 0, reply));
  EXPECT_STREQ(reply, "ERROR: tx_policy must be 'budget' or 'burst <max_ms> <quiet_ms>'");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
