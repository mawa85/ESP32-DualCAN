# Model-Y-Simulator ohne Auto (T-2CAN FD)

Mit einem Board, ohne Auto und ohne zweiten CAN-Adapter: CAN-A sendet simulierte
Model-3/Y-Fahrzeugdaten, CAN-B empfängt sie und prüft Verdrahtung, Abschluss und Bitrate.

Dateien:

| Datei | Zweck |
|---|---|
| `firmware/test_scripts/model_y_sim.be` | Berry-Skript (Sender + Empfangsprüfung), Signale per DBC-Namen |
| `firmware/test_scripts/model_y_sim.bep` | Dasselbe, mit der DBC vorverarbeitet (das läuft auf dem Gerät) |
| `tools/upload_script.py` | Hochladen (.be + .bep), Filter setzen, Starten/Stoppen per BLE (bleak) |
| `tools/dbc_prep/preprocess.ts` | Vorverarbeitung .be → .bep mit dem Web-UI-Preprocessor (Node.js) |
| `tools/can_filters.json` | RX-Filterprofile `bench`, `juniper`, `none` |
| `tools/sim_host/` | Host-Test: echte Berry-VM und Firmware-Bindings, CAN-Treiber als Schleife |

## Verdrahtung

Die Klemmen P2 (CAN-A) und P1 (CAN-B) haben je vier Pole: `CANH`, `CANL`, `SGND`, `DGND`.
Die Reihenfolge ist laut Schaltplan **nicht** aufgedruckt, also vorher messen.

- CAN-A `CANH` an CAN-B `CANH`
- CAN-A `CANL` an CAN-B `CANL`
- CAN-A `SGND` an CAN-B `SGND`. Das wird empfohlen: Die `TD501MCAN` sind **isolierte**
  Transceiver, ihre CAN-Seite liegt nicht auf der Board-Masse, nur über 1 MΩ / 1 nF.
- `DGND` nicht anschließen.

Kurze, verdrillte Drähte reichen.

## Abschluss (60 Ω)

Laut Schaltplan T-2Can-Fd_V1.0 sitzen `RZ2` (CAN-A) und `RZ1` (CAN-B) mit je 120 Ω **fest**
zwischen H und L, ohne Schalter oder Jumper. Verbindest du die beiden Busse, liegen sie
parallel, und es ergeben sich automatisch ~60 Ω.

Prüfen, ohne Strom, Multimeter zwischen H und L:

- **~60 Ω**: passt.
- **~120 Ω**: eine Leitung hat keinen Kontakt, oder ein Widerstand fehlt.
- **~40 Ω oder weniger**: ein zusätzlicher Abschluss hängt dran.

**Der 2-fach-DIP-Schalter ist nicht der Abschluss.** Er schaltet GPIO3 des ESP32-S3:
Pol „RX“ an `INT1#` (RX-Interrupt) des MCP2518FD, Pol „IO3“ an Pad 26 des Erweiterungs-Headers U8.
Die Dorky-Firmware nutzt GPIO3 nur als Eingang mit Pull-up, für den Simulator ist die
Stellung egal.

## Strom

`VCC_5V` (Versorgung der Transceiver) kommt über eine Diode von USB (~4,7 V) **oder** vom
12–24-V-Eingang. Wenn nur gesendet, aber nichts empfangen wird, das Board über die
Schraubklemme mit 12 V versorgen.

## Starten

**Vom PC aus (empfohlen):** Zuerst die Web-UI trennen, sonst hält der Browser die BLE-Sitzung.

```
pip install bleak
python tools/upload_script.py firmware/test_scripts/model_y_sim.be --filters bench --log 120
```

Das Skript setzt die RX-Filter (`bus.set_filter`), lädt Quelltext und `.bep` in einem
`script.write` hoch, aktiviert per `script.enable` und zeigt 120 s lang das Log. Liegt
`model_y_sim.bep` neben der `.be`, braucht der PC kein Node.js. Antwortet `script.enable`
nicht innerhalb von 30 s, fragt das Tool `script.list` ab und meldet „aktiviert“, wenn das
Skript läuft. Bei Verbindungsfehlern (GATT-Timeout) versucht es 3-mal neu. Das Skript läuft sofort und startet nach jedem Reboot automatisch, sobald
der Bus lebt oder spätestens nach 5 s.

- Stoppen, ohne Autostart: `python tools/upload_script.py --stop model_y_sim.be`
- Löschen: `python tools/upload_script.py --delete model_y_sim.be`
- Nur das Log mitschneiden: `python tools/upload_script.py --log 300`
- Übersicht: `python tools/upload_script.py --list`

**Über die Web-UI:** Fahrzeug „Tesla Model Y“ wählen (lädt die DBCs), Automations / Scripts,
neues Skript `model_y_sim.be` anlegen, den Inhalt einfügen, speichern (der Preprocessor löst
die Signalnamen auf) und einschalten. Ohne geladene DBC lässt sich das Skript nicht speichern. Auf der Events-Seite erscheinen
zwei Kacheln: „Sim Start/Stop“ (Pause) und „Sim Report“.

## Einstellungen

Am Kopf des Skripts in `mys_cfg`:

| Schlüssel | Default | Bedeutung |
|---|---|---|
| `enabled` | `true` | `false`: lädt nur, sendet nicht |
| `tx_bus` | `0` | Sendebus: 0 = CAN-A (MCP2518FD), 1 = CAN-B (TWAI) |
| `rx_bus` | `1` | Kontrollbus, `-1` = keine Prüfung |
| `speed` | `1.0` | Zeitraffer des Fahrzyklus |
| `tick_ms` | `20` | Grundtakt. Nicht unter 20, die Firmware lässt pro ID höchstens 100 Hz zu. |
| `report_s` | `10` | Abstand der Zusammenfassung |
| `log_phase` | `true` | Phasenwechsel loggen |
| `throttle` | `true` | Kommt 3 s nichts an, sendet das Skript nur noch 1 Frame/s je ID, damit die TX-Queue die Firmware nicht blockiert. |

## Was simuliert wird

Fahrzyklus von 55 s Länge, läuft in Schleife:

1. Park: P, Fahrertür von 3 bis 6 s offen
2. Ready: Bremse, P → D
3. Anfahren: 0 → 100 km/h, Fahrpedal 40 %, 2,8 m/s², Blinker links
4. Cruise: 100 km/h, Spurwechsel mit Blinker rechts
5. Bremsen: 100 → 0 km/h, −3,5 m/s²
6. Rückwärts einparken: R, bis 5 km/h, Lenkeinschlag bis 400°
7. Park: P, Kofferraum offen

Die Frames liegen auf dem Vehicle-Bus. Bit-Layouts kommen aus `dbc/Model3_VEH.dbc`, die Perioden
aus joshwardell/model3dbc (`GenMsgCycleTime`) oder sind geschätzt:

| ID | Name | Signale | Periode |
|---|---|---|---|
| 0x118 | DI_systemStatus | Gang, Bremspedal, Fahrpedal, Beschleunigung, systemState | 20 ms (echt 10) |
| 0x129 | SCCM_steeringAngleSensor | Lenkwinkel, Lenkgeschwindigkeit | 20 ms (echt 10) |
| 0x257 | DI_speed | Geschwindigkeit (beide Bitlagen 12\|12 und 43\|13), UI-Speed km/h | 20 ms |
| 0x39D | IBST_status | Bremse betätigt, Pedalweg | 40 ms (geschätzt) |
| 0x292 | BMS_socStatus | SoC min/UI/max/avg, sinkt beim Fahren | 100 ms |
| 0x3F5 | VCFRONT_lighting | Blinker links/rechts (~1,5 Hz), Licht, Blinkerhebel | 100 ms (geschätzt) |
| 0x102 | VCLEFT_doorStatus | Türen links (Status und Schloss) | 100 ms (geschätzt) |
| 0x103 | VCRIGHT_doorStatus | Türen rechts, Kofferraumschloss | 100 ms (geschätzt) |

0x118, 0x129, 0x257 und 0x39D tragen einen Counter (Bit 8\|4) und die Tesla-Summen-Checksumme
in Byte 0, wie `firmware/src/can/tesla.c`. Ob 0x129 („Crc“) wirklich diese Checksumme nutzt,
ist nicht bestätigt.

## Log lesen

```
SIM cruise v=100 gear=D soc=79.3 | tx=2150 rx=1764
SIM rx/tx (Counter-IDs) bzw. ok 118:500/500 129:500/500 257:500/500 39D:250/250 292:ok 3F5:ok 102:ok 103:ok
SIM alle 8 IDs empfangen
```

- Bei Counter-IDs ist `rx/tx` eine echte Zählung. Weichen die Werte um mehr als 10 % ab,
  kommt `SIM Verluste` (dann Abschluss oder Fehlerframes prüfen).
- `SIM FEHLT auf bus1: …` heißt, es kommt nichts an: H/L vertauscht, Kontakt, Bitrate, Strom.
- `SIM kein Empfang … drossele auf 1 Hz` und später `Empfang wieder da` erscheinen beim Ab- und
  Anstecken.

## Host-Test

```
FW=firmware tools/sim_host/build.sh
mkdir -p /tmp/s/berry_test_scripts && cp firmware/test_scripts/model_y_sim.be* /tmp/s/berry_test_scripts/
cd /tmp/s && SIM_DUMP=/tmp/frames.txt <repo>/tools/sim_host/run_sim 60 22 41   # 60 s, Kabel ab bei 22 s, dran bei 41 s
python3 <repo>/tools/sim_host/decode_check.py <repo>/dbc/Model3_VEH.dbc /tmp/frames.txt   # pip install cantools
```

Der Host-Test nutzt die echte Berry-VM und die echten Dateien `berry_bindings.c`,
`berry_registry.c`, `script_loader.c`, `can.c` und `rate_limit.c`. Nur der CAN-Treiber ist
ersetzt: TX auf Bus 0 landet als RX auf Bus 1.

`.bep` neu erzeugen, nachdem die `.be` geändert wurde:

```
node tools/dbc_prep/preprocess.ts firmware/test_scripts/model_y_sim.be \
     firmware/test_scripts/model_y_sim.bep dbc/Model3_VEH.dbc dbc/Model3_CH.dbc
```

## DBC und Model Y Juniper

Die Firmware hat keine DBC an Bord. Wie in der Web-UI werden Nachrichten- und Signalnamen
beim Speichern gegen die DBC je Bus in Startbit, Länge, Faktor und Offset übersetzt (`.bep`).
Verwendet werden die mitgelieferten Tesla-DBCs: `dbc/Model3_VEH.dbc` (Bus 0, Vehicle-CAN) und
`dbc/Model3_CH.dbc` (Bus 1). Gegengeprüft wurde mit joshwardell/model3dbc und
commaai/opendbc (`tesla_model3_vehicle.dbc`, `tesla_model3_party.dbc`).

| Punkt | Stand |
|---|---|
| Busse am rechten A-Säulen-Port, Juniper ab SOP10 (Berlin ab 01.04.2026) | Party-, Vehicle- und Body-CAN. Chassis-CAN liegt am neuen linken Port X177 (Tesla-Schaltplan, Community-Bericht) |
| CAN-B im Auto | daher Party-CAN statt Chassis-CAN. Für Bus 1 passt dann `Model3_PARTY.dbc` bzw. opendbc `tesla_model3_party.dbc` |
| IDs 0x118, 0x129, 0x257, 0x39D, 0x292, 0x3F5, 0x102, 0x103 | aus Model 3/Y übernommen, **am Juniper nicht verifiziert** |
| `DI_speed` 0x257 | DBCs unterscheiden sich: VEH `DI_vehicleSpeed` 43\|13, opendbc 12\|12 (VEH-Name `DI_opdVehicleModelSpeedRef`). Der Simulator setzt beide. `DI_uiSpeedUnits` VEH 33\|1, opendbc 32\|1 |
| `IBST_status` 0x39D | VEH 8 Byte, opendbc Party 5 Byte |
| Checksumme Byte 0, Counter 8\|4 | Model 3/Y-Verfahren (`tesla.c`), am Juniper nicht verifiziert |

Vor der ersten Fahrt mit eigenen Skripten: 1–2 Minuten mit `trace.start` (Web-UI Trace)
mitschneiden und die Werte gegen das Kombiinstrument prüfen.

## RX-Filter

Nur die benötigten IDs werden an Skripte und an das BLE-Streaming weitergegeben.
`bus.set_filter {bus, ids}` speichert die Liste in NVS (`busconfig/f0`, `f1`) und gilt sofort.

- CAN-A (MCP2518FD): bis 32 IDs exakt als Hardware-Filter.
- CAN-B (TWAI): eine Sammelmaske in Hardware, der Rest wird in Software verworfen.
- Bus-Gesundheit und RX-Rate zählen weiter alle angenommenen Frames.
- Senden wird nie gefiltert.

| Profil | Bus 0 (Vehicle) | Bus 1 |
|---|---|---|
| `bench` | wie `juniper` | die 8 Simulator-IDs |
| `juniper` | 102 103 118 129 257 273 292 294 2C2 2C3 2E1 313 334 39B 39D 3E9 3F5 | Party: 118 129 145 257 2B9 311 39D |
| `none` | alles | alles |

Konsole: `filter` zeigt die aktiven Listen.

## BLE-Streaming

- Notifies gehen erst raus, wenn der PC/Browser TX abonniert hat, spätestens 2,5 s nach dem Verbinden. Die Service-Discovery hat so die BLE-Puffer für sich.
- Trace nur auf Abo: `trace.start {buses, ids, max_fps}`, Standard 200 Frames/s, maximal 1000. Beim Trennen endet das Abo.
- Trace, Bus-Status, CPU-Info, Log-Spiegel und Berry-`print` werden bei vollem BLE-Puffer verworfen, statt die Firmware zu blockieren.
- Antworten auf Befehle gehen nie verloren.
- Trace wird gebündelt: höchstens 40 Notifies/s, je Notify so viele Frames wie in MTU−3 Bytes passen (alle 25 ms oder wenn voll).
  - Standard: alte `{"type":"trace",…}`-Frames hintereinander in einem Notify (alte Web-UI versteht das).
  - `trace.start {…, batch:true}`: kompakt `{"type":"trace_batch","f":[[bus,id,ts,"HEX"],…]}`, etwa 18 statt 7 Frames pro Notify. Die neue Web-UI fragt das an.
- Trace wird nur gesendet, wenn genug NimBLE-Puffer (msys) frei sind und der interne RAM über der Trace-Schwelle liegt: frei ≥ 10 KB und größter Block ≥ 4 KB, mit 4 KB Hysterese. Sonst warten die Frames in einem Ring (128). Läuft der Ring über, fallen die ältesten weg.
  - Schwellen ändern: `trace.start {…, iram_floor:8192, iram_big_floor:4096}` oder USB-Konsole `trace gate 8192 4096`.
- Status-Pushes (Bus-Status, CPU, Script-Status, Logs) prüfen nur die msys-Puffer, nie den Heap. Antworten auf Befehle warten immer.
- USB-Konsole: `trace status` zeigt die Zähler, `mem` zeigt internen RAM, PSRAM und die Task-Stacks. Beim Booten stehen `mem boot/can/fs/wifi/tesla/scripts/ble`-Zeilen im Log.
- WLAN startet nur, wenn Zugangsdaten gespeichert sind (sonst bleibt es aus und spart internen RAM). Der Datei-Server startet erst, wenn WLAN eine IP hat.
- `trace.stop` liefert `sent`, `notifies`, `rate_dropped` (max_fps), `ble_dropped` (Ring/BLE voll), `push_dropped` und `dropped` (alter Name für `rate_dropped`).
- Heartbeat (Debug an) zeigt `iram`/`iram_min` und eine Zeile `trace=… sent=… rate_drop=… ble_drop=… | ble ntf ok=… enomem=… msys_free=…`.

## Safe Mode und USB-Konsole

**Safe Mode:** Board einschalten oder RESET drücken, **danach** BOOT drücken und gedrückt
halten, mindestens 0,5 s innerhalb der ersten 3 s. Persistente Skripte starten dann nicht.
BLE und Web-UI laufen normal. Im Log steht `SAFE MODE`. BOOT nicht schon beim Reset halten:
dann startet der ROM-Download-Modus.

**Konsole** (USB, COMx, 115200; z. B. `pio device monitor` oder PuTTY), Befehl + Enter:

| Befehl | Wirkung |
|---|---|
| `help` | Befehle anzeigen |
| `scripts` | Skripte und Status |
| `script stop all` | alle Skripte stoppen; Autostart bleibt für den nächsten Boot |
| `script stop <datei>` / `script start <datei>` | einzeln, wird gespeichert |
| `safe` | alle stoppen **und** Autostart leeren |
| `filter` | aktive RX-Filter |
| `debug on` / `debug off` / `debug status` | Debug-Log |
| `reboot` | Neustart |
