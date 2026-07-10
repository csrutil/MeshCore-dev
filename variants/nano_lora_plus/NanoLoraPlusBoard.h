#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>

#ifndef ADC_MULTIPLIER
#define ADC_MULTIPLIER 2.0f
#endif

class NanoLoraPlusBoard : public ESP32Board {
public:
  void begin() {
    ESP32Board::begin();

#ifdef LED_PIN
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_STATE_ON ? LOW : HIGH);
#endif
  }

  uint16_t getBattMilliVolts() override {
#ifdef PIN_VBAT_READ
    uint32_t sum_mv = 0;
    for (int i = 0; i < 15; i++) {
      sum_mv += analogReadMilliVolts(PIN_VBAT_READ);
    }
    return (uint16_t)(getAdcMultiplier() * (sum_mv / 15.0f));
#else
    return 0;
#endif
  }

  float getAdcMultiplier() const override { return ADC_MULTIPLIER; }

  const char *getManufacturerName() const override { return "Nano LoRa Plus"; }
};
