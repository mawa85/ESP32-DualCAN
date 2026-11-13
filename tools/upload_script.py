#!/usr/bin/env python3
"""Laedt ein Berry-Skript per BLE auf den Dorky Commander (ESP32-DualCAN),
wie es die Web-UI macht: Quelltext (.be) + mit der DBC vorverarbeitete
Laufzeitdatei (.bep), optional RX-Filter (bus.set_filter), dann aktivieren
(= Autostart nach Reboot) und N Sekunden Log zeigen.

DBC: Die Firmware hat keine DBC an Bord. Signalnamen (can_msg_new("DI_speed"),
msg_sig_set(m, "DI_vehicleSpeed", v)) werden VOR dem Hochladen zu Bitpositionen
aufgeloest. Reihenfolge:
  1. --bep DATEI, sonst
  2. <skript>.bep neben dem Skript (vorgebaut, kein Node.js noetig), sonst
  3. node tools/dbc_prep/preprocess.ts mit --dbc (Default: dbc/Model3_VEH.dbc
     dbc/Model3_CH.dbc - dieselben Dateien, die die Web-UI fuer Tesla laedt).

Protokoll (firmware/src/protocol/protocol.c):
  Service 6e400001-..., RX (schreiben) 6e400002-..., TX (notify) 6e400003-...
  Frame = 4 Byte Laenge LE (oberes Nibble = Typ, 0 = JSON) + JSON.

WICHTIG: Vorher die Web-UI trennen. Die Firmware haelt nur eine Sitzung.

Beispiele (Windows, Python 3.14, pip install bleak):
  python upload_script.py model_y_sim.be --filters bench   # Filter + hochladen + starten
  python upload_script.py model_y_sim.be --log 300
  python upload_script.py --filters juniper                # nur Filter setzen (Auto)
  python upload_script.py --filters none                   # Filter aus (alles annehmen)
  python upload_script.py --show-filters
  python upload_script.py --stop model_y_sim.be            # deaktivieren (kein Autostart)
  python upload_script.py --delete model_y_sim.be
  python upload_script.py --list
"""
import argparse
import asyncio
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from bleak import BleakClient, BleakScanner

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # wir schreiben
TX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # Geraet notifiziert

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_DBC = [os.path.join(REPO, "dbc", "Model3_VEH.dbc"), os.path.join(REPO, "dbc", "Model3_CH.dbc")]


class Dorky:
    def __init__(self, client, fast=False, quiet_raw=False):
        self.c = client
        self.fast = fast
        self.quiet_raw = quiet_raw
        self.buf = bytearray()
        self.next_id = 1
        self.pending = {}
        self.t0 = time.time()
        self.log_lines = []

    def _on_notify(self, _sender, data: bytearray):
        if len(data) >= 2 and data[0] == 0xFD:          # Disconnect-Grund der Firmware
            print("!! Firmware meldet: Sitzung ersetzt (andere Verbindung)" if data[1] == 1
                  else f"!! Firmware-Notify 0xFD {data[1]:#x}")
            return
        self.buf += data
        while len(self.buf) >= 4:
            hdr = struct.unpack_from("<I", self.buf)[0]
            ln, typ = hdr & 0x0FFFFFFF, hdr >> 28
            if ln > 256 * 1024:                          # kaputter Stream -> neu synchronisieren
                self.buf.clear()
                return
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
            line = msg.get("msg") or msg.get("raw") or ""
            self.log_lines.append(line)
            if "msg" in msg:
                print(ts, f"{msg.get('src', '')}:", msg["msg"])
            elif "raw" in msg and not self.quiet_raw:
                print(ts, msg["raw"])
        elif t == "script_update":
            print(ts, "script_update:", msg.get("script"))
        # trace/bus_status/cpu ignorieren

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
            raise TimeoutError(f"Timeout bei {op} ({timeout:.0f} s)")
        if not resp.get("ok"):
            raise RuntimeError(f"{op} fehlgeschlagen: {resp.get('error')}")
        return resp.get("result")

    async def script_entry(self, fname):
        res = await self.call("script.list", timeout=15.0)
        for s in res.get("scripts", []):
            if s.get("filename") == fname:
                return s
        return None


# ---------------- DBC-Vorverarbeitung ----------------

def strip_comment_lines(code):
    """Reine Kommentarzeilen leeren (Zeilennummern bleiben gleich) - spart
    Platz im script.write-Frame; nur fuer die .bep, nie fuer den Quelltext."""
    return "\n".join("" if l.lstrip().startswith("#") else l for l in code.split("\n"))


UNRESOLVED = re.compile(r'can_msg_new\(\s*"|can_msg_get\([^,()]+,\s*"|msg_sig_(?:set|get)\([^,()]+,\s*"')


def check_bep(bep, origin):
    m = UNRESOLVED.search(strip_comment_lines(bep))
    if m:
        line = bep.count("\n", 0, m.start()) + 1
        sys.exit(f"{origin}: Zeile {line}: DBC-Name nicht aufgeloest - .bep veraltet oder falsche DBC?")


def build_runtime(script, args):
    code = open(script, encoding="utf-8").read()
    uses_dbc = any(c in code for c in ('can_msg_new("', 'msg_sig_set(', 'msg_sig_get(', 'can_msg_get('))
    if args.bep:
        bep, origin = open(args.bep, encoding="utf-8").read(), args.bep
    else:
        side = os.path.splitext(script)[0] + ".bep"
        node = shutil.which("node")
        pre = os.path.join(HERE, "dbc_prep", "preprocess.ts")
        if node and os.path.exists(pre) and all(os.path.exists(d) for d in args.dbc):
            with tempfile.TemporaryDirectory() as td:
                out = os.path.join(td, "out.bep")
                r = subprocess.run([node, pre, script, out, *args.dbc], capture_output=True, text=True)
                sys.stderr.write(r.stderr)
                if r.returncode != 0:
                    sys.exit("DBC-Vorverarbeitung fehlgeschlagen (siehe oben)")
                bep, origin = open(out, encoding="utf-8").read(), "preprocess.ts"
        elif os.path.exists(side):
            if os.path.getmtime(side) + 1 < os.path.getmtime(script):
                print(f"Warnung: {side} ist aelter als {script} - ggf. neu erzeugen")
            bep, origin = open(side, encoding="utf-8").read(), side
        elif uses_dbc:
            sys.exit("Skript nutzt DBC-Namen, aber weder .bep noch node + DBC gefunden.\n"
                     "Loesung: model_y_sim.bep neben das Skript legen oder Node.js installieren.")
        else:
            return code, None
    check_bep(bep, origin)
    print(f"Runtime (.bep) aus {origin}")
    return code, strip_comment_lines(bep)


def load_filters(profile, path):
    cfg = json.load(open(path, encoding="utf-8"))
    if profile not in cfg or profile.startswith("_"):
        sys.exit(f"Filterprofil '{profile}' nicht in {path} (vorhanden: "
                 f"{', '.join(k for k in cfg if not k.startswith('_'))})")
    out = {}
    for bus, ids in cfg[profile].items():
        out[int(bus)] = [int(x, 0) if isinstance(x, str) else int(x) for x in ids]
    return out


# ---------------- Ablauf ----------------

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


async def enable_robust(d, fname, timeout):
    """script.enable mit langem Timeout. Ein Timeout ist nicht fatal, wenn
    script.list (oder das Log) zeigt, dass das Skript laeuft."""
    try:
        await d.call("script.enable", timeout=timeout, filename=fname)
        return True
    except TimeoutError as e:
        print(f"{e} - pruefe Status ueber script.list ...")
    for attempt in range(3):
        try:
            s = await d.script_entry(fname)
            if s and s.get("enabled") and not s.get("errored"):
                print("Status: aktiviert (Antwort kam nur zu spaet)")
                return True
            if s and s.get("errored"):
                print("Status: FEHLER", s.get("error"))
                return False
        except (TimeoutError, RuntimeError) as e:
            print("script.list:", e)
        await asyncio.sleep(2.0)
    if any(("Enabled" in l or "enabled" in l) and fname in l for l in d.log_lines):
        print("Status laut Log: aktiviert")
        return True
    print("Status unklar - mit --list pruefen")
    return False


async def session(args, target, code, runtime, filters):
    async with BleakClient(target, timeout=30.0) as client:
        print("Verbunden, MTU", getattr(client, "mtu_size", "?"))
        d = Dorky(client, fast=args.fast, quiet_raw=args.no_raw)
        await d.start()
        await asyncio.sleep(0.5)   # Firmware autorisiert/kickt nach ~100 ms
        if not client.is_connected:
            sys.exit("Sofort getrennt: Pairing-Fenster zu? BOOT kurz druecken und neu versuchen.")
        info = await d.call("system.info", timeout=15.0)
        print("Firmware:", info.get("fw_version"), "| proto", info.get("proto_version"),
              "| scripting_api", info.get("scripting_api_version"))

        if filters is not None:
            for bus in sorted(filters):
                try:
                    r = await d.call("bus.set_filter", timeout=15.0, bus=bus, ids=filters[bus])
                    n = r.get("count", 0) if r else 0
                    print(f"Filter bus{bus}: {n} IDs" if n else f"Filter bus{bus}: aus (alles)")
                except RuntimeError as e:
                    if "unknown op" in str(e).lower():
                        print("Firmware kennt bus.set_filter nicht (altes Image) - Filter uebersprungen")
                        break
                    raise
        if args.show_filters or filters is not None:
            try:
                r = await d.call("bus.get_filter", timeout=10.0)
                for b in r.get("buses", []):
                    ids = " ".join(f"{i:03X}" for i in b.get("ids", [])) or "alles"
                    print(f"  bus{b['bus']}: {ids}")
            except RuntimeError as e:
                print("bus.get_filter:", e)

        fname = os.path.basename(args.script) if args.script else None
        if args.stop or args.delete:
            if not fname:
                sys.exit("Dateiname fehlt")
            await d.call("script.delete" if args.delete else "script.disable", timeout=20.0, filename=fname)
            print("geloescht" if args.delete else "deaktiviert", fname)
        elif fname:
            params = {"filename": fname, "code": code}
            if runtime is not None:
                params["runtime"] = runtime
            size = len(json.dumps(params).encode())
            print(f"Lade {fname} (Quelltext {len(code.encode())} B"
                  + (f", Runtime {len(runtime.encode())} B" if runtime is not None else "")
                  + f", Frame ~{size} B) ...")
            if size > 32 * 1024 - 64:
                print("Hinweis: Frame > 32 KB - braucht das neue Image (64-KB-Puffer in PSRAM)")
            await d.call("script.write", timeout=45.0, **params)
            print("geschrieben")
            if not args.no_start:
                if await enable_robust(d, fname, args.enable_timeout):
                    print("aktiviert (laeuft jetzt und startet nach jedem Reboot automatisch)")

        if args.list or fname:
            try:
                res = await d.call("script.list", timeout=15.0)
                for s in res.get("scripts", []):
                    flag = "AN " if s.get("enabled") else "aus"
                    err = f"  FEHLER: {s.get('error')}" if s.get("errored") else ""
                    print(f"  [{flag}] {s.get('filename')}{err}")
            except (TimeoutError, RuntimeError) as e:
                print("script.list:", e)

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


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", nargs="?", help="Pfad zur .be-Datei (Dateiname auf dem Geraet = Basisname)")
    ap.add_argument("--name", default="Dorky-21F2", help="BLE-Name (Default Dorky-21F2)")
    ap.add_argument("--address", help="BLE-Adresse statt Suche, z. B. 28:84:85:27:21:F2")
    ap.add_argument("--scan", type=float, default=15.0, help="Suchdauer s")
    ap.add_argument("--retries", type=int, default=3, help="Verbindungsversuche")
    ap.add_argument("--log", type=float, default=60.0, help="Log so viele Sekunden zeigen (0 = nicht)")
    ap.add_argument("--no-start", action="store_true", help="nur hochladen, nicht aktivieren")
    ap.add_argument("--enable-timeout", type=float, default=30.0, help="Timeout fuer script.enable (s)")
    ap.add_argument("--stop", action="store_true", help="Skript deaktivieren (Autostart aus)")
    ap.add_argument("--delete", action="store_true", help="Skript auf dem Geraet loeschen")
    ap.add_argument("--list", action="store_true", help="Skripte auf dem Geraet auflisten")
    ap.add_argument("--dbc", nargs="+", default=DEFAULT_DBC, help="DBC-Dateien in Bus-Reihenfolge")
    ap.add_argument("--bep", help="fertige .bep-Datei statt Vorverarbeitung")
    ap.add_argument("--filters", help="Filterprofil aus can_filters.json (bench, juniper, none)")
    ap.add_argument("--filters-file", default=os.path.join(HERE, "can_filters.json"))
    ap.add_argument("--show-filters", action="store_true", help="aktive RX-Filter anzeigen")
    ap.add_argument("--fast", action="store_true", help="Write without response (schneller)")
    ap.add_argument("--no-raw", action="store_true", help="nur Berry-print()-Zeilen, keine ESP-Logs")
    args = ap.parse_args()

    code = runtime = None
    if args.script and not (args.stop or args.delete):
        code, runtime = build_runtime(args.script, args)
        if not code.endswith("\n"):
            code += "\n"
    filters = load_filters(args.filters, args.filters_file) if args.filters else None

    target = await find(args)
    for attempt in range(1, args.retries + 1):
        try:
            await session(args, target, code, runtime, filters)
            break
        except (TimeoutError, asyncio.TimeoutError, OSError) as e:
            # bleak meldet GATT-Discovery-Timeouts u. ae. als TimeoutError/OSError
            print(f"Versuch {attempt}/{args.retries}: {type(e).__name__}: {e}")
            if attempt == args.retries:
                sys.exit("Aufgegeben. Board neu starten (ggf. mit BOOT = Safe Mode) und erneut versuchen.")
            await asyncio.sleep(3.0)
            if not args.address:
                target = await find(args)
    print("fertig, BLE getrennt")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
