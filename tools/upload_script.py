#!/usr/bin/env python3
"""Laedt ein Berry-Skript per BLE auf den Dorky Commander (ESP32-DualCAN),
aktiviert es (= Autostart nach Reboot) und zeigt N Sekunden lang das Log.

Protokoll (firmware/src/protocol/protocol.c, webui/src/transport/protocol.ts):
  Service 6e400001-..., RX (schreiben) 6e400002-..., TX (notify) 6e400003-...
  Frame = 4 Byte Laenge little-endian (oberes Nibble = Typ, 0 = JSON) + JSON
  Ops: system.info, script.write {filename, code}, script.enable/disable/delete
  {filename}, script.list. Logs kommen als {"type":"log", ...}.

WICHTIG: Vorher die Web-UI trennen (Disconnect / Tab schliessen). Die Firmware
haelt nur eine Sitzung; eine neue autorisierte Verbindung wirft die alte raus.

Beispiele (Windows, Python 3.14, pip install bleak):
  python upload_script.py model_y_sim.be             # hochladen, starten, 60 s Log
  python upload_script.py model_y_sim.be --log 300
  python upload_script.py --stop model_y_sim.be      # deaktivieren (kein Autostart)
  python upload_script.py --delete model_y_sim.be
  python upload_script.py --log 120                  # nur Log mitschneiden
  python upload_script.py --list
"""
import argparse
import asyncio
import json
import os
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # wir schreiben
TX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # Geraet notifiziert


class Dorky:
    def __init__(self, client, fast=False, quiet_raw=False):
        self.c = client
        self.fast = fast
        self.quiet_raw = quiet_raw
        self.buf = bytearray()
        self.next_id = 1
        self.pending = {}
        self.t0 = time.time()

    def _on_notify(self, _sender, data: bytearray):
        if len(data) >= 2 and data[0] == 0xFD:          # Disconnect-Grund der Firmware
            print("!! Firmware meldet: Sitzung ersetzt (andere Verbindung)" if data[1] == 1
                  else f"!! Firmware-Notify 0xFD {data[1]:#x}")
            return
        self.buf += data
        while len(self.buf) >= 4:
            hdr = struct.unpack_from("<I", self.buf)[0]
            ln, typ = hdr & 0x0FFFFFFF, hdr >> 28
            if len(self.buf) < 4 + ln:
                return
            payload = bytes(self.buf[4:4 + ln])
            del self.buf[:4 + ln]
            if typ != 0:
                continue
            try:
                msg = json.loads(payload)
            except ValueError:
                print("?? ungueltiges JSON:", payload[:80])
                continue
            self._handle(msg)

    def _handle(self, msg):
        if "id" in msg and "ok" in msg and msg["id"] in self.pending:
            fut = self.pending.pop(msg["id"])
            if not fut.done():
                fut.set_result(msg)
            return
        t = msg.get("type")
        ts = f"[{time.time() - self.t0:7.1f}s]"
        if t == "log":
            if "msg" in msg:
                print(ts, f"{msg.get('src', '')}:", msg["msg"])
            elif "raw" in msg and not self.quiet_raw:
                print(ts, msg["raw"])
        elif t == "script_update":
            print(ts, "script_update:", msg.get("script"))
        # trace/bus_status usw. ignorieren

    async def start(self):
        await self.c.start_notify(TX_CHAR, self._on_notify)

    async def call(self, op, timeout=10.0, **params):
        rid = self.next_id
        self.next_id += 1
        body = json.dumps({"op": op, "id": rid, **params}, separators=(",", ":")).encode()
        frame = struct.pack("<I", len(body) & 0x0FFFFFFF) + body
        fut = asyncio.get_running_loop().create_future()
        self.pending[rid] = fut
        mtu = getattr(self.c, "mtu_size", 23) or 23
        chunk = max(20, min(mtu - 3, 244)) if mtu > 23 else 180
        for off in range(0, len(frame), chunk):
            await self.c.write_gatt_char(RX_CHAR, frame[off:off + chunk], response=not self.fast)
            if self.fast:
                await asyncio.sleep(0.01)
        try:
            resp = await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self.pending.pop(rid, None)
            raise RuntimeError(f"Timeout bei {op}")
        if not resp.get("ok"):
            raise RuntimeError(f"{op} fehlgeschlagen: {resp.get('error')}")
        return resp.get("result")


async def find(args):
    if args.address:
        return args.address
    print(f"Suche {args.name} ...")
    dev = await BleakScanner.find_device_by_name(args.name, timeout=args.scan)
    if dev is None:
        devs = await BleakScanner.discover(timeout=5.0, service_uuids=[SERVICE])
        dev = devs[0] if devs else None
    if dev is None:
        sys.exit("Nicht gefunden. Web-UI getrennt? Board an? Antenne dran?")
    print("Gefunden:", dev.address, dev.name)
    return dev


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", nargs="?", help="Pfad zur .be-Datei (Dateiname auf dem Geraet = Basisname)")
    ap.add_argument("--name", default="Dorky-21F2", help="BLE-Name (Default Dorky-21F2)")
    ap.add_argument("--address", help="BLE-Adresse statt Suche, z. B. 28:84:85:27:21:F2")
    ap.add_argument("--scan", type=float, default=15.0, help="Suchdauer s")
    ap.add_argument("--log", type=float, default=60.0, help="Log so viele Sekunden zeigen (0 = nicht)")
    ap.add_argument("--no-start", action="store_true", help="nur hochladen, nicht aktivieren")
    ap.add_argument("--stop", action="store_true", help="Skript deaktivieren (Autostart aus)")
    ap.add_argument("--delete", action="store_true", help="Skript auf dem Geraet loeschen")
    ap.add_argument("--list", action="store_true", help="Skripte auf dem Geraet auflisten")
    ap.add_argument("--fast", action="store_true", help="Write without response (schneller)")
    ap.add_argument("--no-raw", action="store_true", help="nur Berry-print()-Zeilen, keine ESP-Logs")
    args = ap.parse_args()

    target = await find(args)
    async with BleakClient(target, timeout=20.0) as client:
        print("Verbunden, MTU", getattr(client, "mtu_size", "?"))
        d = Dorky(client, fast=args.fast, quiet_raw=args.no_raw)
        await d.start()
        await asyncio.sleep(0.5)   # Firmware autorisiert/kickt nach ~100 ms
        if not client.is_connected:
            sys.exit("Sofort getrennt: Pairing-Fenster zu? BOOT kurz druecken und neu versuchen.")
        info = await d.call("system.info")
        print("Firmware:", info.get("fw_version"), "| proto", info.get("proto_version"),
              "| scripting_api", info.get("scripting_api_version"))

        fname = os.path.basename(args.script) if args.script else None
        if args.stop or args.delete:
            if not fname:
                sys.exit("Dateiname fehlt")
            await d.call("script.delete" if args.delete else "script.disable", filename=fname)
            print("geloescht" if args.delete else "deaktiviert", fname)
        elif fname:
            code = open(args.script, encoding="utf-8").read()
            if not code.endswith("\n"):
                code += "\n"
            if len(code.encode()) > 16 * 1024:
                print("Warnung: > 16 KB, script.read (Web-UI-Editor) kann sie nicht mehr anzeigen")
            print(f"Lade {fname} ({len(code.encode())} Bytes) ...")
            await d.call("script.write", timeout=30.0, filename=fname, code=code)
            print("geschrieben")
            if not args.no_start:
                await d.call("script.enable", filename=fname)
                print("aktiviert (laeuft jetzt und startet nach jedem Reboot automatisch)")

        if args.list or fname:
            res = await d.call("script.list")
            for s in res.get("scripts", []):
                flag = "AN " if s.get("enabled") else "aus"
                err = f"  FEHLER: {s.get('error')}" if s.get("errored") else ""
                print(f"  [{flag}] {s.get('filename')}{err}")

        if args.log > 0:
            print(f"Log fuer {args.log:.0f} s (Strg+C beendet) ...")
            end = time.time() + args.log
            try:
                while time.time() < end and client.is_connected:
                    await asyncio.sleep(0.5)
            except (KeyboardInterrupt, asyncio.CancelledError):
                pass
            if not client.is_connected:
                print("Verbindung verloren.")
    print("fertig, BLE getrennt")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
