#include <gtest/gtest.h>
#include "TxPolicy.h"
#include "Dispatcher.h"
#include "helpers/StaticPoolPacketManager.h"

#include <string>
#include <vector>

using namespace mesh;

// ---------------------------------------------------------------- mocks

class MockBudgetEnv : public TxPolicyEnv {
public:
  float factor = 1.0f;
  unsigned long window_ms = 3600000;
  float getAirtimeBudgetFactor() const override { return factor; }
  unsigned long getDutyCycleWindowMs() const override { return window_ms; }
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
};

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
  // 0x90 - 0xFFFFFFF0 wraps to 0x160 = 352 elapsed -> refill 176
  EXPECT_TRUE(policy.canStart(0x90UL, 100, 300, retry_at));
  EXPECT_EQ(policy.remainingTxMs(0x90UL), 176UL);
}

TEST(TxAirtimeBudget, DoesNotChain) {
  MockBudgetEnv env;
  AirtimeBudgetTxPolicy policy(env);
  policy.begin(0);
  EXPECT_FALSE(policy.canChain(100));
}

// ------------------------------------------------------------ burst policy

TEST(TxBurst, ChainsUntilBurstMax) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);

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

TEST(TxBurst, WaitsQuietPeriodWhenPacketDoesNotFit) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);
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

TEST(TxBurst, GapBelowQuietPeriodDoesNotReset) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(10, 800, 10, next_tx);

  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(100, 300, 2550, retry_at));   // 800 + 300 > 1000, gap < 200
  EXPECT_EQ(retry_at, 210UL);
}

TEST(TxBurst, FittingPacketSendsWithoutWaiting) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);

  uint32_t retry_at;
  EXPECT_TRUE(policy.canStart(50, 700, 2550, retry_at));   // 300 + 700 <= 1000
  EXPECT_EQ(retry_at, 50UL);
}

TEST(TxBurst, RejectsSinglePacketAboveBurstMax) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);

  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(0, 2000, 2550, retry_at));
}

TEST(TxBurst, RemainingFallsToZeroAtBurstMax) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);
  EXPECT_EQ(policy.remainingTxMs(10), 700UL);
  policy.onTxDone(20, 1000, 10, next_tx);
  EXPECT_EQ(policy.remainingTxMs(20), 0UL);
}

TEST(TxBurst, TimeoutEndsBurstAndStartsQuietPeriod) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(10, 300, 10, next_tx);

  policy.onTxAborted(50);
  EXPECT_FALSE(policy.canChain(100));

  uint32_t retry_at;
  EXPECT_FALSE(policy.canStart(249, 100, 2550, retry_at));
  EXPECT_EQ(retry_at, 250UL);
  EXPECT_TRUE(policy.canStart(250, 100, 2550, retry_at));
}

TEST(TxBurst, HandlesMillisWrapAround) {
  BurstTxPolicy policy(1000, 200);
  policy.begin(0);
  unsigned long next_tx;
  policy.onTxDone(0xFFFFFF00UL, 800, 10, next_tx);

  uint32_t retry_at;
  // gap below the quiet period: 0xFFFFFF80 - 0xFFFFFF00 = 0x80 = 128
  EXPECT_FALSE(policy.canStart(0xFFFFFF80UL, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 0xFFFFFFC4UL);   // 0xFFFFFF00 + 200 (no wrap yet)

  // 0x20 - 0xFFFFFF00 wraps to 0x120 = 288: quiet period elapsed -> new burst
  EXPECT_TRUE(policy.canStart(0x20UL, 300, 2550, retry_at));
  EXPECT_EQ(retry_at, 0x20UL);
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
    dispatcher.sendPacket(pkt, 0, 0);
    return pkt;
  }
};

#ifndef TX_POLICY_BURST

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

#else   // TX_POLICY_BURST

TEST_F(DispatcherTxTest, ChainsPacketsWithoutReturningToRx) {
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
  queuePacket(180);   // raw len 182 -> est airtime 1820ms > 1000

  dispatcher.loop();
  EXPECT_EQ(radio.send_count, 0);
  EXPECT_EQ(mgr.getOutboundTotal(), 0);
  EXPECT_EQ(mgr.getFreeCount(), 16);   // packet returned to the pool
}

TEST_F(DispatcherTxTest, TxTimeoutEndsBurstAndStartsQuietPeriod) {
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

#endif   // TX_POLICY_BURST

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
