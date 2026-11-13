"""Dekodiert die Host-Dump-Frames (SIM_DUMP) mit dbc/Model3_VEH.dbc und
druckt pro Sekunde die simulierten Werte. pip install cantools"""
import sys, cantools
db = cantools.database.load_file(sys.argv[1], strict=False)
want = {0x118: ['DI_gear', 'DI_brakePedalState', 'DI_accelPedalPos', 'DI_vehicleAcceleration', 'DI_systemState'],
        0x257: ['DI_vehicleSpeed', 'DI_uiSpeed', 'DI_opdVehicleModelSpeedRef'],
        0x129: ['SCCM_steeringAngle'], 0x39D: ['IBST_driverBrakeApply'],
        0x292: ['BMS_socUI'], 0x3F5: ['VCFRONT_indicatorLeftRequest', 'VCFRONT_indicatorRightRequest'],
        0x102: ['VCLEFT_doorClosureStatusFront'], 0x103: ['VCRIGHT_trunkLatchStatus']}
last, sec = {}, -1
for line in open(sys.argv[2]):
    ms, i, h = line.split(); ms = int(ms); i = int(i, 16)
    if i in want:
        d = db.decode_message(i, bytes.fromhex(h), decode_choices=True)
        for k in want[i]: last[k] = d[k]
    if ms // 1000 != sec and ms % 1000 >= 900:
        sec = ms // 1000
        print(f"{sec:3d}s", ' '.join(f"{k.split('_',1)[1]}={v if not isinstance(v,float) else round(v,1)}" for k, v in last.items()))
