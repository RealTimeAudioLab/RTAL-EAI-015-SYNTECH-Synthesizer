# RTAL-EAI-015-SYNTECH-Synthesizer

### Inspired by the legendary Elka Synthex · Built for ESP32-S3 · Powered by real-time DSP

**A compact, polyphonic virtual-analog synthesizer with a touch-first interface, dual-layer architecture, deep modulation and comprehensive MIDI control.**

![Project status](https://img.shields.io/badge/status-active%20development-00d7eb?style=flat-square)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-0077cc?style=flat-square)
![Audio](https://img.shields.io/badge/audio-44.1%20kHz-0077cc?style=flat-square)
![Polyphony](https://img.shields.io/badge/voices-up%20to%206-0077cc?style=flat-square)
![Project](https://img.shields.io/badge/RTAL-EAI--015-00d7eb?style=flat-square)

**[Overview](#the-project) · [Sound engine](#sound-engine) · [Touch interface](#touch-interface) · [MIDI](#midi-control) · [Gallery](#interface-gallery) · [Credits](#credits--acknowledgements)**

</div>

> [!IMPORTANT]
> **Development preview / teaser — not a finished firmware release.** This repository presentation documents the evolving project and shows real interface captures. Build instructions, binaries and complete source releases will follow when a reproducible release package is prepared. Features and performance may change during development.


https://github.com/user-attachments/assets/709780a9-422d-4dee-b969-57c467f08526


## The project

**RTAL SYNTECH** is a standalone embedded virtual-analog synthesizer inspired by the architecture and musical spirit of the **Elka Synthex**. The project combines the classic signal-flow concepts of a vintage polyphonic instrument with a modern graphical user interface, external MIDI control and an ESP32-S3 real-time audio system.

Our goal is not a circuit-identical hardware replica or an officially endorsed Elka product. It is an independently developed instrument that brings Synthex-inspired synthesis into a small, approachable embedded platform — with transparent technical documentation and appropriate upstream attribution.

**Deutsch:** RTAL SYNTECH ist ein eigenständiges, polyphones Virtual-Analog-Synthesizer-Projekt auf ESP32-S3-Basis, inspiriert vom legendären Elka Synthex. Im Mittelpunkt stehen ein musikalischer Klangcharakter, zwei editierbare Klangebenen, umfangreiche Modulationsmöglichkeiten und eine direkt am Gerät bedienbare Touch-Oberfläche. Dieses Repository dient zunächst als Entwicklungs-Vorschau.

### At a glance

| Area | RTAL SYNTECH |
|:--|:--|
| Project ID | **RTAL-EAI-015** |
| Synthesis | Embedded virtual analog / subtractive synthesis |
| Target controller | **ESP32-S3**, Guition **JC4827W543** main module |
| Interface | **480 × 272** color touchscreen, **LVGL 8.4** |
| Audio output | Digital **I²S** audio to an external DAC / audio stage |
| Target sample rate | **32.000 samples/s** |
| Polyphony policy | **6 voices** |
| Layers | **UPPER / LOWER**, with single, split and double concepts |
| Oscillators | 2 per voice; waveform selection, PWM, detune and oscillator sync |
| Additional sources | Noise, pink-noise option, ring and cross modulation |
| Filtering | Multimode filter; LP24 musical resonance behavior is an explicit project focus |
| Modulation | 2 LFOs, filter and amplifier envelopes, two six-destination LFO routing matrices |
| Effects | Chorus and reverb with on-screen controls |
| Presets | 128 planned SD slots: **000–079 factory**, **080–127 user** |
| MIDI | DIN MIDI, MIDI Program Change, configurable CC mapping, MIDI Learn |
| Control schemes | **STANDARD** (direct CC) and **MOPHO** (CC32 banked CC) |
| Engineering | Real-time audio tasking, asynchronous user interface, on-screen DSP/XRUN/queue diagnostics |

> **Image-version note:** Some images in the gallery come from earlier 44.1 kHz revisions and show an accumulated XRUN counter; other enlarged matrix captures are labeled 32.0 kHz. These are authentic development snapshots from **different test configurations**, not simultaneous measurements and not proof of a final performance specification. The target specification is 44.1 kHz.

## Why SYNTECH?

A great analog synthesizer is more than a collection of oscillators. Its personality comes from how oscillators interact, how filters respond to modulation, how envelopes shape a sound, and how easily musicians can reach those parameters. RTAL SYNTECH puts all of this on a compact touchscreen while keeping the most timing-critical DSP work separate from the user interface.

The instrument is designed around three priorities: **sound character**, **musical control** and **real-time stability**. Particularly DSP-intensive cross-modulation combinations are handled conservatively: preserving clean audio is more important than advertising a theoretical maximum polyphony under every condition.

## Sound engine

### Oscillators and mixing

Each synthesis voice provides two oscillator paths with independently adjustable waveforms and pulse width. OSC2 detune, oscillator sync, oscillator mixing and noise broaden the sound palette from classic pads and brass to animated sync leads and metallic textures.

The mixer and cross-modulation section provides **OSC1/OSC2 levels**, **noise**, **CROSS1**, **CROSS2** and **ring modulation**. CROSS1/CROSS2 are computationally demanding at higher depths; a reduced polyphony limit may be applied in high-load conditions.

### Filters and envelopes

The filter section brings together **cutoff**, **resonance**, **filter type**, **envelope amount**, **keyboard tracking** and **pink noise** controls. A carefully tuned **MUSICAL LP24 resonance curve** is part of the project's sound-design direction. Separate filter and amplifier ADSR envelopes provide independent control of movement and loudness.

### LFOs and modulation matrices

Two configurable LFOs support rate, waveform, delay, sync and modulation depth controls. Each LFO can be routed independently and with bipolar amounts to **OSC1 pitch**, **OSC2 pitch**, **OSC1 PWM**, **OSC2 PWM**, **filter cutoff** and **amplitude**. The routing design allows performance-oriented sounds as well as slow, evolving patches.

### Effects

A dedicated FX page exposes **chorus** and **reverb** switches and their key parameters. The captured interface shows chorus mix/rate/depth and reverb mix/size/damping.

## Touch interface

The interface is purpose-built for the **JC4827W543's 480 × 272 display**. Large touch targets and clearly separated functional pages make synthesis parameters accessible without a mouse, external editor or extensive menu diving.

The main navigation provides **OSC**, **MIX**, **FILTER**, **F-ENV**, **A-ENV**, **LFO** and **FX**, alongside **LAYER**, **MODE**, **MIDI**, **BANK** and preset navigation. In the LFO area, the user can switch between **LFO 1**, **LFO 2**, **MAIN**, **MATRIX A** and **MATRIX B**.

An on-screen diagnostic line reports DSP timing as well as **XRUN** and command-queue counters. The counters make timing issues visible during development rather than hiding them behind the UI.

### Interface gallery

*Click an image in GitHub to view its full resolution.*

| Oscillators | Mixer | Filter |
|:--:|:--:|:--:|
| ![Oscillator page](images/RTAL_SYNTECH_09_Oscillators.png) | ![Mixer page](images/RTAL_SYNTECH_08_Mixer.png) | ![Filter page](images/RTAL_SYNTECH_07_Filter.png) |
| **OSC** | **MIX** | **FILTER** |

| Filter envelope | Amplifier envelope | Effects |
|:--:|:--:|:--:|
| ![Filter envelope](images/RTAL_SYNTECH_06_Filter_Envelope.png) | ![Amplifier envelope](images/RTAL_SYNTECH_05_Amp_Envelope.png) | ![Chorus and reverb](images/RTAL_SYNTECH_03_FX.png) |
| **F-ENV** | **A-ENV** | **FX** |

| LFO controls | MIDI setup | Preset manager |
|:--:|:--:|:--:|
| ![LFO controls](images/RTAL_SYNTECH_04_LFO.png) | ![MIDI setup](images/RTAL_SYNTECH_01_MIDI_Setup.png) | ![Preset manager](images/RTAL_SYNTECH_02_Preset.png) |
| **LFO** | **MIDI** | **PRESET** |

### Modulation matrix: detailed views

These enlarged captures highlight the modulation architecture and provide a closer look at the precision-oriented touch controls.

![LFO matrix A enlarged](images/RTAL_SYNTECH_10_LFO_Matrix_A.PNG)

*Matrix A — OSC1/OSC2 pitch and OSC1 PWM modulation.*

![LFO matrix B enlarged](images/RTAL_SYNTECH_11_LFO_Matrix_B.PNG)

*Matrix B — OSC2 PWM, filter and amplitude modulation.*

### Complete interface overview

![SYNTECH screen overview](images/RTAL_SYNTECH_12_OverView.png)

## MIDI control

MIDI is a central part of SYNTECH's design, not an afterthought. Along with note and patch selection, the project provides two alternative parameter-control schemes and a MIDI Learn interface for controller integration.

**STANDARD MODE** assigns a direct MIDI CC to each parameter. For example, the supplied mapping uses **CC27** for Filter Cutoff, **CC28** for Resonance and **CC14** for OSC1 Waveform.

**MOPHO MODE** uses **CC32** to select the active parameter bank first; the same CC number can then address a different function in another bank. The selected bank persists until changed. For example, **Bank 0 / CC102** controls Filter Cutoff, while **Bank 1 / CC106** controls LFO1 Rate and **Bank 2 / CC106** controls LFO2 Rate.

| Example parameter | Standard mode | MOPHO mode |
|:--|:--|:--|
| OSC1 waveform | CC14 | Bank 0 → CC22 |
| OSC2 waveform | CC15 | Bank 0 → CC26 |
| OSC1 PWM | CC16 | Bank 0 → CC23 |
| Filter cutoff | CC27 | Bank 0 → CC102 |
| Filter resonance | CC28 | Bank 0 → CC103 |
| Filter envelope attack | CC33 | Bank 0 → CC109 |
| Amplifier envelope attack | CC37 | Bank 0 → CC118 |
| LFO1 rate | CC42 | Bank 1 → CC106 |
| LFO2 rate | CC45 | Bank 2 → CC106 |
| LFO1 → OSC1 pitch | CC59 | Bank 1 → CC111 |
| LFO2 → filter cutoff | CC73 | Bank 2 → CC119 |
| Chorus mix | CC48 | Bank 1 → CC28 |
| Reverb mix | CC51 | Bank 1 → CC102 |

The source spreadsheet also defines **MIDI Learn identifiers**, **parameter ranges**, **UPPER/LOWER or global scope**, and the rest of the CC matrix. MIDI channel selection is presented through the on-device MIDI dialog; the design supports configurable MIDI channel handling. **MIDI Program Change 0–127** is intended to address the 128 patch slots.

**[Complete MIDI CC reference](midi_implementation/MIDI_CC_REFERENCE.md)** · **[Original editable MIDI spreadsheet](midi_implementation/RTAL_SYNTECH_MIDI_CC_STANDARD_MOPHO_EDITOR.xlsx)**

> [!NOTE]
> This table is a documentation extract from the supplied MIDI editor workbook; it should not be mistaken for an automated verification of the exact firmware currently running in every screenshot.

## Presets and performance

The patch manager is designed for **128 slots**, with a dedicated bank of 80 factory patches and 48 user patches. The touch UI includes patch browsing, a name field, LOAD and SAVE controls and a visual indication of loading state. SD storage and MIDI Program Change access support both standalone use and integration with a DAW or MIDI controller.

- Normal voice target: **6-note polyphony**.
- Primary audio target: **32 kHz**, with a fixed real-time audio block strategy.
- Live timing counters: **DSP processing time, XRUNs and command queue state**.

Because screenshots cover multiple development revisions, displayed historical counter values should not be treated as final benchmarks.

## Embedded platform and architecture

RTAL SYNTECH targets the **Guition JC4827W543** ESP32-S3 touch-display board. The platform combines a responsive QSPI LCD, capacitive touch input, PSRAM and a hardware I²S output path. The audio engine, MIDI handling and GUI are kept logically separated so that editing a patch or switching pages does not require the audio interrupt path to block on drawing or storage operations.

```text
External MIDI controller / sequencer
                 |
             DIN MIDI IN
                 |
           MIDI input task
                 |
        Parameter / note events
                 |
          FreeRTOS queue
                 |
    +---------------------------+
    | Real-time audio engine    |
    |  Voice allocation         |
    |  OSC + MIX + FILTER       |
    |  ADSR + LFO matrices      |
    |  Chorus + Reverb          |
    +---------------------------+
                 |
              I2S DAC ------> Stereo audio out

    Touch GUI / patch manager --> queued engine updates
                   |
                 SD card
```

This is a **conceptual architecture diagram**, not a pin-level wiring schematic or a promise that each branch exists unchanged in every development revision. Hardware connection tables and reproducible build settings will be published with a source release.

## Project development roadmap

| Area | Direction |
|:--|:--|
| Core synthesis | Stable six-voice baseline and preservation of musical response |
| Heavy-modulation handling | Document and test automatic/fixed four-voice behavior for demanding patches |
| Sound design | Finalize filter character, resonance, oscillator response and factory patches |
| MIDI | Validate standard/banked CC modes and persistent MIDI Learn mappings |
| Storage | Consolidate SD preset layout, naming, loading and saving |
| UI | Refine touch interaction, value display and parameter descriptions |
| Release engineering | Publish auditable source, hardware wiring, build instructions, license notices and binaries |
| Documentation | Developer notes, detailed MIDI chart and illustrated user manual |

## Credits & acknowledgements

### Primary upstream inspiration and code heritage — Joakim Eriksson / SCINTILLA

RTAL SYNTECH **acknowledges and credits Joakim Eriksson** for his **SCINTILLA** synthesizer work, which is a **key code and DSP reference** for this project. The upstream synthesizer work and its analysis of Synthex-inspired voice architecture are foundational to the direction of SYNTECH. Please explore and credit the original author and projects:

- **Joakim Eriksson — GitHub:** https://github.com/joakimeriksson
- **SCINTILLA (public project):** https://github.com/joakimeriksson/scintilla-fm1
- **Related ESP32 development:** https://github.com/joakimeriksson/esp32

**This is not presented as an original-from-scratch synthesis engine.** RTAL SYNTECH adapts and expands Synthex-inspired concepts for a different embedded board, with its own UI, project integration, control mapping and firmware development. The exact borrowed files, their upstream revisions and their original copyright/license notices must be recorded in a source-level third-party notice when firmware is published. **Attribution is not a substitute for license compliance.**

### Historic inspiration

The **Elka Synthex** was designed by **Mario Maggi** and is renowned for its layered analog sound and distinctive modulation and filter architecture. **SYNTECH** is a separate project, not endorsed by or affiliated with Elka or any current trademark holder. 

## Licensing and distribution

**The final license for the RTAL SYNTECH repository has not been asserted in this teaser package.** The linked **SCINTILLA FM-1 repository identifies its firmware as GPL-3.0-only** and credits its own upstream materials; relevant licenses depend on precisely which source files were incorporated. Before distributing a source/firmware release, the project must document provenance file by file, preserve notices and choose a compatible license for all derivative material. Do **not** interpret this preliminary README as permission to relicense upstream code.

The visual assets here are project screenshots/graphics supplied for this README; they do not change third-party software license obligations.

## Availability

**Current phase:** public development teaser. Source snapshots, board pinouts, assembly instructions, compatible toolchain configuration, preset banks, test methodology and downloadable firmware will be documented as they become release-ready. This teaser intentionally does not provide untested “quick flash” instructions.

If you are interested in the development, **watch** the repository for progress, screenshots, audio demos and upcoming technical releases.

---

### RTAL SYNTECH

**Classic polyphonic inspiration. Modern embedded implementation.**

*RTAL-EAI-015 · Development preview · November 2026*

</div>
