#pragma once

#include "MeshCore.h"

#include <RadioLib.h>

#ifdef NANO_LORA_PLUS
#define LR2021_RX_IRQ_MASK RADIOLIB_LR2021_IRQ_RX_DONE
#else
#define LR2021_RX_IRQ_MASK (RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID)
#endif

class CustomLR2021 : public LR2021 {
public:
  using LR2021::setOutputPower;

  CustomLR2021(Module *mod) : LR2021(mod) {}

  bool std_init(SPIClass *spi = NULL) {
#ifdef LR2021_TCXO_VOLTAGE
    float tcxo = LR2021_TCXO_VOLTAGE;
#else
    float tcxo = 0.0f;
#endif

    uint8_t cr = LORA_CR;

#if defined(P_LORA_SCLK)
    if (spi) {
      spi->begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
    }
#endif

    irqDioNum = LR2021_IRQ_DIO;
    MESH_DEBUG_PRINTLN("LR2021: begin f=%d bw=%d sf=%d cr=%d tx=%d preamble=16 tcxo=%d irqDio=%d",
                       (int)LORA_FREQ, (int)LORA_BW, (int)LORA_SF, (int)cr, (int)LORA_TX_POWER,
                       (int)(tcxo * 10), (int)irqDioNum);

    setPaTable(lr2021LfPaTable(), false);
    MESH_DEBUG_PRINTLN("LR2021: LF PA table installed pwr=-9..22 entry22 duty=7 slices=7 val=44");

    int status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER,
                       16, tcxo);
    if (status != RADIOLIB_ERR_NONE) {
      MESH_DEBUG_PRINTLN("LR2021: radio init failed status=%d", status);
      return false;
    }

    int err_crc = setCRC(1);
    int err_header = explicitHeader();
    int err_xosc = useXoscStandby();
#ifdef NANO_LORA_PLUS
    int err_boost = setRxBoostedGainMode(7);
#endif
    if (err_xosc != RADIOLIB_ERR_NONE) {
      MESH_DEBUG_PRINTLN("LR2021: XOSC standby failed err=%d", err_xosc);
      return false;
    }
    int err_side = configureRxSideDetectors(LORA_BW, LORA_SF);
#ifdef NANO_LORA_PLUS
    MESH_DEBUG_PRINTLN("LR2021: setCRC=%d explicitHeader=%d rxBoost=%d sideDet=%d", err_crc, err_header,
                       err_boost, err_side);
#else
    MESH_DEBUG_PRINTLN("LR2021: setCRC=%d explicitHeader=%d sideDet=%d", err_crc, err_header, err_side);
#endif

    return true;
  }

  bool isReceiving() {
    uint32_t irq = getIrqStatus();
    return irq & LR2021_RX_IRQ_MASK;
  }

  int16_t setOutputPower(int8_t power) override {
    int16_t err = LR2021::setOutputPower(power);
    logLr2021LfPa(power, err);
    return err;
  }

  int16_t startReceive() override {
    standby(RADIOLIB_LR2021_STANDBY_XOSC);

    int16_t err_fifo = clearRxFifo();
    if (err_fifo != RADIOLIB_ERR_NONE) {
      MESH_DEBUG_PRINTLN("LR2021: startReceive clearRxFifo failed err=%d", err_fifo);
      return err_fifo;
    }

#ifdef NANO_LORA_PLUS
    int16_t err_irq = clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL);
    if (err_irq != RADIOLIB_ERR_NONE) {
      MESH_DEBUG_PRINTLN("LR2021: startReceive clearIrq failed err=%d", err_irq);
      return err_irq;
    }
#endif

    int16_t err = LR2021::startReceive();
    if (err != RADIOLIB_ERR_NONE) {
      MESH_DEBUG_PRINTLN("LR2021: startReceive recovery for %d", err);
#ifdef NANO_LORA_PLUS
      int16_t err_recover = recoverRx();
      if (err_recover != RADIOLIB_ERR_NONE) {
        return err_recover;
      }
      err = LR2021::startReceive();
      MESH_DEBUG_PRINTLN("LR2021: startReceive retry -> %d", err);
#else
      recoverRx();
#endif
    }
    return err;
  }

  int16_t recoverRx() {
    int16_t err_stby = standby(RADIOLIB_LR2021_STANDBY_XOSC);
    int16_t err_irq = clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL);
    int16_t err_fifo = clearRxFifo();
    MESH_DEBUG_PRINTLN("LR2021: recoverRx standby=%d clearIrq=%d clearRxFifo=%d", err_stby, err_irq, err_fifo);
    if (err_stby != RADIOLIB_ERR_NONE) {
      return err_stby;
    }
    if (err_irq != RADIOLIB_ERR_NONE) {
      return err_irq;
    }
    return err_fifo;
  }

  int16_t useXoscStandby() {
    int16_t err_fallback = setRxTxFallbackMode(RADIOLIB_LR2021_FALLBACK_MODE_STBY_XOSC);
    int16_t err_standby = standby(RADIOLIB_LR2021_STANDBY_XOSC);
    if (err_fallback != RADIOLIB_ERR_NONE) {
      return err_fallback;
    }
    return err_standby;
  }

  int16_t configureRxSideDetectors(float bw, uint8_t mainSf) {
    clearSideDetectorState(mainSf);

    if (mainSf >= 12) {
      MESH_DEBUG_PRINTLN("LR2021: side detectors disabled mainSf=%d bw=%d reason=no higher SF", (int)mainSf,
                         (int)bw);
      return RADIOLIB_ERR_NONE;
    }

    size_t max_side = 3;
    if (bw >= 500.0f) {
      max_side = 2;
    }
    if (mainSf >= 10 && max_side > 2) {
      max_side = 2;
    }
    if (bw >= 500.0f && mainSf >= 10) {
      max_side = 1;
    }

    LR2021LoRaSideDetector_t side[3] = {};
    size_t wanted = 0;
    for (uint8_t sf = mainSf + 1; sf <= 12 && wanted < max_side; sf++) {
      side[wanted].sf = sf;
      side[wanted].ldro = sideDetectorLdro(bw, sf);
      side[wanted].invertIQ = false;
      side[wanted].syncWord = RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE;
      wanted++;
    }

    int16_t last_err = RADIOLIB_ERR_NONE;
    for (size_t count = wanted; count > 0; count--) {
      last_err = setSideDetector(side, count);
      if (last_err == RADIOLIB_ERR_NONE) {
        _side_detector_main_sf = mainSf;
        _side_detector_count = count;
        for (size_t i = 0; i < count; i++) {
          _side_detector_sf[i] = side[i].sf;
        }
        logSideDetectorConfig("configured", bw, last_err);
        return last_err;
      }
      MESH_DEBUG_PRINTLN("LR2021: side detectors rejected mainSf=%d bw=%d count=%d err=%d", (int)mainSf,
                         (int)bw, (int)count, last_err);
    }

    logSideDetectorConfig("disabled", bw, last_err);
    return last_err;
  }

  static LR2021PaTableEntry_t *lr2021LfPaTable() {
    static LR2021PaTableEntry_t table[RADIOLIB_LR2021_PA_TABLE_LEN] = {
      { 2, 5, -13 }, // -9 dBm
      { 6, 1, -13 }, // -8 dBm
      { 6, 0, -6 },  // -7 dBm
      { 1, 0, 4 },   // -6 dBm
      { 2, 0, 4 },   // -5 dBm
      { 1, 3, 2 },   // -4 dBm
      { 0, 0, 14 },  // -3 dBm
      { 0, 3, 9 },   // -2 dBm
      { 3, 0, 11 },  // -1 dBm
      { 1, 0, 16 },  // 0 dBm
      { 7, 0, 11 },  // 1 dBm
      { 2, 0, 18 },  // 2 dBm
      { 5, 0, 16 },  // 3 dBm
      { 7, 0, 17 },  // 4 dBm
      { 1, 2, 21 },  // 5 dBm
      { 3, 0, 25 },  // 6 dBm
      { 0, 1, 32 },  // 7 dBm
      { 2, 0, 32 },  // 8 dBm
      { 3, 1, 27 },  // 9 dBm
      { 2, 1, 32 },  // 10 dBm
      { 5, 1, 28 },  // 11 dBm
      { 5, 1, 30 },  // 12 dBm
      { 4, 1, 34 },  // 13 dBm
      { 5, 4, 31 },  // 14 dBm
      { 4, 4, 34 },  // 15 dBm
      { 5, 6, 34 },  // 16 dBm
      { 3, 5, 39 },  // 17 dBm
      { 6, 6, 37 },  // 18 dBm
      { 5, 5, 40 },  // 19 dBm
      { 7, 4, 41 },  // 20 dBm
      { 7, 4, 43 },  // 21 dBm
      { 7, 7, 44 },  // 22 dBm
    };
    static_assert(sizeof(table) / sizeof(table[0]) == RADIOLIB_LR2021_PA_TABLE_LEN,
                  "LR2021 LF PA table length mismatch");
    return table;
  }

  void logLr2021LfPa(int8_t power, int16_t err) {
    if (power < -9 || power > 22) {
      MESH_DEBUG_PRINTLN("LR2021: setOutputPower pwr=%d -> %d outside LF PA table", (int)power, err);
      return;
    }

    LR2021PaTableEntry_t *table = lr2021LfPaTable();
    LR2021PaTableEntry_t entry = table[power + 9];
    MESH_DEBUG_PRINTLN("LR2021: setOutputPower pwr=%d -> %d LF duty=%d slices=%d val=%d", (int)power, err,
                       (int)entry.paDutyCycle, (int)entry.paSlices, (int)entry.paVal);
  }

private:
  uint8_t _side_detector_main_sf = 0;
  uint8_t _side_detector_count = 0;
  uint8_t _side_detector_sf[3] = { 0, 0, 0 };

  static bool sideDetectorLdro(float bw, uint8_t sf) {
    if (bw <= 0.0f) {
      return false;
    }
    return ((float)(1UL << sf) / bw) >= 16.0f;
  }

  void clearSideDetectorState(uint8_t mainSf) {
    _side_detector_main_sf = mainSf;
    _side_detector_count = 0;
    _side_detector_sf[0] = 0;
    _side_detector_sf[1] = 0;
    _side_detector_sf[2] = 0;
  }

  void logSideDetectorConfig(const char *tag, float bw, int16_t err) {
    MESH_DEBUG_PRINTLN("LR2021: side detectors %s mainSf=%d bw=%d count=%d sideSf=%d,%d,%d err=%d", tag,
                       (int)_side_detector_main_sf, (int)bw, (int)_side_detector_count,
                       (int)_side_detector_sf[0], (int)_side_detector_sf[1], (int)_side_detector_sf[2], err);
  }
};
