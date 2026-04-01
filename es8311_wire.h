#pragma once
// ES8311 audio codec init via Arduino Wire (I2C addr 0x18)
// Configured for 44100Hz 16-bit I2S slave mode, MCLK = 256*fs
// Works for any sample rate as long as ESP32 sets MCLK = 256*fs.
#include <Wire.h>

#define ES8311_ADDR 0x18

static void es8311_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  uint8_t err = Wire.endTransmission();
  if (err) Serial.printf("[ES8311] I2C write reg=0x%02X err=%d\n", reg, err);
}

static uint8_t es8311_read(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// Initialize ES8311 for I2S slave mode, MCLK=256*fs, 16-bit.
// All registers are written directly (no read-modify-write) since reset
// puts all registers at 0x00 and the target values are fully deterministic.
// Register values traced from official Espressif ES8311 driver for 44100 Hz,
// MCLK=11289600 Hz (256*44100), coeff entry: pre_div=1, bclk_div=4, OSR=16.
static void es8311_init_44100() {
  // 1. Soft reset
  es8311_write(0x00, 0x1F); delay(20);
  es8311_write(0x00, 0x00);
  es8311_write(0x00, 0x80);  // Power on (CSM_ON)

  // 2. Clock: use MCLK pin, all internal clocks on
  es8311_write(0x01, 0x3F);

  // 3. Clock dividers (for MCLK=11289600, fs=44100)
  es8311_write(0x02, 0x00);  // pre_div=1, pre_multi=1x
  es8311_write(0x03, 0x10);  // ADC OSR=16, single-speed mode
  es8311_write(0x04, 0x10);  // DAC OSR=16
  es8311_write(0x05, 0x00);  // ADC div=1, DAC div=1
  es8311_write(0x06, 0x03);  // BCLK not inverted, div=(4-1)=3
  es8311_write(0x07, 0x00);  // LRCK_H=0
  es8311_write(0x08, 0xFF);  // LRCK_L=255 → LRCK ratio=256

  // 4. Serial port: I2S slave, 16-bit
  es8311_write(0x00, 0x80);  // slave mode (bit6=0)
  es8311_write(0x09, 0x0C);  // SDP-IN  16-bit I2S
  es8311_write(0x0A, 0x0C);  // SDP-OUT 16-bit I2S

  // 5. Power up analog path + DAC output
  es8311_write(0x0D, 0x01);  // power up analog circuitry
  es8311_write(0x0E, 0x02);  // enable analog PGA + ADC modulator
  es8311_write(0x12, 0x00);  // power up DAC
  es8311_write(0x13, 0x10);  // enable output to HP driver
  es8311_write(0x1C, 0x6A);  // ADC EQ bypass, cancel DC offset
  es8311_write(0x37, 0x08);  // bypass DAC EQ

  // 6. Microphone: analog MIC, no digital MIC
  es8311_write(0x17, 0xA8);  // ADC volume
  es8311_write(0x14, 0x1A);  // analog MIC, max PGA gain
}

// volume: 0–100
static void es8311_set_volume(int volume) {
  if (volume < 0)   volume = 0;
  if (volume > 100) volume = 100;
  uint8_t reg32 = (volume == 0) ? 0 : (uint8_t)((volume * 256 / 100) - 1);
  es8311_write(0x32, reg32);
}

static void es8311_mute(bool mute) {
  uint8_t r = es8311_read(0x31);
  if (mute) r |=  (1<<6)|(1<<5);
  else      r &= ~((1<<6)|(1<<5));
  es8311_write(0x31, r);
}
