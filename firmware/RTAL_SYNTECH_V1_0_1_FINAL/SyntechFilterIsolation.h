// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "sc_synth.h"

// V0.9A FIX2 - temporary ENV1 -> cutoff route isolation for diagnosis only.
// The patch (including its modulation matrix) is NEVER modified. Only the
// audio task's prepared coefficient is changed, immediately before sc_render.
// Call after ALL sc_prep()/sc_reprep() work in the audio block, even when
// no commands arrived, so an unrelated re-preparation cannot reenable a bypass.
// dirty_mask marks routes just switched ON; they must be restored from the
// patch without a costly full sc_prep() or affecting any other DSP parameter.
static inline void syntechFilterEnv1Apply(sc_synth_t *s, uint8_t bypass_mask, uint8_t dirty_mask) {
  if (!s || !(bypass_mask | dirty_mask)) return;
  for (unsigned k=0;k<2;k++) {
    const uint8_t bit=(uint8_t)(1u<<k);
    if (bypass_mask & bit) {
      s->layer[k].prep.kc_env1=0;
    } else if (dirty_mask & bit) {
      // Exact sc_prep() formula, Q16 semitones. Do not touch patch.mm[]!
      s->layer[k].prep.kc_env1=96*s->patch.layer[k].mm[SC_MM_E1_CUT];
    }
  }
}
