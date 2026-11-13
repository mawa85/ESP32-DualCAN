# Model-Y-Simulator ohne Auto (T-2CAN FD)

Mit einem Board, ohne Auto und ohne zweiten CAN-Adapter: CAN-A sendet simulierte
Model-3/Y-Fahrzeugdaten, CAN-B empfängt sie und prüft Verdrahtung, Abschluss und Bitrate.

Dateien:

| Datei | Zweck |
|---|---|
| `firmware/test_scripts/model_y_sim.be` | Berry-Skript (Sender + Empfangsprüfung) |
| `tools/upload_script.py` | Hochladen/Starten/Stoppen per BLE vom PC aus (bleak) |
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
python tools/upload_script.py firmware/test_scripts/model_y_sim.be --log 120
```

Das Skript lädt die Datei per `script.write` hoch, aktiviert sie per `script.enable` und zeigt
120 s lang das Log. Das Skript läuft sofort und startet nach jedem Reboot automatisch, sobald
der Bus lebt oder spätestens nach 5 s.

- Stoppen, ohne Autostart: `python tools/upload_script.py --stop model_y_sim.be`
- Löschen: `python tools/upload_script.py --delete model_y_sim.be`
- Nur das Log mitschneiden: `python tools/upload_script.py --log 300`
- Übersicht: `python tools/upload_script.py --list`

**Über die Web-UI:** Automations / Scripts, neues Skript `model_y_sim.be` anlegen, den Inhalt
einfügen, speichern und einschalten. Es braucht keine DBC. Auf der Events-Seite erscheinen
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
mkdir -p /tmp/s/berry_test_scripts && cp firmware/test_scripts/model_y_sim.be /tmp/s/berry_test_scripts/
cd /tmp/s && SIM_DUMP=/tmp/frames.txt <repo>/tools/sim_host/run_sim 60 22 41   # 60 s, Kabel ab bei 22 s, dran bei 41 s
python3 <repo>/tools/sim_host/decode_check.py <repo>/dbc/Model3_VEH.dbc /tmp/frames.txt   # pip install cantools
```

Der Host-Test nutzt die echte Berry-VM und die echten Dateien `berry_bindings.c`,
`berry_registry.c`, `script_loader.c`, `can.c` und `rate_limit.c`. Nur der CAN-Treiber ist
ersetzt: TX auf Bus 0 landet als RX auf Bus 1.
