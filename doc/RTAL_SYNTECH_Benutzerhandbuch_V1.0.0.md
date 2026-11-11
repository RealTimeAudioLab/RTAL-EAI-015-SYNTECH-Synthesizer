# RTAL SYNTECH — Benutzerhandbuch

**RTAL-EAI-015 · Firmware V1.0.0 FINAL · Ausgabe 11. November 2026**  
*Polyphoner Virtual-Analog-Synthesizer · Inspiriert vom Elka Synthex*

![SYNTECH Titelbild](../assets/images/RTAL_SYNTECH_00_Startpage.PNG)

> Dieses Benutzerhandbuch beschreibt die vorliegende Firmware V1.0.0 FINAL und die zugehörige Touch-Oberfläche. Historische Screenshots mit 44,1 kHz sind keine aktuelle Taktspezifikation: Der geprüfte Quelltext verwendet **32 kHz / 256 Frames**. Für die vollständige Inbetriebnahme wird eine kompatible SD-Presetbank benötigt, die nicht im Quellarchiv enthalten ist.

## 1. Das Instrument kennenlernen

RTAL SYNTECH ist ein eigenständig spielbarer, polyphoner Digital-Synthesizer auf Basis eines ESP32-S3. Seine Klangerzeugung verbindet zwei Oszillatoren pro Stimme mit Mischstufe, Rauschquelle, multimodalem Filter, zwei Hüllkurven, LFOs und Effekten. Die Bedienung erfolgt über das 480 × 272-Pixel-Touchdisplay des JC4827W543. DIN-MIDI und USB-MIDI ermöglichen die Steuerung durch Keyboards, Sequenzer und DAWs.

**Wichtige Begriffe:** Eine *Stimme* ist eine gleichzeitig klingende Note. Eine *Ebene* (UPPER/LOWER) ist eine separat bearbeitbare Klangschicht. Ein *Preset* speichert Klangparameter. Ein *LFO* erzeugt eine langsame periodische Modulation. Ein *XRUN* signalisiert ein überzogenes Audiobudget.

### 1.1 Leistungsumfang

| Funktion | V1.0.0 FINAL |
|:--|:--|
| Audio | 32.000 Samples/s, 256-Frame-Blöcke, I²S |
| Polyphonie | Bis zu 6 Stimmen unter normalen Bedingungen |
| Stimmenaufbau | 2 Oszillatoren, Rauschen, Filter, ENV, LFO/Matrix |
| Ebenen | UPPER / LOWER |
| Spielarten | SINGLE, SPLIT, DOUBLE |
| Effekte | Chorus, Reverb |
| Presets | P000–P127, SD-abhängig, Dateiendung `.rtal` |
| MIDI | DIN und USB; Note, Program Change, CC, Learn |
| Bedienung | Touch-GUI auf LVGL 8.4 |

## 2. Hardware und Anschlüsse

### 2.1 Benötigte Komponenten

- JC4827W543 mit ESP32-S3, NV3041A-QSPI-Display und GT911-Touchcontroller.
- Geeignete Stromversorgung und ein zur Platine passendes USB-Kabel.
- Eine microSD-Karte mit kompatiblen SYNTECH-Presetdateien.
- Ein zu **32 kHz** kompatibler I²S-DAC (oder eine entsprechende Audioausgangsstufe) und Verstärker/Aktivlautsprecher.
- Optional: korrekt beschaltete, galvanisch getrennte DIN-MIDI-IN-Schaltung und MIDI-OUT-Treiberschaltung; MIDI-Pins nicht ungeschützt an DIN-Buchsen anschließen.

### 2.2 Relevante GPIO-Belegung laut Firmware

| Signal | ESP32-S3 GPIO | Funktion |
|:--|--:|:--|
| I²S BCLK | 16 | Bitclock zum DAC |
| I²S LRCK / WS | 15 | Audio-Worttakt |
| I²S DOUT | 7 | Audiodaten |
| DIN-MIDI RX | 17 | UART-Eingang, 31.250 Baud |
| DIN-MIDI TX | 18 | UART-Ausgang, 31.250 Baud |
| SD SCK / MISO / MOSI / CS | 12 / 13 / 11 / 10 | SPI-SD-Karte |

Die Display- und Touch-Pins sind in `PINS_JC4827W543.h` beschrieben. Wichtig: Eine reine GPIO-Tabelle ist **kein vollständiger elektrischer Schaltplan**. Insbesondere MIDI-DIN verlangt normgerechte Eingangsbeschaltung und Ausgangstreiber. Der DAC darf nicht an ungeeignete Versorgungsspannungen angeschlossen werden.

### 2.3 Software und Kompilierung

Der Quelltext nennt Arduino IDE **2.3.10**, Arduino-ESP32-Core **3.3.11** und LVGL **8.4.x**. Die Programmierung setzt ein geeignetes ESP32-S3-Profil, die korrekte PSRAM-/Flash-Konfiguration und USB-OTG (TinyUSB) voraus. Den `.ino`-Sketch mit sämtlichen benachbarten Headern und C/C++-Quelldateien öffnen. Eine vollständig reproduzierte Hardware-Installation ist im Veröffentlichungs-Audit noch offen.

## 3. Erster Start

1. Vor dem Flashen die vorhandenen SD-Presets sichern. Die Firmware schreibt fehlende Factory-Dateien **nicht automatisch neu**.
2. Auf der SD-Karte den Ordner `/RTAL_SYNTECH/PRESETS/` anlegen und mindestens eine gültige, kompatible Datei `Pxxx.rtal` bereitstellen; für eine vollständige Bibliothek P000 bis P127.
3. I²S-DAC und MIDI-Interface bei ausgeschalteter Hardware korrekt anschließen; Audio zunächst leise einstellen.
4. Gerät einschalten. Das SYNTECH-Startbild erscheint. Das System prüft die SD- und Presetstruktur und lädt den ersten verwendbaren Patch, bevorzugt P000.
5. Wenn eine SD-/Preset-Fehlermeldung angezeigt wird, Speichermedium, Verzeichnis, Dateinamen und Format prüfen. Ohne gültiges Preset startet die normale Klangerzeugung nicht.
6. MIDI-Keyboard auf den im MIDI-Dialog eingestellten Kanal (werkseitig Kanal 1) setzen. Mit einer Note Ton prüfen.

**Wichtig zur Dateikompatibilität:** Die aktuelle Firmware verlangt Presetformat Version 2 einschließlich Header, Nutzdatengröße und CRC. Beliebige umbenannte Dateien und ältere/incompatible Dumps werden nicht geladen.

## 4. Die Hauptoberfläche

![Überblick aller Touch-Seiten](../assets/images/RTAL_SYNTECH_12_OverView.png)

Oben sind die Presetnummer und der Patchname zu sehen. Die Pfeiltasten wechseln die Nummer. **LAYER** wählt UPPER/LOWER, **MODE** die Spielart. **MIDI** öffnet die MIDI-Einstellungen, **BANK** den Preset-Dialog. Die Hauptreiter heißen **OSC**, **MIX**, **FILTER**, **F-ENV**, **A-ENV**, **LFO** und **FX**.

Die Diagnosezeile `DSP ... us | XRUN ... | Q ...` dient der Betriebs- und Fehlersuche. In der Anzeige bedeutet der DSP-Wert eine gemessene Rechenzeit, XRUN ein verfehltes Zeitbudget und Q die relevante Queue-Diagnose. Historische Bildwerte lassen sich nicht direkt mit anderen Firmwareständen vergleichen.

### 4.1 Slider und Infofenster

Parameter werden durch Berühren und Verschieben der Schieberegler verändert. Ein längeres, ruhiges Berühren kann nach **1,5 Sekunden** die Parameterinformation öffnen; Bewegung am Slider unterbricht das Zeitfenster. Das Informationsfenster soll den aktuellen Parameter samt Wert benennen. Für die MIDI-Learn-Zuweisung kann der entsprechende Dialog benutzt werden.

### 4.2 Preset-Pfeile

Kurzes Tippen schaltet jeweils eine Position weiter. Längeres Halten aktiviert eine beschleunigte Wiederholung: nach rund 500 ms folgen Stufen mit ca. 250 / 150 / 75 ms. Solange gehalten wird, ist dies zunächst eine **Vorschau**; erst nach dem Loslassen wird ein einzelner Preset-Ladevorgang angestoßen. Dadurch bleiben SD-I/O und Audio-Engine weitgehend entkoppelt.

## 5. Oszillatoren — OSC

![OSC-Seite](../assets/images/RTAL_SYNTECH_09_Oscillators.png)

**OSC1 WAVE / OSC2 WAVE:** Wellenformen der beiden Klangerzeuger wählen. Unterschiedliche Wellenformen verändern die Obertonstruktur (beispielsweise weich bis obertonreich). **OSC1 PWM / OSC2 PWM:** Pulsbreite einstellen; besonders für bewegte, chorartige Klänge. **DETUNE:** Zweiten Oszillator gegenüber dem ersten verstimmen. **OSC SYNC:** Oszillator-Synchronisation aktivieren. Ein leichter Detune erzeugt Schwebungen; Sync kann markante Obertonverläufe erzeugen.

**Praxisidee:** Beide Oszillatoren mit ähnlich kräftigen Wellen einstellen, OSC2 etwas verstimmen und mit dem Filter abdunkeln – für einen breiten, analogen Grundklang.

## 6. Mixer und Kreuzmodulation — MIX

![MIX-Seite](../assets/images/RTAL_SYNTECH_08_Mixer.png)

**OSC1 LEVEL / OSC2 LEVEL:** Balance der beiden Oszillatoren. **NOISE:** Rauschanteil zum Klang. **CROSS1 / CROSS2:** Zusätzliche Oszillator-Kreuzmodulation, die metallische, scharfe oder komplexe Spektren erzeugt. **RING MOD:** Ringmodulation für glockenartige oder atonale Klangfarben. Die Einstellungen sind klanglich und rechentechnisch nicht kostenlos.

**CPU-Hinweis:** Die Firmware bietet sechs Stimmen; bei hohen CROSS1-/CROSS2-Werten und komplexen Wellenformen kann die verfügbare Rechenzeit zum begrenzenden Faktor werden. Bei Knacksern zunächst die Zahl gleichzeitig gedrückter Tasten oder die Kreuzmodulation reduzieren. Eine automatische Reduktion auf vier Stimmen ist in der vorliegenden Firmware nicht zugesichert.

## 7. Filter — FILTER

![Filter-Seite](../assets/images/RTAL_SYNTECH_07_Filter.png)

**CUTOFF** steuert die Grenzfrequenz, **RESONANCE** die Betonung im Bereich der Grenzfrequenz. **FILTER TYPE** wechselt zwischen den vorhandenen Filtermodi; darunter befindet sich der musikalisch abgestimmte LP24-Modus. **ENV AMOUNT** beeinflusst den Filterhub durch die Hüllkurve; **KEY TRACK** lässt die Filterwirkung von der Tonhöhe abhängen. **PINK NOISE** schaltet die zugehörige Rauschoption.

Die Firmware V1.0.0 setzt bei Preset-Ladevorgängen die als *MUSICAL LP24* entwickelte Resonanzcharakteristik als Standard. Der genaue Frequenzgang hängt von Preset, Tonhöhe, Resonanz, Modulation und Firmware-Implementierung ab. Hohe Resonanz kann den Pegel/Charakter drastisch ändern; Pegel vorsichtig einstellen.

## 8. Filter-Hüllkurve — F-ENV

![Filterhüllkurve](../assets/images/RTAL_SYNTECH_06_Filter_Envelope.png)

Die Filterhüllkurve formt die Filterbewegung nach Tastendruck: **ATTACK** (Zeit bis zum Maximum), **DECAY** (Abfall zum Haltewert), **SUSTAIN** (Haltewert während gedrückter Taste) und **RELEASE** (Rücklauf nach dem Loslassen). Das hörbare Ergebnis hängt vor allem von **ENV AMOUNT** auf der Filterseite ab. Für Brass-Sounds kurze Attack, mittlere Decay und moderate Sustain probieren; für schwebende Pads längere Attack und Release.

## 9. Verstärker-Hüllkurve — A-ENV

![Verstärkerhüllkurve](../assets/images/RTAL_SYNTECH_05_Amp_Envelope.png)

Die **AMP ATTACK**, **AMP DECAY**, **AMP SUSTAIN** und **AMP RELEASE** Parameter bestimmen den zeitlichen Lautstärkeverlauf. Kurzer Attack und Release ergeben präzise perkussive Klänge; lange Werte führen zu weichen Ein- und Ausblendungen. Bei hoher Polyphonie und langem Release können Stimmen länger gebunden bleiben.

## 10. LFOs und Modulationsmatrix

![LFO-Seite](../assets/images/RTAL_SYNTECH_04_LFO.png)

SYNTECH verfügt über zwei LFOs. Die LFO-Ansicht bietet **RATE** (Geschwindigkeit), **WAVE** (Form), **PITCH/PWM**, **FLT/AMP**, **DELAY** und **SYNC**. Mit **LFO 1** oder **LFO 2** wählen Sie den Modulator; **MAIN** zeigt dessen Grundparameter. Die beiden Matrixseiten erlauben gezielte Modulationen mit positiven oder negativen Beträgen.

### 10.1 MATRIX A: Tonhöhe / PWM

![LFO Matrix A](../assets/images/RTAL_SYNTECH_10_LFO_Matrix_A.PNG)

**OSC1 PITCH**, **OSC2 PITCH** und **OSC1 PWM** regeln die Modulationsintensität. PITCH erzeugt Vibrato oder weitreichende Frequenzbewegungen; PWM erzeugt periodische Klangfarbenänderungen.

### 10.2 MATRIX B: PWM / Filter / Lautstärke

![LFO Matrix B](../assets/images/RTAL_SYNTECH_11_LFO_Matrix_B.PNG)

**OSC2 PWM**, **FILTER** und **AMPLITUDE** modulieren zweite Pulsbreite, Filter bzw. Pegel. Für Tremolo AMPLITUDE nutzen; für automatische Filterfahrten FILTER; für Schwebungen PWM. Bipolare MIDI-Matrixwerte verwenden **64 als Nullstellung**, 0 als negative und 127 als positive Endposition.

### 10.3 Klangbeispiele

**Vibrato:** LFO1 Rate langsam bis mittel, OSC1+OSC2 Pitch mit kleinem Betrag. **PWM-Pad:** LFO1 langsam, beide PWM-Routen moderat, lange AMP-Release. **Wah:** LFO2 in Filter, höhere Resonanz, mäßiges ENV AMOUNT. **Tremolo:** LFO1 auf AMPLITUDE und gewünschte LFO-Wellenform einstellen.

## 11. Effekte — FX

![FX-Seite](../assets/images/RTAL_SYNTECH_03_FX.png)

**CHORUS ON/OFF** aktiviert einen breiter und bewegter klingenden Chorus; **CHORUS MIX** bestimmt seine Stärke, **RATE** die Bewegungsgeschwindigkeit und **DEPTH** die Modulationstiefe. **REVERB ON/OFF** aktiviert räumlichen Hall; **REVERB MIX**, **SIZE** und **DAMPING** bestimmen Anteil, Raumgefühl und Dämpfung. Für klare Bässe oft weniger Hall, für Pads mehr Chorus und ein moderater Hallanteil.

## 12. Ebenen und Spielarten

**UPPER / LOWER:** Über LAYER wählen Sie die Klangebene, deren Parameter Sie bearbeiten. **SINGLE** arbeitet als einzelner Klang, **SPLIT** teilt den Tastaturbereich an einer eingestellten Split-Note und **DOUBLE** schichtet zwei Klanganteile. Im DOUBLE-Betrieb kann ein Tastenanschlag mehr als eine interne Stimme beanspruchen; die maximal gleichzeitig spielbare Notenzahl hängt vom Modus und DSP-Budget ab. Der Splitpunkt ist über MIDI-CC zugänglich; Details in der CC-Referenz.

## 13. Presets und SD-Karte

![Preset-Dialog](../assets/images/RTAL_SYNTECH_02_Preset.png)

### 13.1 Speicherorte

Die Firmware verwendet `/RTAL_SYNTECH/PRESETS/P000.rtal` bis `/RTAL_SYNTECH/PRESETS/P127.rtal`. P000–P079 sind als Factory-Bereich vorgesehen, P080–P127 als User-Bereich. **Wichtig:** Die Benutzeroberfläche bzw. Codepfade ermöglichen Speichervorgänge; die Bezeichnung „Factory“ ist kein technisch garantierter Schreibschutz. Fertigen Sie vor Änderungen ein Backup an.

### 13.2 Laden

Mit den Pfeiltasten einen Presetplatz wählen und loslassen. Oder **BANK** öffnen, SLOT ändern und **LOAD** betätigen. Die Firmware prüft Header, Dateigröße, Version und CRC. Fehlende oder beschädigte Dateien werden nicht stillschweigend ersetzt.

### 13.3 Umbenennen und Speichern

Den Preset-Dialog öffnen. Das Namensfeld antippen und den Namen bearbeiten, Ziel-SLOT auswählen, **SAVE** ausführen. Vorher prüfen, ob dort ein vorhandenes Preset überschrieben werden darf. Der Speichervorgang verwendet eine temporäre Datei und Backup-/Umbenennungslogik für erhöhte Datenintegrität, ersetzt jedoch kein externes Backup.

### 13.4 Program Change

MIDI Program Change 0–127 adressiert die entsprechenden Presetnummern. **PC 0 = P000, PC 127 = P127**; PC127 hat keine Sonderfunktion. In verschiedenen DAWs werden Program Changes in der Oberfläche alternativ ab 1 nummeriert – die tatsächlich gesendete MIDI-Zahl ist maßgeblich.

## 14. MIDI vollständig einrichten

![MIDI-Setup](../assets/images/RTAL_SYNTECH_01_MIDI_Setup.png)

### 14.1 Schnittstellen und Kanal

MIDI wird über DIN-UART und USB MIDI verarbeitet. DIN arbeitet mit 31.250 Baud. Im MIDI SETUP sehen Sie den aktiven Modus und Kanal. **CH − / CH +** ändern den MIDI-Kanal von 1 bis 16; die Auswahl wird im nichtflüchtigen Speicher gehalten. Ein OMNI-Modus ist aus dem geprüften Quelltext nicht als auswählbarer Standard belegt. MIDI-Sende- und Empfangsverhalten hängen vom angeschlossenen Controller und seiner korrekten Kanaleinstellung ab.

### 14.2 STANDARD MODE

Jeder Parameter besitzt eine direkte CC-Nummer. Beispiel auf Kanal 1: Filter Cutoff = CC27, Resonance = CC28, OSC1 Wave = CC14. Es muss keine Bank vorgewählt werden. Geeignet für Controller mit vielen eindeutig zuweisbaren Reglern.

### 14.3 MOPHO MODE (Standard)

Hier wählt **CC32** zunächst die Steuerbank: 0 (Oszillatoren/Mixer/Filter/Performance), 1 (LFO1, FX und LFO1-Matrix) oder 2 (LFO2 und LFO2-Matrix). Danach bestimmt die zugeordnete CC-Nummer den Parameter. Die Bank bleibt bestehen, bis sie umgeschaltet wird. Beispiel: `CC32 = 0`, danach `CC102 = 100` setzt Filter Cutoff; `CC32 = 1`, danach `CC106 = 64` steuert LFO1 Rate.

> Die Zuordnung ist **bankabhängig**: CC106 bedeutet in Bank 0 Filter Env Amount, in Bank 1 LFO1 Rate und in Bank 2 LFO2 Rate.

### 14.4 MIDI LEARN

Im MIDI-Dialog einen Parameter mit **PARAM − / PARAM +** auswählen und anschließend **LEARN** drücken. Bewegen Sie den Controller, der diesem Parameter zugeordnet werden soll. Die Firmware prüft reservierte/mehrdeutige CCs und speichert zulässige Zuweisungen separat für STANDARD und MOPHO. **DEFAULT** setzt die Zuordnung des aktiven Modus zurück (je nach Bedienzustand ist ein Bestätigungsschritt vorgesehen). MIDI Learn ist kein allgemeiner MIDI-Recorder; eine gültige CC-Nachricht auf dem gewählten Kanal wird benötigt.

### 14.5 Noten, All Notes Off und All Sound Off

Note On/Off spielt und beendet Noten. **CC123 – All Notes Off** beendet gehaltene Noten nach MIDI-Semantik; **CC120 – All Sound Off** stoppt die Wiedergabe unmittelbar. Beide werden gemäß V1.0.0 auch bei aktivem MIDI Learn berücksichtigt. Prüfen Sie bei Notenhängern zunächst Kanal, physikalische MIDI-Verbindung und ob ein Controller seine Note-Off-Nachrichten sendet.

### 14.6 Auszug aus der CC-Tabelle

| Parameter | STANDARD | MOPHO |
|:--|--:|:--|
| OSC1 Wave | CC14 | Bank 0, CC22 |
| OSC2 Wave | CC15 | Bank 0, CC26 |
| Filter Cutoff | CC27 | Bank 0, CC102 |
| Filter Resonance | CC28 | Bank 0, CC103 |
| LFO1 Rate | CC42 | Bank 1, CC106 |
| LFO2 Rate | CC45 | Bank 2, CC106 |
| Chorus Mix | CC48 | Bank 1, CC28 |
| Reverb Mix | CC51 | Bank 1, CC102 |

Die **vollständigen 61 Parameterzuordnungen** stehen in [`MIDI_CC_REFERENCE.md`](MIDI_CC_REFERENCE.md), zusätzlich als Excel-Originaltabelle. MIDI Learn kann die Werkseinstellungen verändern.

## 15. Klanggestaltung — praktische Einstiegspatches

### 15.1 Weicher Analog-Pad

Wählen Sie zwei obertonreiche Oszillator-Wellenformen, leichtes Detune, mittleren Filter-Cutoff, wenig Resonanz, längere AMP-Attack/Release und sanften Chorus. Mit einem langsamen LFO auf PWM entsteht Bewegung, ohne dass eine Sequenz programmiert werden muss.

### 15.2 Prägnanter Synth-Brass

Zwei kräftige Oszillatorpegel, kurze AMP-Attack, Filter-ENV mit mittlerem Decay, deutlich hörbare ENV AMOUNT und moderat erhöhte Resonanz. Im DOUBLE-Modus können Schichten den Klang verbreitern – dabei die Stimmenzahl berücksichtigen.

### 15.3 Sync-Lead

OSC SYNC einschalten, Oszillatoren unterschiedlich stimmen und mit Filterfahrten kombinieren. Leichtes Pitch-Vibrato über Matrix A ergänzt den Klang. Bei starker Cross-Modulation zunächst einzeln spielen und XRUN-Anzeige beachten.

### 15.4 Metallic/Bell

Ring-Modulation und eine dosierte Cross-Modulation ergeben metallische Obertonspektren. Für anschlagartige Klänge AMP-Attack kurz und AMP-Decay passend einstellen. Einen Hauch Reverb hinzufügen.

## 16. Betrieb, Leistung und Fehlersuche

| Symptom | Mögliche Ursache | Maßnahme |
|:--|:--|:--|
| Kein Ton nach Boot | Fehlende/ungültige SD-Presets | SD-Struktur und mindestens ein gültiges `.rtal` prüfen |
| Display startet nicht | Board-/Flash-Konfiguration, Verkabelung | Richtige JC4827W543-Konfiguration kontrollieren |
| Kein MIDI | Falscher Kanal, RX/TX verwechselt, MIDI-Interface fehlt | MIDI-Dialog, 31.250 Baud und Hardware prüfen |
| Kein USB-MIDI | Falscher USB-Modus/Host | TinyUSB/USB-OTG und Verbindung kontrollieren |
| Preset wird nicht geladen | CRC-/Versionsfehler oder fehlende Datei | Backup und korrekten Formatstand verwenden |
| Knackser bei Akkorden | DSP-Budget überzogen, besonders CROSS1/2 | Weniger gleichzeitige Noten, Modulationsintensität senken |
| XRUN-Zähler steigt | Audiotask nicht rechtzeitig fertig | Patchlast und Systemkonfiguration prüfen |
| Touch reagiert unerwartet | Berührung/Wischbewegung, Release-Entprellung | Finger vollständig abheben und erneut tippen |
| Slider-Infodialog erscheint nicht | Slider bewegt / keine 1,5 s gehalten | Parameter ruhend berühren |
| MIDI CC wirkt anders als erwartet | Falsche Bank oder Learn-Zuweisung | CC32, Betriebsmodus und Mapping prüfen |

**Audio-Timing:** 256 Samples bei 32.000 Hz entsprechen **8 ms** je Block. Die Audiobearbeitung muss mit Sicherheitsreserve innerhalb dieses Zeitfensters abschließen. Ein einzelner störungsfreier Screenshot ersetzt keinen Belastungstest.

## 17. Pflege, Backups und Versionswechsel

Vor Firmware-Updates die vollständige SD-Karte und insbesondere `/RTAL_SYNTECH/PRESETS/` sichern. Die `.rtal`-Dateien enthalten formatabhängige Binärdaten. Vor einem Versionswechsel prüfen, ob die Firmware ausdrücklich kompatibel ist. Unverifizierte alte Presets nicht überschreiben. Bei der Weitergabe von Projekten MIDI-Learn-Zuordnungen und MIDI-Kanal dokumentieren; sie können im NVS unabhängig von Presets gespeichert werden.

## 18. Technischer Anhang

**Engine:** fixed-point DSP auf Grundlage von Joakim Erikssons SCINTILLA; **DSP-Sampling:** 32 kHz; **Blockgröße:** 256 Frames; **Ausgabe:** 32-Bit-I²S-Slots, Stereo; **UI:** LVGL; **Tasking:** dedizierte Audiotask plus GUI/MIDI und FreeRTOS-Befehlsqueues. Ein Preset verwendet einen versionierten Binärheader, CRC und eine strukturabhängige Patch-Payload. Änderungen an `sc_patch_t` können Formatkompatibilität beeinflussen.

**Quellen:** die in dieser Veröffentlichung enthaltene Firmware V1.0.0 FINAL, `SyntechMidiCC.h`, die hochgeladene MIDI-Excel-Tabelle sowie die bereitgestellten Bedienoberflächenbilder. Für Entwicklerdetails siehe das [Release-Audit](RELEASE_AUDIT_V1.0.0.md). Dieses Handbuch ist keine elektrische Zertifizierung und keine Garantie für einen vollständigen Werks-Presetbestand.

## 19. Danksagung, Lizenz und Marken

**Joakim Eriksson** und sein Projekt [SCINTILLA](https://github.com/joakimeriksson/scintilla-fm1) stellen eine zentrale Quellcode- und DSP-Grundlage dar. SYNTECH ist eine Weiterentwicklung mit eigener ESP32-S3-Integration und Touch-Bedienung; die ursprünglichen Leistungen sind ausdrücklich anzuerkennen. Das abgeleitete Firmwarequellpaket steht unter GPL-3.0-only unter Beachtung der mitgelieferten Drittanbieterhinweise. „Elka“ und „Synthex“ dienen ausschließlich zur Beschreibung der historischen Inspiration; keine offizielle Verbindung oder Billigung ist impliziert.

---

*RTAL SYNTECH · Benutzerhandbuch V1.0.0 · 11. November 2026*
