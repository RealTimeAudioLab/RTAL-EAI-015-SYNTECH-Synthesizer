// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "sc_fixed.h"

// Bipolar GUI route for SC_MM_E1_CUT. 64 is exactly neutral (zero modulation).
// Negative side has 64 increments, positive side 63 increments.
// Existing factory/SD values are read unchanged; this is only the GUI mapping.
static inline int32_t syntechEnv1CutFromSlider(int raw) {
  if(raw<0)raw=0;
  if(raw>127)raw=127;
  const int divisor=(raw<64)?64:63;
  return sc_clamp((int32_t)(((int64_t)(raw-64)*SC_ONE16)/divisor),
                  -SC_ONE16,SC_ONE16);
}
static inline int syntechEnv1CutToSlider(int32_t q) {
  q=sc_clamp(q,-SC_ONE16,SC_ONE16);
  const int scale=(q<0)?64:63;
  const int64_t num=(int64_t)q*scale;
  const int signedSteps=(int)((num>=0?num+SC_ONE16/2:num-SC_ONE16/2)/SC_ONE16);
  const int raw=64+signedSteps;
  return raw<0?0:(raw>127?127:raw);
}
