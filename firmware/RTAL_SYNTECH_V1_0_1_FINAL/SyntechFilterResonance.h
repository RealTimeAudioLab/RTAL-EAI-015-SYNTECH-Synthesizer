// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "sc_synth.h"
#include "sc_fixed.h"

// RTAL SYNTECH V0.9A FIX3: EXPERIMENTAL, transient LP24 Q mapping for A/B tests.
// Original sc_prep() remains unchanged; its Q and attenuation are never altered
// when the A/B mask is zero, or when a non-LP24 filter is selected.
// Original: Q = 0.5 + 24.5 * r^2  (at 42/127, Q ~ 3.18 per SVF stage).
// Musical:  Q = 0.5 +  5.5 * r^3  (at 42/127, Q ~ 0.70 per SVF stage).
// Both LP24 SVF stages still share the same coefficient; no DSP topology changes.
// Cache is AudioTask-only and avoids repeated 64-bit divisions per audio block.
struct SyntechResoCache {
  int32_t lastResoQ16[2];
  int32_t lastKQ28[2];
};

static inline int32_t syntechMusicalResoKQ28(int32_t r) {
  r=sc_clamp(r,0,SC_ONE16);
  const int32_t r2=sc_mul(r,r,16);
  const int32_t r3=sc_mul(r2,r,16);
  const uint32_t q=(uint32_t)(SC_ONE16/2+sc_mul(r3,SC_Q16(5.5),16));
  return (int32_t)sc_udiv64((uint64_t)1<<44,q);
}

static inline void syntechFilterResoApply(sc_synth_t *s,uint8_t musicalMask,SyntechResoCache *cache) {
  if(!s || !cache || !musicalMask)return;
  for(unsigned k=0;k<2;k++) {
    const uint8_t bit=(uint8_t)(1u<<k);
    if(!(musicalMask&bit) || s->patch.layer[k].flt_mode!=SC_FLT_LP24)continue;
    const int32_t r=sc_clamp(s->patch.layer[k].reso,0,SC_ONE16);
    if(cache->lastResoQ16[k]!=r) {
      cache->lastResoQ16[k]=r;
      cache->lastKQ28[k]=syntechMusicalResoKQ28(r);
    }
    s->layer[k].prep.k_q28=cache->lastKQ28[k];
  }
}
