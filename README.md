# STUDIO — Cardputer Music Machine

![STUDIO running on a Cardputer](demo.jpg)

A self-contained groovebox firmware for the **M5 Cardputer (ESP32-S3, ES8311 codec)** —
synth + sequencer + sampler + microphone recorder + song library, all in one firmware.
No M5Unified dependency: uses LovyanGFX for the screen, a small I2C driver for the
keyboard, and the legacy ESP-IDF i2s driver for ES8311 record/playback (so the mic
actually records).

## Quick Start — just flash it

**Don't want to install a build environment? Grab the prebuilt firmware.**

1. Go to the [**Releases**](https://github.com/YONGHU-YUAN/cardputer-studio/releases/latest) page and download `cardputer-studio-v1.1.bin`.
2. Flash it to your Cardputer at offset `0x0` with **M5Burner** (GUI) or **esptool**:
   ```
   esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x0 cardputer-studio-v1.1.bin
   ```
3. **Set up the SD card** (FAT32/exFAT):
   - Download `cardputer-studio-drums-v1.1.zip` from the release and unzip it.
   - Put the whole **`drums`** folder at the card root → `<card>/drums/BD808.wav …`
   - Insert the card and reset. `/samples` (your recordings) and `/songs` (saves) are created automatically.

The drum kits read their samples from `/drums`; if a file is missing that pad falls back to a synth drum. Recordings, samples and songs all live on the card.

## Quick reference card

A one-page printable key map: [English](STUDIO-guide-EN.pdf) · [中文](STUDIO-guide-CN.pdf)

## Features

- **SYNTH / sequencer** — 2 melodic tracks (A/B) + 8-pad drum track + a **phrase track** that plays a trimmed slice of a long recording.
  - Wavetable voices (saw / square / triangle / sine), tone (brightness) control
  - **6 drum kits** (`1` cycles): **ACOU** (synth) + real-sample **808 / 909 / acoustic / lo-fi / retro**, loaded from `/drums`
  - Per-step note length, accent/velocity, slide, probability, chords, swing
  - Per-track effects: delay, octave, sub, fifth, chorus; master **space FX** (echo / reverb)
  - Live FX: bitcrush, tape-stop, arpeggiator, reverse
  - Phrase track per-step controls: stack effects (OCT/SUB/CHO/DLY/REV/STUT), movable window, length, pitch
  - 16 song save slots (saved to SD, including the drum-kit choice)
- **SAMPLER** — play any 16-bit WAV on the SD card pitched across the keyboard; **trim + effects** (normalize, reverse, lo-fi, drive, fade) saved as a new sample, with a preview that plays the processed result
- **RECORDER** — record the mic straight to SD (streamed, low RAM), play back
- **SONGS** — browse / preview / rename / open saved songs
- **LIBRARY** — browse / play / delete / rename recordings

## Hardware (Cardputer Adv / ESP32-S3)

- Display: ST7789 on SPI2 (sclk 36, mosi 35, dc 34, cs 37, rst 33)
- Keyboard: TCA8418 on internal I2C (SDA 8, SCL 9, addr 0x34)
- Codec: ES8311 on internal I2C (addr 0x18); I2S BCLK 41, LRCK 43, DIN 46, DOUT 42
- SD card: separate SPI (sck 40, miso 39, mosi 14, cs 12)

## Build

Arduino + arduino-cli with the ESP32 core:

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=disabled,FlashSize=8M,PartitionScheme=default_8MB studio
arduino-cli upload  -p <PORT> --fqbn esp32:esp32:esp32s3:PSRAM=disabled,FlashSize=8M,PartitionScheme=default_8MB studio
```

Drum-kit WAVs go in `/drums` on the SD card; put your own 16-bit PCM WAVs in `/samples` for the sampler / phrase sources.

## Controls

Press `ctrl` inside SYNTH for the on-device key map. Menu: `1` SYNTH · `2` SAMPLER ·
`3` RECORDER · `4` SONGS · `5` LIBRARY · `` ` `` back.

---
Made on a Cardputer, for fun. 🎛️

Made by Yuan · [GitHub](https://github.com/YONGHU-YUAN)

Licensed under the [MIT License](LICENSE).
