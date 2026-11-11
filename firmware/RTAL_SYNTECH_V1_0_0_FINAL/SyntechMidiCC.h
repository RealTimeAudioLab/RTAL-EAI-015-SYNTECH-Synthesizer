#pragma once
#include <stdint.h>
// Generated from USER UPLOADED "RTAL_SYNTECH_MIDI_CC_Parameterliste_LFO_MATRIX_ZUM_AUSFUELLEN(1).xlsx".
// Confirmed mappings: 61; no duplicate CC per CC32 bank and MIDI channel.
// CC32 ONLY selects parameter banks 0/1/2. Program Change always loads P000..P127.rtal.
struct SyntechCCMap {uint8_t bank;uint8_t cc;uint8_t command;uint8_t id;};
enum : uint8_t { SYN_CC_EDIT=0, SYN_CC_LAYER, SYN_CC_MODE, SYN_CC_SPLIT, SYN_CC_CHORUS, SYN_CC_REVERB, SYN_CC_CUTOFF, SYN_CC_RESO, SYN_CC_FILTER_MODE };
static const SyntechCCMap SYNTECH_CC_MAP[] = {
  {0, 22, SYN_CC_EDIT, 0}, // OSC1 Waveform (XLSX row 2)
  {0, 26, SYN_CC_EDIT, 1}, // OSC2 Waveform (XLSX row 3)
  {0, 23, SYN_CC_EDIT, 2}, // OSC1 PWM (XLSX row 4)
  {0, 27, SYN_CC_EDIT, 3}, // OSC2 PWM (XLSX row 5)
  {0, 31, SYN_CC_EDIT, 4}, // OSC2 Detune (XLSX row 6)
  {0, 30, SYN_CC_EDIT, 38}, // Oscillator Sync (XLSX row 7)
  {0, 28, SYN_CC_EDIT, 5}, // OSC1 Level (XLSX row 8)
  {0, 52, SYN_CC_EDIT, 6}, // OSC2 Level (XLSX row 9)
  {0, 29, SYN_CC_EDIT, 7}, // Noise Level (XLSX row 10)
  {0, 89, SYN_CC_EDIT, 8}, // Cross Mod 1 (XLSX row 11)
  {0, 90, SYN_CC_EDIT, 9}, // Cross Mod 2 (XLSX row 12)
  {0, 77, SYN_CC_EDIT, 39}, // Ring Modulation (XLSX row 13)
  {0, 78, SYN_CC_EDIT, 33}, // Pink Noise (XLSX row 14)
  {0, 102, SYN_CC_CUTOFF, 34}, // Filter Cutoff (XLSX row 15)
  {0, 103, SYN_CC_RESO, 35}, // Filter Resonance (XLSX row 16)
  {0, 105, SYN_CC_FILTER_MODE, 36}, // Filter Type (XLSX row 17)
  {0, 106, SYN_CC_EDIT, 10}, // Filter Env Amount (XLSX row 18)
  {0, 107, SYN_CC_EDIT, 11}, // Filter Key Tracking (XLSX row 19)
  {0, 109, SYN_CC_EDIT, 12}, // Filter Attack (XLSX row 20)
  {0, 110, SYN_CC_EDIT, 13}, // Filter Decay (XLSX row 21)
  {0, 111, SYN_CC_EDIT, 14}, // Filter Sustain (XLSX row 22)
  {0, 112, SYN_CC_EDIT, 15}, // Filter Release (XLSX row 23)
  {0, 118, SYN_CC_EDIT, 16}, // Amp Attack (XLSX row 24)
  {0, 119, SYN_CC_EDIT, 17}, // Amp Decay (XLSX row 25)
  {0, 75, SYN_CC_EDIT, 18}, // Amp Sustain (XLSX row 26)
  {0, 76, SYN_CC_EDIT, 19}, // Amp Release (XLSX row 27)
  {1, 106, SYN_CC_EDIT, 20}, // LFO1 Rate (XLSX row 28)
  {1, 109, SYN_CC_EDIT, 21}, // LFO1 Depth (XLSX row 29)
  {1, 110, SYN_CC_EDIT, 22}, // LFO1 Waveform (XLSX row 30)
  {2, 106, SYN_CC_EDIT, 23}, // LFO2 Rate (XLSX row 31)
  {2, 109, SYN_CC_EDIT, 24}, // LFO2 Depth (XLSX row 32)
  {2, 110, SYN_CC_EDIT, 25}, // LFO2 Waveform (XLSX row 33)
  {1, 28, SYN_CC_EDIT, 26}, // Chorus Mix (XLSX row 34)
  {1, 52, SYN_CC_EDIT, 27}, // Chorus Rate (XLSX row 35)
  {1, 29, SYN_CC_EDIT, 28}, // Chorus Depth (XLSX row 36)
  {1, 102, SYN_CC_EDIT, 29}, // Reverb Mix (XLSX row 37)
  {1, 103, SYN_CC_EDIT, 30}, // Reverb Size (XLSX row 38)
  {1, 105, SYN_CC_EDIT, 31}, // Reverb Damping (XLSX row 39)
  {0, 14, SYN_CC_LAYER, 0}, // Upper / Lower Select (XLSX row 42)
  {0, 104, SYN_CC_MODE, 0}, // Play Mode (XLSX row 43)
  {0, 53, SYN_CC_SPLIT, 0}, // Split Point (XLSX row 44)
  {0, 115, SYN_CC_CHORUS, 0}, // Chorus Enable (XLSX row 46)
  {0, 86, SYN_CC_REVERB, 0}, // Reverb Enable (XLSX row 47)
  {1, 111, SYN_CC_EDIT, 40}, // LFO1 → OSC1 Pitch (XLSX row 50)
  {1, 112, SYN_CC_EDIT, 41}, // LFO1 → OSC2 Pitch (XLSX row 51)
  {1, 115, SYN_CC_EDIT, 42}, // LFO1 → OSC1 PWM (XLSX row 52)
  {1, 118, SYN_CC_EDIT, 43}, // LFO1 → OSC2 PWM (XLSX row 53)
  {1, 119, SYN_CC_EDIT, 44}, // LFO1 → Filter Cutoff (XLSX row 54)
  {1, 75, SYN_CC_EDIT, 45}, // LFO1 → Amplitude (XLSX row 55)
  {1, 76, SYN_CC_EDIT, 52}, // LFO1 Filter/Amp Depth (XLSX row 56)
  {1, 86, SYN_CC_EDIT, 54}, // LFO1 Delay (XLSX row 57)
  {1, 89, SYN_CC_EDIT, 56}, // LFO1 Sync (XLSX row 58)
  {2, 111, SYN_CC_EDIT, 46}, // LFO2 → OSC1 Pitch (XLSX row 59)
  {2, 112, SYN_CC_EDIT, 47}, // LFO2 → OSC2 Pitch (XLSX row 60)
  {2, 115, SYN_CC_EDIT, 48}, // LFO2 → OSC1 PWM (XLSX row 61)
  {2, 118, SYN_CC_EDIT, 49}, // LFO2 → OSC2 PWM (XLSX row 62)
  {2, 119, SYN_CC_EDIT, 50}, // LFO2 → Filter Cutoff (XLSX row 63)
  {2, 75, SYN_CC_EDIT, 51}, // LFO2 → Amplitude (XLSX row 64)
  {2, 76, SYN_CC_EDIT, 53}, // LFO2 Filter/Amp Depth (XLSX row 65)
  {2, 86, SYN_CC_EDIT, 55}, // LFO2 Delay (XLSX row 66)
  {2, 89, SYN_CC_EDIT, 57}, // LFO2 Sync (XLSX row 67)
};
static constexpr unsigned SYNTECH_CC_MAP_COUNT=sizeof(SYNTECH_CC_MAP)/sizeof(SYNTECH_CC_MAP[0]);
static_assert(SYNTECH_CC_MAP_COUNT == 61, "Check user MIDI CC map count");

// STAGE2 STANDARD factory mapping: 61 distinct direct CCs. Reserved standard controllers excluded.
static const uint8_t SYNTECH_STANDARD_CC[SYNTECH_CC_MAP_COUNT] = {
14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77
};
static_assert(sizeof(SYNTECH_STANDARD_CC)/sizeof(SYNTECH_STANDARD_CC[0])==SYNTECH_CC_MAP_COUNT,"STANDARD map size");
