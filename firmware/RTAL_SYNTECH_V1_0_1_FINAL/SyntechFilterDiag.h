// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
// V0.9A FIX1: fixed-size Core1-to-Core0 diagnostic snapshot; no pointers.
struct FilterDiagVoiceSnapshot {
  int16_t note;
  int8_t slot;
  uint8_t coeffValid, stage, held;
  int32_t cutoffNoteQ16, currentNoteQ16;
  int32_t envLevelQ30, lfo1Q20, lfo2Q20, velQ16;
  int32_t a1Q30, a2Q30, a3Q30, cachedKQ28;
};
struct FilterDiagLayerSnapshot {
  int8_t mode;
  uint8_t activeVoices, heldVoices;
  int32_t cutoffQ16, resoQ16, envAmountQ16, keyTrackQ16;
  int32_t baseQ16, kc1Q16, kc2Q16, kcEnvQ16, kcEnv1Q16;
  int32_t kcVelQ16, kcKeyQ16, kQ28, attenQ16;
  int32_t lfoDepthB1Q16, lfoDepthB2Q16;
  int32_t mmL1CutQ16, mmL2CutQ16, mmE1CutQ16, mmVelCutQ16;
  int32_t mmJoyCutQ16;
  uint8_t env1CutBypassed; // FIX2: 1 = ENV1 -> cutoff temporarily bypassed on audio task
  uint8_t lp24Musical; // FIX3: 1 = experimental musical Q curve for LP24, 0 = original
  FilterDiagVoiceSnapshot voice;
};
struct FilterDiagSnapshot {
  uint32_t ms;
  int8_t activeLayer, voiceMode, chorusOn, reverbOn;
  int32_t joyXQ16, joyYQ16, bendFiltQ16, lfo2FiltQ16;
  FilterDiagLayerSnapshot layer[2];
};
