# @name Model Y Simulator
# @description Sendet Model-3/Y-Fahrzeug-CAN-Frames (Fahrzyklus) auf einem Bus und prueft den Empfang auf dem anderen
# @bus 0
#
# Aufbau: CAN-A (Bus 0, MCP2518FD) H/L/SGND mit CAN-B (Bus 1, TWAI) H/L/SGND verbinden.
# Die 120-Ohm-Abschluesse RZ1/RZ2 sind fest verbaut -> zusammen ~60 Ohm.
# Bus 0 sendet, Bus 1 empfaengt und zaehlt. Doku: docs/simulator.md
#
# Quellen fuer IDs/Bits (alle Intel/little-endian):
#   VEH = dbc/Model3_VEH.dbc in diesem Repo (Tesla Model 3/Y Vehicle-Bus)
#   JW  = joshwardell/model3dbc Model3CAN.dbc (GenMsgCycleTime = Sendeperiode)
#   OP  = commaai/opendbc tesla_model3_party.dbc (gleiche Layouts 0x118/0x129/0x257/0x39D)
# Checksumme (Byte 0): (ID_lo + ID_hi + Summe Bytes 1..7) & 0xFF, wie
#   firmware/src/can/tesla.c bzw. opendbc tesla checksum.
#
# Nur Firmware-APIs: can_send_raw, can_recv_raw, timer_every, timer_cancel,
# action_register, millis, print, format, str, int, bytes.setbits.

# ---------------- Konfiguration ----------------
var mys_cfg = {
  'enabled':   true,    # false = Skript laedt, sendet aber nichts
  'tx_bus':    0,       # 0 = CAN-A (MCP2518FD), 1 = CAN-B (TWAI)
  'rx_bus':    1,       # Kontrollbus; -1 = keine Empfangspruefung
  'speed':     1.0,     # Zeitraffer des Fahrzyklus (2.0 = doppelt so schnell)
  'tick_ms':   20,      # Grundtakt; >= 20 wegen Firmware-Ratelimit (10 ms/ID)
  'report_s':  10,      # Abstand der RX-Zusammenfassung im Log
  'log_phase': true,    # Phasenwechsel ins Log schreiben
  'throttle':  true,    # ohne Empfang: nur noch 1 Frame/s je ID senden
}

# ID -> Periode in ms (Quelle). 10-ms-Frames laufen mit 20 ms (Ratelimit).
var mys_msgs = [
  [0x118, 20],    # DI_systemStatus: Gang, Bremse, Fahrpedal  (JW 10 ms)
  [0x129, 20],    # SCCM_steeringAngleSensor: Lenkwinkel        (JW 10 ms)
  [0x257, 20],    # DI_speed: Geschwindigkeit                   (JW 20 ms)
  [0x39D, 40],    # IBST_status: Bremspedal                     (geschaetzt)
  [0x292, 100],   # BMS_socStatus: Ladestand                    (JW 100 ms)
  [0x3F5, 100],   # VCFRONT_lighting: Blinker, Warnblinker      (geschaetzt)
  [0x102, 100],   # VCLEFT_doorStatus: Tueren links             (geschaetzt)
  [0x103, 100],   # VCRIGHT_doorStatus: Tueren rechts, Kofferraum (geschaetzt)
]

# IDs mit 4-Bit-Counter: hier ist rx/tx eine echte Frame-Zaehlung,
# die anderen werden nur auf "kommt an und Inhalt stimmt" geprueft.
var mys_ctr = {0x118: true, 0x129: true, 0x257: true, 0x39D: true}

# Fahrzyklus: [Name, Dauer s]
var mys_phases = [
  ['park',    8],   # P, Fahrertuer 3-6 s offen
  ['ready',   2],   # Bremse treten, P -> D
  ['accel',  10],   # 0 -> 100 km/h, Blinker links 0-3 s
  ['cruise', 15],   # 100 km/h, Spurwechsel rechts 5-8 s
  ['brake',   8],   # 100 -> 0 km/h
  ['reverse', 6],   # R, Einparken mit Lenkeinschlag
  ['park2',   6],   # P, Kofferraum 1-5 s offen
]

class MYSim
  var cfg, tx, rx, ticks, t, phase, pt, cnt, txc, txlast, rxlast, rxupd, rxseen, rxok
  var last_rx_ms, throttled, h_tick, h_rep, soc, v, gear, pedal, brake, acc
  var steer, steer_rate, blink_l, blink_r, door_fl, door_fr, door_rl, door_rr, trunk

  def init(cfg)
    self.cfg = cfg
    self.tx = cfg['tx_bus']
    self.rx = cfg['rx_bus']
    self.ticks = 0
    self.t = 0.0
    self.phase = 0
    self.pt = 0.0
    self.cnt = {}
    self.txc = {}
    self.txlast = {}
    self.rxlast = {}
    self.rxupd = {}
    self.rxseen = {}
    self.rxok = {}
    self.last_rx_ms = millis()
    self.throttled = false
    self.soc = 80.0
    for m: mys_msgs
      self.cnt[m[0]] = 0
      self.txc[m[0]] = 0
      self.rxupd[m[0]] = 0
      self.rxseen[m[0]] = false
      self.rxok[m[0]] = 0
    end
    self.drive_state()
  end

  # ---------- Fahrzustand aus Phase + Phasenzeit ----------
  def drive_state()
    var name = mys_phases[self.phase][0]
    var p = self.pt
    self.v = 0.0  self.gear = 1  self.pedal = 0.0  self.brake = false
    self.acc = 0.0  self.steer = 0.0  self.steer_rate = 0.0
    self.blink_l = false  self.blink_r = false
    self.door_fl = false  self.door_fr = false  self.door_rl = false  self.door_rr = false
    self.trunk = false
    if name == 'park'
      self.door_fl = (p >= 3 && p < 6)
    elif name == 'ready'
      self.brake = true
      self.gear = p < 1 ? 1 : 4
    elif name == 'accel'
      self.gear = 4  self.v = p * 10.0  self.pedal = 40.0  self.acc = 2.78
      self.blink_l = p < 3
      if p < 3 self.steer = -5.0 * p end
    elif name == 'cruise'
      self.gear = 4  self.v = 100.0  self.pedal = 15.0
      self.blink_r = (p >= 5 && p < 8)
      if p >= 5 && p < 8 self.steer = 3.0 end
    elif name == 'brake'
      self.gear = 4  self.v = 100.0 - p * 12.5  self.brake = true  self.acc = -3.47
      if self.v < 0 self.v = 0.0 end
    elif name == 'reverse'
      self.gear = 2
      if p < 1 self.brake = true
      elif p < 3 self.v = (p - 1) * 2.5  self.pedal = 8.0  self.steer = (p - 1) * 200.0  self.steer_rate = 200.0
      elif p < 5 self.v = 5.0 - (p - 3) * 2.5  self.pedal = 4.0  self.steer = 400.0 - (p - 3) * 200.0  self.steer_rate = -200.0
      else self.brake = true
      end
    elif name == 'park2'
      self.trunk = (p >= 1 && p < 5)
    end
  end

  # ---------- Frame-Hilfen ----------
  def finish(id, b)
    var c = self.cnt[id]
    b.setbits(8, 4, c)                         # Counter Bit 8|4 (VEH/OP)
    self.cnt[id] = (c + 1) % 16
    var s = (id & 0xFF) + ((id >> 8) & 0xFF)
    for i: 1..7 s += b[i] end
    b[0] = s & 0xFF                             # Checksumme Byte 0
    return b
  end

  def blink_phase()   # Blinker-Takt ~1.5 Hz: 2 = ACTIVE_HIGH, 1 = ACTIVE_LOW (VEH VAL_)
    return ((millis() / 333) % 2) == 0 ? 2 : 1
  end

  def build(id)
    var b = bytes(-8)
    if id == 0x118
      # VEH/OP DI_systemStatus: immobilizer 13|3=3 DISARMED, systemState 16|3 (2 STANDBY, 5 ENABLE),
      # brakePedalState 19|2, gear 21|3 (1 P,2 R,3 N,4 D), hvil 27|2=2 CLOSED,
      # accelPedalPos 32|8 x0.4 %, vehicleAcceleration 52|12 signed x0.01 m/s2
      b.setbits(13, 3, 3)
      b.setbits(16, 3, self.gear == 1 ? 2 : 5)
      b.setbits(19, 2, self.brake ? 1 : 0)
      b.setbits(21, 3, self.gear)
      b.setbits(27, 2, 2)
      b.setbits(32, 8, int(self.pedal * 2.5))
      b.setbits(52, 12, int(self.acc * 100) & 0xFFF)
      return self.finish(id, b)
    elif id == 0x129
      # VEH/OP SCCM_steeringAngleSensor: status 14|2=0 OK, angle 16|14 (0.1,-819.2) deg,
      # validity 30|2=1 VALID, angleSpeed 32|14 (0.5,-4096) deg/s.
      # Byte 0 heisst hier "Crc" - Algorithmus unbestaetigt, wir nutzen die Summen-Checksumme.
      b.setbits(16, 14, int((self.steer + 819.2) * 10))
      b.setbits(30, 2, 1)
      b.setbits(32, 14, int((self.steer_rate + 4096) * 2))
      return self.finish(id, b)
    elif id == 0x257
      # VEH DI_speed: vehicleSpeed 43|13 (0.08,-40) km/h, uiSpeed 24|9 km/h, units 33|1=1 KPH,
      # accelPedalPressed 34|1, velocityEstimatorState 56|3=1.
      # Bit 12|12 (0.08,-40): VEH "opdVehicleModelSpeedRef", in JW/aelteren DBCs "DI_vehicleSpeed" -> gleicher Wert.
      var raw = int((self.v + 40) * 12.5)
      b.setbits(12, 12, raw)
      b.setbits(24, 9, int(self.v + 0.5))
      b.setbits(33, 1, 1)
      b.setbits(34, 1, self.pedal > 0 ? 1 : 0)
      b.setbits(43, 13, raw)
      b.setbits(56, 3, 1)
      return self.finish(id, b)
    elif id == 0x39D
      # VEH/OP IBST_status: iBoosterStatus 12|3 (5 READY, 6 ACTUATION), driverBrakeApply 16|2
      # (1 NOT_APPLIED, 2 APPLYING), sInputRodDriver 21|12 (0.015625,-5) mm
      b.setbits(12, 3, self.brake ? 6 : 5)
      b.setbits(16, 2, self.brake ? 2 : 1)
      b.setbits(21, 12, int(((self.brake ? 12.0 : 0.0) + 5) * 64))
      return self.finish(id, b)
    elif id == 0x292
      # VEH BMS_socStatus: socMin 0|10, socUI 10|10, socMax 20|10, socAvg 30|10 (x0.1 %),
      # enoughEnergyForConvenienceFeatures 40|1. Kein Counter/Checksumme.
      var s = int(self.soc * 10)
      b.setbits(0, 10, s - 5)
      b.setbits(10, 10, s)
      b.setbits(20, 10, s + 5)
      b.setbits(30, 10, s)
      b.setbits(40, 1, 1)
      return b
    elif id == 0x3F5
      # VEH VCFRONT_lighting: indicatorLeftRequest 0|2, indicatorRightRequest 2|2,
      # hazardLightRequest 4|4=0, lightingCoreState 32|4 (2 DRL), turnIndicatorStalk 40|2,
      # indicatorLeft/RightInternal 50|2 / 52|2. Kein Counter.
      var ph = self.blink_phase()
      var l = self.blink_l ? ph : 0
      var r = self.blink_r ? ph : 0
      b.setbits(0, 2, l)
      b.setbits(2, 2, r)
      b.setbits(32, 4, self.gear == 1 ? 1 : 2)
      b.setbits(40, 2, self.blink_l ? 1 : (self.blink_r ? 2 : 0))
      b.setbits(50, 2, l)
      b.setbits(52, 2, r)
      return b
    elif id == 0x102 || id == 0x103
      # VEH VCLEFT/VCRIGHT_doorStatus: doorClosureStatusFront 0|4, Rear 4|4 (1 OPENED, 2 CLOSED),
      # frontLatchStatus 14|4, rearLatchStatus 18|4 (1 OPENED, 2 CLOSED),
      # 0x103 zusaetzlich trunkLatchStatus 56|4.
      var f = id == 0x102 ? self.door_fl : self.door_fr
      var r = id == 0x102 ? self.door_rl : self.door_rr
      b.setbits(0, 4, f ? 1 : 2)
      b.setbits(4, 4, r ? 1 : 2)
      b.setbits(14, 4, f ? 1 : 2)
      b.setbits(18, 4, r ? 1 : 2)
      if id == 0x103 b.setbits(56, 4, self.trunk ? 1 : 2) end
      return b
    end
    return nil
  end

  # ---------- Empfangspruefung ----------
  def rx_poll()
    if self.rx < 0 return end
    var now = millis()
    for m: mys_msgs
      var id = m[0]
      var d = can_recv_raw(self.rx, id, 0)
      if d != nil
        # Der Cache haelt immer den letzten Frame: "neu" erkennt man nur an
        # geaenderten Daten. Frames mit Counter aendern sich bei jedem Senden.
        var prev = self.rxlast.find(id)
        var t = self.txlast.find(id)
        var alive = now - self.last_rx_ms < 500
        if prev == nil || !(prev == d)
          self.rxupd[id] += 1
          self.rxlast[id] = d
          self.rxseen[id] = true
          if mys_ctr.find(id) != nil self.last_rx_ms = now end
        elif alive && t != nil && t == d
          self.rxseen[id] = true
        end
        if t != nil && t == d self.rxok[id] += 1 end
      end
    end
  end

  def report()
    var tot_tx = 0  var tot_rx = 0  var miss = []  var bad = []  var lossy = []
    var line = ''
    for m: mys_msgs
      var id = m[0]
      tot_tx += self.txc[id]
      tot_rx += self.rxupd[id]
      if mys_ctr.find(id) != nil && self.txc[id] > 0 && self.rxupd[id] * 10 < self.txc[id] * 9
        lossy.push(format('%03X', id))
      end
      if !self.rxseen[id] miss.push(format('%03X', id))
      elif self.rxok[id] == 0 bad.push(format('%03X', id))
      end
      if mys_ctr.find(id) != nil
        line += format(' %03X:%d/%d', id, self.rxupd[id], self.txc[id])
      else
        line += format(' %03X:%s', id, self.rxseen[id] ? 'ok' : '--')
      end
    end
    var ph = mys_phases[self.phase][0]
    print(format('SIM %s v=%.0f gear=%s soc=%.1f | tx=%d rx=%d', ph, self.v,
          'xPRND'[self.gear], self.soc, tot_tx, tot_rx))
    if self.rx >= 0
      print('SIM rx/tx (Counter-IDs) bzw. ok' .. line)
      if size(lossy) > 0 && size(miss) == 0
        print('SIM Verluste >10 %: ' .. lossy.concat(' ') .. ' (Fehlerframes? Abschluss pruefen)')
      end
      if size(bad) > 0
        print('SIM Daten weichen ab: ' .. bad.concat(' '))
      end
      if size(miss) > 0
        print('SIM FEHLT auf bus' .. str(self.rx) .. ': ' .. miss.concat(' ') ..
              ' -> Verdrahtung H/L, 60 Ohm, Bitrate pruefen')
      else
        print('SIM alle ' .. str(size(mys_msgs)) .. ' IDs empfangen' ..
              (self.throttled ? ' (gedrosselt)' : ''))
      end
    end
    for m: mys_msgs self.txc[m[0]] = 0  self.rxupd[m[0]] = 0  self.rxseen[m[0]] = false  self.rxok[m[0]] = 0 end
  end

  # ---------- Takt ----------
  def tick()
    var dt = self.cfg['tick_ms'] / 1000.0 * self.cfg['speed']
    self.rx_poll()
    # Fahrzyklus weiterschalten
    self.pt += dt
    if self.pt >= mys_phases[self.phase][1]
      self.pt = 0.0
      self.phase = (self.phase + 1) % size(mys_phases)
      if self.cfg['log_phase'] print('SIM Phase: ' .. mys_phases[self.phase][0]) end
    end
    self.drive_state()
    # SoC sinkt ~0,05 %/s bei 100 km/h (bewusst schnell, damit man es sieht)
    if self.v > 0 self.soc -= dt * 0.05 * self.v / 100.0 end
    if self.soc < 10 self.soc = 80.0 end
    # Drosseln, wenn auf dem RX-Bus nichts ankommt (vermeidet volle TX-Queue/50-ms-Blockaden)
    if self.cfg['throttle'] && self.rx >= 0
      var silent = millis() - self.last_rx_ms > 3000
      if silent != self.throttled
        self.throttled = silent
        print(silent ? 'SIM kein Empfang auf bus' .. str(self.rx) .. ' seit 3 s -> drossele auf 1 Hz'
                     : 'SIM Empfang wieder da -> volle Rate')
      end
    end
    self.ticks += 1
    var ms = self.ticks * self.cfg['tick_ms']
    for m: mys_msgs
      var per = self.throttled ? 1000 : m[1]
      if ms % per == 0
        var b = self.build(m[0])
        if b != nil
          can_send_raw(self.tx, m[0], b)
          self.txc[m[0]] += 1
          self.txlast[m[0]] = b
        end
      end
    end
  end

  def start()
    if self.rx >= 0
      if self.rx == self.tx print('SIM WARNUNG: rx_bus == tx_bus, eigene Frames kommen nicht zurueck') end
      for m: mys_msgs can_recv_raw(self.rx, m[0], 0) end   # Cache-Slots registrieren
    end
    var me = self
    self.h_tick = timer_every(self.cfg['tick_ms'], def () me.tick() end)
    self.h_rep = timer_every(self.cfg['report_s'] * 1000, def () me.report() end)
    print(format('SIM start: tx bus%d -> rx bus%d, %d IDs, Zeitraffer %.1fx',
          self.tx, self.rx, size(mys_msgs), self.cfg['speed']))
  end

  def stop()
    if self.h_tick != nil timer_cancel(self.h_tick)  self.h_tick = nil end
    if self.h_rep != nil timer_cancel(self.h_rep)  self.h_rep = nil end
    print('SIM gestoppt')
  end
end

def setup()
  var sim = MYSim(mys_cfg)
  action_register('Sim Start/Stop', def ()
    if sim.h_tick == nil sim.start() else sim.stop() end
  end)
  action_register('Sim Report', def () sim.report() end)
  if mys_cfg['enabled'] sim.start() else print('SIM geladen, deaktiviert (enabled=false)') end
end

def teardown()
  print('SIM beendet')
end
