#pragma once

#include "CustomLR2021.h"
#include "RadioLibWrappers.h"

class CustomLR2021Wrapper : public RadioLibWrapper {
public:
  CustomLR2021Wrapper(CustomLR2021 &radio, mesh::MainBoard &board) : RadioLibWrapper(radio, board) {}

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    CustomLR2021 *lr2021 = (CustomLR2021 *)_radio;
    lr2021->setFrequency(freq);
    lr2021->setSpreadingFactor(sf);
    lr2021->setBandwidth(bw);
    lr2021->setCodingRate(cr);
    lr2021->configureRxSideDetectors(bw, sf);
    _sf = sf;
    updatePreamble(sf);
  }

  bool isReceiving() override {
    if (!isReceivingPacket()) {
      bool active = isChannelActive();
      if (active) {
        logCadState("channel active");
      }
      return active;
    }

    if (isChannelActive()) {
      logCadState("rx irq active");
      return true;
    }

    logCadState("clearing stale rx irq");
    ((CustomLR2021 *)_radio)->clearIrqFlags(LR2021_RX_IRQ_MASK);
    return false;
  }

  bool isReceivingPacket() override { return ((CustomLR2021 *)_radio)->isReceiving(); }

  float getCurrentRSSI() override { return ((CustomLR2021 *)_radio)->getRSSI(false, true); }

  float getLastRSSI() const override { return ((CustomLR2021 *)_radio)->getRSSI(); }

  float getLastSNR() const override { return ((CustomLR2021 *)_radio)->getSNR(); }

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, _sf, packet_len); }

  uint8_t getSpreadingFactor() const override { return _sf; }

  bool setRxBoostedGainMode(bool enable) override {
    ((CustomLR2021 *)_radio)->gainModeLf = enable ? 0x04 : RADIOLIB_LR2021_RX_BOOST_LF;
    return true;
  }

  bool getRxBoostedGainMode() const override {
    return ((CustomLR2021 *)_radio)->gainModeLf != RADIOLIB_LR2021_RX_BOOST_LF;
  }

  void powerOff() override { ((CustomLR2021 *)_radio)->sleep(false, 0); }

private:
  uint8_t _sf = LORA_SF;

  void logCadState(const char *tag) {
    static uint32_t count = 0;
    count++;
    if (count != 1 && (count % 32) != 0) {
      return;
    }

    CustomLR2021 *lr2021 = (CustomLR2021 *)_radio;
    uint32_t irq = lr2021->getIrqStatus();
    MESH_DEBUG_PRINTLN("LR2021: CAD %s irq=0x%08lx rssi=%d floor=%d threshold=%d", tag, irq,
                       (int)getCurrentRSSI(), (int)_noise_floor, (int)_threshold);
  }
};
