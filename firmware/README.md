# RTAL SYNTECH — Firmware

**Project:** [RTAL-EAI-015-SYNTECH-Synthesizer](../README.md)  
**Firmware:** `V1.0.0 FINAL`  
**Target:** ESP32-S3 · Guition JC4827W543 · 480 × 272 touchscreen

![Firmware](https://img.shields.io/badge/firmware-v1.0.0%20FINAL-00d7eb?style=flat-square)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-0077cc?style=flat-square)
![Audio](https://img.shields.io/badge/audio-32%20kHz-0077cc?style=flat-square)
![Voices](https://img.shields.io/badge/polyphony-up%20to%206-0077cc?style=flat-square)

> [!IMPORTANT]
> **Source release with installation prerequisites.** The V1.0.0 firmware has previously been reported working on the target hardware, and six included native host-test groups passed the publication audit. A clean ESP32-S3 rebuild/reflash of this exact archive was **not** independently reproduced. **A valid SD preset bank is required** and is **not included** in the supplied firmware archive. This is not yet a self-contained, ready-to-flash installation bundle.

## 1. Firmware package

The current source is in:

```text
firmware/
├── README.md                          ← this file
└── RTAL_SYNTECH_V1_0_0_FINAL/
    ├── RTAL_SYNTECH_V1_0_0_FINAL.ino  ← Arduino IDE entry point
    ├── Board.cpp / Board.h            ← board and display integration
    ├── PINS_JC4827W543.h              ← board pin definitions
    ├── SyntechMidiCC.h                ← MIDI parameter assignments
    ├── SyntechCommands.h              ← engine command definitions
    ├── Syntech*.h                     ← auxiliary controls and diagnostics
    ├── sc_voice.c / sc_voice.h        ← voice DSP
    ├── sc_synth.c / sc_synth.h        ← synthesizer DSP
    ├── sc_patch.h / sc_fixed.h        ← DSP patch and fixed-point support
    ├── sc_tables.h / sc_profile.h     ← DSP tables and profiling
    ├── SyntechSplash.h                ← embedded splash graphic
    ├── lv_conf.h                      ← LVGL configuration
```

Open the **`.ino` file inside the identically named project directory** to build the sketch.

## 2. Implemented capabilities

- Up to **six voices** in normal operation; heavy CROSS1/CROSS2 settings can require a lower practical voice count.
- Two oscillator paths per voice, PWM, detune, sync, noise, ring and cross modulation.
- Multimode filtering with **MUSICAL LP24** resonance as the default behavior.
- Independent filter and amplifier envelopes, two LFOs and modulation matrices.
- Chorus and reverb effects.
- **SINGLE / SPLIT / DOUBLE** performance modes and UPPER / LOWER editing.
- Touch interface, on-screen DSP timing, XRUN and command-queue diagnostics.
- DIN MIDI and USB MIDI device support, Program Change `0–127`, STANDARD / CC32-banked MOPHO parameter control, persistent MIDI Learn.
- MIDI **CC120 (All Sound Off)** and **CC123 (All Notes Off)** are processed even during MIDI Learn.
- SD-based patch storage with **128 slots**: factory `P000–P079`, user `P080–P127`.

See the [project overview](../README.md), [MIDI CC reference](../doc/RTAL_SYNTECH_User_Manual_V1.0.0_EN.md), and the [English user manual](../doc/RTAL_SYNTECH_User_Manual_V1.0.0_EN.md) for details.

## 3. Target hardware and audio settings

| Component | Firmware configuration |
|:--|:--|
| Main processor | ESP32-S3 on Guition JC4827W543 |
| Screen | 480 × 272 NV3041A QSPI LCD |
| Touch controller | GT911 |
| Digital audio | I²S output to external DAC/audio stage |
| Audio sample rate | **32,000 Hz** |
| Audio processing block | **256 frames** |
| DIN MIDI input | **GPIO17**, 31,250 baud |
| DIN MIDI output | **GPIO18**, 31,250 baud |
| I²S BCLK / LRCK / DOUT | **GPIO16 / GPIO15 / GPIO7** |
| SD SPI SCK / MISO / MOSI / CS | **GPIO12 / GPIO13 / GPIO11 / GPIO10** |
| SD SPI speed | 8 MHz |

The board-specific QSPI, touch and backlight assignments are defined in `RTAL_SYNTECH_V1_0_0_FINAL/PINS_JC4827W543.h`. **Do not use an unrelated ESP32-S3 board's wiring diagram.** The source and its build comments refer to an ESP32-S3 target with **16 MB flash / 8 MB PSRAM**.

> **Version note:** Some screenshots in the project README originate from earlier 44.1-kHz development builds. The V1.0.0 FINAL source here explicitly configures **32 kHz / 256 frames**.

## 4. Build prerequisites

The source identifies the following development environment:

- **Arduino IDE 2.3.10**
- **Arduino-ESP32 core 3.3.11**
- **LVGL 8.4.x** with the bundled `lv_conf.h`
- Board/display, SD and USB-MIDI/TinyUSB dependencies required by the sketch and board support files
- ESP32-S3 configuration with the correct **flash and PSRAM options**
- **USB Mode: USB-OTG (TinyUSB)** — required by the source; do not select Hardware CDC/JTAG instead

The original archive does not contain a fully locked dependency manifest. Consult source includes and use a matching board configuration; do not assume that installing Arduino-ESP32 alone is sufficient for a clean build.

### Building from source

1. Install the Arduino IDE, the appropriate ESP32-S3 board package and required libraries.
2. Open `RTAL_SYNTECH_V1_0_0_FINAL/RTAL_SYNTECH_V1_0_0_FINAL.ino`.
3. Select the correct JC4827W543-compatible ESP32-S3 board configuration, 16 MB flash/8 MB PSRAM where applicable, and **USB-OTG (TinyUSB)** mode.
4. Prepare the SD card as described below **before** starting the instrument.
5. Compile, examine the complete build log and upload to the board.
6. Connect a compatible **external I²S DAC** and properly wired MIDI interface; verify audio output and controls at safe volume.

**Build reproducibility:** The publication audit did **not** perform a fresh ESP32-S3 Arduino compilation or reflash. The steps above describe the expected workflow, not an independently validated turnkey recipe.

## 5. SD card and presets — required for startup

Preset files are read from:

```text
/RTAL_SYNTECH/PRESETS/
├── P000.rtal
├── P001.rtal
├── ...
└── P127.rtal
```

- Slots `000–079` are the designated factory range; `080–127` are the user range.
- Firmware reads **version-2 `.rtal` patch files**, with integrity checking.
- The firmware performs a scan of the expected preset slots and requires **at least one valid compatible preset** to proceed with startup. The full 128-slot bank is still the intended normal installation layout.
- **No complete compatible preset bank was supplied in the V1.0.0 firmware ZIP.** Do not create arbitrary text files with `.rtal` extensions: they are binary patch records and must match the implemented format.

Until a rights-cleared, tested factory-preset pack or provisioning utility is published, building the sketch alone is **not sufficient** for first-time deployment.

## 6. MIDI and real-time design

The system separates time-critical synthesis from GUI and MIDI handling. Control and note operations are passed to the audio engine through FreeRTOS command handling rather than executing drawing or SD operations in the audio processing path.

- **STANDARD mode:** direct parameter CC mapping.
- **MOPHO mode:** `CC32` first selects a persistent parameter bank; subsequent CC messages address that bank's controls.
- **MIDI Learn:** persistent per-parameter mappings.
- **Program Change:** addresses slots `0–127`.

For the authoritative parameter names and CC mapping, see [`midi_implementation/README.md`](../midi_implementation/README.md). Verify MIDI wiring and controller channel settings during integration.

## 7. Upstream attribution and licenses

**Joakim Eriksson's SCINTILLA project is a primary source-code and DSP ancestor of RTAL SYNTECH.** We explicitly acknowledge the original author and do not present the synthesis engine as having been written from scratch.

- **Joakim Eriksson:** https://github.com/joakimeriksson
- **SCINTILLA:** https://github.com/joakimeriksson/scintilla-fm1

The package includes the GNU GPL v3 license text at [`../LICENSE`](../LICENSE) and third-party attribution/provenance notes at [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md). Retain applicable upstream headers, license notices and source distribution obligations when redistributing or modifying the firmware. An exact source-file-to-upstream-commit provenance audit remains outstanding; the credit here does **not** replace that requirement.

**Elka Synthex** is the historical musical inspiration. SYNTECH is an independent project, not an official or endorsed Elka product.

---

**RealTimeAudioLab · RTAL-EAI-015 · Firmware V1.0.0 FINAL**  
[Main project README](../README.md) · [User manual (EN)](../doc/RTAL_SYNTECH_User_Manual_V1.0.0_EN.md) · [Release audit](../doc/RELEASE_AUDIT_V1.0.0.md)
