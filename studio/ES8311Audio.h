#ifndef ES8311_AUDIO_H
#define ES8311_AUDIO_H

#include <Arduino.h>

class TwoWire;

// ============================================================
// ES8311Audio — ES8311 codec driver for Cardputer-Adv
//
// Verified against ES8311 datasheet Rev 10.0 (January 2021).
//
// Key registers used:
//   0x14  PGA gain (mic amplifier, 0-30 dB in 3 dB steps)
//   0x17  ADC digital volume (0x00=-95.5dB, 0xBF=0dB, 0xFF=+32dB)
//   0x18  ALC_EN [7], ADC_AUTOMUTE_EN [6], ALC_WINSIZE [3:0]
//   0x19  ALC_MAXLEVEL [7:4], ALC_MINLEVEL [3:0]
//   0x1C  ADC_EQBYPASS [6], ADC_HPF [5], ADC_HPFS2 [4:0]
//   0x1D-0x30  ADCEQ biquad coefficients (30-bit Q1.29 each)
//   0x32  DAC volume (same scale as 0x17)
//   0x33  DAC_OFFSET (not DRC!)
//   0x34  DRC_EN [7], DRC_WINSIZE [3:0]
//   0x35  DRC_MAXLEVEL [7:4], DRC_MINLEVEL [3:0]
//   0x37  DAC_RAMPRATE [7:4], DAC_EQBYPASS [3]
//
// IMPORTANT: 0x33 is DAC_OFFSET, NOT DRC_EN.
//            DRC_EN is at 0x34 bit 7.
//            ALC registers are at 0x18-0x19, NOT 0x1C-0x1F.
// ============================================================

class ES8311Audio {
public:
    ES8311Audio();
    void begin(TwoWire* wire);

    // ---- DAC volume ----
    void    setVolume(uint8_t volume);   // 0-100 %
    uint8_t getVolume() { return _volume; }

    // ---- Mic PGA gain ----
    void    setMicGain(uint8_t gain);    // 0-10  (x3 dB = 0-30 dB)
    uint8_t getMicGain() { return _micGain; }

    // ---- ALC (Automatic Level Control) — ADC path ----
    // Hardware servo loop: measures ADC output level every N samples
    // and adjusts PGA gain to hit the target. Reg 0x18/0x19.
    //
    // ALC_WINSIZE controls how many LRCK cycles per 0.25 dB gain step:
    //   Low:  winsize=10 (0.25dB/1024 LRCK = ~64ms/step) — slow, gentle
    //   Mid:  winsize=7  (0.25dB/128 LRCK  =  ~8ms/step) — medium
    //   High: winsize=4  (0.25dB/16 LRCK   =  ~1ms/step) — fast, responsive
    //
    // Target level table (MAXLEVEL, from datasheet):
    //   0=-30.1  1=-24.1  2=-20.6  3=-18.1  4=-16.1  5=-14.5
    //   6=-13.2  7=-12.0  8=-11.0  9=-10.1  10=-9.3  11=-8.5
    //   12=-7.8  13=-7.2  14=-6.6  15=-6.0  (dBFS)
    enum AlcMode { ALC_OFF = 0, ALC_LOW = 1, ALC_MID = 2, ALC_HIGH = 3 };
    void    setAlcMode(AlcMode m);
    AlcMode getAlcMode()  { return _alcMode; }
    bool    isAlcActive() { return _alcMode != ALC_OFF; }
    void    cycleAlc();
    void    getAlcLabel(char* buf, int sz);

    // ---- DRC (Dynamic Range Compression) — DAC path ----
    // Controls DAC output volume to stay within a target level window.
    // DRC_WINSIZE sets gain-change rate (same scale as ALC_WINSIZE).
    // DRC_MAXLEVEL/MINLEVEL set the target window. Regs 0x34/0x35.
    //
    // Modes target different level windows:
    //   Med  : max=-12.0dB  min=-18.1dB  slow  (winsize=6)
    //   Hard : max= -9.3dB  min=-14.5dB  medium(winsize=5)
    //   Crush: max= -7.2dB  min=-12.0dB  fast  (winsize=0)
    //   NUKE : max= -6.0dB  min= -6.0dB  fastest(winsize=0) — 0dB window, brick limiter
    enum DrcMode { DRC_OFF = 0, DRC_MED = 1, DRC_HARD = 2, DRC_CRUSH = 3, DRC_NUKE = 4 };
    void    setDrcMode(DrcMode m);
    DrcMode getDrcMode()  { return _drcMode; }
    bool    isDrcActive() { return _drcMode != DRC_OFF; }
    void    cycleDrc();
    void    getDrcLabel(char* buf, int sz);

    // ---- Misc ----
    void mute(bool enable);
    bool testConnection();

    bool    playTone(uint16_t frequency, uint32_t duration_ms);
    int32_t recordToBuffer(int16_t* buffer, int32_t maxSamples,
                           uint32_t sampleRate = 16000);
    void    stopRecording();
    bool    playFromBuffer(const int16_t* buffer, int32_t numSamples,
                           uint32_t sampleRate = 16000);
    bool    isRecording() { return _recording; }

    void enableMicForStream();
    void enableSpkForStream();

private:
    TwoWire* _wire;
    uint8_t  _addr;
    bool     _recording;
    uint8_t  _volume;
    uint8_t  _micGain;
    AlcMode  _alcMode;
    DrcMode  _drcMode;

    static const int PIN_I2S_SCLK = 41;
    static const int PIN_I2S_LRCK = 43;
    static const int PIN_I2S_DOUT = 42;
    static const int PIN_I2S_DIN  = 46;

    void    writeReg(uint8_t reg, uint8_t value);
    uint8_t readReg(uint8_t reg);

    void enableSpeaker();
    void enableMicrophone();

    void applyAdcGain();
    void applyAlc();   // writes 0x18-0x19
    void applyDrc();   // writes 0x34-0x35
};

#endif
