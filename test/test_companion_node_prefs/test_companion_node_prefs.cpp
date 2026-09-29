#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../examples/companion_radio/NodePrefs.h"

class ReplayStream : public Stream {
  const char* _text;
  int _pos = 0;
  int _len;

public:
  explicit ReplayStream(const char* text) : _text(text), _len(strlen(text)) { }

  int available() override { return _len - _pos; }
  int read() override { return _pos < _len ? _text[_pos++] : -1; }
  int peek() override { return _pos < _len ? _text[_pos] : -1; }
};

class CaptureStream : public Stream {
  std::string _text;

  size_t emit(long long value) {
    char text[24];
    int length = snprintf(text, sizeof(text), "%lld", value);
    return write(reinterpret_cast<const uint8_t*>(text), length);
  }

public:
  size_t write(uint8_t value) override {
    _text.push_back(static_cast<char>(value));
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    _text.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

  size_t print(unsigned char value, int = DEC) override { return emit(value); }
  size_t print(int value, int = DEC) override { return emit(value); }
  size_t print(unsigned int value, int = DEC) override { return emit(value); }
  size_t print(long value, int = DEC) override { return emit(value); }
  size_t print(unsigned long value, int = DEC) override { return emit(value); }
  size_t print(long long value, int = DEC) override { return emit(value); }
  size_t print(unsigned long long value, int = DEC) override { return emit(value); }

  const std::string& text() const { return _text; }
};

#if 0
// Re-enable test once we can SET fem_ values in companion
TEST(CompanionNodePrefs, RxGainSettingsRoundTripIndependently) {
  NodePrefs saved;
  saved.rx_boosted_gain = 0;
  saved.radio_fem_rxgain = 1;
  saved.radio_fem_txgain = 0;

  CaptureStream output;
  ASSERT_TRUE(saved.saveSerial(output));
  EXPECT_NE(std::string::npos, output.text().find("rxgain:0"));
  EXPECT_NE(std::string::npos, output.text().find("fem_rxgain:1"));
  EXPECT_NE(std::string::npos, output.text().find("fem_txgain:0"));

  ReplayStream input("{radio:{rxgain:1,fem_rxgain:0,fem_txgain:1}}");
  NodePrefs loaded;
  loaded.rx_boosted_gain = 0;
  loaded.radio_fem_rxgain = 1;
  loaded.radio_fem_txgain = 0;

  ASSERT_TRUE(loaded.loadSerial(input));
  EXPECT_EQ(1, loaded.rx_boosted_gain);
  EXPECT_EQ(0, loaded.radio_fem_rxgain);
  EXPECT_EQ(1, loaded.radio_fem_txgain);
}
#endif

// The tx_policy keys are new: prefs saved by older firmware contain none of them, and
// ConfigSerializer::def() only assigns a value when its key matches, so the in-memory
// defaults must survive loading such a file.
TEST(CompanionNodePrefs, MissingTxPolicyKeysKeepDefaults) {
  ReplayStream input("{radio:{freq:869.618}}");
  NodePrefs prefs;   // defaults: budget policy, burst limit 1000 ms, quiet time 0

  ASSERT_TRUE(prefs.loadSerial(input));
  EXPECT_EQ((uint8_t)TX_POLICY_MODE_BUDGET, prefs.getRadioPrefs()->getTxPolicyMode());
  EXPECT_EQ(1000UL, prefs.getRadioPrefs()->getBurstMaxTxMs());
  EXPECT_EQ(0UL, prefs.getRadioPrefs()->getBurstQuietMs());
}

TEST(CompanionNodePrefs, TxPolicyKeysRoundTrip) {
  NodePrefs saved;
  saved.getRadioPrefs()->setTxPolicyMode(TX_POLICY_MODE_BURST);
  saved.getRadioPrefs()->setBurstMaxTxMs(1500);
  saved.getRadioPrefs()->setBurstQuietMs(300);

  CaptureStream output;
  ASSERT_TRUE(saved.saveSerial(output));
  EXPECT_NE(std::string::npos, output.text().find("tx_policy:1"));
  EXPECT_NE(std::string::npos, output.text().find("burst_max_ms:1500"));
  EXPECT_NE(std::string::npos, output.text().find("burst_quiet_ms:300"));

  ReplayStream input(output.text().c_str());
  NodePrefs loaded;
  ASSERT_TRUE(loaded.loadSerial(input));
  EXPECT_EQ((uint8_t)TX_POLICY_MODE_BURST, loaded.getRadioPrefs()->getTxPolicyMode());
  EXPECT_EQ(1500UL, loaded.getRadioPrefs()->getBurstMaxTxMs());
  EXPECT_EQ(300UL, loaded.getRadioPrefs()->getBurstQuietMs());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
