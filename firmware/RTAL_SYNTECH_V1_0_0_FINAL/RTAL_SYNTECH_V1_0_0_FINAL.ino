// RTAL SYNTECH Synthesizer V1.0
// SPDX-License-Identifier: GPL-3.0-only
// Derived from Scintilla by Joakim Eriksson (upstream engine imported separately).
// Arduino IDE 2.3.10 / esp32 core 3.3.11 / ESP32-S3 16MB flash, 8MB PSRAM.

#include "sc_profile.h"
#include <Arduino.h>
#if ARDUINO_USB_MODE
#error "RTAL SYNTECH USB MIDI Device requires Tools > USB Mode > USB-OTG (TinyUSB), not Hardware CDC/JTAG."
#endif
#include "USB.h"
#include "USBMIDI.h"
#include <Wire.h>
#include "Board.h"
#include "SyntechSplash.h"
#include <lvgl.h>
#include <cstring>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h> // FILTER DIAGNOSTICS: Hz conversion only on Core0
#include <esp_system.h>
#include <driver/i2s_std.h>
#include "SyntechCommands.h" // declare command types before Arduino-generated function prototypes
#include "SyntechFilterDiag.h" // types visible before Arduino-generated prototypes
#include "SyntechMidiCC.h"
#include <Preferences.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "sc_synth.h"
#include "sc_fixed.h"
#ifdef __cplusplus
}
#endif
#include "SyntechFilterIsolation.h" // testable, audio-task-only temporary route override
#include "SyntechFilterResonance.h" // FIX3/4: testable LP24 Q curve A/B helper
#include "SyntechEnv1CutControl.h" // FIX4: dedicated ENV1 -> CUTOFF GUI route

static constexpr int PIN_BCLK=16, PIN_LRCK=15, PIN_DOUT=7;
static constexpr int FRAMES=256;
static constexpr uint32_t RTAL_AUDIO_SAMPLE_RATE=32000;
static constexpr uint8_t RTAL_AUDIO_SLOT_BITS=32;
static constexpr uint8_t RTAL_AUDIO_CHANNELS=2;
static_assert(SC_FS == RTAL_AUDIO_SAMPLE_RATE, "Scintilla DSP and I2S must have identical sample rates");
static i2s_chan_handle_t tx = nullptr;
static sc_synth_t *synth = nullptr;
static QueueHandle_t cmdq;
static volatile uint32_t avg_us=0, max_us=0, xrun=0, block_count=0;
static volatile int current_preset=0;
static volatile uint32_t last_pcm_peak=0, last_pcm_clipped=0;
// STAGE2 diagnostics: counters only; no serial output from the audio task.
static volatile uint32_t diag_pcm_clipped_total=0, diag_pcm_clipped_blocks=0;
static volatile uint32_t diag_audio_total_peak=0, diag_audio_total_avg=0;
static volatile uint32_t diag_audio_core=255, diag_gui_core=255;
static volatile uint32_t diag_gui_max_gap_ms=0, diag_gui_max_handler_us=0;
static volatile uint32_t diag_gui_calls=0;

static HardwareSerial midi(1);
static USBMIDI usbMidi("RTAL_SYNTECH");
static volatile uint32_t usbMidiRxPackets=0;
static volatile uint32_t usbMidiRxInvalid=0;
static constexpr int MIDI_RX_PIN=17;
static constexpr int MIDI_TX_PIN=18;
static bool midiEnabled=true;
static uint8_t midiCCBank=0; // MOPHO parameter bank, 0..2
static uint8_t midiCCMode=0; // 0=MOPHO (factory default), 1=STANDARD
static uint8_t midiChannel=1; // 1..16, persisted in NVS
static Preferences midiPreferences;
// STAGE3: Independent, persistent learned CC maps. DSP and preset format unchanged.
static uint8_t learnedMophoCC[SYNTECH_CC_MAP_COUNT];
static uint8_t learnedStandardCC[SYNTECH_CC_MAP_COUNT];

// STAGE6: UI-origin MIDI TX only. RX callbacks never enqueue outgoing CC.
struct MidiTxCC { uint8_t cc, value, bank, mode, channel; };
static QueueHandle_t midiTxQueue=nullptr;
static volatile uint32_t midiTxDropped=0, midiTxSent=0;
static void queueUiMidiCC(uint8_t command, uint8_t id, uint8_t value) {
  if(!midiTxQueue)return;
  for(unsigned i=0;i<SYNTECH_CC_MAP_COUNT;i++) {
    const SyntechCCMap &m=SYNTECH_CC_MAP[i];
    if(m.command!=command || (command==SYN_CC_EDIT && m.id!=id))continue;
    const MidiTxCC item={midiCCMode?learnedStandardCC[i]:learnedMophoCC[i],
                         value,m.bank,midiCCMode,midiChannel};
    if(xQueueSend(midiTxQueue,&item,0)!=pdTRUE)++midiTxDropped;
    return;
  }
}
static void midiTxSendRaw(uint8_t channel,uint8_t cc,uint8_t value) {
  const uint8_t status=(uint8_t)(0xB0|((channel-1)&0x0F));
  const uint8_t bytes[3]={status,cc,value};
  midi.write(bytes,3);
  midiEventPacket_t packet={0x0B,status,cc,value};
  usbMidi.writePacket(&packet);
  ++midiTxSent;
}
static void midiTxDrain() {
  if(!midiTxQueue)return;
  MidiTxCC item;
  // Always transmit CC32 before each MOPHO CC: external bank state is unknown.
  for(int n=0;n<8 && xQueueReceive(midiTxQueue,&item,0)==pdTRUE;n++) {
    if(item.mode==0)midiTxSendRaw(item.channel,32,item.bank);
    midiTxSendRaw(item.channel,item.cc,item.value);
  }
}

static int16_t midiLearnIndex=-1;
static uint8_t midiLearnMode=0;
static bool midiCCLearnReserved(uint8_t cc) {
  return cc==0 || cc==32 || cc==64 || cc==120 || cc==121 || cc==123;
}
static void midiLearnDefaults(uint8_t mode) {
  for(unsigned i=0;i<SYNTECH_CC_MAP_COUNT;i++) {
    if(mode==0) learnedMophoCC[i]=SYNTECH_CC_MAP[i].cc;
    else learnedStandardCC[i]=SYNTECH_STANDARD_CC[i];
  }
}
static void midiLearnSave(uint8_t mode) {
  if(mode==0) midiPreferences.putBytes("mapm",learnedMophoCC,sizeof(learnedMophoCC));
  else midiPreferences.putBytes("maps",learnedStandardCC,sizeof(learnedStandardCC));
}
static bool midiLearnMapValid(uint8_t mode) {
  const uint8_t *a=mode==0?learnedMophoCC:learnedStandardCC;
  for(unsigned i=0;i<SYNTECH_CC_MAP_COUNT;i++) {
    if(midiCCLearnReserved(a[i]))return false;
    for(unsigned j=0;j<i;j++) {
      if(a[i]==a[j] && (mode==1 || SYNTECH_CC_MAP[i].bank==SYNTECH_CC_MAP[j].bank))return false;
    }
  }
  return true;
}
static void midiLearnLoad() {
  midiLearnDefaults(0); midiLearnDefaults(1);
  if(midiPreferences.getBytesLength("mapm")==sizeof(learnedMophoCC)) {
    midiPreferences.getBytes("mapm",learnedMophoCC,sizeof(learnedMophoCC));
    if(!midiLearnMapValid(0)) {Serial.println("[MIDI LEARN] invalid MOPHO NVS map: defaults restored");midiLearnDefaults(0);}
  }
  if(midiPreferences.getBytesLength("maps")==sizeof(learnedStandardCC)) {
    midiPreferences.getBytes("maps",learnedStandardCC,sizeof(learnedStandardCC));
    if(!midiLearnMapValid(1)) {Serial.println("[MIDI LEARN] invalid STANDARD NVS map: defaults restored");midiLearnDefaults(1);}
  }
}
static void midiLearnAssign(uint8_t cc) {
  if(midiLearnIndex<0 || midiLearnIndex>=(int)SYNTECH_CC_MAP_COUNT)return;
  if(midiCCLearnReserved(cc)) {Serial.printf("[MIDI LEARN] CC%u reserved; waiting for another CC\n",cc);return;}
  uint8_t *a=midiLearnMode==0?learnedMophoCC:learnedStandardCC;
  const unsigned idx=(unsigned)midiLearnIndex;
  const uint8_t previous=a[idx];
  for(unsigned j=0;j<SYNTECH_CC_MAP_COUNT;j++) {
    if(j==idx || a[j]!=cc)continue;
    if(midiLearnMode==0 && SYNTECH_CC_MAP[j].bank!=SYNTECH_CC_MAP[idx].bank)continue;
    // Exchange mappings instead of silently losing the previous assignment.
    a[j]=previous;
    Serial.printf("[MIDI LEARN] conflict: swapped entry %u to CC%u\n",j,previous);
    break;
  }
  a[idx]=cc;
  if(!midiLearnMapValid(midiLearnMode)) {
    Serial.println("[MIDI LEARN] rejected invalid map; restoring factory defaults for this mode");
    midiLearnDefaults(midiLearnMode);
  } else {
    midiLearnSave(midiLearnMode);
    Serial.printf("[MIDI LEARN] saved %s entry %u bank %u -> CC%u\n",midiLearnMode?"STANDARD":"MOPHO",idx,SYNTECH_CC_MAP[idx].bank,cc);
  }
  midiLearnIndex=-1;
}

static volatile int active_layer=0; // 0=UPPER, 1=LOWER; changed only in audioTask
static volatile uint32_t dropped_cmds=0;
static volatile uint32_t partial_writes=0;
// The audio task is sole owner of synth->patch. Transfer whole patches through queues.
static QueueHandle_t patchOutQ=nullptr, patchInQ=nullptr;
static volatile uint32_t patchGeneration=0;
// FIX7B: Audio task publishes the effective mode/split; LVGL reads only
// these small snapshots, never synth->patch from the UI core.
static volatile int8_t publishedVoiceMode=SC_VM_SINGLE;
static volatile int8_t publishedSplit=60;
static volatile uint32_t modeGeneration=0;
static volatile bool userPatchActive=false;
static char userPatchName[32]=""; // read/written only by Arduino loop
static constexpr int USER_SLOTS=128;
// sc_patch.h order is P1, P2, CUT, PW1, PW2, AMP.
// Touch layout order deliberately differs: P1, P2, PW1, PW2, CUT, AMP.
// NEVER use SC_MM_Lx_P1 + route: it would exchange FILTER/PWM destinations.
static constexpr uint8_t LFO_MATRIX_ROUTE[2][6] = {
  {SC_MM_L1_P1, SC_MM_L1_P2, SC_MM_L1_PW1, SC_MM_L1_PW2, SC_MM_L1_CUT, SC_MM_L1_AMP},
  {SC_MM_L2_P1, SC_MM_L2_P2, SC_MM_L2_PW1, SC_MM_L2_PW2, SC_MM_L2_CUT, SC_MM_L2_AMP}
};

static int selectedSlot=0;
static volatile int requestedPreset=-1;  // set by MIDI/UI; SD I/O ONLY in Arduino loop
// Tag each request to distinguish real MIDI Program Change from LVGL/touch and Serial.
enum : uint8_t { PC_ORIGIN_BOOT=0, PC_ORIGIN_MIDI=1, PC_ORIGIN_TOUCH=2,
                 PC_ORIGIN_SERIAL=3, PC_ORIGIN_RESTORE=4, PC_ORIGIN_QUEUE=5 };
static volatile uint8_t requestedPresetOrigin=PC_ORIGIN_BOOT;
// PRESET AUTO-REPEAT: independent of the generic 240-ms button click guard.
// A PRESSED event starts one browse gesture. Only the physical GT911 release,
// stable for 80 ms, commits exactly one SD preset load; never load on repeats.
enum TouchPresetState : uint8_t { TOUCH_NAV_IDLE=0, TOUCH_NAV_HELD=1, TOUCH_NAV_COOLDOWN=2 };
static TouchPresetState touchNavState=TOUCH_NAV_IDLE;
static int8_t touchNavDirection=0;
static int touchNavTarget=0;
static bool touchNavCanceled=false;
static uint32_t touchNavPressMs=0;
static uint32_t touchNavLastStepMs=0;
static uint32_t touchNavCommitMs=0;
static constexpr uint32_t TOUCH_PRESET_RELEASE_MS=80;
static constexpr uint32_t TOUCH_PRESET_REARM_MS=120;
static uint32_t ignoredTouchPresetClicks=0;
static uint32_t touchNavSteps=0;
static uint32_t touchNavCommits=0;
static uint32_t touchNavCancels=0;
static uint32_t ignoredMidiPCDuplicates=0;
static uint32_t ignoredBankNavigationClicks=0;
static char currentPresetName[32]="Chunky Synth";
static uint32_t failedLoads=0;
static bool presetBankComplete=false;
static bool sdPresetReady=false;
static constexpr uint32_t PRESET_MAGIC=0x59535452; // RTSY (little endian)
static constexpr uint32_t PRESET_VERSION=2;
struct PresetHeader {
  uint32_t magic, version, payloadSize, crc;
  char name[32];
};
// 128 * 32 bytes = 4096 bytes. Names are cached on boot/after SAVE/LOAD.
// Browsing with PRESET < > never accesses the SD card or the DSP task.
static char presetNameCache[USER_SLOTS][32] = {};
static bool presetNameValid[USER_SLOTS] = {};
static void cachePresetName(int slot, const char *name) {
  if(slot<0 || slot>=USER_SLOTS || !name)return;
  snprintf(presetNameCache[slot],sizeof(presetNameCache[slot]),"%s",name);
  presetNameValid[slot]=true;
}

static uint32_t presetCRC(const uint8_t *data,size_t n) {
  uint32_t crc=0xFFFFFFFFu;
  for(size_t i=0;i<n;i++){crc^=data[i];for(int j=0;j<8;j++)crc=(crc>>1)^((crc&1u)?0xEDB88320u:0u);}
  return ~crc;
}
static String slotPath(int slot) {
  char path[64]; snprintf(path,sizeof(path),"/RTAL_SYNTECH/PRESETS/P%03d.rtal",slot);return String(path);
}
static String slotName(int slot) {
  if(!sdPresetReady)return "NO SD";
  if(slot<0 || slot>=USER_SLOTS)return "INVALID";
  return presetNameValid[slot]?String(presetNameCache[slot]):String("INVALID");
}
static bool sendCmd(CmdType t, int note=0, int32_t val=0);
// SD ONLY: no embedded factory bank, no seed/migration, no automatic writes.
// Version-2 .rtal format retained byte-for-byte for compatibility with FIX6.
static bool validatePresetFile(const String &path, char *verifiedName) {
  File f=SD.open(path,FILE_READ);
  if(!f)return false;
  PresetHeader h={}; sc_patch_t p={};
  bool ok=f.size()==(sizeof(h)+sizeof(p));
  ok=ok && f.read((uint8_t*)&h,sizeof(h))==sizeof(h);
  ok=ok && h.magic==PRESET_MAGIC && h.version==PRESET_VERSION && h.payloadSize==sizeof(p);
  ok=ok && f.read((uint8_t*)&p,sizeof(p))==sizeof(p);
  f.close();
  ok=ok && h.crc==presetCRC((const uint8_t*)&p,sizeof(p));
  if(ok && verifiedName) {
    h.name[31]=0;
    snprintf(verifiedName,32,"%s",h.name);
  }
  return ok;
}
static bool validatePresetFile(const String &path) {
  return validatePresetFile(path,nullptr);
}
static int auditPresetBank() {
  if(!sdPresetReady)return 0;
  int valid=0;
  for(int slot=0;slot<USER_SLOTS;slot++) {
    char name[32]={};
    if(validatePresetFile(slotPath(slot),name)) {
      valid++;
      cachePresetName(slot,name);
    } else {
      presetNameValid[slot]=false;
      Serial.printf("[SD ONLY] slot %03d missing/invalid (kept untouched)\n",slot);
    }
  }
  presetBankComplete=(valid==USER_SLOTS);
  Serial.printf("[SD ONLY] valid=%d/128 missing_or_invalid=%d; no files created/overwritten\n",valid,USER_SLOTS-valid);
  return valid;
}
// Read an existing valid SD preset before starting the audio engine.
// Never silently replace a missing/corrupt file with firmware factory data.
static bool readBootPatch(int slot, sc_patch_t &out) {
  String path=slotPath(slot);
  if(!validatePresetFile(path))return false;
  File f=SD.open(path,FILE_READ);
  if(!f)return false;
  PresetHeader h={};
  bool ok=f.read((uint8_t*)&h,sizeof(h))==sizeof(h);
  ok=ok && f.read((uint8_t*)&out,sizeof(out))==sizeof(out);
  f.close();
  if(!ok)return false;
  out.polyphony=6; // Retain original six-note main configuration.
  return true;
}

static bool requestPatch(sc_patch_t &dst) {
  sc_patch_t stale;
  while(xQueueReceive(patchOutQ,&stale,0)==pdTRUE) {}
  if(!sendCmd(SNAPSHOT_PATCH))return false;
  return xQueueReceive(patchOutQ,&dst,pdMS_TO_TICKS(80))==pdTRUE;
}
static bool writeSlot(int slot,const char *name) {
  if(!sdPresetReady || slot<0 || slot>=USER_SLOTS)return false;
  sc_patch_t patch;
  if(!requestPatch(patch))return false;
  PresetHeader h={};h.magic=PRESET_MAGIC;h.version=PRESET_VERSION;h.payloadSize=sizeof(patch);
  snprintf(h.name,sizeof(h.name),"%s",name);
  h.crc=presetCRC((const uint8_t*)&patch,sizeof(patch));
  String path=slotPath(slot), tmp=path+".TMP";
  SD.remove(tmp);
  File f=SD.open(tmp,FILE_WRITE);if(!f)return false;
  bool ok=(f.write((const uint8_t*)&h,sizeof(h))==sizeof(h));
  ok=ok&&(f.write((const uint8_t*)&patch,sizeof(patch))==sizeof(patch));
  f.flush();f.close();
  if(!ok){SD.remove(tmp);return false;}
  String backup=path+".BAK";
  SD.remove(backup);
  const bool hadOld=SD.exists(path);
  if(hadOld && !SD.rename(path,backup)){SD.remove(tmp);return false;}
  if(!SD.rename(tmp,path)){
    if(hadOld)SD.rename(backup,path);
    SD.remove(tmp);return false;
  }
  if(hadOld)SD.remove(backup);
  cachePresetName(slot,name);
  return true;
}
static bool readSlot(int slot,char *nameOut) {
  if(!sdPresetReady || slot<0 || slot>=USER_SLOTS)return false;
  File f=SD.open(slotPath(slot),FILE_READ);if(!f)return false;
  PresetHeader h={};sc_patch_t patch;
  bool ok=f.size()==(sizeof(h)+sizeof(patch));
  ok=ok && f.read((uint8_t*)&h,sizeof(h))==sizeof(h);
  ok=ok&&h.magic==PRESET_MAGIC&&h.version==PRESET_VERSION&&h.payloadSize==sizeof(patch);
  ok=ok&&(f.read((uint8_t*)&patch,sizeof(patch))==sizeof(patch));f.close();
  ok=ok&&(presetCRC((const uint8_t*)&patch,sizeof(patch))==h.crc);
  if(!ok)return false;
  patch.polyphony=6; // instrument invariant
  if(xQueueSend(patchInQ,&patch,0)!=pdTRUE)return false;
  if(!sendCmd(APPLY_USER_PATCH)) { sc_patch_t discarded; xQueueReceive(patchInQ,&discarded,0);return false; }
  h.name[31]=0;
  snprintf(nameOut,32,"%s",h.name);
  cachePresetName(slot,nameOut);
  return true;
}




static bool sendCmd(CmdType t, int note, int32_t val) {
  Cmd c{t, (uint8_t)note, val};
  if (xQueueSend(cmdq,&c,0)==pdTRUE) return true;
  dropped_cmds++;
  return false;
}

// STAGE4: distinguish command/preparation cost from pure render cost.
static volatile uint32_t diag_cmd_peak_us=0, diag_cmd_last_us=0;
static volatile uint32_t diag_render_peak_us=0, diag_render_last_us=0;
static volatile uint32_t diag_cmd_batch_peak=0, diag_reprep_batches=0;
// FIX3: bound control work per audio block; no audio-core SD/GUI operations added.
static constexpr uint32_t RTAL_MAX_COMMANDS_PER_BLOCK=12;
static volatile uint32_t diag_cmd_deferred_blocks=0, diag_cmd_pending_peak=0;
static volatile uint32_t diag_audio_compute_last=0, diag_audio_compute_peak=0;
// FIX7H: lock-free diagnostic counters. AudioTask is the only writer.
// The GUI reads these every two seconds; no Serial output or locks on Core1.
static volatile uint32_t h_cmd_over=0, h_render_over=0, h_compute_over=0;
static volatile uint32_t h_i2s_fail=0, h_late_start=0;
static volatile uint32_t h_cmd_max=0, h_render_max=0, h_pcm_max=0;
static volatile uint32_t h_compute_max=0, h_write_max=0, h_gap_max=0;
static volatile uint32_t h_cmd_count=0, h_reprep_count=0;
static volatile uint32_t h_last_cmd=0, h_last_render=0, h_last_pcm=0;
static volatile uint32_t h_last_compute=0, h_last_write=0, h_last_gap=0;
// FIX7J: causal, per-block WCET classification. Counters are cumulative.
// A parameter-prep block is identified separately from a command-only block.
static volatile uint32_t j_prep_us_last=0, j_prep_us_peak=0, j_prep_blocks=0;
static volatile uint32_t j_render_over_prep=0, j_render_over_no_prep=0;
static volatile uint32_t j_render_over_cmd=0, j_render_over_idle=0;
static volatile uint32_t j_render_peak_prep=0, j_render_peak_no_prep=0;
static volatile uint32_t j_cmd_peak_prep=0, j_cmd_peak_no_prep=0;

// FIX7K: AudioTask writes, GUI task only reads diagnostic snapshots.
static volatile uint32_t k_profiled_blocks=0, k_profiled_over=0;
static volatile uint32_t k_worst_render_us=0, k_worst_voices=0, k_worst_had_prep=0;
static volatile uint32_t k_component_peak_cycles[7]={0};
// FIX8B: General path is forced by CROSS2; sampled block correlation only.
static volatile uint32_t b_worst_general_cycles=0, b_peak_general_cycles=0;
static volatile uint32_t b_worst_general_voices=0, b_worst_cross2_voices=0;
static volatile uint32_t b_profiled_general_blocks=0, b_profiled_cross2_blocks=0;
static volatile uint32_t b_profiled_general_over=0, b_profiled_cross2_over=0;
static volatile uint32_t b_worst_general_render_us=0;
static volatile uint32_t k_worst_components[7]={0};

// V0.9A FIX1: One-shot, READ-ONLY snapshots captured by the Core1 audio owner.
// No extra operations inside sc_voice_render()/update_coeffs(); no Serial on Core1.
// The GUI receives a copy over a length-1 FreeRTOS queue, never reads synth state.
static QueueHandle_t filterDiagQ=nullptr;
static bool filterDiagPeriodic=false; // Core0 GUI task only
static uint32_t filterDiagLastRequestMs=0;
// FIX2: audio-task-owned, transient diagnostic bypass (bit0=UPPER, bit1=LOWER).
// Defaults to OFF; reset on preset load and reboot; never stored in SD/NVS.
static uint8_t filterEnv1BypassMask=0;
// FIX4: MUSICAL is now the standard LP24 resonance curve for BOTH layers.
// ORIGINAL remains a transient A/B option; every new preset restores MUSICAL.
// No preset-format change and no edits to sc_voice.c/sc_synth.c.
static uint8_t filterResoMusicalMask=3u;
static SyntechResoCache filterResoCache={{-1,-1},{0,0}};

// Capture after sc_render(), so coeff_last_cutoff is the actual most-recent
// coefficient-cache key of a rendered voice, not a GUI-derived prediction.
static void filterDiagCaptureOnAudioTask() {
  if (!filterDiagQ || !synth) return;
  FilterDiagSnapshot d={};
  d.ms=(uint32_t)(esp_timer_get_time()/1000);
  d.activeLayer=(int8_t)active_layer;
  d.voiceMode=synth->patch.voice_mode;
  d.chorusOn=synth->patch.fx.chorus.on;
  d.reverbOn=synth->patch.fx.reverb.on;
  d.joyXQ16=synth->perf.joy_x;
  d.joyYQ16=synth->perf.joy_y;
  d.bendFiltQ16=synth->perf.bend_filt;
  d.lfo2FiltQ16=synth->perf.lfo2_filt;
  for(int k=0;k<2;k++) {
    const sc_layer_t &L=synth->patch.layer[k];
    const sc_layer_state_t &S=synth->layer[k];
    const sc_prep_t &P=S.prep;
    FilterDiagLayerSnapshot &D=d.layer[k];
    D.mode=L.flt_mode;
    D.cutoffQ16=L.cutoff; D.resoQ16=L.reso;
    D.envAmountQ16=L.env_amt; D.keyTrackQ16=L.key_track;
    D.baseQ16=P.c_base; D.kc1Q16=P.kc[0]; D.kc2Q16=P.kc[1];
    D.kcEnvQ16=P.kc_env; D.kcEnv1Q16=P.kc_env1;
    D.kcVelQ16=P.kc_vel; D.kcKeyQ16=P.kc_key;
    D.kQ28=P.k_q28; D.attenQ16=P.atten;
    D.lfoDepthB1Q16=L.lfo[0].depth_b;
    D.lfoDepthB2Q16=L.lfo[1].depth_b;
    D.mmL1CutQ16=L.mm[SC_MM_L1_CUT];
    D.mmL2CutQ16=L.mm[SC_MM_L2_CUT];
    D.mmE1CutQ16=L.mm[SC_MM_E1_CUT];
    D.mmVelCutQ16=L.mm[SC_MM_VEL_CUT];
    D.mmJoyCutQ16=L.mm[SC_MM_JY_CUT];
    D.env1CutBypassed=(filterEnv1BypassMask & (1u<<k))?1:0;
    D.lp24Musical=(filterResoMusicalMask & (1u<<k))?1:0;
    int chosen=-1;
    for(int i=0;i<SC_VOICES;i++) {
      if(sc_voice_idle(&S.v[i]))continue;
      D.activeVoices++;
      const bool held=(S.released[i]==0xFFFFFFFFu);
      if(held)D.heldVoices++;
      if(chosen<0 || (held && S.released[chosen]!=0xFFFFFFFFu))chosen=i;
    }
    D.voice.slot=-1; D.voice.note=-1;
    if(chosen>=0) {
      const sc_voice_t &V=S.v[chosen];
      D.voice.slot=(int8_t)chosen;
      D.voice.note=S.note[chosen];
      D.voice.held=(S.released[chosen]==0xFFFFFFFFu);
      D.voice.stage=(uint8_t)V.env2.stage;
      D.voice.coeffValid=V.coeff_cache_valid;
      D.voice.cutoffNoteQ16=V.coeff_last_cutoff;
      D.voice.currentNoteQ16=(int32_t)(V.cur>>16);
      D.voice.envLevelQ30=V.env1.level;
      D.voice.lfo1Q20=V.l1; D.voice.lfo2Q20=V.l2;
      D.voice.velQ16=V.vel;
      D.voice.a1Q30=V.a1; D.voice.a2Q30=V.a2; D.voice.a3Q30=V.a3;
      D.voice.cachedKQ28=V.coeff_last_k;
    }
  }
  xQueueOverwrite(filterDiagQ,&d); // nonblocking, never wait in AudioTask
}

static void audioTask(void *) {
  diag_audio_core=(uint32_t)xPortGetCoreID();
  static int32_t stereo[2*FRAMES];
  static int32_t output[2*FRAMES];
  const int64_t budget=(int64_t)FRAMES*1000000/SC_FS;
  uint64_t total=0; uint32_t peak=0, blocks=0;
  int64_t lastBlockStart=0;
  bool filterDiagRequested=false; // AudioTask-local; set only by queued FILTER_DIAG
  while(true) {
    const int64_t cmdStart=esp_timer_get_time();
    if(lastBlockStart) {
      const uint32_t gap=(uint32_t)(cmdStart-lastBlockStart);
      h_last_gap=gap;
      if(gap>h_gap_max)h_gap_max=gap;
      if(gap>(uint32_t)budget+250)++h_late_start;
    }
    lastBlockStart=cmdStart;
    uint32_t cmdBatch=0;
    uint8_t layerPrepMask=0;
    uint8_t filterEnv1DirtyMask=0; // route switch; restore original kc_env1 on ON
    bool needReverbPrep=false;
    Cmd c;
    while(cmdBatch<RTAL_MAX_COMMANDS_PER_BLOCK && xQueueReceive(cmdq,&c,0)==pdTRUE) {
      ++cmdBatch;
      switch(c.type) {
        case FILTER_DIAG: filterDiagRequested=true; break;
        case FILTER_RESO_AB: {
          // note: 0 UPPER, 1 LOWER, 2 BOTH; val: 0 ORIGINAL, 1 MUSICAL.
          if(c.note<=2 && (c.val==0 || c.val==1)) {
            const uint8_t mask=(c.note==2)?3u:(uint8_t)(1u<<c.note);
            if(c.val==1)filterResoMusicalMask|=mask;
            else filterResoMusicalMask&=(uint8_t)~mask;
            // Restore ORIGINAL via the exact unchanged sc_prep() when turning off.
            // On MUSICAL the helper overrides only LP24 after preparation.
            layerPrepMask|=mask;
          }
          break;
        }
        case FILTER_ENV1_ROUTE: {
          // note: 0=UPPER, 1=LOWER, 2=BOTH; val: 0=OFF/bypass, 1=ON/original.
          // Only the Core1 audio owner modifies the bypass state.
          if(c.note<=2 && (c.val==0 || c.val==1)) {
            const uint8_t mask=(c.note==2)?3u:(uint8_t)(1u<<c.note);
            if(c.val==0) filterEnv1BypassMask|=mask;
            else filterEnv1BypassMask&=(uint8_t)~mask;
            filterEnv1DirtyMask|=mask;
          }
          break;
        }
        case SNAPSHOT_PATCH: {
          sc_patch_t snapshot=synth->patch;
          xQueueSend(patchOutQ,&snapshot,0);break;
        }
        case APPLY_USER_PATCH: {
          sc_patch_t incoming;
          if(xQueueReceive(patchInQ,&incoming,0)==pdTRUE) {
            sc_all_off(synth);synth->patch=incoming;synth->patch.polyphony=6;
            sc_reprep(synth);userPatchActive=true;
            // Temporary diagnostic isolation must never silently survive a preset load.
            filterEnv1BypassMask=0;
            filterEnv1DirtyMask=0;
            filterResoMusicalMask=3u; // FIX4: every preset uses MUSICAL LP24 by default
            publishedVoiceMode=synth->patch.voice_mode;
            publishedSplit=synth->patch.split;
            modeGeneration++;
            patchGeneration++;
          }
          break;
        }
        case EDIT_PARAM: {
          // parameter ID in note; edit always executes on the audio core.
          sc_layer_t &p=synth->patch.layer[active_layer];
          const int32_t q=(int32_t)(((int64_t)c.val*65536)/127);
          const int32_t t=(int32_t)(((int64_t)c.val*65536)/16); // 0..8 seconds
          switch(c.note) {
            case 0:p.o1_wave=(int8_t)(c.val*5/128);break;
            case 1:p.o2_wave=(int8_t)(c.val*5/128);break;
            case 2:p.o1_pwm=q;break;
            case 3:p.o2_pwm=q;break;
            case 4:p.o2_detune=(int32_t)(((int64_t)(c.val-64)*65536*100)/64);break;
            case 5:p.mix_o1=q;break;
            case 6:p.mix_o2=q;break;
            case 7:p.mix_noise=q;break;
            case 8:p.cross=q;break;
            case 9:p.cross2=q;break;
            case 10:p.env_amt=(int32_t)(((int64_t)c.val*65536*4)/127);break; // 0..4 Q16; preserve legacy values >1.0
            case 11:p.key_track=q;break;
            case 12:p.env_f.a=t;break;
            case 13:p.env_f.d=t;break;
            case 14:p.env_f.s=q;break;
            case 15:p.env_f.r=t;break;
            case 16:p.env_a.a=t;break;
            case 17:p.env_a.d=t;break;
            case 18:p.env_a.s=q;break;
            case 19:p.env_a.r=t;break;
            case 20:p.lfo[0].rate=(int32_t)(((int64_t)c.val*65536*20)/127);break;
            case 21:p.lfo[0].depth_a=q;break;
            case 22:p.lfo[0].shape=(int8_t)(c.val*6/128);break;
            case 23:p.lfo[1].rate=(int32_t)(((int64_t)c.val*65536*20)/127);break;
            case 24:p.lfo[1].depth_a=q;break;
            case 25:p.lfo[1].shape=(int8_t)(c.val*6/128);break;
            case 26:synth->patch.fx.chorus.mix=q;break;
            case 27:synth->patch.fx.chorus.rate=(int32_t)(((int64_t)c.val*65536*10)/127);break;
            case 28:synth->patch.fx.chorus.depth=q;break;
            case 29:synth->patch.fx.reverb.mix=q;break;
            case 30:synth->patch.fx.reverb.size=q;break;
            case 31:synth->patch.fx.reverb.damping=q;break;
            case 32:p.pan=(int32_t)(((int64_t)(c.val-64)*65536)/64);break;
            case 33:p.noise_pink=(c.val>=64);break;
            case 38:p.o2_sync=(c.val>=64);break;
            case 39:p.ring=(c.val>=64);break;
            // LFO matrix: 6 independent routes per LFO, bipolar -1..+1 Q16.
            // Depth A modulates oscillator pitch/PWM; depth B filter/amplitude.
            case 40: case 41: case 42: case 43: case 44: case 45:
            case 46: case 47: case 48: case 49: case 50: case 51: {
              const int lfo=(c.note-40)/6, route=(c.note-40)%6;
              const int mmIndex=LFO_MATRIX_ROUTE[lfo][route];
              p.mm[mmIndex]=(int32_t)(((int64_t)(c.val-64)*65536)/63);
              if(p.mm[mmIndex]>65536)p.mm[mmIndex]=65536;
              if(p.mm[mmIndex]<-65536)p.mm[mmIndex]=-65536;
              break;
            }
            case 58: { // FIX4: dedicated bipolar ENV1 -> CUTOFF route (64 = neutral)
              p.mm[SC_MM_E1_CUT]=syntechEnv1CutFromSlider(c.val);
              // An explicit user edit re-enables this route even if the FIX2
              // temporary diagnostic bypass was previously active.
              const uint8_t bit=(uint8_t)(1u<<active_layer);
              filterEnv1BypassMask&=(uint8_t)~bit;
              filterEnv1DirtyMask|=bit;
              break;
            }
            case 52:p.lfo[0].depth_b=q;break;
            case 53:p.lfo[1].depth_b=q;break;
            case 54:p.lfo[0].delay=t;break;
            case 55:p.lfo[1].delay=t;break;
            case 56:p.lfo[0].sync=(c.val>=64);break;
            case 57:p.lfo[1].sync=(c.val>=64);break;
          }
          if(c.note==30 || c.note==31) needReverbPrep=true;
          else if(c.note<26 || c.note==32 || c.note==33 || c.note==38 || c.note==39 || (c.note>=40 && c.note<=58)) layerPrepMask |= (uint8_t)(1u << active_layer);
          // Chorus/reverb mix and other direct FX parameters do not require layer preparation.
          break;
        }
        case NOTE_ON: sc_note_on(synth,c.note,c.val); break;
        case NOTE_OFF: sc_note_off(synth,c.note); break;
        case ALL_OFF: sc_all_off(synth); break;
        case BEND: sc_pitch_bend(synth,c.val); break;
        case PRESET:  // all PC requests must use SD, on Core0
          if(c.val>=0 && c.val<USER_SLOTS) {requestedPresetOrigin=PC_ORIGIN_QUEUE; requestedPreset=c.val;}
          break;
        case BENCH:
          sc_all_off(synth);
          if(c.val>=1 && c.val<=6)
            for(int n=0;n<c.val;n++) sc_note_on(synth, 48+n*2, 50000);
          break;
        case RESTORE: requestedPresetOrigin=PC_ORIGIN_RESTORE; requestedPreset=current_preset; break;
        case SELECT_LAYER:
          if(c.val==0 || c.val==1) active_layer=c.val;
          break;
        case MODE:
          if(c.val>=SC_VM_SINGLE && c.val<=SC_VM_DOUBLE) {
            sc_all_off(synth);
            synth->patch.voice_mode=(int8_t)c.val;
            sc_reprep(synth);
            publishedVoiceMode=synth->patch.voice_mode;
            modeGeneration++;
          }
          break;
        case SPLIT:
          if(c.val>=0 && c.val<=127) { sc_all_off(synth); synth->patch.split=(int8_t)c.val;
            publishedSplit=synth->patch.split;
            modeGeneration++; }
          break;
        case POLY:
          if(c.val>=1 && c.val<=6) { sc_all_off(synth); synth->patch.polyphony=(int8_t)c.val; }
          break;
        case CHORUS:
          if(c.val==0 || c.val==1) synth->patch.fx.chorus.on=(int8_t)c.val;
          break;
        case REVERB:
          if(c.val==0 || c.val==1) { synth->patch.fx.reverb.on=(int8_t)c.val; sc_reprep(synth); }
          break;
        case FILTER_MODE:
          if(c.val>=SC_FLT_LP24 && c.val<=SC_FLT_HP12) {
            synth->patch.layer[active_layer].flt_mode=(int8_t)c.val;
            sc_reprep(synth);
          }
          break;
        case CUTOFF:
          // 0..127 maps to the preset parameter's normalized 0..1 range in Q16.
          if(c.val>=0 && c.val<=127) {
            synth->patch.layer[active_layer].cutoff=(int32_t)(((int64_t)c.val*65536)/127);
            layerPrepMask |= (uint8_t)(1u << active_layer);
          }
          break;
        case RESO:
          if(c.val>=0 && c.val<=127) {
            synth->patch.layer[active_layer].reso=(int32_t)(((int64_t)c.val*65536)/127);
            layerPrepMask |= (uint8_t)(1u << active_layer);
          }
          break;
        case RING:
          if(c.val==0 || c.val==1) { synth->patch.layer[active_layer].ring=(int8_t)c.val; sc_reprep(synth); }
          break;
        case SYNC:
          if(c.val==0 || c.val==1) { synth->patch.layer[active_layer].o2_sync=(int8_t)c.val; sc_reprep(synth); }
          break;
      }
    }
    const bool jHadPrep = (layerPrepMask != 0 || needReverbPrep);
    const int64_t jPrepStart = jHadPrep ? esp_timer_get_time() : 0;
    if(layerPrepMask || needReverbPrep){
      // FIX7I: prepare only the changed layers/FX, once per audio block.
      // Preserve the original full sc_reprep() for patch/mode transitions.
      if(layerPrepMask & 1u) sc_prep(&synth->layer[0].prep, &synth->patch.layer[0], &synth->perf);
      if(layerPrepMask & 2u) sc_prep(&synth->layer[1].prep, &synth->patch.layer[1], &synth->perf);
      if(needReverbPrep) sc_reverb_prep(&synth->rev, &synth->patch.fx.reverb);
      ++diag_reprep_batches; ++h_reprep_count;
    }
    // FIX2: apply transient ENV1-cutoff isolation AFTER all prep work and
    // BEFORE rendering. No patch mutation; works across normal parameter edits.
    // The exact original coefficient is restored on ON without a full reprep.
    syntechFilterEnv1Apply(synth,filterEnv1BypassMask,filterEnv1DirtyMask);
    // FIX4: MUSICAL LP24 is default for both layers; ORIGINAL opt-in bypasses this helper.
    syntechFilterResoApply(synth,filterResoMusicalMask,&filterResoCache);
    if(jHadPrep) {
      const uint32_t prepUs=(uint32_t)(esp_timer_get_time()-jPrepStart);
      j_prep_us_last=prepUs;
      if(prepUs>j_prep_us_peak)j_prep_us_peak=prepUs;
      ++j_prep_blocks;
    }
    const uint32_t pending=(uint32_t)uxQueueMessagesWaiting(cmdq);
    if(pending){++diag_cmd_deferred_blocks;if(pending>diag_cmd_pending_peak)diag_cmd_pending_peak=pending;}
    const uint32_t cmdUs=(uint32_t)(esp_timer_get_time()-cmdStart);
    if(jHadPrep) { if(cmdUs>j_cmd_peak_prep)j_cmd_peak_prep=cmdUs; }
    else { if(cmdUs>j_cmd_peak_no_prep)j_cmd_peak_no_prep=cmdUs; }
    h_last_cmd=cmdUs;
    if(cmdUs>h_cmd_max)h_cmd_max=cmdUs;
    h_cmd_count+=cmdBatch;
    if(cmdUs>(uint32_t)budget)++h_cmd_over;
    diag_cmd_last_us=cmdUs;
    if(cmdUs>diag_cmd_peak_us)diag_cmd_peak_us=cmdUs;
    if(cmdBatch>diag_cmd_batch_peak)diag_cmd_batch_peak=cmdBatch;
    const int64_t blockStart=esp_timer_get_time();
    const int64_t t0=blockStart;
    // IMPORTANT: sc_render ADDS Q15 PCM into the destination (+=), it does not overwrite it.
    // Without clearing stereo every block, samples accumulate and become distorted.
    memset(stereo, 0, sizeof(stereo));
    sc_render(synth,stereo,FRAMES);
    const uint32_t elapsed=(uint32_t)(esp_timer_get_time()-t0);
    if(jHadPrep) {
      if(elapsed>j_render_peak_prep)j_render_peak_prep=elapsed;
      if(elapsed>(uint32_t)budget)++j_render_over_prep;
    } else {
      if(elapsed>j_render_peak_no_prep)j_render_peak_no_prep=elapsed;
      if(elapsed>(uint32_t)budget)++j_render_over_no_prep;
    }
    if(elapsed>(uint32_t)budget) {
      if(cmdBatch) ++j_render_over_cmd; else ++j_render_over_idle;
    }
    // FIX7K: correlate component costs with the SAME profiled over-budget block.
    // Sampling is 1/8 to limit measurement overhead. No Serial in AudioTask.
    static uint32_t k_last_prof_seq=0;
    if(sc_prof.seq!=k_last_prof_seq) {
      k_last_prof_seq=sc_prof.seq;
      ++k_profiled_blocks;
      if(sc_prof.general_voices) { ++b_profiled_general_blocks; if(elapsed>(uint32_t)budget) ++b_profiled_general_over; }
      if(sc_prof.cross2_voices) { ++b_profiled_cross2_blocks; if(elapsed>(uint32_t)budget) ++b_profiled_cross2_over; }
      if(sc_prof.general>b_peak_general_cycles) b_peak_general_cycles=sc_prof.general;
      if(sc_prof.general_voices && elapsed>b_worst_general_render_us) {
        b_worst_general_render_us=elapsed; b_worst_general_cycles=sc_prof.general;
        b_worst_general_voices=sc_prof.general_voices; b_worst_cross2_voices=sc_prof.cross2_voices;
      }
      if(elapsed>(uint32_t)budget) {
        ++k_profiled_over;
        const uint32_t components[7]={sc_prof.control,sc_prof.osc1,sc_prof.osc2,
          sc_prof.mixfilter,sc_prof.chorus,sc_prof.reverb,sc_prof.master};
        for(int ki=0;ki<7;ki++) if(components[ki]>k_component_peak_cycles[ki])
          k_component_peak_cycles[ki]=components[ki];
        if(elapsed>k_worst_render_us) {
          k_worst_render_us=elapsed;
          k_worst_voices=sc_prof.voices;
          k_worst_had_prep=jHadPrep?1:0;
          for(int ki=0;ki<7;ki++) k_worst_components[ki]=components[ki];
        }
      }
    }
    h_last_render=elapsed;
    if(elapsed>h_render_max)h_render_max=elapsed;
    if(elapsed>(uint32_t)budget)++h_render_over;
    diag_render_last_us=elapsed;
    if(elapsed>diag_render_peak_us)diag_render_peak_us=elapsed;
    total+=elapsed;
    if(elapsed>peak) peak=elapsed;
    blocks++;
    if(elapsed>(uint32_t)budget) xrun++;
    const int64_t pcmStart=esp_timer_get_time();
    // Scintilla produces stereo signed Q15 in int32_t cells; I2S 32-bit slots.
    uint32_t pcmPeak=0, pcmClipped=0;
    for(int i=0;i<2*FRAMES;i++) {
      int32_t v=stereo[i];
      uint32_t mag=(v<0)?(uint32_t)(-(int64_t)v):(uint32_t)v;
      if(mag>pcmPeak) pcmPeak=mag;
      if(v>32767 || v< -32768) pcmClipped++;
      if(v>32767) v=32767;
      if(v< -32768) v=-32768;
      output[i]=(int32_t)(v * 65536); // signed conversion without left-shifting negative values
    }
    last_pcm_peak=pcmPeak; last_pcm_clipped=pcmClipped;
    if(pcmClipped){diag_pcm_clipped_blocks++;diag_pcm_clipped_total+=pcmClipped;}
    const uint32_t pcmUs=(uint32_t)(esp_timer_get_time()-pcmStart);
    h_last_pcm=pcmUs;
    if(pcmUs>h_pcm_max)h_pcm_max=pcmUs;
    const uint32_t computeUs=(uint32_t)(esp_timer_get_time()-cmdStart);
    h_last_compute=computeUs;
    if(computeUs>h_compute_max)h_compute_max=computeUs;
    if(computeUs>(uint32_t)budget)++h_compute_over;
    diag_audio_compute_last=computeUs;
    if(computeUs>diag_audio_compute_peak)diag_audio_compute_peak=computeUs;
    size_t written=0;
    const int64_t writeStart=esp_timer_get_time();
    esp_err_t e=i2s_channel_write(tx,output,sizeof(output),&written,portMAX_DELAY);
    const uint32_t writeUs=(uint32_t)(esp_timer_get_time()-writeStart);
    h_last_write=writeUs;
    if(writeUs>h_write_max)h_write_max=writeUs;
    if(e!=ESP_OK || written!=sizeof(output)) { xrun++; partial_writes++; ++h_i2s_fail; }
    // Includes DSP, PCM conversion and time waiting for the I2S driver.
    const uint32_t fullUs=(uint32_t)(esp_timer_get_time()-blockStart);
    if(fullUs>diag_audio_total_peak) diag_audio_total_peak=fullUs;
    diag_audio_total_avg=fullUs;
    // Snapshot only when explicitly requested, AFTER I2S transfer. This does not
    // change the render or compute timing measurements above.
    if(filterDiagRequested) {filterDiagCaptureOnAudioTask();filterDiagRequested=false;}
    if(blocks>=256) {
      avg_us=(uint32_t)(total/blocks);
      max_us=peak;
      block_count+=blocks;
      total=0;peak=0;blocks=0;
    }
  }
}

static void i2sInit() {
  i2s_chan_config_t cfg=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
  cfg.dma_desc_num=4;
  cfg.dma_frame_num=FRAMES;
  ESP_ERROR_CHECK(i2s_new_channel(&cfg,&tx,nullptr));
  i2s_std_config_t stdcfg={};
  stdcfg.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(RTAL_AUDIO_SAMPLE_RATE);
  stdcfg.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_STEREO);
  stdcfg.gpio_cfg.mclk=I2S_GPIO_UNUSED;
  stdcfg.gpio_cfg.bclk=(gpio_num_t)PIN_BCLK;
  stdcfg.gpio_cfg.ws=(gpio_num_t)PIN_LRCK;
  stdcfg.gpio_cfg.dout=(gpio_num_t)PIN_DOUT;
  stdcfg.gpio_cfg.din=I2S_GPIO_UNUSED;
  stdcfg.gpio_cfg.invert_flags.mclk_inv=false;
  stdcfg.gpio_cfg.invert_flags.bclk_inv=false;
  stdcfg.gpio_cfg.invert_flags.ws_inv=false;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx,&stdcfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx));
}

// Minimal DIN-MIDI channel voice parser. System realtime bytes do not disrupt running status.
static int uiPreset=0,uiLayer=0,uiMode=0,uiPage=0;
static uint8_t uiLfo=0, uiLfoTab=0; // 0 MAIN, 1 MATRIX A, 2 MATRIX B
// MIDI DIN: running status, 1-based channel, banked CC, Program Change 0..127.
// Only Core0 parses MIDI. Audio parameters are always applied by queued commands on Core1.
static volatile bool midiUiDirty=false;
static volatile bool midiParamDirty=false;
// FIX7W: diagnostic isolation. MIDI CC continues to reach DSP, but does not refresh LVGL.
static volatile bool midiCcDisplayEnabled=true; // FIX7X default: optimized GUI ON
static volatile uint32_t midiCcMatchedCount=0;
// FIX7X: Audio path remains immediate; Core0 only renders the latest value.
static volatile uint32_t gui7xCoalesced=0;
static uint32_t gui7xWidgetChanges=0, gui7xPageBuilds=0;
static uint32_t gui7xLvglMaxUs=0;
static constexpr uint32_t GUI7X_CC_REFRESH_MS=50;  // max 20 Hz
static constexpr uint32_t GUI7X_PAGE_REFRESH_MS=100; // max 10 Hz

static uint8_t status=0, data1=0, have=0;
// Diagnostic only: log the real MIDI status bytes that precede program changes.
// Do not modify the MIDI routing, preset files or sound generation.
static uint32_t midiPcCount=0;
static uint32_t midiStatusC0Count=0;
static uint32_t midiNoteOnCount=0;
static uint32_t midiNoteOffCount=0;
static uint32_t midiPcTraceCount=0;
// Ignore only rapid repeated *identical* MIDI PCs, not different PC 0..127 events.
// The MIDI PC command always remains available, independent of CC32 parameter bank.
static uint8_t lastMidiPC=255;
static uint32_t lastMidiPCMs=0;
static constexpr uint32_t MIDI_IDENTICAL_PC_GUARD_MS=170;
static bool midiDiagEnabled=true;
static uint8_t midiDiagLastStatus=0;
static uint32_t midiDiagLastStatusMs=0;
static void syntechHandleCC(uint8_t cc,uint8_t v) {
  // MIDI safety commands must remain effective even while MIDI Learn is armed.
  if(cc==120 || cc==123) { sendCmd(ALL_OFF); return; }
  if(midiLearnIndex>=0) {
    // In MOPHO mode CC32 only selects the bank; Learn uses the bank stored
    // with the selected parameter. Do not consume an unrelated bank's CC.
    if(midiLearnMode==midiCCMode && cc!=32 &&
       (midiLearnMode==1 || midiCCBank==SYNTECH_CC_MAP[midiLearnIndex].bank)) {
      midiLearnAssign(cc);
      return; // learned event must not alter the current sound
    }
  }

  if(midiCCMode==0 && cc==32) {
    if(v<=2) midiCCBank=v;
    else Serial.printf("[MIDI CC32] invalid parameter bank %u (keep %u)\n",v,midiCCBank);
    return;
  }
  // Standard Bank Select MSB CC0 is intentionally NOT used for patch selection.
  if(cc==0 || cc==121 || (midiCCMode==1 && cc==32)) return;
  for(unsigned i=0;i<SYNTECH_CC_MAP_COUNT;i++) {
    const SyntechCCMap &m=SYNTECH_CC_MAP[i];
    if(midiCCMode==0) {
      if(m.bank!=midiCCBank || learnedMophoCC[i]!=cc) continue;
    } else {
      if(learnedStandardCC[i]!=cc) continue;
    }
    switch(m.command) {
      case SYN_CC_EDIT: sendCmd(EDIT_PARAM,m.id,v);break;
      case SYN_CC_LAYER:
        sendCmd(SELECT_LAYER,0,v>=64?1:0);
        uiLayer=v>=64?1:0; if(midiCcDisplayEnabled)midiUiDirty=true;
        // UI refresh takes place on Core0 via normal loop (no LVGL call in audio task).
        break;
      case SYN_CC_MODE: sendCmd(MODE,0,(int32_t)v*3/128); uiMode=(int)v*3/128; if(midiCcDisplayEnabled)midiUiDirty=true;break;
      case SYN_CC_SPLIT: sendCmd(SPLIT,0,v);break;
      case SYN_CC_CHORUS: sendCmd(CHORUS,0,v>=64?1:0);break;
      case SYN_CC_REVERB: sendCmd(REVERB,0,v>=64?1:0);break;
      case SYN_CC_CUTOFF: sendCmd(CUTOFF,0,v);break;
      case SYN_CC_RESO: sendCmd(RESO,0,v);break;
      case SYN_CC_FILTER_MODE: sendCmd(FILTER_MODE,0,(int32_t)v*5/128);break;
    }
    midiCcMatchedCount++;
    if(midiCcDisplayEnabled) {
      if(midiParamDirty) ++gui7xCoalesced;
      midiParamDirty=true; // latest value only, no LVGL work on MIDI task
    }
    return;
  }
}
static void midiByte(uint8_t b) {
  if(b>=0xF8) return; // realtime status may occur between data bytes
  if(b&0x80) {
    status=(b<0xF0)?b:0;have=0;
    midiDiagLastStatus=b;
    midiDiagLastStatusMs=millis();
    if((b&0xF0)==0xC0 && b<0xF0) midiStatusC0Count++;
    return;
  }
  if(!status || (status&0x0F)!=(midiChannel-1)) return;
  const uint8_t kind=status&0xF0;
  if(kind==0xC0 || kind==0xD0) {
    if(kind==0xC0) {
      ++midiPcCount;
      if(midiDiagEnabled && midiPcTraceCount<80){
        Serial.printf("[MIDI-DIAG PC] value=%u status=0x%02X lastStatus=0x%02X ageMs=%lu PC=%lu C0status=%lu NoteOn=%lu NoteOff=%lu\n",
          (unsigned)b,(unsigned)status,(unsigned)midiDiagLastStatus,
          (unsigned long)(millis()-midiDiagLastStatusMs),
          (unsigned long)midiPcCount,(unsigned long)midiStatusC0Count,
          (unsigned long)midiNoteOnCount,(unsigned long)midiNoteOffCount);
        ++midiPcTraceCount;
      }
      const uint32_t pcNow=millis();
      if(lastMidiPC==b && lastMidiPCMs && (uint32_t)(pcNow-lastMidiPCMs)<MIDI_IDENTICAL_PC_GUARD_MS) {
        ++ignoredMidiPCDuplicates;
        if(midiDiagEnabled) Serial.printf("[PC GUARD] duplicate MIDI PC=%03u ignored (%lu ms)\n",
          (unsigned)b,(unsigned long)(pcNow-lastMidiPCMs));
      } else {
        lastMidiPC=b;
        lastMidiPCMs=pcNow;
        requestedPresetOrigin=PC_ORIGIN_MIDI;
        requestedPreset=b; // PC127 is normal, never a special case
      }
    }
    return;
  }
  if(kind<0x80 || kind>0xE0)return;
  if(!have){data1=b;have=1;return;}
  have=0;
  if(kind==0x90){if(b)++midiNoteOnCount;else ++midiNoteOffCount;sendCmd(b?NOTE_ON:NOTE_OFF,data1,(int32_t)b*65536/127);}
  else if(kind==0x80){++midiNoteOffCount;sendCmd(NOTE_OFF,data1);}
  else if(kind==0xB0)syntechHandleCC(data1,b);
  else if(kind==0xE0){
    const int32_t raw=((int32_t)b<<7)|data1;
    sendCmd(BEND,0,((raw-8192)*2*65536)/8192);
  }
}
// FIX14: USB-MIDI packets are decoded on the existing Core0 MIDI task.
// A USB packet contains its own status byte. Preserve DIN running-status parser
// state so USB and DIN streams cannot corrupt each other.
// V0.7B: USB SysEx request receiver. Keep USB callbacks free of SD/DSP/LVGL work.
// F0 7D 52 54 53 CMD SLOT F7; CMD 01=INFO, 02=CURRENT.
static QueueHandle_t sysexRequestQ=nullptr;
struct SysexRequest { uint8_t cmd, slot; };
static uint8_t sysexRxBuf[16];
static uint8_t sysexRxLen=0;
static bool sysexRxActive=false;
static void sysexReceiveByte(uint8_t b) {
  if(b==0xF0){sysexRxActive=true;sysexRxLen=0;return;}
  if(!sysexRxActive)return;
  if(b==0xF7){
    if(sysexRxLen==6 && sysexRxBuf[0]==0x7D && sysexRxBuf[1]==0x52 &&
       sysexRxBuf[2]==0x54 && sysexRxBuf[3]==0x53 &&
       (sysexRxBuf[4]==1 || sysexRxBuf[4]==2) && sysexRequestQ){
      const SysexRequest req={sysexRxBuf[4],sysexRxBuf[5]};
      xQueueSend(sysexRequestQ,&req,0);
    }
    sysexRxActive=false;return;
  }
  if(b>=0x80 || sysexRxLen>=sizeof(sysexRxBuf)){sysexRxActive=false;return;}
  sysexRxBuf[sysexRxLen++]=b;
}
// FIX4: Non-blocking USB MIDI SysEx sender. One USB event packet per
// service iteration, with a bounded inter-packet interval. This avoids
// flooding the TinyUSB TX queue and keeps LVGL/other Core0 work responsive.
// USBMIDI::writePacket() in this core does not expose a completion ACK;
// the transport is deliberately paced, not claimed to be lossless.
static uint8_t sysexTxFrame[100];
static size_t sysexTxLen=0,sysexTxOffset=0;
static uint32_t sysexTxNextMs=0;
static uint8_t sysexTxStage=0; // 0 idle, 1 metadata, 2 chunks, 3 end
static uint8_t sysexTxSeq=0;
static uint32_t sysexTxPatchSize=0,sysexTxPatchCRC=0;
static sc_patch_t sysexTxPatch={};
static constexpr uint32_t SYSEX_USB_EVENT_GAP_MS=2;
static constexpr uint32_t SYSEX_USB_FRAME_GAP_MS=16;
static void sysexUsbReply(uint8_t cmd,uint8_t slot,uint8_t seq,const uint8_t *data,size_t len){
  if(sysexTxLen!=0 || len>44)return; // 9-byte header + 2*44 + F7 <= 100
  size_t n=0;
  sysexTxFrame[n++]=0xF0;sysexTxFrame[n++]=0x7D;
  sysexTxFrame[n++]=0x52;sysexTxFrame[n++]=0x54;sysexTxFrame[n++]=0x53;
  sysexTxFrame[n++]=cmd;sysexTxFrame[n++]=slot;sysexTxFrame[n++]=seq;
  sysexTxFrame[n++]=(uint8_t)len;
  for(size_t i=0;i<len;i++){
    sysexTxFrame[n++]=data[i]&15;
    sysexTxFrame[n++]=(data[i]>>4)&15;
  }
  sysexTxFrame[n++]=0xF7;
  sysexTxOffset=0;sysexTxLen=n;
}
static void serviceSysexRequests(){
  const uint32_t now=millis();
  // Send at most ONE USB MIDI event packet per loop iteration.
  if(sysexTxLen!=0){
    if((int32_t)(now-sysexTxNextMs)<0)return;
    const size_t left=sysexTxLen-sysexTxOffset;
    const uint8_t count=(left>=3)?3:(uint8_t)left;
    const uint8_t cin=(left>3)?4:(uint8_t)(4+count);
    const size_t i=sysexTxOffset;
    midiEventPacket_t pkt={cin,sysexTxFrame[i],
      (uint8_t)(count>1?sysexTxFrame[i+1]:0),
      (uint8_t)(count>2?sysexTxFrame[i+2]:0)};
    usbMidi.writePacket(&pkt);
    sysexTxOffset+=count;
    sysexTxNextMs=now+SYSEX_USB_EVENT_GAP_MS;
    if(sysexTxOffset>=sysexTxLen){
      sysexTxLen=0;sysexTxOffset=0;
      sysexTxNextMs=now+SYSEX_USB_FRAME_GAP_MS;
    }
    return;
  }
  if((int32_t)(now-sysexTxNextMs)<0)return;
  if(sysexTxStage==1){
    uint8_t meta[8];
    for(int i=0;i<4;i++){
      meta[i]=(sysexTxPatchSize>>(8*i))&255;
      meta[i+4]=(sysexTxPatchCRC>>(8*i))&255;
    }
    sysexUsbReply(0x42,0,0,meta,sizeof(meta));
    sysexTxStage=2;return;
  }
  if(sysexTxStage==2){
    const size_t offset=(size_t)sysexTxSeq*32;
    if(offset<sysexTxPatchSize){
      const size_t left=sysexTxPatchSize-offset;
      const size_t count=left<32?left:32;
      sysexUsbReply(0x43,0,sysexTxSeq,
        ((const uint8_t*)&sysexTxPatch)+offset,count);
      ++sysexTxSeq;return;
    }
    sysexTxStage=3;
  }
  if(sysexTxStage==3){
    sysexUsbReply(0x44,0,0,nullptr,0);
    sysexTxStage=0;return;
  }
  SysexRequest req;
  if(!sysexRequestQ || xQueueReceive(sysexRequestQ,&req,0)!=pdTRUE)return;
  if(req.cmd==1){
    const uint8_t info[]={0,7,2,1,0,128,0};
    sysexUsbReply(0x41,0,0,info,sizeof(info));return;
  }
  if(req.cmd==2){
    // A snapshot is taken once. UI and DSP are not blocked during transfer.
    if(!requestPatch(sysexTxPatch)){
      sysexUsbReply(0x7E,0,0,nullptr,0);return;
    }
    sysexTxPatchSize=(uint32_t)sizeof(sysexTxPatch);
    sysexTxPatchCRC=presetCRC((const uint8_t*)&sysexTxPatch,sizeof(sysexTxPatch));
    sysexTxSeq=0;sysexTxStage=1;
  }
}

static void serviceUSBMidiPacket(const midiEventPacket_t &packet) {
  const uint8_t cin=packet.header & 0x0F;
  uint8_t len=0;
  switch(cin) {
    case 0x2: case 0x6: case 0xC: case 0xD: len=2;break;
    case 0x3: case 0x4: case 0x7: case 0x8: case 0x9:
    case 0xA: case 0xB: case 0xE: len=3;break;
    case 0x5: case 0xF: len=1;break;
    default: ++usbMidiRxInvalid;return;
  }
  const uint8_t bytes[3]={packet.byte1,packet.byte2,packet.byte3};
  if(cin==4 || cin==5 || cin==6 || cin==7){
    for(uint8_t i=0;i<len;i++)sysexReceiveByte(bytes[i]);
    return;
  }
  // Current firmware supports channel voice messages (0x80..0xEF).
  // SysEx/system messages are intentionally ignored, never fed to parser.
  if(bytes[0]<0x80 || bytes[0]>=0xF0) return;
  const uint8_t expected=((bytes[0]&0xF0)==0xC0 || (bytes[0]&0xF0)==0xD0)?2:3;
  if(len!=expected){++usbMidiRxInvalid;return;}
  const uint8_t savedStatus=status, savedData1=data1, savedHave=have;
  status=0;have=0;
  for(uint8_t i=0;i<len;i++)midiByte(bytes[i]);
  status=savedStatus;data1=savedData1;have=savedHave;
  ++usbMidiRxPackets;
}
// FIX7: MIDI UART is serviced independently of synchronous QSPI/LVGL flushes.
// No LVGL, SD or display operations are permitted in this task.
static TaskHandle_t midiRxTaskHandle=nullptr;
static volatile uint32_t midiRxMaxBatch=0;
static volatile uint32_t midiRxBytes=0;
static void midiRxTask(void *) {
  for(;;) {
    uint32_t count=0;
    // Bounded work keeps other Core0 tasks schedulable under continuous MIDI.
    while(count<128 && midi.available()>0) {
      midiByte((uint8_t)midi.read());
      ++count;
    }
    midiRxBytes+=count;
    if(count>midiRxMaxBatch)midiRxMaxBatch=count;
    // Bounded USB packet processing; no LVGL, SD or blocking serial output.
    midiEventPacket_t usbPacket={0,0,0,0};
    uint32_t usbCount=0;
    while(usbCount<64 && usbMidi.readPacket(&usbPacket)){
      serviceUSBMidiPacket(usbPacket);
      ++usbCount;
    }
    midiTxDrain();
    // 1 tick latency is small relative to a 31.25 kbaud MIDI byte (~320 us).
    vTaskDelay(1);
  }
}
static lv_obj_t *presetLabel,*layerLabel,*modeLabel,*pageArea,*pageButtons[7];
static void updateHeader();
static void buildPage();
static lv_obj_t *bankPanel=nullptr,*bankSlotLabel=nullptr,*bankNameField=nullptr,*bankMessage=nullptr,*nameKeyboard=nullptr;
static void updateBankLabel();

static double filterDiagHz(double midiNote) {
  const double hz=440.0*pow(2.0,(midiNote-69.0)/12.0);
  return hz<20.0?20.0:(hz>14400.0?14400.0:hz);
}
static void filterDiagPrint(const FilterDiagSnapshot &d) {
  static const char *names[]={"LP24","LP12","BP12","BP6","HP12"};
  Serial.printf("[FILTDIAG9A] t=%lums activeLayer=%s voiceMode=%d chorus=%d reverb=%d FS=%d\n",
    (unsigned long)d.ms,d.activeLayer?"LOWER":"UPPER",(int)d.voiceMode,
    (int)d.chorusOn,(int)d.reverbOn,SC_FS);
  Serial.printf("[FILTDIAG9A PERF] joyX=%.3f joyY=%.3f bendFilt=%.3f lfo2Filt=%.3f (Q16)\n",
    d.joyXQ16/65536.0,d.joyYQ16/65536.0,d.bendFiltQ16/65536.0,d.lfo2FiltQ16/65536.0);
  for(int k=0;k<2;k++) {
    const FilterDiagLayerSnapshot &L=d.layer[k];
    const FilterDiagVoiceSnapshot &V=L.voice;
    const int cutoffRaw=(int)(((int64_t)L.cutoffQ16*127+32768)/65536);
    const int resoRaw=(int)(((int64_t)L.resoQ16*127+32768)/65536);
    const double nominalNote=24.0+96.0*L.cutoffQ16/65536.0;
    const double baseNote=L.baseQ16/65536.0;
    const double q=(L.kQ28>0)?(268435456.0/L.kQ28):0.0;
    Serial.printf("[FILTDIAG9A %s] filter=%s cutoff=%d/127 reso=%d/127 envAmt=%.3f keyTrack=%.3f active=%u held=%u\n",
      k?"LOWER":"UPPER",(L.mode>=0&&L.mode<5)?names[(int)L.mode]:"INVALID",
      cutoffRaw,resoRaw,L.envAmountQ16/65536.0,L.keyTrackQ16/65536.0,
      (unsigned)L.activeVoices,(unsigned)L.heldVoices);
    Serial.printf("[FILTDIAG9A %s] nominalNote=%.3f nominalHz=%.1f prepBaseNote=%.3f prepBaseHz=%.1f QperStage=%.3f kQ28=%ld atten=%.3f\n",
      k?"LOWER":"UPPER",nominalNote,filterDiagHz(nominalNote),
      baseNote,filterDiagHz(baseNote),q,(long)L.kQ28,L.attenQ16/65536.0);
    Serial.printf("[FILTRESO9A %s] mode=%s lp24=%s QperStage=%.3f originalQ=%.3f musicalQ=%.3f\n",
      k?"LOWER":"UPPER",L.lp24Musical?"MUSICAL":"ORIGINAL",
      L.mode==SC_FLT_LP24?"YES":"NO (other filter unchanged)",q,
      0.5+24.5*pow(L.resoQ16/65536.0,2.0),
      0.5+5.5*pow(L.resoQ16/65536.0,3.0));
    Serial.printf("[FILTDIAG9A %s ROUTES] LFO1_CUT=%.3f depthB1=%.3f kc1=%.3f LFO2_CUT=%.3f depthB2=%.3f kc2=%.3f ENV1_CUT=%.3f kcEnv1=%.3f VEL_CUT=%.3f JOY_CUT=%.3f\n",
      k?"LOWER":"UPPER",L.mmL1CutQ16/65536.0,L.lfoDepthB1Q16/65536.0,L.kc1Q16/65536.0,
      L.mmL2CutQ16/65536.0,L.lfoDepthB2Q16/65536.0,L.kc2Q16/65536.0,
      L.mmE1CutQ16/65536.0,L.kcEnv1Q16/65536.0,L.mmVelCutQ16/65536.0,L.mmJoyCutQ16/65536.0);
    Serial.printf("[FILTISO9A %s] ENV1_CUT routePreset=%.3f bypass=%s kcEnv1Preset=%.3f kcEnv1Effective=%.3f (Q16 semitones)\n",
      k?"LOWER":"UPPER",L.mmE1CutQ16/65536.0,L.env1CutBypassed?"ON":"OFF",
      (96.0*L.mmE1CutQ16)/65536.0,L.kcEnv1Q16/65536.0);
    if(V.slot<0 || !V.coeffValid) {
      Serial.printf("[FILTDIAG9A %s VOICE] no active voice with valid SVF coefficients. Hold a MIDI note, then run filterdiag.\n",
        k?"LOWER":"UPPER");
      continue;
    }
    // Same fixed-point cutoff contribution arithmetic as sc_voice.c:update_coeffs(),
    // evaluated from the captured values on Core0. A few samples of drift are
    // possible because the coefficients are updated once per 96 samples.
    const int64_t lfo=((int64_t)V.lfo1Q20*L.kc1Q16+(int64_t)V.lfo2Q20*L.kc2Q16)>>20;
    const int64_t vel=((int64_t)(V.velQ16-32768)*L.kcVelQ16)>>16;
    const int64_t env=((int64_t)V.envLevelQ30*L.kcEnvQ16)>>30;
    const int64_t env1=((int64_t)(V.envLevelQ30-(1<<29))*L.kcEnv1Q16)>>30;
    const int64_t key=((int64_t)(V.currentNoteQ16-(60<<16))*L.kcKeyQ16)>>16;
    const double effectiveNote=V.cutoffNoteQ16/65536.0;
    const double predictedNote=(L.baseQ16+lfo+vel+env+env1+key)/65536.0;
    Serial.printf("[FILTDIAG9A %s VOICE] slot=%d midiNote=%d held=%u stage=%u curNote=%.3f envLevel=%.3f lfo1=%.3f lfo2=%.3f\n",
      k?"LOWER":"UPPER",(int)V.slot,(int)V.note,(unsigned)V.held,(unsigned)V.stage,
      V.currentNoteQ16/65536.0,V.envLevelQ30/1073741824.0,
      V.lfo1Q20/1048576.0,V.lfo2Q20/1048576.0);
    Serial.printf("[FILTDIAG9A %s DELTAS] lfo=%.3f vel=%.3f env=%.3f matrixEnv=%.3f key=%.3f predictedNote=%.3f\n",
      k?"LOWER":"UPPER",lfo/65536.0,vel/65536.0,env/65536.0,env1/65536.0,
      key/65536.0,predictedNote);
    Serial.printf("[FILTDIAG9A %s SVF] cachedNote=%.3f tableHzApprox=%.1f a1=%ld a2=%ld a3=%ld cachedK=%ld (Q30/Q28)\n",
      k?"LOWER":"UPPER",effectiveNote,filterDiagHz(effectiveNote),
      (long)V.a1Q30,(long)V.a2Q30,(long)V.a3Q30,(long)V.cachedKQ28);
    if(effectiveNote>=129.3)
      Serial.printf("[FILTDIAG9A %s WARN] Cutoff at/near 14.4 kHz table ceiling; inspect modulation and FFT.\n",k?"LOWER":"UPPER");
  }
  Serial.println("[FILTDIAG9A] END (read-only snapshot; nominal/table Hz are estimates, not measured FFT peaks)");
}

static void printHelp() {
  Serial.println("on <0..127> | off <0..127> | alloff | pc <0..127> | bench <1..6> | benchstats");
  Serial.println("mode <0=single 1=split 2=double> | split <0..127> | poly <1..6>");
  Serial.println("layer <0=upper 1=lower> | filter <0..4> | cutoff <0..127> | reso <0..127>");
  Serial.println("filterdiag [on|off] : read-only effective cutoff, Q, SVF coefficients for BOTH layers");
  Serial.println("filterenv1 [upper|lower|both] on|off : TEMPORARY ENV1->CUTOFF route; off=bypass, on=preset value");
  Serial.println("filterenv1 [status] : show both layers; isolation resets on preset load/reboot");
  Serial.println("env1cut <0..127> : set selected layer ENV1->CUTOFF matrix route; 64=neutral (save preset to persist)");
  Serial.println("filterreso [upper|lower|both] original|musical : LP24 A/B; default MUSICAL on both layers");
  Serial.println("filterreso status : report A/B mode, Q per stage; preset load/reboot => MUSICAL");
  Serial.println("ring <0|1> | sync <0|1> | chorus <0|1> | reverb <0|1>");
  Serial.println("restore | slots | save <0..127> | load <0..127> | name <text> | status | help");
  Serial.printf("MIDI: channel %u, MOPHO CC32 bank 0/1/2 or STANDARD direct CC; PC 0..127 loads SD preset\n",(unsigned)midiChannel);
  Serial.println("midichannel [1..16] | midimode mopho|standard | midimap | midilearn <0..60>|cancel | mididefaults [all]");
  Serial.println("TOUCH: preset < > press=preview, hold=accelerate, stable release=load once");
  Serial.println("FIX8A: guiprobe normal|widgets_off|handler_off|stats; memstats");
  Serial.println("FIX7Z: lvglrate 5|10|20|33|50|100|200|stats; displaytest normal|flush_off|gui_off|stats; ccdisplay off|on|stats");
}

// FIX8A: Core0 GUI diagnostic modes; no DSP or MIDI task changes.
// 0 normal, 1 suppress MIDI-triggered widget/page refresh, 2 suspend LVGL handler.
static uint8_t guiProbeMode8A = 0;
static uint32_t gui8aPageRefresh = 0, gui8aSliderRefresh = 0;
static uint32_t gui8aPanelRefresh = 0, gui8aFxRefresh = 0;
static uint32_t gui8aPageUsMax = 0, gui8aSliderUsMax = 0;
static uint32_t gui8aPanelUsMax = 0, gui8aFxUsMax = 0;

static void serialCommand(String line) {
  line.trim();
  if(!line.length()) return;
  int sp=line.indexOf(' ');
  String action=(sp<0)?line:line.substring(0,sp); action.toLowerCase();
  String arg=(sp<0)?String(""):line.substring(sp+1);
  arg.trim();
  if(action=="guiprobe") {
    String a=arg; a.toLowerCase();
    if(a=="normal") {guiProbeMode8A=0;Board::setLvglHandlerPaused8A(false);midiUiDirty=true;midiParamDirty=true;}
    else if(a=="widgets_off") {guiProbeMode8A=1;Board::setLvglHandlerPaused8A(false);}
    else if(a=="handler_off") {guiProbeMode8A=2;Board::setLvglHandlerPaused8A(true);}
    else if(a=="stats") {
      Serial.printf("[WIDGET8A] mode=%u page=%lu/%luus slider=%lu/%luus panel=%lu/%luus fx=%lu/%luus\n",
        (unsigned)guiProbeMode8A,(unsigned long)gui8aPageRefresh,(unsigned long)gui8aPageUsMax,
        (unsigned long)gui8aSliderRefresh,(unsigned long)gui8aSliderUsMax,
        (unsigned long)gui8aPanelRefresh,(unsigned long)gui8aPanelUsMax,
        (unsigned long)gui8aFxRefresh,(unsigned long)gui8aFxUsMax);
      Board::printMemoryStats8A();Board::printLvglIntervalStats7Z();return;
    } else {Serial.println("Usage: guiprobe normal|widgets_off|handler_off|stats");return;}
    Serial.printf("[GUI8A] mode=%u (0 normal,1 MIDI widgets suppressed,2 LVGL handler paused)\n",(unsigned)guiProbeMode8A);return;
  }
  if(action=="filterreso") {
    String a=arg; a.toLowerCase(); a.trim();
    int target=(int)active_layer;
    if(a=="" || a=="status") {
      Serial.println(sendCmd(FILTER_DIAG)?"[FILTRESO9A] status snapshot requested":"[FILTRESO9A] ERROR: command queue full");
      return;
    }
    if(a.startsWith("upper ")) {target=0;a.remove(0,6);a.trim();}
    else if(a.startsWith("lower ")) {target=1;a.remove(0,6);a.trim();}
    else if(a.startsWith("both ")) {target=2;a.remove(0,5);a.trim();}
    if(a!="original" && a!="musical") {
      Serial.println("Usage: filterreso [upper|lower|both] original|musical | filterreso status");return;
    }
    const int musical=(a=="musical")?1:0;
    if(!sendCmd(FILTER_RESO_AB,target,musical)) {
      Serial.println("[FILTRESO9A] ERROR: command queue full; curve unchanged");return;
    }
    Serial.printf("[FILTRESO9A] %s LP24 curve %s queued (A/B override; new preset restores MUSICAL)\n",
      target==2?"BOTH":target==0?"UPPER":"LOWER",musical?"MUSICAL":"ORIGINAL");
    if(!sendCmd(FILTER_DIAG))Serial.println("[FILTRESO9A] NOTE: snapshot queue full; run filterdiag");
    return;
  }
  if(action=="filterenv1") {
    String a=arg; a.toLowerCase(); a.trim();
    int target=(int)active_layer; // freeze selected layer at command creation
    if(a=="" || a=="status") {
      Serial.println(sendCmd(FILTER_DIAG)?"[FILTISO9A] status snapshot requested":"[FILTISO9A] ERROR: command queue full");
      return;
    }
    if(a.startsWith("upper ")) {target=0;a.remove(0,6);a.trim();}
    else if(a.startsWith("lower ")) {target=1;a.remove(0,6);a.trim();}
    else if(a.startsWith("both ")) {target=2;a.remove(0,5);a.trim();}
    if(a!="on" && a!="off") {
      Serial.println("Usage: filterenv1 [upper|lower|both] on|off | filterenv1 status");return;
    }
    const int enabled=(a=="on")?1:0;
    if(!sendCmd(FILTER_ENV1_ROUTE,target,enabled)) {
      Serial.println("[FILTISO9A] ERROR: command queue full; route unchanged");return;
    }
    Serial.printf("[FILTISO9A] %s ENV1->CUTOFF %s queued (temporary; %s)\n",
      target==2?"BOTH":target==0?"UPPER":"LOWER",enabled?"ON":"OFF",
      enabled?"original preset route restored":"matrix route bypassed");
    if(!sendCmd(FILTER_DIAG))Serial.println("[FILTISO9A] NOTE: snapshot queue full; run filterdiag");
    return;
  }
  if(action=="filterdiag") {
    String a=arg; a.toLowerCase();
    if(a=="off") {filterDiagPeriodic=false;Serial.println("[FILTDIAG9A] periodic OFF");return;}
    if(a=="on") filterDiagPeriodic=true;
    else if(a.length()) {Serial.println("Usage: filterdiag [on|off]");return;}
    filterDiagLastRequestMs=millis();
    Serial.println(sendCmd(FILTER_DIAG)?"[FILTDIAG9A] snapshot requested":"[FILTDIAG9A] ERROR: command queue full");
    if(filterDiagPeriodic)Serial.println("[FILTDIAG9A] periodic ON (every 2 s; filterdiag off to stop)");
    return;
  }
  if(action=="memstats") {Board::printMemoryStats8A();return;}
  // FIX7Z: GUI task remains on Core0. Only the LVGL handler cadence changes.
  // Touch polling remains in LVGL, so slower rates also reduce touch responsiveness.
  if(action=="lvglrate") {
    String a=arg; a.toLowerCase();
    if(a=="stats") {Board::printLvglIntervalStats7Z();return;}
    const int ms=a.toInt();
    if(ms!=5 && ms!=10 && ms!=20 && ms!=33 && ms!=50 && ms!=100 && ms!=200) {
      Serial.println("Usage: lvglrate 5|10|20|33|50|100|200|stats");return;
    }
    Board::setLvglInterval7Z((uint32_t)ms);
    Board::printLvglIntervalStats7Z();return;
  }
  if(action=="displaytest") {
    String a=arg; a.toLowerCase();
    if(a=="normal" || a=="0") Board::setDisplayTestMode(0);
    else if(a=="flush_off" || a=="1") Board::setDisplayTestMode(1);
    else if(a=="gui_off" || a=="2") Board::setDisplayTestMode(2);
    else if(a=="stats") {Board::printDisplayTestStats(); return;}
    else {Serial.println("Usage: displaytest normal|flush_off|gui_off|stats");return;}
    Board::printDisplayTestStats();return;
  }
  if(action=="ccdisplay") {
    String a=arg; a.toLowerCase();
    if(a=="on") {midiCcDisplayEnabled=true; midiUiDirty=true; midiParamDirty=true;}
    else if(a=="off") {midiCcDisplayEnabled=false; midiUiDirty=false; midiParamDirty=false;}
    else if(a=="stats") {Serial.printf("[GUI7X] enabled=%u matchedCC=%lu coalesced=%lu widgets=%lu pageBuilds=%lu lvglMaxUs=%lu\n",(unsigned)midiCcDisplayEnabled,(unsigned long)midiCcMatchedCount,(unsigned long)gui7xCoalesced,(unsigned long)gui7xWidgetChanges,(unsigned long)gui7xPageBuilds,(unsigned long)gui7xLvglMaxUs);return;}
    else {Serial.println("Usage: ccdisplay on|off|stats");return;}
    Serial.printf("[GUI7X] display=%s matchedCC=%lu\n",midiCcDisplayEnabled?"ON":"OFF",(unsigned long)midiCcMatchedCount);
    return;
  }
  if(action=="midimode") {
    if(!arg.length()) {Serial.printf("[MIDI] mode=%s bank=%u\n",midiCCMode?"STANDARD":"MOPHO",(unsigned)midiCCBank);return;}
    if(arg=="mopho" || arg=="0") midiCCMode=0;
    else if(arg=="standard" || arg=="1") midiCCMode=1;
    else {Serial.println("[MIDI] usage: midimode mopho|standard");return;}
    midiCCBank=0; status=0;have=0; midiLearnIndex=-1;
    midiPreferences.putUChar("ccmode",midiCCMode);
    Serial.printf("[MIDI] mode=%s saved; parameter bank reset to 0; sound and preset unchanged\n",midiCCMode?"STANDARD":"MOPHO");
    return;
  }
  if(action=="midimap") {
    Serial.printf("[MIDI MAP] mode=%s entries=%u\n",midiCCMode?"STANDARD":"MOPHO",(unsigned)SYNTECH_CC_MAP_COUNT);
    for(unsigned i=0;i<SYNTECH_CC_MAP_COUNT;i++) {
      const SyntechCCMap &m=SYNTECH_CC_MAP[i];
      if(midiCCMode) Serial.printf("[%02u] CC%03u command=%u id=%u\n",i,(unsigned)learnedStandardCC[i],m.command,m.id);
      else Serial.printf("[%02u] BANK%u CC%03u command=%u id=%u\n",i,m.bank,learnedMophoCC[i],m.command,m.id);
    }
    return;
  }
  if(action=="midilearn") {
    if(arg=="cancel") {midiLearnIndex=-1;Serial.println("[MIDI LEARN] cancelled");return;}
    if(!arg.length()) {Serial.printf("[MIDI LEARN] %s entry=%d\n",midiLearnIndex<0?"idle":"waiting",(int)midiLearnIndex);return;}
    for(unsigned k=0;k<arg.length();k++)if(arg[k]<'0'||arg[k]>'9') {Serial.println("[MIDI LEARN] use: midilearn <0..60>|cancel");return;}
    int idx=arg.toInt();
    if(idx<0 || idx>=(int)SYNTECH_CC_MAP_COUNT){Serial.println("[MIDI LEARN] entry out of range");return;}
    midiLearnIndex=idx;midiLearnMode=midiCCMode;
    if(midiLearnMode==0)midiCCBank=SYNTECH_CC_MAP[idx].bank;
    Serial.printf("[MIDI LEARN] waiting mode=%s entry=%d bank=%u; move MIDI CC control\n",midiLearnMode?"STANDARD":"MOPHO",idx,(unsigned)midiCCBank);
    return;
  }
  if(action=="mididefaults") {
    if(arg!="" && arg!="all") {Serial.println("[MIDI] usage: mididefaults [all]");return;}
    midiLearnIndex=-1;
    if(arg=="all") {midiLearnDefaults(0);midiLearnDefaults(1);midiLearnSave(0);midiLearnSave(1);}
    else {midiLearnDefaults(midiCCMode);midiLearnSave(midiCCMode);}
    Serial.printf("[MIDI] factory mapping restored: %s\n",arg=="all"?"BOTH":(midiCCMode?"STANDARD":"MOPHO"));return;
  }
  if(action=="midichannel") {
    if(!arg.length()){Serial.printf("[MIDI] channel=%u\n",(unsigned)midiChannel);return;}
    int ch=arg.toInt();
    if(ch<1 || ch>16){Serial.println("[MIDI] channel must be 1..16");return;}
    midiChannel=(uint8_t)ch;
    midiPreferences.putUChar("channel",midiChannel);
    status=0;have=0; midiCCBank=0; // reset bank and stale running status after channel change
    Serial.printf("[MIDI] channel=%u saved\n",(unsigned)midiChannel);
    return;
  }
  // FIX7U SERIAL FIX: intercept text diagnostics before numeric argument parsing.
  if(action=="touchtest") {
    arg.toLowerCase();
    if(arg=="normal" || arg=="0") Board::setTouchDiagnosticMode(0);
    else if(arg=="off" || arg=="1") Board::setTouchDiagnosticMode(1);
    else if(arg=="read" || arg=="readonly" || arg=="2") Board::setTouchDiagnosticMode(2);
    else Serial.println("Usage: touchtest normal|off|read");
    return;
  }
  if(action=="touchstats") { Board::printTouchDiagnostics(); return; }
  if(action=="help" || action=="?") {printHelp();Serial.println("mididiag [on|off|reset] | midistats | touchtest normal|off|read | touchstats | ccdisplay off|on|stats");return;}
  if(action=="midistats") {
    Serial.printf("[MIDI-DIAG STATS] PC=%lu C0status=%lu NoteOn=%lu NoteOff=%lu current=0x%02X lastStatus=0x%02X\n",
      (unsigned long)midiPcCount,(unsigned long)midiStatusC0Count,
      (unsigned long)midiNoteOnCount,(unsigned long)midiNoteOffCount,
      (unsigned)status,(unsigned)midiDiagLastStatus);
    Serial.printf("[PRESET NAV] state=%u direction=%d preview=%03d steps=%lu loads=%lu cancel=%lu ignored=%lu midiDup=%lu bankIgnored=%lu\n",
      (unsigned)touchNavState,(int)touchNavDirection,touchNavTarget,
      (unsigned long)touchNavSteps,(unsigned long)touchNavCommits,
      (unsigned long)touchNavCancels,(unsigned long)ignoredTouchPresetClicks,
      (unsigned long)ignoredMidiPCDuplicates,(unsigned long)ignoredBankNavigationClicks);
    return;
  }
  if(action=="mididiag") {
    if(arg=="on")midiDiagEnabled=true;
    else if(arg=="off")midiDiagEnabled=false;
    else if(arg=="reset") {
      midiPcCount=midiStatusC0Count=midiNoteOnCount=midiNoteOffCount=midiPcTraceCount=0;
      ignoredTouchPresetClicks=ignoredMidiPCDuplicates=ignoredBankNavigationClicks=0;
      touchNavSteps=touchNavCommits=touchNavCancels=0;
      lastMidiPC=255; lastMidiPCMs=0; midiDiagEnabled=true;
    }
    Serial.printf("[MIDI-DIAG] enabled=%d printed=%lu/80\n",midiDiagEnabled,(unsigned long)midiPcTraceCount);
    return;
  }
  if(action=="name") { if(bankNameField)lv_textarea_set_text(bankNameField,arg.c_str());return; }
  if(action=="perfdiag") {
    Serial.printf("[PERF STAGE2] core audio=%lu gui=%lu (Arduino loop)\n",
      (unsigned long)diag_audio_core,(unsigned long)diag_gui_core);
    Serial.printf("[PERF STAGE2] dsp=%lu/%lu us totalLast=%lu totalPeak=%lu us budget=%lu us\n",
      (unsigned long)avg_us,(unsigned long)max_us,
      (unsigned long)diag_audio_total_avg,(unsigned long)diag_audio_total_peak,
      (unsigned long)((uint64_t)FRAMES*1000000/SC_FS));
    Serial.printf("[PERF STAGE2] guiCalls=%lu maxLoopGap=%lu ms maxLvglCall=%lu us\n",
      (unsigned long)diag_gui_calls,(unsigned long)diag_gui_max_gap_ms,
      (unsigned long)diag_gui_max_handler_us);
    Serial.printf("[PERF STAGE2] pcmPeak=%lu lastClipped=%lu clippedTotal=%lu clippedBlocks=%lu xrun=%lu i2sErrors=%lu\n",
      (unsigned long)last_pcm_peak,(unsigned long)last_pcm_clipped,
      (unsigned long)diag_pcm_clipped_total,(unsigned long)diag_pcm_clipped_blocks,
      (unsigned long)xrun,(unsigned long)partial_writes);
    return;
  }
  if(action=="stage4diag") {
    Serial.printf("[STAGE4] audioCore=%lu guiCore=%lu renderLast/Peak=%lu/%lu us cmdLast/Peak=%lu/%lu us\n",
      (unsigned long)diag_audio_core,(unsigned long)diag_gui_core,
      (unsigned long)diag_render_last_us,(unsigned long)diag_render_peak_us,
      (unsigned long)diag_cmd_last_us,(unsigned long)diag_cmd_peak_us);
    Serial.printf("[FIX3] cmdCap=%lu deferredBlocks=%lu pendingPeak=%lu computeLast/Peak=%lu/%lu us\n",
      (unsigned long)RTAL_MAX_COMMANDS_PER_BLOCK,(unsigned long)diag_cmd_deferred_blocks,
      (unsigned long)diag_cmd_pending_peak,(unsigned long)diag_audio_compute_last,
      (unsigned long)diag_audio_compute_peak);
    Serial.printf("[STAGE4] maxCmdBatch=%lu reprepBatches=%lu xrun=%lu guiFrames=%lu guiFlushes=%lu\n",
      (unsigned long)diag_cmd_batch_peak,(unsigned long)diag_reprep_batches,
      (unsigned long)xrun,(unsigned long)Board::guiFrameCount(),(unsigned long)Board::guiFlushCount());
    return;
  }
  if(action=="benchstats") {
    const uint32_t budgetUs=(uint32_t)((uint64_t)FRAMES*1000000/SC_FS);
    const uint32_t avg=avg_us, peak=max_us, xr=xrun, blocks=block_count;
    Serial.printf("[DSP BENCH V0.7C] sampleRate=%d block=%d budget=%lu us poly=%d mode=%d\n",
      SC_FS,FRAMES,(unsigned long)budgetUs,(int)synth->patch.polyphony,(int)synth->patch.voice_mode);
    Serial.printf("[DSP BENCH] avg=%lu us (%lu%%) peak=%lu us (%lu%%) xrun=%lu blocks=%lu qdrop=%lu i2sErrors=%lu\n",
      (unsigned long)avg,(unsigned long)(100ULL*avg/budgetUs),
      (unsigned long)peak,(unsigned long)(100ULL*peak/budgetUs),
      (unsigned long)xr,(unsigned long)blocks,
      (unsigned long)dropped_cmds,(unsigned long)partial_writes);
    Serial.println("[DSP BENCH] Metrics cover latest 256-block window; xrun is cumulative since boot.");
    return;
  }
  if(action=="status") {
    Serial.printf("preset=%d '%s' layer=%d mode=%d split=%d poly=%d chorus=%d reverb=%d\n",
      current_preset,currentPresetName,active_layer,
      synth->patch.voice_mode,synth->patch.split,synth->patch.polyphony,
      synth->patch.fx.chorus.on,synth->patch.fx.reverb.on);
    Serial.printf("DSP avg=%lu us peak=%lu us xrun=%lu blocks=%lu budget=%lu us, freeHeap=%lu PSRAM=%lu pcmPeak=%lu clippedSamples=%lu qDropped=%lu i2sErrors=%lu\n",
      (unsigned long)avg_us,(unsigned long)max_us,(unsigned long)xrun,
      (unsigned long)block_count,(unsigned long)((uint64_t)FRAMES*1000000/SC_FS),
      (unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getFreePsram(),
      (unsigned long)last_pcm_peak,(unsigned long)last_pcm_clipped,
      (unsigned long)dropped_cmds,(unsigned long)partial_writes);
    return;
  }
  if(action=="slots") {for(int i=0;i<USER_SLOTS;i++)Serial.printf("%03d: %s\n",i,slotName(i).c_str());return;}
  if(action=="save" || action=="load") {
    int slot=arg.toInt();if(slot<0||slot>=USER_SLOTS){Serial.println("Use slot 0..127");return;}
    selectedSlot=slot;
    if(action=="save"){
      const char *name=lv_textarea_get_text(bankNameField);
      Serial.println(writeSlot(selectedSlot,name&&name[0]?name:"USER PATCH")?"SAVED":"SAVE FAILED");
    }else {
      char name[32]={};bool ok=readSlot(selectedSlot,name);
      if(ok){snprintf(currentPresetName,32,"%s",name);current_preset=selectedSlot;uiPreset=selectedSlot;updateHeader();buildPage();lv_textarea_set_text(bankNameField,name);}
      Serial.println(ok?"LOADED":"LOAD FAILED");
    }
    updateBankLabel();return;
  }
  if(action=="alloff" || action=="restore") {
    if(action=="restore"){requestedPresetOrigin=PC_ORIGIN_RESTORE;requestedPreset=current_preset;}
    else sendCmd(ALL_OFF);
    Serial.println("queued");return;
  }
  if(!arg.length()) {Serial.println("Missing number. Type help");return;}
  for(size_t i=0;i<arg.length();i++) {
    if(!isDigit(arg[i]) && !(i==0 && arg[i]=='-')) {Serial.println("Invalid number");return;}
  }
  long value=arg.toInt();
  CmdType t;
  int minv=0,maxv=127;
  if(action=="on") {t=NOTE_ON;}
  else if(action=="off") {t=NOTE_OFF;}
  else if(action=="pc") {t=PRESET;maxv=127;}
  else if(action=="bench") {t=BENCH;minv=1;maxv=6;}
  else if(action=="mode") {t=MODE;maxv=2;}
  else if(action=="split") {t=SPLIT;}
  else if(action=="poly") {t=POLY;minv=1;maxv=6;}
  else if(action=="layer") {t=SELECT_LAYER;maxv=1;}
  else if(action=="chorus") {t=CHORUS;maxv=1;}
  else if(action=="reverb") {t=REVERB;maxv=1;}
  else if(action=="filter") {t=FILTER_MODE;maxv=4;}
  else if(action=="cutoff") {t=CUTOFF;}
  else if(action=="reso") {t=RESO;}
  else if(action=="env1cut") {t=EDIT_PARAM;} // FIX4 GUI parameter 58
  else if(action=="ring") {t=RING;maxv=1;}
  else if(action=="sync") {t=SYNC;maxv=1;}
  else {Serial.println("Unknown command. Type help");return;}
  if(value<minv || value>maxv) {Serial.printf("Range: %d..%d\n",minv,maxv);return;}
  int32_t payload=(t==NOTE_ON)?50000:(int32_t)value;
  if(t==PRESET){requestedPresetOrigin=PC_ORIGIN_SERIAL;requestedPreset=value;Serial.println("requested from SD");return;}
  if(sendCmd(t,(t==EDIT_PARAM)?58:((t==NOTE_ON || t==NOTE_OFF)?(int)value:0),payload)) Serial.println("queued");
  else Serial.println("ERROR: command queue full");
}

// JC4827W543 display + GT911 from tested MiniAmused v1.3.0.
// V0.3A: six editor pages, queue-only parameter changes, NO virtual keyboard.
// FIX10: Dedicated 480x272 preset name editor. No lv_keyboard automatic text events.
// Each physical release is committed once; editing is entirely separate from BANK.
static uint32_t bankOpenedAtMs=0;
static bool bankShown=false;
static lv_obj_t *nameEditLabel=nullptr;
static lv_obj_t *nameOriginalLabel=nullptr;
static char nameEditBuffer[32]={};
static uint32_t nameKeyLastMs=0;
static int nameKeyLastCode=-1;
static uint8_t nameKeyboardMode=0; // 0=ABC, 1=abc, 2=123/#+=
static lv_obj_t *nameKeyButtons[26]={};
static lv_obj_t *nameKeyLabels[26]={};
static lv_obj_t *nameModeLabel=nullptr;
static void refreshNameEditor(){
  if(nameEditLabel){
    char cursorText[36];snprintf(cursorText,sizeof(cursorText),"%s_",nameEditBuffer);
    lv_label_set_text(nameEditLabel,cursorText);
    lv_obj_invalidate(nameEditLabel);
  }
  // Mirror the in-progress text in the large, top-line preview.
  if(nameOriginalLabel){
    char live[80];snprintf(live,sizeof(live),"NAME: %s",nameEditBuffer);
    lv_label_set_text(nameOriginalLabel,live);
    lv_obj_invalidate(nameOriginalLabel);
  }
}
static void hidePresetKeyboard(){
  lv_indev_t *indev=lv_indev_get_act();if(indev)lv_indev_wait_release(indev);
  if(nameKeyboard)lv_obj_add_flag(nameKeyboard,LV_OBJ_FLAG_HIDDEN);
  if(bankNameField)lv_obj_clear_state(bankNameField,LV_STATE_FOCUSED);
}
static void onNameKey(lv_event_t *e);
static void setKeyboardMode(uint8_t mode){
  nameKeyboardMode=mode%3;
  const char *upper="QWERTYUIOPASDFGHJKLZXCVBNM";
  const char *lower="qwertyuiopasdfghjklzxcvbnm";
  const char *special="1234567890!@#$%&*()-_+=./:;?";
  const char *keys=nameKeyboardMode==0?upper:(nameKeyboardMode==1?lower:special);
  for(int i=0;i<26;i++){
    char label[2]={keys[i],0};
    if(nameKeyLabels[i])lv_label_set_text(nameKeyLabels[i],label);
  }
  if(nameModeLabel)lv_label_set_text(nameModeLabel,
    nameKeyboardMode==0?"abc >":(nameKeyboardMode==1?"123 >":"ABC >"));
}
static void onNameKey(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED)return;
  const int code=(int)(intptr_t)lv_event_get_user_data(e);
  const uint32_t now=millis();
  // Suppress only duplicate CLICK of the SAME key within a short interval.
  if(code==nameKeyLastCode && (uint32_t)(now-nameKeyLastMs)<180)return;
  nameKeyLastMs=now;nameKeyLastCode=code;
  if(code==1004){setKeyboardMode(nameKeyboardMode+1);return;}
  if(code==1000){lv_textarea_set_text(bankNameField,nameEditBuffer);hidePresetKeyboard();return;}
  if(code==1001){hidePresetKeyboard();return;}
  size_t len=strlen(nameEditBuffer);
  if(code==1002){if(len)nameEditBuffer[len-1]=0;}
  else if(code==1003){if(len<31){nameEditBuffer[len]=' ';nameEditBuffer[len+1]=0;}}
  else if(code>=2000 && code<2026 && len<31){
    const char *upper="QWERTYUIOPASDFGHJKLZXCVBNM";
    const char *lower="qwertyuiopasdfghjklzxcvbnm";
    const char *special="1234567890!@#$%&*()-_+=./:;?";
    const char *keys=nameKeyboardMode==0?upper:(nameKeyboardMode==1?lower:special);
    nameEditBuffer[len]=keys[code-2000];nameEditBuffer[len+1]=0;
  }
  refreshNameEditor();
}
static void onNameTap(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED || !bankShown || !nameKeyboard)return;
  if((uint32_t)(millis()-bankOpenedAtMs)<350)return;
  snprintf(nameEditBuffer,sizeof(nameEditBuffer),"%s",lv_textarea_get_text(bankNameField));
  nameKeyLastCode=-1;nameKeyLastMs=0;setKeyboardMode(false);refreshNameEditor();
  lv_obj_clear_flag(nameKeyboard,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(nameKeyboard);
  lv_indev_t *indev=lv_indev_get_act();if(indev)lv_indev_wait_release(indev);
}
static lv_obj_t *addNameKey(const char *text,int code,int x,int y,int w,int h){
  lv_obj_t *b=lv_btn_create(nameKeyboard);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
  lv_obj_clear_flag(b,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b,onNameKey,LV_EVENT_CLICKED,(void*)(intptr_t)code);
  lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_center(l);
  if(code>=2000 && code<2026){nameKeyButtons[code-2000]=b;nameKeyLabels[code-2000]=l;}
  return b;
}
static void createNameKeyboard(lv_obj_t *root){
  nameKeyboard=lv_obj_create(root);lv_obj_set_pos(nameKeyboard,0,0);lv_obj_set_size(nameKeyboard,480,272);
  lv_obj_set_style_bg_color(nameKeyboard,lv_color_hex(0x111923),0);
  lv_obj_set_style_pad_all(nameKeyboard,2,0);lv_obj_clear_flag(nameKeyboard,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title=lv_label_create(nameKeyboard);lv_label_set_text(title,"PRESET NAME EDITOR");lv_obj_set_pos(title,8,2);
  nameOriginalLabel=lv_label_create(nameKeyboard);lv_obj_set_pos(nameOriginalLabel,8,22);
  lv_obj_set_width(nameOriginalLabel,464);
  lv_label_set_long_mode(nameOriginalLabel,LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(nameOriginalLabel,lv_color_hex(0x8CDFFF),0);
  lv_obj_t *editCaption=lv_label_create(nameKeyboard);
  lv_label_set_text(editCaption,"LIVE:");lv_obj_set_pos(editCaption,8,43);
  nameEditLabel=lv_label_create(nameKeyboard);lv_obj_set_pos(nameEditLabel,87,43);
  lv_obj_set_width(nameEditLabel,385);lv_label_set_long_mode(nameEditLabel,LV_LABEL_LONG_DOT);
  const char *rows[]={"QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
  int keyIndex=0;
  for(int r=0;r<3;r++){
    int n=strlen(rows[r]);int w=43;int gap=3;int x=(480-(n*w+(n-1)*gap))/2;
    for(int j=0;j<n;j++){
      char txt[2]={rows[r][j],0};addNameKey(txt,2000+keyIndex++,x+j*(w+gap),72+r*46,w,40);
    }
  }
  lv_obj_t *mode=addNameKey("abc >",1004,5,214,90,42);
  nameModeLabel=lv_obj_get_child(mode,0);
  addNameKey("SPACE",1003,99,214,83,42);
  addNameKey("BACK",1002,186,214,70,42);
  addNameKey("CANCEL",1001,260,214,104,42);
  addNameKey("OK",1000,368,214,104,42);
  lv_obj_add_flag(nameKeyboard,LV_OBJ_FLAG_HIDDEN);
}
static uint32_t lastBankNavClickMs=0;
static constexpr uint32_t BANK_NAV_GUARD_MS=420;
static void updateBankLabel(){
  if(!bankSlotLabel)return;
  String t="PRESET "+String(selectedSlot)+" / 127  "+slotName(selectedSlot);
  lv_label_set_text(bankSlotLabel,t.c_str());
}
static void onBankAction(lv_event_t *e){
  if(!acceptButton())return;
  int a=(int)(intptr_t)lv_event_get_user_data(e);
  if(a==0 || a==1) {
    const uint32_t now=millis();
    if(lastBankNavClickMs && (uint32_t)(now-lastBankNavClickMs)<BANK_NAV_GUARD_MS) {
      ++ignoredBankNavigationClicks;
      return;
    }
    lastBankNavClickMs=now;
    selectedSlot=(selectedSlot+USER_SLOTS+(a==0?-1:1))%USER_SLOTS;
    updateBankLabel();
    return;
  }
  if(a==4){bankShown=false;hidePresetKeyboard();lv_obj_add_flag(bankPanel,LV_OBJ_FLAG_HIDDEN);return;}
  if(!sdPresetReady){lv_label_set_text(bankMessage,"SD NOT READY");return;}
  if(a==2){
    const char *n=lv_textarea_get_text(bankNameField);
    if(!n||!n[0])n="USER PATCH";
    bool ok=writeSlot(selectedSlot,n);
    if(ok){snprintf(userPatchName,sizeof(userPatchName),"%s",n);snprintf(currentPresetName,sizeof(currentPresetName),"%s",n);current_preset=selectedSlot;uiPreset=selectedSlot;updateHeader();}
    lv_label_set_text(bankMessage,ok?"SAVED":"SAVE FAILED");
  }else if(a==3){
    char name[32]={};bool ok=readSlot(selectedSlot,name);
    if(ok){snprintf(userPatchName,sizeof(userPatchName),"%s",name);snprintf(currentPresetName,sizeof(currentPresetName),"%s",name);current_preset=selectedSlot;uiPreset=selectedSlot;updateHeader();buildPage();lv_textarea_set_text(bankNameField,name);}
    lv_label_set_text(bankMessage,ok?"LOADED":"LOAD FAILED / EMPTY");
  }
  updateBankLabel();
}
static void onBankOpen(lv_event_t *e){
  (void)e;if(!acceptButton())return;
  bankShown=!bankShown;
  if(bankShown){
    bankOpenedAtMs=millis();
    lv_obj_add_flag(nameKeyboard,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(bankNameField,LV_STATE_FOCUSED);
    updateBankLabel();
    lv_obj_clear_flag(bankPanel,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bankPanel);
  }else{
    lv_obj_add_flag(nameKeyboard,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(bankNameField,LV_STATE_FOCUSED);
    lv_obj_add_flag(bankPanel,LV_OBJ_FLAG_HIDDEN);
  }
}

static const char* const MIDI_PARAMETER_NAMES[SYNTECH_CC_MAP_COUNT] = {
  "OSC1 Waveform",
  "OSC2 Waveform",
  "OSC1 PWM",
  "OSC2 PWM",
  "OSC2 Detune",
  "Oscillator Sync",
  "OSC1 Level",
  "OSC2 Level",
  "Noise Level",
  "Cross Mod 1",
  "Cross Mod 2",
  "Ring Modulation",
  "Pink Noise",
  "Filter Cutoff",
  "Filter Resonance",
  "Filter Type",
  "Filter Env Amount",
  "Filter Key Tracking",
  "Filter Attack",
  "Filter Decay",
  "Filter Sustain",
  "Filter Release",
  "Amp Attack",
  "Amp Decay",
  "Amp Sustain",
  "Amp Release",
  "LFO1 Rate",
  "LFO1 Depth",
  "LFO1 Waveform",
  "LFO2 Rate",
  "LFO2 Depth",
  "LFO2 Waveform",
  "Chorus Mix",
  "Chorus Rate",
  "Chorus Depth",
  "Reverb Mix",
  "Reverb Size",
  "Reverb Damping",
  "Upper / Lower Select",
  "Play Mode",
  "Split Point",
  "Chorus Enable",
  "Reverb Enable",
  "LFO1 → OSC1 Pitch",
  "LFO1 → OSC2 Pitch",
  "LFO1 → OSC1 PWM",
  "LFO1 → OSC2 PWM",
  "LFO1 → Filter Cutoff",
  "LFO1 → Amplitude",
  "LFO1 Filter/Amp Depth",
  "LFO1 Delay",
  "LFO1 Sync",
  "LFO2 → OSC1 Pitch",
  "LFO2 → OSC2 Pitch",
  "LFO2 → OSC1 PWM",
  "LFO2 → OSC2 PWM",
  "LFO2 → Filter Cutoff",
  "LFO2 → Amplitude",
  "LFO2 Filter/Amp Depth",
  "LFO2 Delay",
  "LFO2 Sync"
};

// STAGE4 FIX1: named MIDI learn plus long-press on sliders (Core0 only).
static lv_obj_t *midiPanel=nullptr,*midiInfoLabel=nullptr,*midiLearnLabel=nullptr;
static bool midiPanelShown=false,midiDefaultsArmed=false;
static bool midiOpenedFromSlider=false;
static int midiSelectedEntry=0;
// Guard against GT911 release bounce and click-through when overlay opens.
static uint32_t midiLastActionMs=0;
static uint32_t midiOpenedMs=0;
static constexpr uint32_t MIDI_ACTION_GUARD_MS=280;
static constexpr uint32_t MIDI_OPEN_GUARD_MS=550;
static void midiPanelRefresh(){
  if(!midiInfoLabel || !midiLearnLabel)return;
  const SyntechCCMap &m=SYNTECH_CC_MAP[midiSelectedEntry];
  const unsigned cc=midiCCMode?learnedStandardCC[midiSelectedEntry]:learnedMophoCC[midiSelectedEntry];
  char buf[160];
  snprintf(buf,sizeof(buf),"MODE %s    CH %u\n%s\nBANK %u   CC %u",
    midiCCMode?"STANDARD":"MOPHO",(unsigned)midiChannel,MIDI_PARAMETER_NAMES[midiSelectedEntry],
    (unsigned)(midiCCMode?0:m.bank),cc);
  lv_label_set_text(midiInfoLabel,buf);
  lv_label_set_text(midiLearnLabel,midiLearnIndex>=0?"LEARN: move MIDI CC now":
    (midiDefaultsArmed?"DEFAULT: tap again to confirm":"Select parameter and press LEARN"));
}
static void onMidiPanel(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED)return;
  const intptr_t action=(intptr_t)lv_event_get_user_data(e);
  const uint32_t now=millis();
  // Opening the overlay must not also activate MODE or another underlying control.
  if(action!=0 && (!midiPanelShown || (uint32_t)(now-midiOpenedMs)<MIDI_OPEN_GUARD_MS))return;
  // Ignore duplicate CLICKED callbacks from GT911 contact bounce.
  if(midiLastActionMs && (uint32_t)(now-midiLastActionMs)<MIDI_ACTION_GUARD_MS)return;
  midiLastActionMs=now;
  if(action==0){
    midiPanelShown=!midiPanelShown;
    if(midiPanelShown){midiOpenedMs=now;lv_obj_clear_flag(midiPanel,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(midiPanel);}
    else {lv_obj_add_flag(midiPanel,LV_OBJ_FLAG_HIDDEN);midiDefaultsArmed=false;midiOpenedFromSlider=false;midiLearnIndex=-1;}
  }else if(action==1){
    midiCCMode^=1;midiCCBank=0;status=0;have=0;midiLearnIndex=-1;
    midiPreferences.putUChar("ccmode",midiCCMode);midiDefaultsArmed=false;
  }else if(action==2 || action==3){
    midiChannel=(uint8_t)(1+(midiChannel-1+(action==2?15:1))%16);
    midiCCBank=0;status=0;have=0;midiLearnIndex=-1;
    midiPreferences.putUChar("channel",midiChannel);midiDefaultsArmed=false;
  }else if(action==4 || action==5){
    midiSelectedEntry=(midiSelectedEntry+(action==4?(int)SYNTECH_CC_MAP_COUNT-1:1))%SYNTECH_CC_MAP_COUNT;
    midiLearnIndex=-1;midiDefaultsArmed=false;
  }else if(action==6){
    if(midiLearnIndex==midiSelectedEntry)midiLearnIndex=-1;
    else {
      midiLearnIndex=midiSelectedEntry;midiLearnMode=midiCCMode;
      if(midiLearnMode==0)midiCCBank=SYNTECH_CC_MAP[midiSelectedEntry].bank;
    }
    midiDefaultsArmed=false;
  }else if(action==7){
    if(midiDefaultsArmed){midiLearnIndex=-1;midiLearnDefaults(midiCCMode);midiLearnSave(midiCCMode);midiDefaultsArmed=false;}
    else midiDefaultsArmed=true;
  }
  midiPanelRefresh();
}
static uint8_t controlIds[8];
static void onSliderLongPress(int slot){
  if(slot<0||slot>=6 || controlIds[slot]==255)return;
  const uint8_t id=controlIds[slot];
  for(unsigned i=0;i<SYNTECH_CC_MAP_COUNT;i++){
    const SyntechCCMap &m=SYNTECH_CC_MAP[i];
    if(m.id!=id)continue;
    // Filter controls have specialized commands but share the same DSP parameter ID.
    if(m.command!=SYN_CC_EDIT && !(id==34 && m.command==SYN_CC_CUTOFF)
       && !(id==35 && m.command==SYN_CC_RESO)
       && !(id==36 && m.command==SYN_CC_FILTER_MODE))continue;
    midiSelectedEntry=(int)i;
    midiLearnIndex=(int)i;
    midiLearnMode=midiCCMode;
    if(midiCCMode==0)midiCCBank=m.bank;
    midiPanelShown=true;midiOpenedMs=millis();midiLastActionMs=midiOpenedMs;midiOpenedFromSlider=true;midiDefaultsArmed=false;
    lv_obj_clear_flag(midiPanel,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(midiPanel);
    midiPanelRefresh();
    Serial.printf("[MIDI TOUCH LEARN] %s bank=%u\n",MIDI_PARAMETER_NAMES[i],m.bank);
    return;
  }
  Serial.printf("[MIDI TOUCH LEARN] no mapping for DSP parameter %u\n",id);
}
// FIX7 PARAM INFO: Reuse the proven MIDI-LABEL hold event path from FIX4.
// The transparent label hitbox receives LVGL PRESSED/PRESSING/LONG_PRESSED.
// A GUI-loop timer checks 1500ms while the finger is physically down.
// A 250ms event gap tolerance absorbs intermittent GT911 RELEASED glitches.
// A slider-origin drag is not a label-origin press (LVGL captures initial target).
#include "SyntechParamInfoDelay.h"
static SyntechParamInfoGate paramInfoGate={};
static bool sliderTouchHeld=false;
static bool sliderTouchInfoFired=false;
static int sliderTouchSlot=-1;
static uint32_t sliderTouchLastEventMs=0;
static uint32_t sliderTouchReleaseSinceMs=0;
static int16_t sliderTouchOriginX=0, sliderTouchOriginY=0;
static lv_obj_t *controlLabelHits[6]={};
static void onParameterLabelHoldEvent(lv_event_t *e){
  const lv_event_code_t code=lv_event_get_code(e);
  const int slot=(int)(intptr_t)lv_event_get_user_data(e);
  if(slot<0 || slot>=6 || controlIds[slot]==255)return;
  const uint32_t now=millis();
  if(code==LV_EVENT_PRESSED || code==LV_EVENT_PRESSING || code==LV_EVENT_LONG_PRESSED){
    // An open overlay must not rearm on another PRESSING from the same gesture.
    if(sliderTouchInfoFired || midiPanelShown || bankShown)return;
    if(!sliderTouchHeld){
      if(!syntechParamInfoPress(paramInfoGate,slot,controlIds[slot],uiPage,uiLayer,
                                current_preset,now)){
        Serial.printf("[PARAM INFO7] GUARD slot=%d\n",slot);
        return;
      }
      sliderTouchHeld=true;sliderTouchSlot=slot;
      sliderTouchOriginX=sliderTouchOriginY=0;
      int16_t x=0,y=0;
      if(Board::touchPosition(x,y)){
        sliderTouchOriginX=x;sliderTouchOriginY=y;
      }else{
        // LVGL's own press point remains valid even if GT911 momentarily drops out.
        lv_indev_t *indev=lv_indev_get_act();
        if(indev){lv_point_t p;lv_indev_get_point(indev,&p);
          sliderTouchOriginX=p.x;sliderTouchOriginY=p.y;}
      }
      Serial.printf("[PARAM INFO7] START slot=%d x=%d y=%d\n",slot,
                    (int)sliderTouchOriginX,(int)sliderTouchOriginY);
    }
    if(sliderTouchSlot==slot)sliderTouchLastEventMs=now;
  }else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){
    // Do not cancel on a single GT911 RELEASED glitch. The GUI loop
    // requires a >250ms gap and a real press at the opening threshold.
  }
}
static lv_obj_t *controlSliders[8];
static uint32_t lastUiMs=0;
// Touch protection: ignore duplicate CLICK events caused by short GT911 release glitches.
static uint32_t lastButtonMs=0;
static constexpr uint32_t BUTTON_GUARD_MS=240;
static bool acceptButton(){
  const uint32_t now=millis();
  if(lastButtonMs && (uint32_t)(now-lastButtonMs)<BUTTON_GUARD_MS)return false;
  lastButtonMs=now;
  return true;
}
// Parameter events are filtered per displayed slider, not globally: simultaneous controls remain responsive.
static uint32_t lastSliderMs[6]={};
static int16_t lastSliderValue[6]={-1,-1,-1,-1,-1,-1};
static constexpr uint32_t SLIDER_INTERVAL_MS=45;
static const lv_color_t COL_BG=lv_color_hex(0x071B30),COL_PANEL=lv_color_hex(0x123C61),COL_ACC=lv_color_hex(0x18D8E8);
static lv_obj_t *fxChorusButton=nullptr,*fxReverbButton=nullptr;
static int8_t fxChorusShown=-1,fxReverbShown=-1;
static void refreshFxButtons(){
  if(uiPage!=6 || !fxChorusButton || !fxReverbButton)return;
  const bool chorus=synth->patch.fx.chorus.on!=0;
  const bool reverb=synth->patch.fx.reverb.on!=0;
  if(fxChorusShown==(int)chorus && fxReverbShown==(int)reverb)return;
  fxChorusShown=chorus;fxReverbShown=reverb;
  lv_obj_t *buttons[2]={fxChorusButton,fxReverbButton};
  const bool enabled[2]={chorus,reverb};
  for(int i=0;i<2;i++){
    lv_obj_set_style_bg_color(buttons[i],enabled[i]?COL_ACC:COL_PANEL,0);
    lv_obj_set_style_text_color(lv_obj_get_child(buttons[i],0),enabled[i]?lv_color_hex(0x06192B):lv_color_white(),0);
  }
}
static void onFxToggle(lv_event_t *e){
  if(!acceptButton())return;
  const int effect=(int)(intptr_t)lv_event_get_user_data(e);
  const bool enabled=effect==0?synth->patch.fx.chorus.on:synth->patch.fx.reverb.on;
  if(sendCmd(effect==0?CHORUS:REVERB,0,enabled?0:1)){
    queueUiMidiCC(effect==0?SYN_CC_CHORUS:SYN_CC_REVERB,0,enabled?0:127);
    // Refresh from authoritative DSP state in loop(), after the audio task applies it.
    fxChorusShown=fxReverbShown=-1;
  }
}
static const char *pages[7]={"OSC","MIX","FILTER","F-ENV","A-ENV","LFO","FX"};
struct ParamDef {uint8_t id;const char*title;};
static const ParamDef defs[7][6]={
  {{0,"OSC1 WAVE"},{1,"OSC2 WAVE"},{2,"OSC1 PWM"},{3,"OSC2 PWM"},{4,"DETUNE"},{38,"OSC SYNC"}},
  {{5,"OSC1 LEVEL"},{6,"OSC2 LEVEL"},{7,"NOISE"},{8,"CROSS 1"},{9,"CROSS 2"},{39,"RING MOD"}},
  {{34,"CUTOFF"},{35,"RESONANCE"},{36,"FILTER TYPE"},{10,"ENV AMOUNT"},{11,"KEY TRACK"},{33,"PINK NOISE"}},
  {{12,"FLT ATTACK"},{13,"FLT DECAY"},{14,"FLT SUSTAIN"},{15,"FLT RELEASE"},{255,""},{255,""}},
  {{16,"AMP ATTACK"},{17,"AMP DECAY"},{18,"AMP SUSTAIN"},{19,"AMP RELEASE"},{255,""},{255,""}},
  {{20,"LFO1 RATE"},{21,"LFO1 DEPTH"},{22,"LFO1 WAVE"},{23,"LFO2 RATE"},{24,"LFO2 DEPTH"},{25,"LFO2 WAVE"}},
  {{26,"CHORUS MIX"},{27,"RATE"},{28,"DEPTH"},{29,"REVERB MIX"},{30,"SIZE"},{31,"DAMPING"}}
};
static int qto127(int32_t q){int64_t v=((int64_t)q*127+32768)/65536;return (int)constrain(v,0,127);}
static int valFor(uint8_t id) {
  const sc_layer_t &p=synth->patch.layer[uiLayer];
  switch(id){
    case 0:return p.o1_wave*127/4;case 1:return p.o2_wave*127/4;
    case 2:return qto127(p.o1_pwm);case 3:return qto127(p.o2_pwm);
    case 4:return constrain(64+(int)((int64_t)p.o2_detune*64/(100*65536)),0,127);
    case 5:return qto127(p.mix_o1);case 6:return qto127(p.mix_o2);
    case 7:return qto127(p.mix_noise);case 8:return qto127(p.cross);
    case 9:return qto127(p.cross2);case 10:return constrain((int)((int64_t)p.env_amt*127/(4LL*65536)),0,127);
    case 11:return qto127(p.key_track);
    case 12:return constrain((int)((int64_t)p.env_f.a*16/65536),0,127);
    case 13:return constrain((int)((int64_t)p.env_f.d*16/65536),0,127);
    case 14:return qto127(p.env_f.s);
    case 15:return constrain((int)((int64_t)p.env_f.r*16/65536),0,127);
    case 16:return constrain((int)((int64_t)p.env_a.a*16/65536),0,127);
    case 17:return constrain((int)((int64_t)p.env_a.d*16/65536),0,127);
    case 18:return qto127(p.env_a.s);
    case 19:return constrain((int)((int64_t)p.env_a.r*16/65536),0,127);
    case 20:return constrain((int)((int64_t)p.lfo[0].rate*127/(20*65536)),0,127);
    case 21:return qto127(p.lfo[0].depth_a);case 22:return p.lfo[0].shape*127/5;
    case 23:return constrain((int)((int64_t)p.lfo[1].rate*127/(20*65536)),0,127);
    case 24:return qto127(p.lfo[1].depth_a);case 25:return p.lfo[1].shape*127/5;
    case 26:return qto127(synth->patch.fx.chorus.mix);
    case 27:return constrain((int)((int64_t)synth->patch.fx.chorus.rate*127/(10*65536)),0,127);
    case 28:return qto127(synth->patch.fx.chorus.depth);
    case 29:return qto127(synth->patch.fx.reverb.mix);
    case 30:return qto127(synth->patch.fx.reverb.size);
    case 31:return qto127(synth->patch.fx.reverb.damping);
    case 32:return constrain(64+(int)((int64_t)p.pan*64/65536),0,127);
    case 33:return p.noise_pink?127:0;
    case 38:return p.o2_sync?127:0;
    case 39:return p.ring?127:0;
    case 34:return qto127(p.cutoff);case 35:return qto127(p.reso);
    case 36:return p.flt_mode*127/4;
    case 37:return p.ring?127:(p.o2_sync?64:0);
    case 58:return syntechEnv1CutToSlider(p.mm[SC_MM_E1_CUT]);
    case 52:return qto127(p.lfo[0].depth_b);
    case 53:return qto127(p.lfo[1].depth_b);
    case 54:return constrain((int)((int64_t)p.lfo[0].delay*16/65536),0,127);
    case 55:return constrain((int)((int64_t)p.lfo[1].delay*16/65536),0,127);
    case 56:return p.lfo[0].sync?127:0;
    case 57:return p.lfo[1].sync?127:0;
    case 40:case 41:case 42:case 43:case 44:case 45:
    case 46:case 47:case 48:case 49:case 50:case 51: {
      const int lfo=(id-40)/6, route=(id-40)%6;
      const int mmIndex=LFO_MATRIX_ROUTE[lfo][route];
      const int64_t signedAmount=p.mm[mmIndex];
      // Negative side has 64 steps (0..64), positive side 63 (64..127).
      // Preserve all endpoints: -1.0->0, 0.0->64, +1.0->127.
      return constrain(64+(int)(signedAmount*(signedAmount<0?64:63)/65536),0,127);
    }
  }
  return 0;
}
static lv_obj_t *button(lv_obj_t *parent,const char*label,int x,int y,int w,int h,lv_event_cb_t cb,intptr_t id){
  lv_obj_t*b=lv_btn_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
  lv_obj_set_style_bg_color(b,COL_PANEL,0);lv_obj_set_style_radius(b,7,0);
  lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_color(b,lv_color_hex(0x3E9FC4),0);
  lv_obj_set_style_pad_all(b,0,0);
  if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,(void*)id);
  lv_obj_t *text=lv_label_create(b);lv_label_set_text(text,label);
  lv_obj_set_style_text_color(text,lv_color_white(),0);
  lv_obj_center(text);return b;
}
static void buildPage();
static void updateHeader(){
  char text[80];
  if(touchNavState==TOUCH_NAV_HELD) {
    // A leading > denotes PREVIEW ONLY: loaded sound remains unchanged.
    snprintf(text,sizeof(text),">%03d  %.24s",touchNavTarget,
             presetNameValid[touchNavTarget]?presetNameCache[touchNavTarget]:"INVALID");
  } else {
    snprintf(text,sizeof(text),"%03d  %.26s",uiPreset,currentPresetName);
  }
  lv_label_set_text(presetLabel,text);
  lv_label_set_text(layerLabel,uiLayer?"LOWER":"UPPER");
  static const char*modes[]={"SINGLE","SPLIT","DOUBLE"};lv_label_set_text(modeLabel,modes[uiMode]);
}
static void onPreset(lv_event_t*e){
  if(lv_event_get_code(e)!=LV_EVENT_PRESSED)return;
  const int step=(int)(intptr_t)lv_event_get_user_data(e);
  if(step!=1 && step!=-1)return;
  if(touchNavState!=TOUCH_NAV_IDLE) {
    ++ignoredTouchPresetClicks;
    return; // repeated touch events cannot start another browse gesture
  }
  // GT911 event marks the start of exactly one gesture, not a CLICKED pulse.
  touchNavDirection=(int8_t)step;
  touchNavState=TOUCH_NAV_HELD;
  touchNavCanceled=false;
  touchNavPressMs=millis();
  touchNavLastStepMs=touchNavPressMs;
  touchNavTarget=(current_preset+USER_SLOTS+step)%USER_SLOTS;
  ++touchNavSteps;
  updateHeader();
}
static void serviceTouchPresetNavigation(){
  const uint32_t now=millis();
  if(touchNavState==TOUCH_NAV_IDLE)return;
  if(touchNavState==TOUCH_NAV_COOLDOWN) {
    // Prevent re-triggering if GT911 emits extra press/release pairs on the same finger lift.
    if(!Board::touchPressed() &&
       (uint32_t)(now-Board::touchLastChangeMs())>=TOUCH_PRESET_RELEASE_MS &&
       (uint32_t)(now-touchNavCommitMs)>=TOUCH_PRESET_REARM_MS) {
      touchNavState=TOUCH_NAV_IDLE;
      touchNavDirection=0;
    }
    return;
  }
  // MIDI / Serial / RESTORE program changes override an unfinished touch browse.
  if(requestedPreset>=0 && requestedPresetOrigin!=PC_ORIGIN_TOUCH)touchNavCanceled=true;
  if(Board::touchPressed()) {
    if(touchNavCanceled)return;
    const uint32_t holdMs=(uint32_t)(now-touchNavPressMs);
    // First repeat after 500 ms; speed ramps 250 -> 150 -> 75 ms.
    const uint32_t repeatMs=holdMs<500?500:(holdMs<1500?250:(holdMs<3000?150:75));
    if((uint32_t)(now-touchNavLastStepMs)>=repeatMs) {
      touchNavTarget=(touchNavTarget+USER_SLOTS+touchNavDirection)%USER_SLOTS;
      touchNavLastStepMs=now;
      ++touchNavSteps;
      updateHeader();
    }
    return;
  }
  // Do not mistake momentary GT911 dropouts for a real release.
  if((uint32_t)(now-Board::touchLastChangeMs())<TOUCH_PRESET_RELEASE_MS)return;
  touchNavState=TOUCH_NAV_COOLDOWN;
  touchNavCommitMs=now;
  touchNavDirection=0;
  if(touchNavCanceled || requestedPreset>=0 || touchNavTarget==current_preset) {
    if(touchNavCanceled || requestedPreset>=0)++touchNavCancels;
    uiPreset=current_preset;
    updateHeader();
    return;
  }
  requestedPresetOrigin=PC_ORIGIN_TOUCH;
  requestedPreset=touchNavTarget;  // exactly ONE SD read on release
  ++touchNavCommits;
  // The real header will be refreshed when the SD load finishes below.
  updateHeader();
}
static void onLayer(lv_event_t*e){(void)e;if(!acceptButton())return;uiLayer^=1;sendCmd(SELECT_LAYER,0,uiLayer);updateHeader();buildPage();}
static void onMode(lv_event_t*e){(void)e;if(!acceptButton())return;
  // Use the last confirmed audio-engine state, not a stale GUI-only mode.
  const int baseMode=(publishedVoiceMode>=SC_VM_SINGLE && publishedVoiceMode<=SC_VM_DOUBLE)
    ?publishedVoiceMode:uiMode;
  const int nextMode=(baseMode+1)%3;
  if(sendCmd(MODE,0,nextMode)){uiMode=nextMode;updateHeader();}
}
static void onPage(lv_event_t*e){if(!acceptButton())return;uiPage=(int)(intptr_t)lv_event_get_user_data(e);buildPage();}
static void onLfoSelect(lv_event_t*e){
  if(!acceptButton())return;
  uiLfo=(uint8_t)(intptr_t)lv_event_get_user_data(e);buildPage();
}
static void onLfoTab(lv_event_t*e){
  if(!acceptButton())return;
  uiLfoTab=(uint8_t)(intptr_t)lv_event_get_user_data(e);buildPage();
}
// UI-owned pending values ensure the final position is never lost to throttling.
static bool sliderPending[6]={};
static int16_t sliderPendingValue[6]={};
static uint8_t sliderPendingId[6]={};
static void dispatchSliderValue(uint8_t id,int value){
  bool queued=false;
  if(id==34)queued=sendCmd(CUTOFF,0,value);
  else if(id==35)queued=sendCmd(RESO,0,value);
  else if(id==36)queued=sendCmd(FILTER_MODE,0,value*5/128);
  else queued=sendCmd(EDIT_PARAM,id,value);
  if(queued)queueUiMidiCC(id==34?SYN_CC_CUTOFF:id==35?SYN_CC_RESO:id==36?SYN_CC_FILTER_MODE:SYN_CC_EDIT,id,(uint8_t)value);
}
static void flushSliderPending(){
  for(int i=0;i<6;i++)if(sliderPending[i]){
    const uint8_t id=sliderPendingId[i];
    const int value=sliderPendingValue[i];
    CmdType type=id==34?CUTOFF:id==35?RESO:id==36?FILTER_MODE:EDIT_PARAM;
    int val=id==36?value*5/128:value;
    if(sendCmd(type,id==34||id==35||id==36?0:id,val)){
      sliderPending[i]=false;lastSliderValue[i]=value;lastSliderMs[i]=millis();
      queueUiMidiCC(id==34?SYN_CC_CUTOFF:id==35?SYN_CC_RESO:id==36?SYN_CC_FILTER_MODE:SYN_CC_EDIT,id,(uint8_t)value);
    }
  }
}
// FIX7 PARAM INFO: The footer updates immediately. The popup is opened
// only by serviceParamInfoPopup() while the label is STILL held at 1500 ms.
// Slider activity cancels any pending hold; MIDI Learn uses the popup button.
static lv_obj_t *paramInfoPopup=nullptr;
static int paramInfoSlot=-1;
static void onParamInfoMidiLearn(lv_event_t*e);
static lv_obj_t *paramInfoValue=nullptr;
static lv_obj_t *paramInfoDesc=nullptr;
static lv_obj_t *paramInfoInline=nullptr;
static uint8_t paramInfoId=255;
static uint8_t paramInfoPopupId=255; // independently tracks the popup header
static uint32_t paramInfoLastUpdate=0;
static int paramInfoLastValue=-1;
static const char* parameterDescription(uint8_t id){
  switch(id){
    case 0:case 1:return "Oszillator-Wellenform: Dreieck, Saegezahn, Rechteck, Sinus oder Rauschen.";
    case 2:case 3:return "Pulsbreite der Rechteckwelle; veraendert Obertoene und Klangfarbe.";
    case 4:return "Feinverstimmung des zweiten Oszillators gegenueber OSC1.";
    case 5:case 6:return "Lautstaerkeanteil dieses Oszillators im Mix.";
    case 7:return "Rauschanteil im Oszillatormischer.";
    case 8:return "CROSS1: Kreuzmodulation zwischen den Oszillatoren.";
    case 9:return "CROSS2: zusaetzliche gekoppelte Oszillatormodulation.";
    case 10:return "Staerke der Filterhuellkurve auf die Filterfrequenz.";
    case 11:return "Keyboard-Tracking: Filterfrequenz folgt der gespielten Note.";
    case 12:case 16:return "Attack: Anstiegszeit der Huellkurve.";
    case 13:case 17:return "Decay: Abfallzeit bis zum Sustain-Pegel.";
    case 14:case 18:return "Sustain: Haltepegel waehrend eine Taste gedrueckt ist.";
    case 15:case 19:return "Release: Ausklingzeit nach Loslassen der Taste.";
    case 20:case 23:return "LFO-Geschwindigkeit (0 bis 20 Hz laut Parameterkennlinie).";
    case 21:case 24:return "LFO-Modulationstiefe fuer Pitch/PWM.";
    case 22:case 25:return "LFO-Wellenform: TRI, SQR, S&H, RANDOM, RAMP oder SQRU.";
    case 26:return "Anteil des Chorus-Effekts am Signal.";
    case 27:return "Chorus-Modulationsgeschwindigkeit (0 bis 10 Hz).";
    case 28:return "Modulationstiefe des Chorus-Effekts.";
    case 29:return "Anteil des Reverb-Effekts am Signal.";
    case 30:return "Groesse des simulierten Hallraums.";
    case 31:return "Daempfung der hohen Frequenzen im Hall.";
    case 33:return "Rauschfarbe: OFF = Weiss, ON = Pink.";
    case 34:return "Filter-Cutoff: Grenz- bzw. Mittenfrequenz des Filters.";
    case 35:return "Filter-Resonanz: Betonung um die Grenzfrequenz.";
    case 36:return "Filtertyp: LP24, LP12, BP12, BP6 oder HP12.";
    case 58:return "ENV1 auf Cutoff: 64 = keine Modulation, 0 = negativ, 127 = positiv. Im Preset gespeichert.";
    case 38:return "Hard Sync: OSC2 wird an OSC1 synchronisiert.";
    case 39:return "Ringmodulation der Oszillatoren ein- oder ausschalten.";
    case 52:case 53:return "Zweite LFO-Modulationstiefe fuer Filter/Amplitude.";
    case 54:case 55:return "Verzoegerung bis zum Einsatz des LFOs.";
    case 56:case 57:return "Synchronisierung des LFOs ein- oder ausschalten.";
    default:if(id>=40&&id<=51)return "Bipolare LFO-Matrixroute: 64 = neutral, darunter negativ, darueber positiv.";
      return "Parameterwert der aktiven Synthesizer-Ebene.";
  }
}
static const char* parameterTitle(uint8_t id){
  for(int p=0;p<7;p++)for(int i=0;i<6;i++)if(defs[p][i].id==id)return defs[p][i].title;
  if(id>=40&&id<=51){static const char*names[]={"OSC1 PITCH","OSC2 PITCH","OSC1 PWM","OSC2 PWM","FILTER","AMPLITUDE"};return names[(id-40)%6];}
  if(id==58)return "ENV1 -> CUTOFF";
  if(id==52||id==53)return "LFO FLT/AMP";
  if(id==54||id==55)return "LFO DELAY";
  if(id==56||id==57)return "LFO SYNC";
  return "PARAMETER";
}
static void parameterValueText(uint8_t id,int v,char*out,size_t cap){
  if(id==0||id==1){const char*names[]={"TRIANGLE","SAWTOOTH","SQUARE","SINE","NOISE"};snprintf(out,cap,"%s (%d/127)",names[constrain(v*5/128,0,4)],v);return;}
  if(id==22||id==25){const char*names[]={"TRI","SQUARE","SAMPLE&HOLD","RANDOM","RAMP","SQRU"};snprintf(out,cap,"%s (%d/127)",names[constrain(v*6/128,0,5)],v);return;}
  if(id==36){const char*names[]={"LP24","LP12","BP12","BP6","HP12"};snprintf(out,cap,"%s (%d/127)",names[constrain(v*5/128,0,4)],v);return;}
  if(id==33||id==38||id==39||id==56||id==57){snprintf(out,cap,"%s (%d/127)",v>=64?"ON":"OFF",v);return;}
  if((id>=40&&id<=51)||id==58){snprintf(out,cap,"%+d (%d/127; 64=OFF)",v-64,v);return;}
  if(id==20||id==23){snprintf(out,cap,"%.2f Hz (%d/127)",20.0f*v/127.0f,v);return;}
  if(id==27){snprintf(out,cap,"%.2f Hz (%d/127)",10.0f*v/127.0f,v);return;}
  snprintf(out,cap,"%d/127 (%d%%)",v,(v*100+63)/127);
}
static void parameterInfoRefresh(uint8_t id,int v){
  char value[72],line[150];parameterValueText(id,v,value,sizeof(value));
  snprintf(line,sizeof(line),"%s: %s",parameterTitle(id),value);
  if(paramInfoInline)lv_label_set_text(paramInfoInline,line);
  if(paramInfoPopup && paramInfoPopupId==id && paramInfoValue)lv_label_set_text(paramInfoValue,line);
  paramInfoId=id;paramInfoLastValue=v;paramInfoLastUpdate=millis();
}
static void closeParamInfo(lv_event_t*e){
  (void)e;
  syntechParamInfoCancel(paramInfoGate);
  paramInfoSlot=-1;paramInfoPopupId=255;
  if(paramInfoPopup){lv_obj_del(paramInfoPopup);paramInfoPopup=nullptr;paramInfoValue=nullptr;paramInfoDesc=nullptr;}
}
static void showParamInfo(int slot){
  if(slot<0||slot>=6||controlIds[slot]==255||midiPanelShown||bankShown)return;
  const uint8_t id=controlIds[slot];const int v=controlSliders[slot]?lv_slider_get_value(controlSliders[slot]):valFor(id);
  if(paramInfoPopup)closeParamInfo(nullptr);
  paramInfoSlot=slot;
  paramInfoPopupId=id; // set BEFORE initial refresh (FIX3 could leave default "Text")
  lv_obj_t*root=lv_scr_act();
  paramInfoPopup=lv_obj_create(root);lv_obj_set_pos(paramInfoPopup,10,146);lv_obj_set_size(paramInfoPopup,460,105);
  lv_obj_set_style_bg_color(paramInfoPopup,lv_color_hex(0x122B40),0);
  lv_obj_set_style_border_color(paramInfoPopup,lv_color_hex(0x44BFD5),0);
  lv_obj_set_style_border_width(paramInfoPopup,1,0);lv_obj_set_style_pad_all(paramInfoPopup,8,0);
  lv_obj_clear_flag(paramInfoPopup,LV_OBJ_FLAG_SCROLLABLE);
  paramInfoValue=lv_label_create(paramInfoPopup);lv_obj_set_width(paramInfoValue,430);lv_obj_set_pos(paramInfoValue,2,2);
  lv_obj_set_style_text_color(paramInfoValue,lv_color_white(),0);
  // Explicitly initialize the first line; never show LVGL's default "Text".
  char infoValue[72],infoLine[150];
  parameterValueText(id,v,infoValue,sizeof(infoValue));
  snprintf(infoLine,sizeof(infoLine),"%s: %s",parameterTitle(id),infoValue);
  lv_label_set_text(paramInfoValue,infoLine);
  paramInfoDesc=lv_label_create(paramInfoPopup);lv_obj_set_width(paramInfoDesc,430);lv_obj_set_pos(paramInfoDesc,2,28);
  lv_label_set_long_mode(paramInfoDesc,LV_LABEL_LONG_WRAP);
  lv_label_set_text(paramInfoDesc,parameterDescription(id));lv_obj_set_style_text_color(paramInfoDesc,lv_color_hex(0xBBDDE7),0);
  lv_obj_t*learn=lv_btn_create(paramInfoPopup);lv_obj_set_pos(learn,272,65);lv_obj_set_size(learn,100,28);
  if(id!=58)lv_obj_add_event_cb(learn,onParamInfoMidiLearn,LV_EVENT_CLICKED,nullptr);
  else lv_obj_add_state(learn,LV_STATE_DISABLED); // no unassigned MIDI CC / learn index
  lv_obj_t*learnLabel=lv_label_create(learn);
  lv_label_set_text(learnLabel,id==58?"GUI ONLY":"MIDI LEARN");lv_obj_center(learnLabel);
  lv_obj_t*close=lv_btn_create(paramInfoPopup);lv_obj_set_pos(close,382,65);lv_obj_set_size(close,52,28);
  lv_obj_add_event_cb(close,closeParamInfo,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*label=lv_label_create(close);lv_label_set_text(label,"OK");lv_obj_center(label);
  parameterInfoRefresh(id,v);
}
static void serviceParamInfoPopup(uint32_t now){
  int16_t tx=0,ty=0;
  const bool physicalPressed=Board::touchPosition(tx,ty);
  // Once opened, ignore residual LVGL PRESSING until a genuine release.
  if(sliderTouchInfoFired){
    if(physicalPressed)sliderTouchReleaseSinceMs=0;
    else{
      if(!sliderTouchReleaseSinceMs)sliderTouchReleaseSinceMs=now;
      if((uint32_t)(now-sliderTouchReleaseSinceMs)>=SYNTECH_PARAM_INFO_RELEASE_GRACE_MS){
        sliderTouchInfoFired=false;
        sliderTouchReleaseSinceMs=0;
      }
    }
  }
  if(!sliderTouchHeld || paramInfoGate.slot<0)return;
  const int slot=sliderTouchSlot;
  // Exactly the mechanism previously used for MIDI Learn: no immediate
  // cancellation from GT911 release glitches; no trigger after a short tap.
  if(!syntechParamInfoEventFresh(now,sliderTouchLastEventMs)){
    Serial.printf("[PARAM INFO7] CANCEL release/event gap slot=%d\n",slot);
    syntechParamInfoCancel(paramInfoGate);
    sliderTouchHeld=false;sliderTouchSlot=-1;
    return;
  }
  if(slot<0 || slot>=6 || midiPanelShown || bankShown || paramInfoPopup ||
     uiPage!=paramInfoGate.page || uiLayer!=paramInfoGate.layer ||
     current_preset!=paramInfoGate.preset || controlIds[slot]!=paramInfoGate.id ||
     !controlLabelHits[slot]){
    syntechParamInfoCancel(paramInfoGate);
    sliderTouchHeld=false;sliderTouchSlot=-1;
    Serial.println("[PARAM INFO7] CANCEL context");
    return;
  }
  if(physicalPressed){
    lv_area_t rect;lv_obj_get_coords(controlLabelHits[slot],&rect);
    if(!syntechParamInfoStationary(sliderTouchOriginX,sliderTouchOriginY,tx,ty) ||
       !syntechParamInfoInRect(tx,ty,rect.x1-6,rect.y1-6,rect.x2+6,rect.y2+6)){
      syntechParamInfoCancel(paramInfoGate);
      sliderTouchHeld=false;sliderTouchSlot=-1;
      Serial.printf("[PARAM INFO7] CANCEL movement x=%d y=%d\n",(int)tx,(int)ty);
      return;
    }
  }
  // Physical touch is mandatory at the instant of opening. A released tap
  // cannot open after 1.5 seconds, even during the GT911 glitch grace window.
  if(!syntechParamInfoHeldReady(paramInfoGate,now,sliderTouchLastEventMs,physicalPressed))return;
  sliderTouchInfoFired=true;
  sliderTouchReleaseSinceMs=0;
  syntechParamInfoCancel(paramInfoGate);
  sliderTouchHeld=false;sliderTouchSlot=-1;
  Serial.printf("[PARAM INFO7] OPEN slot=%d hold=%lums\n",slot,
                (unsigned long)(now-paramInfoGate.pressedMs));
  showParamInfo(slot);
}
static void onParam(lv_event_t*e){
  const lv_event_code_t code=lv_event_get_code(e);
  if(code!=LV_EVENT_VALUE_CHANGED && code!=LV_EVENT_RELEASED && code!=LV_EVENT_PRESS_LOST)return;
  if(midiPanelShown)return;
  const int index=(int)(intptr_t)lv_event_get_user_data(e);
  if(index<0||index>=6)return;
  const uint8_t id=controlIds[index];if(id==255)return;
  const int value=lv_slider_get_value(lv_event_get_target(e));
  syntechParamInfoSliderActivity(paramInfoGate,millis());
  if(sliderTouchHeld){
    sliderTouchHeld=false;sliderTouchSlot=-1;sliderTouchInfoFired=true;
    Serial.println("[PARAM INFO7] CANCEL slider activity");
  }
  if(paramInfoPopup)closeParamInfo(nullptr); // sliders always take priority over overlay
  sliderPending[index]=true;sliderPendingValue[index]=value;sliderPendingId[index]=id;
  parameterInfoRefresh(id,value);
  const uint32_t now=millis();
  if(code!=LV_EVENT_VALUE_CHANGED || !lastSliderMs[index] || (uint32_t)(now-lastSliderMs[index])>=SLIDER_INTERVAL_MS)
    flushSliderPending();
}

static void addEditorSlider(int slot, uint8_t id, const char*label, int x, int y, int width){
  if(slot<0||slot>=6)return;
  // FIX21: Dedicated transparent hit box. The LVGL label itself is too small
  // and is not a reliable long-press target on the 480x272 GT911 screen.
  // Keep the box entirely to the LEFT of the slider to avoid stealing drags.
  const int sliderOffset=(width==231?106:115);
  lv_obj_t *hit=lv_obj_create(pageArea);
  lv_obj_set_pos(hit,x,y);
  lv_obj_set_size(hit,sliderOffset-3,(uiPage==5 && uiLfoTab==2)?22:30);
  lv_obj_set_style_bg_opa(hit,LV_OPA_TRANSP,0);
  lv_obj_set_style_border_width(hit,0,0);
  lv_obj_set_style_pad_all(hit,0,0);
  lv_obj_clear_flag(hit,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hit,LV_OBJ_FLAG_CLICKABLE);
  // FIX7: same LVGL label events as the former MIDI-Learn hold.
  // LVGL retains the original pressed object, so slider drags cannot arm it.
  lv_obj_add_event_cb(hit,onParameterLabelHoldEvent,LV_EVENT_PRESSED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(hit,onParameterLabelHoldEvent,LV_EVENT_PRESSING,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(hit,onParameterLabelHoldEvent,LV_EVENT_LONG_PRESSED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(hit,onParameterLabelHoldEvent,LV_EVENT_RELEASED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(hit,onParameterLabelHoldEvent,LV_EVENT_PRESS_LOST,(void*)(intptr_t)slot);
  controlLabelHits[slot]=hit;
  lv_obj_t *name=lv_label_create(hit);
  lv_label_set_text(name,label);lv_obj_set_pos(name,3,4);
  lv_obj_set_style_text_color(name,lv_color_white(),0);
  // The hit box, not the label, owns the touch.
  lv_obj_clear_flag(name,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *sl=lv_slider_create(pageArea);
  controlSliders[slot]=sl;controlIds[slot]=id;
  const bool compactMatrixB=(uiPage==5 && uiLfoTab==2);
  lv_obj_set_pos(sl,x+(width==231?106:115),y+(compactMatrixB?5:8));
  lv_obj_set_size(sl,width==231?112:width-130,12);
  lv_obj_set_ext_click_area(sl,compactMatrixB?5:12); // avoid 4-row overlap
  lv_slider_set_range(sl,0,127);
  lv_slider_set_value(sl,valFor(id),LV_ANIM_OFF);
  lv_obj_add_event_cb(sl,onParam,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(sl,onParam,LV_EVENT_RELEASED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(sl,onParam,LV_EVENT_PRESS_LOST,(void*)(intptr_t)slot);
}
static void buildPage(){
  ++gui7xPageBuilds;
  flushSliderPending();
  // Preserve the pressed label during the physical 1.5s hold.
  if(sliderTouchHeld)return;
  fxChorusButton=fxReverbButton=nullptr;fxChorusShown=fxReverbShown=-1;
  for(int i=0;i<6;i++){lastSliderValue[i]=-1;lastSliderMs[i]=0;controlSliders[i]=nullptr;controlLabelHits[i]=nullptr;controlIds[i]=255;}
  syntechParamInfoCancel(paramInfoGate);
  if(paramInfoPopup)closeParamInfo(nullptr);
  if(pageArea)lv_obj_clean(pageArea);
  for(int i=0;i<7;i++){
    lv_obj_set_style_bg_color(pageButtons[i],i==uiPage?COL_ACC:COL_PANEL,0);
    lv_obj_set_style_text_color(lv_obj_get_child(pageButtons[i],0),i==uiPage?lv_color_hex(0x06192B):lv_color_white(),0);
  }
  if(uiPage==6){
    // Two FX enable switches above the six existing parameters. State is read
    // from the DSP patch, so incoming MIDI CC115/CC86 is reflected on screen.
    fxChorusButton=button(pageArea,"CHORUS ON/OFF",5,1,224,27,onFxToggle,0);
    fxReverbButton=button(pageArea,"REVERB ON/OFF",239,1,224,27,onFxToggle,1);
    refreshFxButtons();
    for(int i=0;i<6;i++){
      const int col=i/3,row=i%3;
      const ParamDef &d=defs[6][i];
      addEditorSlider(i,d.id,d.title,col*231,31+row*29,231);
    }
    return;
  }
  if(uiPage==5){
    // FIX24: five equal-height, deliberately aligned selectors (no blank gaps).
    // LFO 1 / LFO 2 select the oscillator; MAIN / MATRIX A / MATRIX B select the view.
    lv_obj_t *b0=button(pageArea,"LFO 1",2,1,83,28,onLfoSelect,0);
    lv_obj_t *b1=button(pageArea,"LFO 2",89,1,83,28,onLfoSelect,1);
    lv_obj_t *t0=button(pageArea,"MAIN",182,1,73,28,onLfoTab,0);
    lv_obj_t *t1=button(pageArea,"MATRIX A",259,1,99,28,onLfoTab,1);
    lv_obj_t *t2=button(pageArea,"MATRIX B",362,1,104,28,onLfoTab,2);
    lv_obj_set_style_bg_color(uiLfo?b1:b0,COL_ACC,0);
    lv_obj_set_style_bg_color(uiLfoTab==0?t0:(uiLfoTab==1?t1:t2),COL_ACC,0);
    lv_obj_set_style_text_color(lv_obj_get_child(uiLfo?b1:b0,0),lv_color_hex(0x06192B),0);
    lv_obj_set_style_text_color(lv_obj_get_child(uiLfoTab==0?t0:(uiLfoTab==1?t1:t2),0),lv_color_hex(0x06192B),0);
    if(uiLfoTab==0){
      const uint8_t ids[2][6]={{20,21,52,22,54,56},{23,24,53,25,55,57}};
      const char *labels[6]={"RATE","PITCH/PWM","FLT/AMP","WAVE","DELAY","SYNC"};
      for(int i=0;i<6;i++)addEditorSlider(i,ids[uiLfo][i],labels[i],(i/3)*231,32+(i%3)*31,231);
    }else{
      static const char* names[6]={"OSC1 PITCH","OSC2 PITCH","OSC1 PWM","OSC2 PWM","FILTER","AMPLITUDE"};
      const int offset=uiLfoTab==1?0:3;
      if(uiLfoTab==2) {
        // FIX4: four compact rows fit the 122px editor area; the fourth
        // is an independent ENV1 matrix route (not an LFO depth control).
        for(int i=0;i<3;i++){
          const int route=offset+i;
          addEditorSlider(i,(uint8_t)(40+6*uiLfo+route),names[route],0,31+i*23,450);
        }
        addEditorSlider(3,58,"ENV1 -> CUTOFF",0,100,450);
      } else {
        for(int i=0;i<3;i++){
          const int route=offset+i;
          addEditorSlider(i,(uint8_t)(40+6*uiLfo+route),names[route],0,35+i*29,450);
        }
      }
    }
    return;
  }
  for(int i=0;i<6;i++){
    const int col=i/3,row=i%3;const ParamDef &d=defs[uiPage][i];
    if(d.id==255)continue;
    addEditorSlider(i,d.id,d.title,col*231,row*43,231);
  }
}
static void uiInit(){
  lv_obj_t *root=lv_scr_act();lv_obj_set_style_bg_color(root,COL_BG,0);
  lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);lv_obj_set_style_border_width(root,0,0);
  lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*t=lv_label_create(root);lv_label_set_text(t,"RealTimeAudioLab SYNTECH");
  lv_obj_set_pos(t,9,4);lv_obj_set_style_text_color(t,COL_ACC,0);
  // PRESET arrows use PRESSED, not CLICKED. Click triggers are never used here.
  lv_obj_t *prevPreset=button(root,"<",6,26,36,30,nullptr,-1);
  lv_obj_add_event_cb(prevPreset,onPreset,LV_EVENT_PRESSED,(void*)(intptr_t)-1);
  presetLabel=lv_label_create(root);lv_obj_set_pos(presetLabel,50,34);
  lv_obj_set_style_text_color(presetLabel,lv_color_white(),0);
  lv_obj_t *nextPreset=button(root,">",438,26,36,30,nullptr,1);
  lv_obj_add_event_cb(nextPreset,onPreset,LV_EVENT_PRESSED,(void*)(intptr_t)1);
  button(root,"LAYER",8,62,74,29,onLayer,0);
  layerLabel=lv_label_create(root);lv_obj_set_pos(layerLabel,87,70);
  button(root,"MODE",145,62,75,29,onMode,0);
  modeLabel=lv_label_create(root);lv_obj_set_pos(modeLabel,226,70);
  lv_obj_set_style_text_color(layerLabel,lv_color_white(),0);
  lv_obj_set_style_text_color(modeLabel,lv_color_white(),0);
  // Evenly spaced navigation, same width and gaps across the 480px panel.
  for(int i=0;i<7;i++)pageButtons[i]=button(root,pages[i],4+i*68,96,64,27,onPage,i);
  pageArea=lv_obj_create(root);lv_obj_set_pos(pageArea,4,127);lv_obj_set_size(pageArea,472,122);
  lv_obj_set_style_bg_color(pageArea,COL_BG,0);
  lv_obj_set_style_radius(pageArea,5,0);
  lv_obj_set_style_border_width(pageArea,0,0);lv_obj_set_style_pad_all(pageArea,0,0);
  lv_obj_clear_flag(pageArea,LV_OBJ_FLAG_SCROLLABLE);
  paramInfoInline=lv_label_create(root);lv_obj_set_pos(paramInfoInline,9,250);
  lv_obj_set_width(paramInfoInline,460);lv_label_set_long_mode(paramInfoInline,LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(paramInfoInline,lv_color_white(),0);
  lv_label_set_text(paramInfoInline,"PARAM INFO: Name 1,5s halten = Hilfe; MIDI LEARN im Fenster");
  // SD user preset overlay; NO virtual keyboard required for playback.
  button(root,"MIDI",319,62,72,29,onMidiPanel,0);
  button(root,"BANK",395,62,78,29,onBankOpen,0);
  bankPanel=lv_obj_create(root);lv_obj_set_pos(bankPanel,10,28);lv_obj_set_size(bankPanel,460,232);
  lv_obj_set_style_bg_color(bankPanel,COL_BG,0);
  lv_obj_set_style_border_color(bankPanel,COL_ACC,0);
  lv_obj_set_style_pad_all(bankPanel,4,0);
  lv_obj_clear_flag(bankPanel,LV_OBJ_FLAG_SCROLLABLE);
  bankSlotLabel=lv_label_create(bankPanel);lv_obj_set_pos(bankSlotLabel,10,9);
  lv_obj_set_style_text_color(bankSlotLabel,lv_color_white(),0);
  lv_obj_t *namelabel=lv_label_create(bankPanel);lv_label_set_text(namelabel,"NAME (tap to edit)");lv_obj_set_pos(namelabel,10,43);
  lv_obj_set_style_text_color(namelabel,lv_color_white(),0);
  bankNameField=lv_textarea_create(bankPanel);lv_obj_set_pos(bankNameField,150,37);lv_obj_set_size(bankNameField,282,34);
  lv_textarea_set_one_line(bankNameField,true);lv_textarea_set_max_length(bankNameField,31);
  lv_textarea_set_text(bankNameField,"MY SYNTH PATCH");
  lv_obj_set_style_text_color(bankNameField,lv_color_hex(0x071B30),0);
  lv_obj_set_style_bg_color(bankNameField,lv_color_hex(0xA9E9FF),0);
  lv_obj_add_event_cb(bankNameField,onNameTap,LV_EVENT_CLICKED,nullptr);
  // Slot selection and actions; entering names is additionally supported via serial 'name'.
  button(bankPanel,"< SLOT",4,88,100,36,onBankAction,0);
  button(bankPanel,"SLOT >",112,88,100,36,onBankAction,1);
  button(bankPanel,"SAVE",225,88,102,36,onBankAction,2);
  button(bankPanel,"LOAD",336,88,102,36,onBankAction,3);
  button(bankPanel,"CLOSE",335,146,103,35,onBankAction,4);
  bankMessage=lv_label_create(bankPanel);lv_label_set_text(bankMessage,"Choose slot, then SAVE or LOAD");
  lv_obj_set_pos(bankMessage,9,156);
  lv_obj_set_style_text_color(bankMessage,lv_color_white(),0);
  createNameKeyboard(root);
  // MIDI overlay: no keyboard, no preset loading, no DSP changes.
  midiPanel=lv_obj_create(root);lv_obj_set_pos(midiPanel,10,24);lv_obj_set_size(midiPanel,460,238);
  lv_obj_set_style_bg_color(midiPanel,COL_BG,0);
  lv_obj_set_style_border_color(midiPanel,COL_ACC,0);
  lv_obj_set_style_pad_all(midiPanel,5,0);
  lv_obj_clear_flag(midiPanel,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *mt=lv_label_create(midiPanel);lv_label_set_text(mt,"MIDI SETUP");lv_obj_set_pos(mt,8,4);
  lv_obj_set_style_text_color(mt,COL_ACC,0);
  button(midiPanel,"CLOSE",360,0,88,28,onMidiPanel,0);
  midiInfoLabel=lv_label_create(midiPanel);lv_obj_set_pos(midiInfoLabel,8,34);
  lv_obj_set_style_text_color(midiInfoLabel,lv_color_white(),0);
  button(midiPanel,"MODE",290,38,154,30,onMidiPanel,1);
  button(midiPanel,"CH -",290,75,72,30,onMidiPanel,2);
  button(midiPanel,"CH +",370,75,74,30,onMidiPanel,3);
  button(midiPanel,"PARAM -",8,120,106,32,onMidiPanel,4);
  button(midiPanel,"PARAM +",122,120,106,32,onMidiPanel,5);
  button(midiPanel,"LEARN",238,120,100,32,onMidiPanel,6);
  button(midiPanel,"DEFAULT",345,120,100,32,onMidiPanel,7);
  midiLearnLabel=lv_label_create(midiPanel);lv_obj_set_pos(midiLearnLabel,8,173);
  lv_obj_set_style_text_color(midiLearnLabel,COL_ACC,0);
  lv_obj_add_flag(midiPanel,LV_OBJ_FLAG_HIDDEN);
  midiPanelRefresh();
  updateBankLabel();lv_obj_add_flag(bankPanel,LV_OBJ_FLAG_HIDDEN);
  updateHeader();buildPage();lastUiMs=millis();lv_obj_invalidate(root);
}
static void onParamInfoMidiLearn(lv_event_t*e){
  (void)e;
  const int slot=paramInfoSlot;
  if(paramInfoPopup){lv_obj_del(paramInfoPopup);paramInfoPopup=nullptr;paramInfoValue=nullptr;paramInfoDesc=nullptr;}
  paramInfoSlot=-1;paramInfoPopupId=255;
  syntechParamInfoCancel(paramInfoGate);
  if(slot>=0 && slot<6 && controlIds[slot]!=255){
    sliderTouchHeld=false;sliderTouchInfoFired=true;sliderTouchSlot=-1;
    onSliderLongPress(slot);
  }
}


static void uiLoop(){
  diag_gui_core=(uint32_t)xPortGetCoreID();
  const uint32_t uiNow=millis();
  static uint32_t lastUiCallMs=0;
  if(lastUiCallMs){const uint32_t gap=uiNow-lastUiCallMs; if(gap>diag_gui_max_gap_ms)diag_gui_max_gap_ms=gap;}
  lastUiCallMs=uiNow;
  const int64_t uiStart=esp_timer_get_time();
  Board::serviceLVGL();
  const uint32_t uiUs=(uint32_t)(esp_timer_get_time()-uiStart);
  if(uiUs>diag_gui_max_handler_us)diag_gui_max_handler_us=uiUs;
  if(uiUs>gui7xLvglMaxUs)gui7xLvglMaxUs=uiUs;
  diag_gui_calls++;
  flushSliderPending();uint32_t now=millis();
  serviceParamInfoPopup(now); // FIX7: MIDI-Learn-style LVGL hold + 250ms GT911 glitch tolerance
  if(now-lastUiMs>=300){lastUiMs=now;
    // PARAMETER INFO V2: footer is reserved permanently for contextual parameter text.
    // Performance and error counters remain available via Serial diagnostics.
    // MIDI PC changes also reflect in the header without restarting the GUI.
    if(!sliderTouchHeld && touchNavState!=TOUCH_NAV_HELD && uiPreset!=current_preset){uiPreset=current_preset;updateHeader();buildPage();}
    static uint32_t seenModeGen=0;
    if(seenModeGen!=modeGeneration){
      seenModeGen=modeGeneration;
      const int effectiveMode=publishedVoiceMode;
      if(effectiveMode>=SC_VM_SINGLE && effectiveMode<=SC_VM_DOUBLE){
        uiMode=effectiveMode;
        updateHeader();
      }
    }
    static uint32_t seenGen=0;
    if(!sliderTouchHeld && seenGen!=patchGeneration){seenGen=patchGeneration;updateHeader();buildPage();}
  }
}

// FIX12: SD recovery UI lives on the setup/Core0 LVGL thread.
static volatile bool sdRecoveryRequested=false;
static void onSDRecoveryClick(lv_event_t *e) {
  if(lv_event_get_code(e)==LV_EVENT_CLICKED) sdRecoveryRequested=true;
}
static void sdErrorView(lv_obj_t *screen,const char *heading,const char *detail) {
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen,lv_color_hex(0x071322),0);
  lv_obj_t *title=lv_label_create(screen);
  lv_label_set_text(title,"RTAL SYNTECH");
  lv_obj_set_style_text_color(title,lv_color_hex(0x65D8FF),0);
  lv_obj_align(title,LV_ALIGN_TOP_MID,0,15);
  lv_obj_t *err=lv_label_create(screen);
  lv_label_set_text(err,heading);
  lv_obj_set_style_text_color(err,lv_color_hex(0xFFB45E),0);
  lv_obj_align(err,LV_ALIGN_TOP_MID,0,69);
  lv_obj_t *desc=lv_label_create(screen);
  lv_label_set_text(desc,detail);
  lv_obj_set_width(desc,448);
  lv_label_set_long_mode(desc,LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(desc,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_text_color(desc,lv_color_hex(0xD7E5F0),0);
  lv_obj_align(desc,LV_ALIGN_TOP_MID,0,112);
  lv_obj_t *retry=lv_btn_create(screen);
  lv_obj_set_size(retry,204,48);
  lv_obj_align(retry,LV_ALIGN_BOTTOM_MID,0,-18);
  lv_obj_add_event_cb(retry,onSDRecoveryClick,LV_EVENT_CLICKED,nullptr);
  lv_obj_t *lbl=lv_label_create(retry);
  lv_label_set_text(lbl,"RETRY SD CARD");
  lv_obj_center(lbl);
  Board::serviceLVGL();
}
static void sdProgressView(lv_obj_t *screen,const char *message){
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen,lv_color_hex(0x071322),0);
  lv_obj_t *title=lv_label_create(screen);
  lv_label_set_text(title,"RTAL SYNTECH");
  lv_obj_set_style_text_color(title,lv_color_hex(0x65D8FF),0);
  lv_obj_align(title,LV_ALIGN_TOP_MID,0,42);
  lv_obj_t *msg=lv_label_create(screen);
  lv_label_set_text(msg,message);
  lv_obj_set_width(msg,450);
  lv_label_set_long_mode(msg,LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(msg,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_align(msg,LV_ALIGN_CENTER,0,10);
  lv_obj_t *spin=lv_spinner_create(screen,1000,65);
  lv_obj_set_size(spin,42,42);
  lv_obj_align(spin,LV_ALIGN_BOTTOM_MID,0,-24);
  Board::serviceLVGL();
  delay(35);
  Board::serviceLVGL();
}
static bool sdRecoveryUsed=false;
static bool sdRecoverMount(lv_obj_t *screen) {
  // First attempt already performed by Board::begin() at 8 MHz.
  if(Board::sdReady() && SD.cardType()!=CARD_NONE) return true;
  sdRecoveryUsed=true;
  Serial.println("[SD] initial mount failed; automatic attempt 2 at 4 MHz");
  if(Board::retrySD(4000000)) return true;
  while(true) {
    sdRecoveryRequested=false;
    sdErrorView(screen,"SD CARD ERROR",
      "SD-Karte nicht erkannt. Zwei Startversuche fehlgeschlagen.\n"
      "Karte und Kontakte pruefen.");
    while(!sdRecoveryRequested) {Board::serviceLVGL();delay(10);}
    // The user explicitly requested retry; attempt 8 MHz, then 4 MHz.
    sdProgressView(screen,"SD-Karte wird erneut verbunden ...");
    if(Board::retrySD(8000000) || Board::retrySD(4000000))return true;
  }
}
static int sdRecoverPresets(lv_obj_t *screen) {
  while(true) {
    if(sdRecoveryUsed)sdProgressView(screen,"Preset-Bank wird geprueft ...");
    int valid=auditPresetBank();
    if(valid>0) return valid;
    sdRecoveryUsed=true;
    sdRecoveryRequested=false;
    sdErrorView(screen,"PRESET DATA ERROR",
      "SD-Karte erkannt, aber keine gueltige .rtal-Datei gefunden.\n"
      "Bitte Preset-Backup wiederherstellen.");
    while(!sdRecoveryRequested){Board::serviceLVGL();delay(10);}
    sdProgressView(screen,"SD-Karte wird erneut verbunden ...");
    if(!Board::retrySD(8000000) && !Board::retrySD(4000000)) {
      if(!sdRecoverMount(screen))continue;
    }
  }
}
static void guiTask(void *);

void setup() {
  Serial.begin(115200);
  // USB-OTG device (TinyUSB). Keep native USB enumeration independent of SD.
  USB.productName("RTAL_SYNTECH");
  USB.manufacturerName("RTAL");
  sysexRequestQ=xQueueCreate(4,sizeof(SysexRequest));
  midiTxQueue=xQueueCreate(48,sizeof(MidiTxCC));
  if(!midiTxQueue)Serial.println("[STAGE6] MIDI TX queue allocation failed");
  usbMidi.begin();
  USB.begin();
  delay(700);
  Serial.println("[USB MIDI] TinyUSB device RTAL_SYNTECH initialized");
  Serial.println("[CROSS2 SQFAST TEST] LVGL / 32000 Hz / FRAMES256 / DMA4 / CROSS2 SQFAST");
  Serial.println("[RTAL SYNTECH V1.0.0 FINAL] / MUSICAL LP24 / PARAM INFO HOLD 1500ms");
  Serial.println("RTAL SYNTECH V0.5A STAGE4 FIX14 - USB MIDI DEVICE");
  Serial.printf("[RESET] cause=%d freeHeap=%lu\n", (int)esp_reset_reason(), (unsigned long)ESP.getFreeHeap());
  midiPreferences.begin("syntech-midi",false);
  midiChannel=midiPreferences.getUChar("channel",1);
  if(midiChannel<1 || midiChannel>16)midiChannel=1;
  midiCCMode=midiPreferences.getUChar("ccmode",0);
  if(midiCCMode>1)midiCCMode=0;
  midiCCBank=0;
  midiLearnLoad();
  Serial.printf("[MIDI] stored channel=%u mode=%s\n",(unsigned)midiChannel,midiCCMode?"STANDARD":"MOPHO");
  // STAGE5 FIX1: explicit, initialized split allocations. Never read an uninitialized rev.buf.
  synth=(sc_synth_t*)heap_caps_calloc(1, sizeof(sc_synth_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const bool synthInternal=(synth!=nullptr);
  if(!synth) synth=(sc_synth_t*)heap_caps_calloc(1, sizeof(sc_synth_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!synth) {Serial.println("FATAL: synth state allocation failed");abort();}
  // FIX5: Prefer 64 KiB internal SRAM for reverb; preserve original PSRAM fallback.
  const size_t reverbBytes = (size_t)SC_REV_LEN * SC_REV_LINES * sizeof(int32_t);
  const size_t internalLargestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  synth->rev.buf=(int32_t (*)[SC_REV_LINES])heap_caps_calloc(
      SC_REV_LEN * SC_REV_LINES, sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const bool reverbInternal=(synth->rev.buf!=nullptr);
  if(!synth->rev.buf) synth->rev.buf=(int32_t (*)[SC_REV_LINES])heap_caps_calloc(
      SC_REV_LEN * SC_REV_LINES, sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!synth->rev.buf) { Serial.println("FATAL: reverb ring allocation failed"); abort(); }
  Serial.printf("[FIX5 MEMORY] requested=SRAM actual=%s bytes=%u largestInternalBefore=%u largestInternalAfter=%u\n",
      reverbInternal ? "SRAM" : "PSRAM_FALLBACK", (unsigned)reverbBytes,
      (unsigned)internalLargestBefore,
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.printf("[STAGE5 FIX1] state=%s stateBytes=%u reverbBytes=%u freeInternal=%u freePSRAM=%u\n",
      synthInternal ? "INTERNAL" : "PSRAM_FALLBACK", (unsigned)sizeof(sc_synth_t),
      (unsigned)(SC_REV_LEN * SC_REV_LINES * sizeof(int32_t)),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cmdq=xQueueCreate(64,sizeof(Cmd));
  patchOutQ=xQueueCreate(1,sizeof(sc_patch_t));
  patchInQ=xQueueCreate(1,sizeof(sc_patch_t));
  filterDiagQ=xQueueCreate(1,sizeof(FilterDiagSnapshot));
  if(!patchOutQ||!patchInQ||!filterDiagQ)abort();
  if(!cmdq) abort();
  // Board mounts SD; complete this before initializing the DSP (no factory bank in flash).
  if(!Board::begin()) { Serial.println("FATAL: MiniAmused Board::begin failed"); abort(); }
  // Show analog-hardware splash while checking SD presets and starting DSP.
  // Created only on the Arduino/Core0 thread, before the GUI is built.
  lv_obj_t *splashScreen=lv_obj_create(nullptr);
  lv_obj_clear_flag(splashScreen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(splashScreen,0,0);
  lv_obj_set_style_border_width(splashScreen,0,0);
  lv_obj_t *splashImage=lv_img_create(splashScreen);
  lv_img_set_src(splashImage,&syntech_splash_image);
  lv_obj_set_pos(splashImage,0,0);
  lv_scr_load(splashScreen);
  const uint32_t splashStartedMs=millis();
  for(int frame=0;frame<8;frame++){Board::serviceLVGL();delay(5);}
  sdPresetReady=sdRecoverMount(splashScreen);
  const int validCount=sdRecoverPresets(splashScreen);
  if(sdRecoveryUsed)sdProgressView(splashScreen,"Preset wird geladen, Audio startet ...");
  sc_patch_t bootPatch={};
  int bootSlot=-1;
  // Prefer PC0; if corrupt/missing, load first valid slot without modifying any SD data.
  if(readBootPatch(0,bootPatch))bootSlot=0;
  else for(int i=1;i<USER_SLOTS;i++) if(readBootPatch(i,bootPatch)){bootSlot=i;break;}
  if(bootSlot<0) {
    Serial.println("[SD ONLY] FATAL: no readable boot preset despite audit.");
    sdRecoveryRequested=false;
    sdErrorView(splashScreen,"PRESET READ ERROR","Keine Presetdatei lesbar. SD-Karte und Backup pruefen.");
    while(!sdRecoveryRequested){Board::serviceLVGL();delay(10);}
    ESP.restart(); // Cleanly reinitialize the whole boot path after explicit user action.
  }
  sc_init(synth,&bootPatch);
  current_preset=bootSlot;
  selectedSlot=bootSlot;
  uiPreset=bootSlot;
  i2sInit();
  // DIN MIDI on dedicated pins; UART1 hardware peripheral.
  if(midiEnabled) {
    midi.setRxBufferSize(2048); // before begin(): absorb brief scheduler stalls
    midi.begin(31250,SERIAL_8N1,MIDI_RX_PIN,MIDI_TX_PIN);
    const BaseType_t midiStarted=xTaskCreatePinnedToCore(
        midiRxTask,"SynMidiRX",4096,nullptr,10,&midiRxTaskHandle,0);
    if(midiStarted!=pdPASS){Serial.println("FATAL: MIDI RX task failed");abort();}
    Serial.println("[MIDI] FIX7 dedicated Core0 RX task, priority 10, UART buffer 2048");
  }
  BaseType_t result=xTaskCreatePinnedToCore(audioTask,"ScAudio",12288,nullptr,23,nullptr,1);
  if(result!=pdPASS) {Serial.println("FATAL: cannot launch audio task");abort();}
  requestedPresetOrigin=PC_ORIGIN_BOOT; requestedPreset=bootSlot; // sync SD name/UI after init
  Serial.printf("[BANK] SD=%d slots=%d ready=%d initial=%03d\n",sdPresetReady,USER_SLOTS,validCount==USER_SLOTS,bootSlot);
  Serial.println("[S20] Board begin OK; start UI creation");
  // Guarantee the boot artwork is visible, without touching audio/MIDI tasks.
  while(!sdRecoveryUsed && (uint32_t)(millis()-splashStartedMs)<2000){Board::serviceLVGL();delay(5);}
  lv_obj_t *mainScreen=lv_obj_create(nullptr);
  lv_scr_load(mainScreen);
  uiInit();
  lv_obj_del(splashScreen);

  Serial.println("[S21] UI objects created; entering Arduino loop");
  Serial.printf("[PERF STAGE2] setupCore=%d audioCore=%lu O3=sc_voice+sc_synth\n",xPortGetCoreID(),(unsigned long)diag_audio_core);
  Serial.printf("Engine state: %u bytes. Enter 'on 60' to test audio.\n",(unsigned)sizeof(sc_synth_t));
  Serial.printf("DIN MIDI: UART1 RX GPIO%d TX GPIO%d @31250 baud\n", MIDI_RX_PIN, MIDI_TX_PIN);
  printHelp();
  // STAGE3: setup finishes LVGL initialization before its sole runtime owner starts.
  BaseType_t guiStarted=xTaskCreatePinnedToCore(guiTask,"SynGUI",16384,nullptr,5,nullptr,0);
  if(guiStarted!=pdPASS){Serial.println("FATAL: cannot launch Core0 GUI task");abort();}
  Serial.println("[STAGE4] Core0 GUI 5, Core1 audio 23; LVGL refresh 40ms, touch service 5ms");
}

// Runs only from SynGUI on Core0. No LVGL calls from Arduino loop after setup.
static void guiServiceOnce() {
  serviceSysexRequests();
  // FIX7Y GUI OFF: preserve serial recovery and MIDI/audio tasks, but do
  // not execute LVGL, widget refreshes, touch handling or page rebuilds.
  // Intentionally defer SD/preset UI work until NORMAL is restored.
  if(Board::displayTestMode()==2) {
    static String display7yInput;
    while(Serial.available()) {
      char c=(char)Serial.read();
      if(c=='\n'||c=='\r') {if(display7yInput.length()) serialCommand(display7yInput); display7yInput="";}
      else if(display7yInput.length()<80) display7yInput+=c;
    }
    delay(1);
    return;
  }
  // FIX7X: coalesce MIDI-triggered full page rebuilds. Never rebuild during touch.
  static uint32_t lastMidiPageRefresh=0;
  const uint32_t gui7xNow=millis();
  if(guiProbeMode8A!=1 && midiCcDisplayEnabled && midiUiDirty && !sliderTouchHeld &&
     (uint32_t)(gui7xNow-lastMidiPageRefresh)>=GUI7X_PAGE_REFRESH_MS){
    lastMidiPageRefresh=gui7xNow;
    midiUiDirty=false;
    const uint32_t t8a=micros();updateHeader();buildPage();
    const uint32_t dt8a=(uint32_t)(micros()-t8a);++gui8aPageRefresh;if(dt8a>gui8aPageUsMax)gui8aPageUsMax=dt8a;
  }
  // FIX7X: latest-value-only refresh <=20 Hz; MIDI-to-DSP remains immediate.
  static uint32_t lastMidiUiRefresh=0;
  if(guiProbeMode8A!=1 && midiCcDisplayEnabled && midiParamDirty && !sliderTouchHeld &&
     (uint32_t)(gui7xNow-lastMidiUiRefresh)>=GUI7X_CC_REFRESH_MS){
    lastMidiUiRefresh=millis();midiParamDirty=false;
    const uint32_t t8a=micros();++gui8aSliderRefresh;
    for(int i=0;i<6;i++) if(controlSliders[i] && controlIds[i]!=255){
      const int value=valFor(controlIds[i]);
      if(lv_slider_get_value(controlSliders[i])!=value){
        lv_obj_remove_event_cb(controlSliders[i],onParam);
        lv_slider_set_value(controlSliders[i],value,LV_ANIM_OFF);
        lv_obj_add_event_cb(controlSliders[i],onParam,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)i);
        lastSliderValue[i]=value;
        syntechParamInfoSliderActivity(paramInfoGate,millis());
        parameterInfoRefresh(controlIds[i],value);
        ++gui7xWidgetChanges;
      }
    }
    const uint32_t dt8a=(uint32_t)(micros()-t8a);if(dt8a>gui8aSliderUsMax)gui8aSliderUsMax=dt8a;
  }
    // End of MIDI slider refresh instrumentation.
  static uint32_t lastMidiPanelRefresh=0;
  if(guiProbeMode8A!=1 && midiPanelShown && (uint32_t)(millis()-lastMidiPanelRefresh)>=100){
    lastMidiPanelRefresh=millis();const uint32_t t8a=micros();midiPanelRefresh();
    const uint32_t dt8a=(uint32_t)(micros()-t8a);++gui8aPanelRefresh;if(dt8a>gui8aPanelUsMax)gui8aPanelUsMax=dt8a;
  }
  // Synchronize FX switches with both MIDI CC and touch changes, without
  // touching LVGL from the audio or MIDI tasks.
  static uint32_t lastFxRefresh=0;
  if(guiProbeMode8A!=1 && uiPage==6 && midiCcDisplayEnabled &&
     (uint32_t)(gui7xNow-lastFxRefresh)>=GUI7X_CC_REFRESH_MS){
    lastFxRefresh=gui7xNow;
    const uint32_t t8a=micros();refreshFxButtons();
    const uint32_t dt8a=(uint32_t)(micros()-t8a);++gui8aFxRefresh;if(dt8a>gui8aFxUsMax)gui8aFxUsMax=dt8a;
  }
  // Always service UI before polling MIDI; continuous incoming MIDI must not starve LVGL.
  uiLoop();
  serviceTouchPresetNavigation();
  // FIX7: label LVGL hold now opens HELP; MIDI Learn uses HELP popup button.
  // SD reads always execute on Arduino loop/Core0, never the audio callback.
  int target=requestedPreset;
  if(target>=0 && target<USER_SLOTS){
    const uint8_t pcOrigin=requestedPresetOrigin;
    requestedPreset=-1;
    static const char* const pcSourceNames[]={"BOOT","MIDI","TOUCH","SERIAL","RESTORE","QUEUE"};
    const char *source=(pcOrigin<6)?pcSourceNames[pcOrigin]:"UNKNOWN";
    Serial.printf("[PC REQUEST] source=%s value=%03d\n", source, target);
    char name[32]={};
    if(readSlot(target,name)){
      current_preset=target;uiPreset=target;selectedSlot=target;
      snprintf(currentPresetName,sizeof(currentPresetName),"%s",name);
      snprintf(userPatchName,sizeof(userPatchName),"%s",name);
      if(bankNameField)lv_textarea_set_text(bankNameField,name);
      updateHeader();buildPage();updateBankLabel();
      Serial.printf("[PC] loaded %03d '%s'\n",target,name);
    }else{
      failedLoads++; Serial.printf("[PC] FAILED %03d (SD or CRC)\n",target);
      uiPreset=current_preset;updateHeader();
    }
  }
  // MIDI is read exclusively by midiRxTask; never block MIDI behind LVGL.
  static String input;
  while(Serial.available()) {
    char c=(char)Serial.read();
    if(c=='\n'||c=='\r') { if(input.length()) serialCommand(input);input=""; }
    else if(input.length()<80) input+=c;
  }
  // Core0 only: print queued filter snapshots and optionally request them at 0.5 Hz.
  if(filterDiagQ) {
    FilterDiagSnapshot fd={};
    if(xQueueReceive(filterDiagQ,&fd,0)==pdTRUE)filterDiagPrint(fd);
  }
  if(filterDiagPeriodic && (uint32_t)(millis()-filterDiagLastRequestMs)>=2000) {
    filterDiagLastRequestMs=millis();
    sendCmd(FILTER_DIAG); // no waiting; audio task captures after its next render
  }
  static uint32_t lastDiag=0;
  const uint32_t now=millis();
  if(now-lastDiag >= 2000) {
    lastDiag=now;
    Serial.printf("[LIVE] sec=%lu frames=%lu flush=%lu pixels=%llu touch=%u dsp=%lu/%lu xrun=%lu qdrop=%lu heap=%lu cmd=%lu/%lu compute=%lu/%lu pending=%lu\n",
      (unsigned long)(now/1000), (unsigned long)Board::guiFrameCount(),
      (unsigned long)Board::guiFlushCount(), (unsigned long long)Board::guiPixelCount(),
      (unsigned)Board::touchPressed(), (unsigned long)avg_us, (unsigned long)max_us,
      (unsigned long)xrun, (unsigned long)dropped_cmds, (unsigned long)ESP.getFreeHeap(),
      (unsigned long)diag_cmd_last_us,(unsigned long)diag_cmd_peak_us,
      (unsigned long)diag_audio_compute_last,(unsigned long)diag_audio_compute_peak,
      (unsigned long)uxQueueMessagesWaiting(cmdq));
    // FIX7H: cumulative counters; compare consecutive lines for 2-second deltas.
    // NOTE: a long I2S write can be normal DMA backpressure, not proof of a dropout.
    Serial.printf("[DEADLINE7H] budget=%lu cmd=%lu/%lu render=%lu/%lu pcm=%lu/%lu compute=%lu/%lu write=%lu/%lu gap=%lu/%lu overCmd=%lu overRender=%lu overCompute=%lu lateGap=%lu i2sFail=%lu commands=%lu reprep=%lu\n",
      (unsigned long)((uint64_t)FRAMES*1000000/SC_FS),
      (unsigned long)h_last_cmd,(unsigned long)h_cmd_max,
      (unsigned long)h_last_render,(unsigned long)h_render_max,
      (unsigned long)h_last_pcm,(unsigned long)h_pcm_max,
      (unsigned long)h_last_compute,(unsigned long)h_compute_max,
      (unsigned long)h_last_write,(unsigned long)h_write_max,
      (unsigned long)h_last_gap,(unsigned long)h_gap_max,
      (unsigned long)h_cmd_over,(unsigned long)h_render_over,
      (unsigned long)h_compute_over,(unsigned long)h_late_start,
      (unsigned long)h_i2s_fail,(unsigned long)h_cmd_count,
      (unsigned long)h_reprep_count);
    Serial.printf("[WCET7J] prepLast=%lu prepPeak=%lu prepBlocks=%lu renderOverPrep=%lu renderOverNoPrep=%lu renderOverCmd=%lu renderOverIdle=%lu renderPeakPrep=%lu renderPeakNoPrep=%lu cmdPeakPrep=%lu cmdPeakNoPrep=%lu\n",
      (unsigned long)j_prep_us_last,(unsigned long)j_prep_us_peak,(unsigned long)j_prep_blocks,
      (unsigned long)j_render_over_prep,(unsigned long)j_render_over_no_prep,
      (unsigned long)j_render_over_cmd,(unsigned long)j_render_over_idle,
      (unsigned long)j_render_peak_prep,(unsigned long)j_render_peak_no_prep,
      (unsigned long)j_cmd_peak_prep,(unsigned long)j_cmd_peak_no_prep);
    // FIX7K: peaks only among sampled blocks that actually exceeded the budget.
    // Units are microseconds at the fixed 240 MHz CPU clock.
    Serial.printf("[PEAK7K] sampled=%lu over=%lu worstRender=%lu voices=%lu prep=%lu worstCtrl=%lu worstO1=%lu worstO2=%lu worstFilter=%lu worstChorus=%lu worstReverb=%lu worstMaster=%lu peakCtrl=%lu peakO1=%lu peakO2=%lu peakFilter=%lu peakChorus=%lu peakReverb=%lu peakMaster=%lu\n",
      (unsigned long)k_profiled_blocks,(unsigned long)k_profiled_over,
      (unsigned long)k_worst_render_us,(unsigned long)k_worst_voices,(unsigned long)k_worst_had_prep,
      (unsigned long)(k_worst_components[0]/240),(unsigned long)(k_worst_components[1]/240),
      (unsigned long)(k_worst_components[2]/240),(unsigned long)(k_worst_components[3]/240),
      (unsigned long)(k_worst_components[4]/240),(unsigned long)(k_worst_components[5]/240),
      (unsigned long)(k_worst_components[6]/240),
      (unsigned long)(k_component_peak_cycles[0]/240),(unsigned long)(k_component_peak_cycles[1]/240),
      (unsigned long)(k_component_peak_cycles[2]/240),(unsigned long)(k_component_peak_cycles[3]/240),
      (unsigned long)(k_component_peak_cycles[4]/240),(unsigned long)(k_component_peak_cycles[5]/240),
      (unsigned long)(k_component_peak_cycles[6]/240));
    Serial.printf("[ROOT8B] sampledGeneral=%lu overGeneral=%lu sampledCross2=%lu overCross2=%lu worstGeneralRenderUs=%lu generalUs=%lu generalVoices=%lu cross2Voices=%lu peakGeneralUs=%lu lastGeneralUs=%lu lastGeneralVoices=%lu lastCross2Voices=%lu\n",
      (unsigned long)b_profiled_general_blocks,(unsigned long)b_profiled_general_over,
      (unsigned long)b_profiled_cross2_blocks,(unsigned long)b_profiled_cross2_over,
      (unsigned long)b_worst_general_render_us,(unsigned long)(b_worst_general_cycles/240),
      (unsigned long)b_worst_general_voices,(unsigned long)b_worst_cross2_voices,
      (unsigned long)(b_peak_general_cycles/240),(unsigned long)(sc_prof.general/240),
      (unsigned long)sc_prof.general_voices,(unsigned long)sc_prof.cross2_voices);
    // FIX5: PCM clipping diagnostic. clipTotal and clipBlocks are cumulative since boot.
    Serial.printf("[PCM] peak=%lu clipLast=%lu clipTotal=%lu clipBlocks=%lu xrun=%lu\n",
      (unsigned long)last_pcm_peak, (unsigned long)last_pcm_clipped,
      (unsigned long)diag_pcm_clipped_total, (unsigned long)diag_pcm_clipped_blocks,
      (unsigned long)xrun);
    // FIX4 sampled voice/FX timings, cycles / 240 = microseconds at 240 MHz.
    // One profiled render block per 128 blocks; not a continuously averaged statistic.
    static uint32_t lastProfileSeq=0;
    uint32_t seq=sc_prof.seq;
    if(seq!=lastProfileSeq) {
      lastProfileSeq=seq;
      Serial.printf("[DSPPROFILE] seq=%lu voices=%lu ctrl=%lu osc1=%lu osc2=%lu mixflt=%lu general=%lu chorus=%lu reverb=%lu master=%lu (us @240MHz)\n",
        (unsigned long)seq,(unsigned long)sc_prof.voices,
        (unsigned long)(sc_prof.control/240),(unsigned long)(sc_prof.osc1/240),
        (unsigned long)(sc_prof.osc2/240),(unsigned long)(sc_prof.mixfilter/240),
        (unsigned long)(sc_prof.general/240),(unsigned long)(sc_prof.chorus/240),
        (unsigned long)(sc_prof.reverb/240),(unsigned long)(sc_prof.master/240));
      Serial.printf("[FIX6 OSC2] core=%lu noise=%lu total=%lu (us @240MHz; sampled block)\n",
        (unsigned long)(sc_prof.osc2_core/240),
        (unsigned long)(sc_prof.osc2_noise/240),
        (unsigned long)(sc_prof.osc2/240));
    }

  }
  delay(1);
}

// Dedicated owner for LVGL, touch, presets, serial command service and SysEx.
static void guiTask(void *) {
  for (;;) guiServiceOnce(); // guiServiceOnce yields via delay(1)
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
