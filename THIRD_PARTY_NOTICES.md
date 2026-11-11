# Third-party notices and licensing — RTAL SYNTECH V1.0.0

## Joakim Eriksson — SCINTILLA (primary code ancestor)

- Original author: **Joakim Eriksson** — https://github.com/joakimeriksson
- Upstream: **SCINTILLA** — https://github.com/joakimeriksson/scintilla-fm1
- The upstream SCINTILLA project is published under **GPL-3.0-only** (`LICENSE` in the upstream repository).
- Upstream SCINTILLA describes its DSP as an integer C port of **synthex-web** by Joakim Eriksson, whose engine is described upstream as **BSD-2-Clause** licensed. The precise upstream revision used in this supplied snapshot has **not** been reconstructed. Preserve original per-file notices and check the precise origin and full text of BSD-2-Clause notices in any incorporated material.

## Code scope in the shipped release

The supplied firmware source contains SCINTILLA-origin or SCINTILLA-derived modules under `firmware/RTAL_SYNTECH_V1_0_0_FINAL/`, including `sc_voice.c/.h`, `sc_synth.c/.h`, `sc_patch.h`, `sc_fixed.h`, `sc_tables.h`, and other associated DSP elements. The main Arduino sketch explicitly identifies itself as derived from SCINTILLA. Numerous sources have existing `SPDX-License-Identifier: GPL-3.0-only` headers. The SYNTECH UI, board integration, preset and MIDI functionality include further project-specific work. This description is a **provenance summary**, not a file-by-file copyright audit and not a claim that unmarked files were originally authored by RTAL.

## Redistributing the package

This source distribution includes the full GNU GPL version 3 text at `LICENSE`. Retain all inherited copyright and license notices; provide complete corresponding source and license text with any distributed firmware binaries as required. Keep distinct third-party dependencies (Arduino-ESP32, LVGL, display libraries, etc.) under their respective license terms. Do not infer that GPL terms for the derived code automatically relicense all separate libraries or artwork.

## Trademarks and endorsement

Elka and Synthex names are used to identify historical inspiration. RTAL SYNTECH is not an official Elka Synthex product and is not endorsed by the trademark holders or by Joakim Eriksson. The screenshots and promotional artwork have separate asset provenance; no third-party trademark rights are conveyed.

## Provenance still to complete

Before broad binary distribution, record the exact SCINTILLA commit(s), any imported synthex-web source, original copyright headers and applicable BSD-2-Clause notice where relevant. A matching source-level record was not supplied with the firmware ZIP, so we do not invent one here.
