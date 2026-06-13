#include "ES8311Audio.h"
#include <Wire.h>
#include <driver/i2s.h>

ES8311Audio::ES8311Audio() {
    _wire      = nullptr;
    _addr      = 0x18;
    _recording = false;
    _volume    = 80;
    _micGain   = 6;      // 18dB PGA — ALC will adjust dynamically from here
    _alcMode   = ALC_MID;  // ALC on by default — prevents clipping
    _drcMode   = DRC_OFF;
}

void ES8311Audio::begin(TwoWire* wire) {
    _wire = wire;
    delay(50);
    Serial.println("ES8311 I2C initialized");
}

// ============================================================
// ES8311 mode switching
// ============================================================

void ES8311Audio::enableSpeaker() {
    writeReg(0x00, 0x80);  // CSM_ON
    writeReg(0x01, 0xB5);  // MCLK=BCLK, BCLK on, DAC clk on
    writeReg(0x02, 0x18);  // MULT_PRE=3
    writeReg(0x09, 0x0C);  // SDP IN: 16-bit I2S (WL=3)
    writeReg(0x0A, 0x0C);  // SDP OUT: 16-bit I2S (WL=3)
    writeReg(0x0D, 0x01);  // Power up analog
    writeReg(0x12, 0x00);  // Power up DAC (PDN_DAC=0)
    writeReg(0x13, 0x10);  // Enable HP driver (HPSW=1)
    setVolume(_volume);
    writeReg(0x37, 0x08);  // DAC_RAMPRATE=0 (disabled), DAC_EQBYPASS=1
    applyDrc();
    delay(50);
}

void ES8311Audio::enableMicrophone() {
    writeReg(0x00, 0x80);  // CSM_ON
    writeReg(0x01, 0xBA);  // MCLK=BCLK, BCLK on, ADC clk on
    writeReg(0x02, 0x18);  // MULT_PRE=3
    writeReg(0x09, 0x0C);  // SDP IN: 16-bit I2S (WL=3)
    writeReg(0x0A, 0x0C);  // SDP OUT: 16-bit I2S (WL=3)
    writeReg(0x0D, 0x01);  // Power up analog
    // Reg 0x0E: PDN_PGA=0(bit6), PDN_MOD=0(bit5) = enable PGA + ADC modulator
    //           RST_MOD=0(bit4), VROI=1(bit3)=low impedance, LPVREFBUF=0(bit2)
    //           Keep VROI=1 (low impedance, matches default 0x6A with power bits cleared)
    writeReg(0x0E, 0x0A);  // 0b0000_1010 = PGA on, MOD on, VROI=1(low-Z)
    // Always start from the user-set micGain. When ALC is active the
    // servo will ride from this starting point up or down as needed.
    uint8_t pgaVal = (_micGain > 10) ? 10 : _micGain;
    writeReg(0x14, 0x10 | pgaVal);  // LINSEL=1 (Mic1p-Mic1n), PGAGAIN
    // Reg 0x16: ADC_SCALE — leave at chip default (+24dB, value=4).
    // This is needed for decent volume from the small MEMS mic.
    // ALC will dynamically reduce PGA gain to prevent clipping.
    // Do NOT write reg 0x16 — let it stay at default 0x04.
    //
    // IMPORTANT: Apply ALC BEFORE ADC gain — when ALC is on, reg 0x17
    // becomes MAXGAIN (the ceiling ALC can boost to), not static volume.
    applyAlc();        // regs 0x15, 0x18-0x19, and 0x17 when ALC is on
    applyAdcGain();    // reg 0x17 — sets volume when ALC is off
    // Reg 0x1C: ADC_EQBYPASS=1, ADC_HPF=1 (dynamic HPF on), HPFS2=0x0A
    // Matches M5Unified default. HPFS2 must NOT be 0 (kills bass).
    writeReg(0x1C, 0x7F);
    delay(50);
}

void ES8311Audio::enableMicForStream() { enableMicrophone(); }
void ES8311Audio::enableSpkForStream() { enableSpeaker(); }

// ============================================================
// DAC volume  (reg 0x32)
// 0x00=-95.5dB, 0xBF=0dB, 0xFF=+32dB, 0.5dB/step
// ============================================================

void ES8311Audio::setVolume(uint8_t volume) {
    if (volume > 100) volume = 100;
    _volume = volume;
    uint8_t regValue;
    if (volume == 0) {
        regValue = 0x00;
    } else if (volume <= 50) {
        regValue = 0x60 + (uint8_t)((volume * (0xBF - 0x60)) / 50);
    } else {
        regValue = 0xC0 + (uint8_t)(((volume - 51) * (0xFF - 0xC0)) / 49);
    }
    writeReg(0x32, regValue);
    Serial.printf("DAC vol: %d%% (reg32=0x%02X)\n", volume, regValue);
}

// ============================================================
// Mic PGA gain  (reg 0x14 bits[3:0])
// ============================================================

void ES8311Audio::setMicGain(uint8_t gain) {
    if (gain > 10) gain = 10;
    _micGain = gain;
    writeReg(0x14, 0x10 | gain);  // LINSEL=1 always
    Serial.printf("Mic PGA: %ddB\n", gain * 3);
}

// ============================================================
// ADC digital volume  (reg 0x17)
// When ALC is OFF: fixed at 0dB (0xBF).
// When ALC is ON:  reg 0x17 = MAXGAIN, set to 0xFF (+32dB) by applyAlc()
//                  so the ALC servo can boost as well as attenuate.
//                  applyAdcGain() must NOT overwrite it in that case.
// ============================================================

void ES8311Audio::applyAdcGain() {
    // When ALC is active, reg 0x17 = MAXGAIN (set by applyAlc).
    // Do NOT overwrite it here — ALC controls the gain.
    if (_alcMode != ALC_OFF) {
        Serial.println("ADC gain: skipped (ALC active, reg17=MAXGAIN)");
        return;
    }
    // Fixed 0dB — no user-adjustable digital boost on recording path
    writeReg(0x17, 0xBF);
    Serial.println("ADC vol reg17=0xBF (0dB)");
}

// ============================================================
// ALC (Automatic Level Control) — ADC path
//
// REAL register map (verified against datasheet Rev 10.0):
//
// REG 0x18:
//   [7]   ALC_EN          1=enable ALC
//   [6]   ADC_AUTOMUTE_EN 1=enable automute (keep 0)
//   [3:0] ALC_WINSIZE     gain step rate: 0.25dB per (2^(WINSIZE+1)) LRCKs
//                          0 = 0.25dB/2 LRCK  (~fastest)
//                          5 = 0.25dB/64 LRCK (~4ms/step at 16kHz)
//                          8 = 0.25dB/512 LRCK (~32ms/step at 16kHz)
//
// REG 0x19:
//   [7:4] ALC_MAXLEVEL    target ceiling (see table in header)
//   [3:0] ALC_MINLEVEL    target floor (gain decays when above max,
//                          boosts when below min)
//
// Level table (dBFS): 0=-30.1 1=-24.1 2=-20.6 3=-18.1 4=-16.1 5=-14.5
//                     6=-13.2 7=-12.0 8=-11.0 9=-10.1 10=-9.3 11=-8.5
//                     12=-7.8 13=-7.2 14=-6.6 15=-6.0
//
// NOTE: When ALC is on, reg 0x17 (ADC_VOLUME) becomes MAXGAIN.
//       Must be set to 0xFF (+32dB) to give ALC full boost range.
//       Setting it to 0xBF (0dB) as before crippled ALC — it could
//       only attenuate, never boost a quiet signal.
// ============================================================

void ES8311Audio::setAlcMode(AlcMode m) {
    _alcMode = m;
    applyAlc();
    char buf[8]; getAlcLabel(buf, sizeof(buf));
    Serial.printf("ALC: %s\n", buf);
}

void ES8311Audio::cycleAlc() {
    setAlcMode((AlcMode)(((int)_alcMode + 1) % 4));
}

void ES8311Audio::getAlcLabel(char* buf, int sz) {
    switch (_alcMode) {
        case ALC_OFF:  snprintf(buf, sz, "OFF");  break;
        case ALC_LOW:  snprintf(buf, sz, "Low");  break;
        case ALC_MID:  snprintf(buf, sz, "Mid");  break;
        case ALC_HIGH: snprintf(buf, sz, "High"); break;
    }
}

void ES8311Audio::applyAlc() {
    if (_alcMode == ALC_OFF) {
        writeReg(0x18, 0x00);  // ALC_EN=0, AUTOMUTE=0, WINSIZE=0
        writeReg(0x19, 0x00);  // Clear level settings
        writeReg(0x15, 0x00);  // ADC_RAMPRATE=0 (no soft ramp needed when ALC off)
        Serial.println("ALC: OFF (reg18=0x00 reg19=0x00)");
        return;
    }

    uint8_t winsize = 0;
    uint8_t maxLvl  = 0;
    uint8_t minLvl  = 0;
    uint8_t rampRate = 0;

    switch (_alcMode) {
        case ALC_LOW:
            // Slow: 0.25dB / 512 LRCK = ~32ms/step at 16kHz
            // Target: -18.1 dBFS (index 3), floor: -24.1 dBFS (index 1)
            // Gentle, wide window — good for consistent quiet environments
            winsize = 10;
            maxLvl  = 3;   // -18.1 dBFS
            minLvl  = 1;   // -24.1 dBFS
            rampRate = 10;
            break;
        case ALC_MID:
            // Medium: 0.25dB / 128 LRCK = ~8ms/step at 16kHz
            // Target: -13.2 dBFS (index 6), floor: -18.1 dBFS (index 3)
            // Balanced — tracks voice level without pumping
            winsize = 7;
            maxLvl  = 6;   // -13.2 dBFS
            minLvl  = 3;   // -18.1 dBFS
            rampRate = 7;
            break;
        case ALC_HIGH:
            // Fast: 0.25dB / 32 LRCK = ~2ms/step at 16kHz
            // Target: -11.0 dBFS (index 8), floor: -16.1 dBFS (index 4)
            // Responsive but with enough headroom to avoid ADC clipping
            winsize = 4;
            maxLvl  = 8;   // -11.0 dBFS
            minLvl  = 4;   // -16.1 dBFS
            rampRate = 4;
            break;
        default: break;
    }

    // Set ADC ramp rate (reg 0x15) — smooths gain transitions
    writeReg(0x15, (rampRate & 0x0F) << 4);

    uint8_t r18 = (1 << 7) | (winsize & 0x0F);  // ALC_EN=1, AUTOMUTE=0
    uint8_t r19 = ((maxLvl & 0xF) << 4) | (minLvl & 0xF);

    writeReg(0x18, r18);
    writeReg(0x19, r19);

    // MAXGAIN capped at 0xD0 (~+10dB). 0xFF (+32dB) was too hot —
    // the ALC would boost aggressively into ADC clipping before the
    // soft limiter on the I2S path could do anything about it.
    writeReg(0x17, 0xD0);

    Serial.printf("ALC regs: 15=0x%02X 18=0x%02X 19=0x%02X 17=0xD0(maxgain~+10dB)\n",
                  (rampRate & 0x0F) << 4, r18, r19);
}

// ============================================================
// DRC (Dynamic Range Compression) — DAC path
//
// REAL register map (verified against datasheet Rev 10.0):
//
// REG 0x33: DAC_OFFSET  (NOT DRC! Leave at 0x00)
//
// REG 0x34:
//   [7]   DRC_EN        1=enable DRC
//   [3:0] DRC_WINSIZE   gain step rate (same scale as ALC_WINSIZE)
//
// REG 0x35:
//   [7:4] DRC_MAXLEVEL  target ceiling (same level table as ALC)
//   [3:0] DRC_MINLEVEL  target floor
//
// DRC works like ALC but on the DAC output: when the DAC signal
// exceeds MAXLEVEL the chip attenuates DAC volume; when it falls
// below MINLEVEL it boosts. This is a compander, not a ratio
// compressor — there are no ratio/threshold fields.
//
// NOTE: reg 0x32 (DAC_VOLUME) comment in datasheet says
// "When DRC is on, ADC_VOLUME = MAXGAIN" — this appears to be
// a copy-paste error in the datasheet; it means DAC_VOLUME
// becomes the DRC gain readback when DRC is active.
// ============================================================

void ES8311Audio::setDrcMode(DrcMode m) {
    _drcMode = m;
    applyDrc();
    char buf[8]; getDrcLabel(buf, sizeof(buf));
    Serial.printf("DRC: %s\n", buf);
}

void ES8311Audio::cycleDrc() {
    setDrcMode((DrcMode)(((int)_drcMode + 1) % 5));
}

void ES8311Audio::getDrcLabel(char* buf, int sz) {
    switch (_drcMode) {
        case DRC_OFF:   snprintf(buf, sz, "OFF");   break;
        case DRC_MED:   snprintf(buf, sz, "Med");   break;
        case DRC_HARD:  snprintf(buf, sz, "Hard");  break;
        case DRC_CRUSH: snprintf(buf, sz, "Crush"); break;
        case DRC_NUKE:  snprintf(buf, sz, "NUKE");  break;
    }
}

void ES8311Audio::applyDrc() {
    writeReg(0x33, 0x00);  // DAC_OFFSET = 0 (this is NOT DRC!)

    if (_drcMode == DRC_OFF) {
        writeReg(0x34, 0x00);  // DRC_EN=0
        writeReg(0x35, 0x00);
        Serial.println("DRC: OFF (reg34=0x00 reg35=0x00)");
        return;
    }

    uint8_t winsize = 0;
    uint8_t maxLvl  = 0;
    uint8_t minLvl  = 0;

    switch (_drcMode) {
        case DRC_MED:
            // Slow: ~32ms/step. Window: -12.0 to -18.1 dBFS
            winsize = 6;
            maxLvl  = 7;   // -12.0 dBFS
            minLvl  = 3;   // -18.1 dBFS
            break;
        case DRC_HARD:
            // Medium: ~4ms/step. Window: -9.3 to -14.5 dBFS
            winsize = 5;
            maxLvl  = 10;  // -9.3 dBFS
            minLvl  = 5;   // -14.5 dBFS
            break;
        case DRC_CRUSH:
            // Fast: ~0.5ms/step. Window: -7.2 to -12.0 dBFS
            winsize = 0;
            maxLvl  = 13;  // -7.2 dBFS
            minLvl  = 7;   // -12.0 dBFS
            break;
        case DRC_NUKE:
            // Brick limiter: fastest gain, 0dB window pinned at -6.0 dBFS.
            // WINSIZE=0 = gain change every LRCK cycle.
            // MAXLEVEL=MINLEVEL=15 (-6.0dBFS) — zero tolerance window,
            // the codec fights to hold output at exactly max loudness.
            winsize = 0;
            maxLvl  = 15;  // -6.0 dBFS
            minLvl  = 15;  // -6.0 dBFS — same as max, 0dB window
            break;
        default: break;
    }

    uint8_t r34 = (1 << 7) | (winsize & 0x0F);  // DRC_EN=1
    uint8_t r35 = ((maxLvl & 0xF) << 4) | (minLvl & 0xF);

    writeReg(0x34, r34);
    writeReg(0x35, r35);
    Serial.printf("DRC regs: 34=0x%02X 35=0x%02X\n", r34, r35);
}

// ============================================================
// Mute & test
// ============================================================

void ES8311Audio::mute(bool enable) {
    writeReg(0x31, enable ? 0x20 : 0x00);
}

bool ES8311Audio::testConnection() {
    uint8_t id1 = readReg(0xFD);
    uint8_t id2 = readReg(0xFE);
    Serial.printf("ES8311 ID: 0x%02X 0x%02X (expect 0x83 0x11)\n", id1, id2);
    return (id1 == 0x83 && id2 == 0x11);
}

// ============================================================
// Recording
// ============================================================

int32_t ES8311Audio::recordToBuffer(int16_t* buffer, int32_t maxSamples,
                                    uint32_t sampleRate) {
    enableMicrophone();
    const i2s_port_t PORT = I2S_NUM_0;

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = sampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 128;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_SCLK;
    pins.ws_io_num  = PIN_I2S_LRCK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_I2S_DIN;

    if (i2s_driver_install(PORT, &cfg, 0, NULL) != ESP_OK) return -1;
    i2s_set_pin(PORT, &pins);
    i2s_zero_dma_buffer(PORT);
    delay(100);

    int16_t discard[256];
    size_t br;
    for (int i = 0; i < 4; i++) i2s_read(PORT, discard, sizeof(discard), &br, portMAX_DELAY);

    _recording = true;
    const int RF = 128;
    int16_t stereo[RF * 2];
    int32_t n = 0;

    while (_recording && n < maxSamples) {
        int want = min(RF, (int)(maxSamples - n));
        i2s_read(PORT, stereo, want * 4, &br, portMAX_DELAY);
        int got = br / 4;
        for (int i = 0; i < got && n < maxSamples; i++) buffer[n++] = stereo[i * 2 + 1];
    }

    _recording = false;
    i2s_driver_uninstall(PORT);
    return n;
}

void ES8311Audio::stopRecording() { _recording = false; }

// ============================================================
// Playback from buffer
// ============================================================

bool ES8311Audio::playFromBuffer(const int16_t* buffer, int32_t numSamples,
                                 uint32_t sampleRate) {
    enableSpeaker();
    const i2s_port_t PORT = I2S_NUM_1;

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_SCLK;
    pins.ws_io_num  = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(PORT, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_set_pin(PORT, &pins);
    i2s_zero_dma_buffer(PORT);
    delay(100);

    const int BF = 128;
    int16_t stereo[BF * 2];
    size_t bw;
    int32_t idx = 0;

    while (idx < numSamples) {
        int fw = min(BF, (int)(numSamples - idx));
        for (int i = 0; i < fw; i++) {
            stereo[i*2]   = buffer[idx+i];
            stereo[i*2+1] = buffer[idx+i];
        }
        i2s_write(PORT, stereo, fw * 4, &bw, portMAX_DELAY);
        idx += fw;
    }

    memset(stereo, 0, sizeof(stereo));
    for (int i = 0; i < 4; i++) i2s_write(PORT, stereo, sizeof(stereo), &bw, portMAX_DELAY);
    delay(50);
    i2s_driver_uninstall(PORT);
    return true;
}

// ============================================================
// Tone generation
// ============================================================

bool ES8311Audio::playTone(uint16_t frequency, uint32_t duration_ms) {
    enableSpeaker();
    const i2s_port_t PORT = I2S_NUM_1;
    const uint32_t sr = 48000;

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sr;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_SCLK;
    pins.ws_io_num  = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(PORT, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_set_pin(PORT, &pins);
    i2s_zero_dma_buffer(PORT);
    delay(100);

    const int total = (sr * duration_ms) / 1000;
    const float spc = (float)sr / frequency;
    const int BF = 128;
    int16_t buf[BF * 2];
    size_t bw;
    int idx = 0;
    while (idx < total) {
        int fw = min(BF, total - idx);
        for (int i = 0; i < fw; i++) {
            int16_t s = (int16_t)(sinf(2.0f * PI * (idx + i) / spc) * 24000);
            buf[i*2] = s; buf[i*2+1] = s;
        }
        i2s_write(PORT, buf, fw * 4, &bw, portMAX_DELAY);
        idx += fw;
    }
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 4; i++) i2s_write(PORT, buf, sizeof(buf), &bw, portMAX_DELAY);
    delay(50);
    i2s_driver_uninstall(PORT);
    return true;
}

// ============================================================
// I2C helpers
// ============================================================

void ES8311Audio::writeReg(uint8_t reg, uint8_t value) {
    if (!_wire) return;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(value);
    _wire->endTransmission();
}

uint8_t ES8311Audio::readReg(uint8_t reg) {
    if (!_wire) return 0;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)1);
    return _wire->available() ? _wire->read() : 0;
}
