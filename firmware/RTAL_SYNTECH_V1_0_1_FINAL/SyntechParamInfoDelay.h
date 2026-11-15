// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// FIX7: A popup is permitted ONLY during a continuous 1500 ms label
// press that started on the parameter-label hitbox. Release cancels it.
// Slider activity cancels it, with a short guard against drag crossover.
static constexpr uint32_t SYNTECH_PARAM_INFO_HOLD_MS=1500u;
static constexpr uint32_t SYNTECH_PARAM_INFO_SLIDER_GUARD_MS=300u;
static constexpr uint32_t SYNTECH_PARAM_INFO_RELEASE_GRACE_MS=250u;
struct SyntechParamInfoGate {
  int8_t slot=-1;
  uint8_t id=255, page=255, layer=255;
  int16_t preset=-1;
  uint32_t pressedMs=0, lastSliderMs=0;
  bool sliderSeen=false;
  bool pressed=false;
};
static inline void syntechParamInfoCancel(SyntechParamInfoGate &g) {
  g.slot=-1;
  g.pressed=false;
}
static inline void syntechParamInfoSliderActivity(SyntechParamInfoGate &g,uint32_t now) {
  g.lastSliderMs=now;
  g.sliderSeen=true;
  syntechParamInfoCancel(g);
}
static inline bool syntechParamInfoPress(SyntechParamInfoGate &g,int slot,
  uint8_t id,uint8_t page,uint8_t layer,int preset,uint32_t now) {
  if(slot<0 || slot>=6 || id==255 ||
     (g.sliderSeen && (uint32_t)(now-g.lastSliderMs)<SYNTECH_PARAM_INFO_SLIDER_GUARD_MS)) {
    syntechParamInfoCancel(g);return false;
  }
  g.slot=(int8_t)slot;g.id=id;g.page=page;g.layer=layer;
  g.preset=(int16_t)preset;g.pressedMs=now;g.pressed=true;
  return true;
}
static inline bool syntechParamInfoReady(const SyntechParamInfoGate &g,uint32_t now) {
  return g.slot>=0 && g.pressed &&
         (uint32_t)(now-g.pressedMs)>=SYNTECH_PARAM_INFO_HOLD_MS;
}

// Same event-gap grace as the earlier MIDI Learn label hold: a single
// GT911 RELEASED glitch must not restart the 1.5-second clock.
static inline bool syntechParamInfoEventFresh(uint32_t now,uint32_t lastEventMs) {
  return (uint32_t)(now-lastEventMs)<=SYNTECH_PARAM_INFO_RELEASE_GRACE_MS;
}
static inline bool syntechParamInfoHeldReady(const SyntechParamInfoGate &g,
  uint32_t now,uint32_t lastEventMs,bool physicalPressed) {
  return physicalPressed && syntechParamInfoEventFresh(now,lastEventMs) &&
         syntechParamInfoReady(g,now);
}

// Physical gesture geometry. Origin must lie in the LABEL hit rectangle,
// and remain stationary; a slider-origin drag can never arm the gate.
static inline bool syntechParamInfoInRect(int16_t x,int16_t y,
  int16_t x1,int16_t y1,int16_t x2,int16_t y2) {
  return x>=x1 && x<=x2 && y>=y1 && y<=y2;
}
static inline bool syntechParamInfoStationary(int16_t x0,int16_t y0,
  int16_t x,int16_t y) {
  const int dx=(int)x-(int)x0,dy=(int)y-(int)y0;
  return dx*dx+dy*dy<=18*18;
}
