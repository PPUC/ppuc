# RS485-Bus-Timing: Messanleitung

**Für Michael.** Eine Frage, drei Messungen. Sollte in einer Sitzung machbar
sein.

Dies ist die deutsche Fassung des Arbeitsteils von
[`BUS_MEASUREMENT.md`](BUS_MEASUREMENT.md). Der elektrische Hintergrund
(ADM3483-Schwellen, Vorspannung, Jumper) steht im Anhang der englischen Fassung
und wird für die Messungen unten **nicht** gebraucht.

---

## Was Runde 1 gezeigt hat

Zwei Boards auf dem Testsetup, stark abgespeckte YAML, 115200 Baud. Ergebnis:
alle ~10 ms ein Frame, saubere Pegel bei 3,54 V, gültiges v2-Framing
(`A5 09 01 25 01 01` decodiert als Sync / SwitchNoChange / nextBoard 1 /
Sequence 0x25 / Epoch 1) — und **keine Spur der 2000 µs**.

Das war richtig gemessen, der Fehler lag in der Anleitung.
`switchReplyDelayUs` ist **standardmäßig 0**, und zwar überall: in der Firmware,
in libppuc und im `ppuc`-Binary. Es ist **kein YAML-Feld**, kann also in keiner
Spiel-YAML stehen — auch nicht in einer vollständigen. Die alte Anleitung
behauptete, der Wert sei "derzeit auf 2000–4000 µs gesetzt", sagte aber nirgends,
dass man ihn setzen muss. Auf einem Testsetup mit Standardwerten gibt es die
"Blockerzeit" also schlicht nicht zu sehen.

Zwei Dinge folgen daraus, beide nützlich:

- **Zwei Boards ganz ohne Umschaltverzögerung laufen stabil.** Das ist eine
  belastbare Eingrenzung. Das Delay ist nicht grundsätzlich nötig — es ist
  etwas an *mehreren* Boards, das es nötig macht.
- Framing, CRC und Sequenzzählung sind auf einem ruhigen Bus in Ordnung. Der
  Verdacht verschiebt sich auf das, was passiert, wenn sich mehrere Boards
  abwechseln.

---

## Die Frage, die diese Runde beantwortet

> **Treiben jemals zwei Boards gleichzeitig den Bus — und wie nahe kommen sie
> daran?**

Sonst nichts. Überlappen sich zwei Treiber, haben wir die Ursache. Überlappen
sie sich auch bei Delay 0 nie, liegt die Ursache woanders und wir hören hier auf
zu suchen.

---

## Vorab: das Delay explizit setzen

Das ist der Schritt, der letztes Mal gefehlt hat. Der Wert muss auf der
Kommandozeile oder in der INI gesetzt werden — er steht nie in der Spiel-YAML:

```
ppuc … --switch-reply-delay-us 2000
```

oder in der INI-Datei:

```
SwitchReplyDelayUs = 2000
```

Ohne eines von beidem ist der Wert **0**. Bitte pro Aufnahme notieren, welcher
Wert aktiv war.

---

## Aufbau

- **Vier oder mehr Boards am Bus.** Das ist die einzige harte Voraussetzung.
  Zwei Boards können eine Kollision zwischen den Umschaltvorgängen
  *verschiedener* Boards nicht zeigen — und genau darum geht es. Playfield oder
  echte Lasten sind nicht nötig, die Boards müssen nur angesprochen werden und
  antworten.
- Abschluss und Vorspannung so, wie du sie normalerweise betreibst. Du hast
  gesagt, dass das korrekt aufgebaut ist, und ich bitte nicht darum, das erneut
  zu prüfen.
- Bitte notieren, welcher Host verwendet wird: Raspberry-Pi-Breakout oder
  USB-Adapter.

---

## Messpunkte

RP2040-Pins, laut `io-boards/src/main.cpp` und `io-boards/src/PPUC.h`:

| Kanal | Signal | Pin | Auf welchem Board |
|-------|--------|-----|-------------------|
| CH0 | **DE** (Sendefreigabe) | GPIO 2 | Board **A** |
| CH1 | **DE** (Sendefreigabe) | GPIO 2 | Board **B** |
| CH2 | RS485 **RX** | GPIO 1 | eines von beiden — führt den gesamten Busverkehr |
| GND | Masse | — | erforderlich |

**Die beiden DE-Kanäle sind die eigentliche Messung.** Letztes Mal war die
Aufnahme eine einkanalige UART-Messung. Die zeigt den Ablauf sehr schön, kann
aber nicht zeigen, ob sich zwei Treiber in die Quere kommen. DE zeigt exakt,
wann welches Board den Bus treibt.

Bitte zwei Boards wählen, die in der **Abfragereihenfolge benachbart** sind —
dort findet die Übergabe statt. Falls die Reihenfolge nicht offensichtlich ist,
sind Board 1 und Board 2 in Ordnung.

**Wie das DE-Signal aussehen sollte**, damit ein fester Versatz nicht als Fehler
gelesen wird: Die Firmware wartet nach dem Setzen von DE fest **50 µs**
(`RS485_MODE_SWITCH_DELAY`), bevor das erste Bit rausgeht — und ebenso rund um
das Zurücknehmen. Erwartet wird also: DE geht auf HIGH, ~50 µs Ruhe auf der
Leitung, dann Daten. Das konfigurierte `switchReplyDelayUs` liegt *vor* der
steigenden DE-Flanke, nicht danach.

---

## M1 — DE-Überlappung (die Hauptmessung)

Auf die **fallende Flanke von CH0** triggern (Board A gibt den Bus frei) und bis
über das Setzen von DE auf Board B hinaus aufzeichnen.

Für mehrere Übergaben notieren:

- **Abstand**: Zeit von der fallenden DE-Flanke auf Board A bis zur steigenden
  DE-Flanke auf Board B.
- **Überlappung**: Wird dieser Abstand jemals negativ — sind also beide
  DE-Leitungen gleichzeitig HIGH, und sei es nur kurz?
- Ob der darauf folgende Frame auf CH2 sauber decodiert.

Zuerst mit `--switch-reply-delay-us 2000`, danach mit `0`. Interessant ist, wie
sich der Abstand verändert und ob dabei eine Überlappung auftritt.

**Jede Überlappung, egal wie kurz, ist bereits die Antwort.** Ein einziger
Abtastpunkt mit beiden DE-Leitungen HIGH genügt — sie muss nicht weiter
charakterisiert werden.

## M2 — Delay-Messreihe

Gleicher Aufbau mit vier Boards, DE-Kanäle bleiben angeschlossen. Für jeden
Wert:

```
4000, 2000, 1000, 500, 200, 0
```

Jeweils ein paar Minuten laufen lassen und notieren: Verhält sich die Maschine
normal, gehen Schaltermeldungen verloren, und wie groß ist der kleinste
beobachtete DE-zu-DE-Abstand?

Gesucht ist der **kleinste Wert, der noch zuverlässig läuft**. Er bestimmt,
wie viel des Zyklus mit Warten statt mit Arbeit verbracht wird.

## M3 — Wo es bricht

Falls M2 einen Wert findet, bei dem es Probleme gibt: bitte einen Fehlerfall
aufzeichnen — CH2 rund um den Moment, in dem eine Schaltermeldung ausbleibt oder
verfälscht ankommt, zusammen mit beiden DE-Kanälen.

Eine gute Aufnahme des Fehlerfalls ist mehr wert als viele Aufnahmen des
Normalbetriebs.

---

## Was pro Durchlauf zu notieren ist

Kurz genügt — eine Zeile pro Durchlauf plus Screenshots:

| Feld | Beispiel |
|------|----------|
| Boards am Bus | 4 |
| `switchReplyDelayUs` | 2000 |
| Host | Pi-Breakout / USB-Adapter |
| Kleinster DE-zu-DE-Abstand | 82 µs |
| Überlappung beobachtet | nein |
| Verhalten | 5 min stabil / Schalter fielen aus |

---

## Was das klärt

- Ob die Kollision beim Umschalten zwischen Boards die Ursache der Instabilität
  bei mehreren Boards ist — oder ob es etwas anderes ist.
- Auf welchen Wert das Delay gefahrlos reduziert werden kann. Das ist
  Durchsatz, den wir zurückbekommen.

Zeigt M1 bei keinem Delay-Wert eine Überlappung, ist die Umschalt-Hypothese
erledigt. Die nächsten Verdächtigen wären dann der Störabstand der
Ruhepegel-Vorspannung am Empfänger (siehe Anhang der englischen Fassung) und die
Behandlung des Antwortfensters in der Firmware. So oder so wissen wir dann mehr.

---

## Noch eine Anmerkung zur Baudrate

Der Bus läuft mit 115200 (`kBaudRate` in `PPUCProtocolV2.h`), also bei rund 46 %
der 250 kbps, die der ADM3483 maximal kann. Da ist echter Spielraum — aber
**bitte für diese Messungen nicht verändern**. Eine andere Baudrate würde genau
den Vergleich verfälschen, auf den es hier ankommt. Das ist eine eigene Frage
für später, wenn die Stabilität geklärt ist.
