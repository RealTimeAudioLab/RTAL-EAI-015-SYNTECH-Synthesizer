# RTAL SYNTECH — MIDI CC reference

> **Mapping source:** `RTAL_SYNTECH_MIDI_CC_STANDARD_MOPHO_EDITOR.xlsx` supplied with this documentation. This table records the supplied design mapping; actual behavior must be checked against the firmware revision before release. MIDI Learn assignments may override defaults.

## Two controller modes

- **STANDARD MODE:** Each parameter has a direct MIDI Control Change number. No CC32 bank selection is required.
- **MOPHO MODE:** Send **CC32** with the bank value **0**, **1** or **2**, then send the parameter CC shown in the table. The last selected bank remains active until changed.
- CC values normally range from **0–127**. Modulation matrix routing parameters are bipolar and use **64 as neutral**, according to the source table.
- The sheet lists channels `OMNI, 1–16` as mapping applicability. Actual selectable channel/OMNI behavior depends on firmware configuration; the user interface screenshot shows channel 1.
- MIDI Program Change supports the patch range 0–127 in the project design. Actual bank content and preset availability depend on the installed SD card.

## Complete mapping from supplied spreadsheet

| Section | Parameter | Standard CC | CC32 bank | MOPHO CC | Value range |
|:--|:--|--:|--:|--:|:--|
| OSC | OSC1 Waveform | 14 | 0 | 22 | 0–127 |
| OSC | OSC2 Waveform | 15 | 0 | 26 | 0–127 |
| OSC | OSC1 PWM | 16 | 0 | 23 | 0–127 |
| OSC | OSC2 PWM | 17 | 0 | 27 | 0–127 |
| OSC | OSC2 Detune | 18 | 0 | 31 | 0–127 |
| OSC | Oscillator Sync | 19 | 0 | 30 | 0–127 |
| MIX | OSC1 Level | 20 | 0 | 28 | 0–127 |
| MIX | OSC2 Level | 21 | 0 | 52 | 0–127 |
| MIX | Noise Level | 22 | 0 | 29 | 0–127 |
| MIX | Cross Mod 1 | 23 | 0 | 89 | 0–127 |
| MIX | Cross Mod 2 | 24 | 0 | 90 | 0–127 |
| MIX | Ring Modulation | 25 | 0 | 77 | 0–127 |
| MIX | Pink Noise | 26 | 0 | 78 | 0–127 |
| FILTER | Filter Cutoff | 27 | 0 | 102 | 0–127 |
| FILTER | Filter Resonance | 28 | 0 | 103 | 0–127 |
| FILTER | Filter Type | 29 | 0 | 105 | 0–127 |
| FILTER | Filter Env Amount | 30 | 0 | 106 | 0–127 |
| FILTER | Filter Key Tracking | 31 | 0 | 107 | 0–127 |
| FILTER ENV | Filter Attack | 33 | 0 | 109 | 0–127 |
| FILTER ENV | Filter Decay | 34 | 0 | 110 | 0–127 |
| FILTER ENV | Filter Sustain | 35 | 0 | 111 | 0–127 |
| FILTER ENV | Filter Release | 36 | 0 | 112 | 0–127 |
| AMP ENV | Amp Attack | 37 | 0 | 118 | 0–127 |
| AMP ENV | Amp Decay | 39 | 0 | 119 | 0–127 |
| AMP ENV | Amp Sustain | 40 | 0 | 75 | 0–127 |
| AMP ENV | Amp Release | 41 | 0 | 76 | 0–127 |
| LFO1 | LFO1 Rate | 42 | 1 | 106 | 0–127 |
| LFO1 | LFO1 Depth | 43 | 1 | 109 | 0–127 |
| LFO1 | LFO1 Waveform | 44 | 1 | 110 | 0–127 |
| LFO1 | LFO1 Filter/Amp Depth | 66 | 1 | 76 | 0–127 |
| LFO1 | LFO1 Delay | 67 | 1 | 86 | 0–127 |
| LFO1 | LFO1 Sync | 68 | 1 | 89 | 0–127 (Aus/Ein) |
| LFO1 MATRIX | LFO1 → OSC1 Pitch | 59 | 1 | 111 | 0–127 (64 = neutral) |
| LFO1 MATRIX | LFO1 → OSC2 Pitch | 60 | 1 | 112 | 0–127 (64 = neutral) |
| LFO1 MATRIX | LFO1 → OSC1 PWM | 61 | 1 | 115 | 0–127 (64 = neutral) |
| LFO1 MATRIX | LFO1 → OSC2 PWM | 62 | 1 | 118 | 0–127 (64 = neutral) |
| LFO1 MATRIX | LFO1 → Filter Cutoff | 63 | 1 | 119 | 0–127 (64 = neutral) |
| LFO1 MATRIX | LFO1 → Amplitude | 65 | 1 | 75 | 0–127 (64 = neutral) |
| LFO2 | LFO2 Rate | 45 | 2 | 106 | 0–127 |
| LFO2 | LFO2 Depth | 46 | 2 | 109 | 0–127 |
| LFO2 | LFO2 Waveform | 47 | 2 | 110 | 0–127 |
| LFO2 | LFO2 Filter/Amp Depth | 75 | 2 | 76 | 0–127 |
| LFO2 | LFO2 Delay | 76 | 2 | 86 | 0–127 |
| LFO2 | LFO2 Sync | 77 | 2 | 89 | 0–127 (Aus/Ein) |
| LFO2 MATRIX | LFO2 → OSC1 Pitch | 69 | 2 | 111 | 0–127 (64 = neutral) |
| LFO2 MATRIX | LFO2 → OSC2 Pitch | 70 | 2 | 112 | 0–127 (64 = neutral) |
| LFO2 MATRIX | LFO2 → OSC1 PWM | 71 | 2 | 115 | 0–127 (64 = neutral) |
| LFO2 MATRIX | LFO2 → OSC2 PWM | 72 | 2 | 118 | 0–127 (64 = neutral) |
| LFO2 MATRIX | LFO2 → Filter Cutoff | 73 | 2 | 119 | 0–127 (64 = neutral) |
| LFO2 MATRIX | LFO2 → Amplitude | 74 | 2 | 75 | 0–127 (64 = neutral) |
| FX | Chorus Enable | 57 | 0 | 115 | 0-127 |
| FX | Chorus Mix | 48 | 1 | 28 | 0–127 |
| FX | Chorus Rate | 49 | 1 | 52 | 0–127 |
| FX | Chorus Depth | 50 | 1 | 29 | 0–127 |
| FX | Reverb Enable | 58 | 0 | 86 | 0-127 |
| FX | Reverb Mix | 51 | 1 | 102 | 0–127 |
| FX | Reverb Size | 52 | 1 | 103 | 0–127 |
| FX | Reverb Damping | 53 | 1 | 105 | 0–127 |
| PERFORMANCE | Upper / Lower Select | 54 | 0 | 14 | Siehe Hinweis |
| PERFORMANCE | Play Mode | 55 | 0 | 104 | Siehe Hinweis |
| PERFORMANCE | Split Point | 56 | 0 | 53 | Siehe Hinweis |

## Examples

```text
# STANDARD — adjust filter cutoff
B0 1B 64     # CC27, value 100, on MIDI channel 1

# MOPHO — bank 0, filter cutoff
B0 20 00     # CC32 = 0
B0 66 64     # CC102 = 100

# MOPHO — bank 1, LFO1 rate
B0 20 01     # CC32 = 1
B0 6A 40     # CC106 = 64

# MOPHO — bank 2, LFO2 rate
B0 20 02     # CC32 = 2
B0 6A 40     # CC106 = 64
```

The hexadecimal MIDI examples assume **channel 1** (`B0`), and illustrate the mapping rather than a hardware protocol trace. Both modes reuse several numerical CC values for different parameters; never apply the MOPHO CC column without first selecting the intended bank. The complete spreadsheet is the editable primary reference.
