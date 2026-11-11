# RTAL SYNTECH V1.0.0 FINAL — independent publication audit

**Date:** 2026-11-11  
**Inspected archive:** `RTAL_SYNTECH_V1_0_0_FINAL(1).zip`  
**SHA-256:** `daf8813b48bb80abce3a1f4433c136494f29fea3b02ff84c0729421c0ab375b4`

## Scope and outcome

The archive contains **50 paths** including the Arduino sketch, board drivers, DSP engine, MIDI CC map, diagnostic / comparison tests, historical `.diff` files and the original firmware README. The source identifies itself as `V1.0.0 FINAL`; the actual firmware sample rate is **32,000 Hz** and the I²S render block size is **256 frames**, not 44.1 kHz as asserted by an older teaser screenshot. Firmware parameters include MIDI DIN RX17/TX18, I²S BCLK16/LRCK15/DOUT7, USB MIDI via TinyUSB and LVGL 8.4.x.

### Tests executed on this audit machine (native host; not ESP32 hardware)

| Bundled test | Result |
|:--|:--|
| `tests/run_host_test.sh` | **PASS** — sample-rate/pitch, LFO, cutoff and stress scenarios |
| `tests/run_cross2_sqfast_diff.sh` | **PASS** — optimized vs. reference DSP comparison |
| `tests/run_filterdiag_host.sh` | **PASS** — diagnostic compile/runtime |
| `tests/run_filterenv1_isolation.sh` | **PASS** — filter-envelope isolation |
| `tests/run_filterreso_ab.sh` | **PASS** — original vs. MUSICAL resonance behavior |
| `tests/run_fix4.sh` | **PASS** — bundled regression |

These tests are valuable but **do not validate** the entire Arduino firmware, display, timing under load, actual audio quality, SD I/O or USB/DIN MIDI in hardware.

## Findings and release actions

1. **Highest priority — required SD content missing.** Firmware intentionally performs *SD-only* preset loading, audits its 128 expected slots, and waits/restarts if it cannot find any valid version-2 `.rtal` preset. No usable complete preset bank is supplied in the firmware archive. A GitHub visitor cannot boot the instrument from source alone without supplying compatible preset files. Provide a separately rights-cleared and tested SD image/pack or clear provisioning instructions and a compatible patch generator.
2. **ESP32-S3 release build not reproduced.** `arduino-cli`/Arduino ESP32 toolchain is unavailable in this audit runtime. No claim of passing a clean Arduino IDE 2.3.10 + core 3.3.11 + LVGL 8.4.x build is made. Validate exact board options (including USB-OTG TinyUSB and 16 MB flash / 8 MB PSRAM) on a clean installation and archive build logs.
3. **No independently reproduced physical test.** The developer has previously confirmed that V1.0.0 works on hardware, but the exact files in the published candidate were not reflashed or listened to in this audit. Exercise clean boot, all seven tabs, both layers, FX, preset save/reload, DIN+USB MIDI and heavy modulation before attaching a ready-to-flash binary.
4. **License provenance incomplete.** Existing sources declare `GPL-3.0-only`; original firmware ZIP contained **no top-level `LICENSE`** and no detailed provenance/notice document. The publication package adds GNU GPL-3.0 and `THIRD_PARTY_NOTICES.md`; it still lacks an upstream commit hash and a complete file-by-file inherited copyright record. Verify the exact SCINTILLA commit and any BSD-2-Clause material before mass distribution.
5. **Outdated original firmware README.** It correctly lists 32 kHz but claims that the exact FINAL package was not compiled/tested 'here'. The public README transparently distinguishes historical hardware confirmation and the host checks completed in this audit.
6. **Historical 44.1 kHz screenshots.** Retain with a prominent note rather than misrepresent them as V1.0.0 benchmarks. Older XRUN totals are not measurements of the release.
7. **Dependency reproduction.** Project pins ESP32-Arduino 3.3.11, IDE 2.3.10 and LVGL 8.4.x in comments, but there is no locked reproducible dependency manifest or automated ESP32 build pipeline. Recommend recording exact library versions and publish a CI build script after verification.
8. **Distribution hygiene.** Archive includes historical diff files, host test artifacts and diagnostic interfaces. They are not themselves known release-blocking defects, but should be clearly separated from the end-user firmware folder or documented as developer tooling.

## Honest verdict

**Suitable for a transparent GitHub source-code release with the limitations explicitly stated.** **Not yet independently verified as a complete, plug-and-play end-user firmware release** because the preset bank is absent and a clean device build/flash was not repeated. No sound engine refactoring or behavior changes were made in this documentation package.
