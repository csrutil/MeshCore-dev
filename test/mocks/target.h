#pragma once

// Minimal target.h mock for native tests: provides the radio_driver symbol that
// src/helpers/CommonRadioPrefs.cpp references (variants/*/target.h on firmware).

class MockRadioDriver {
public:
  bool setRxBoostedGainMode(bool enable) { (void)enable; return false; }
  void setTxPower(int8_t dbm) { (void)dbm; }
};

#define WRAPPER_CLASS MockRadioDriver

extern MockRadioDriver radio_driver;
