#!/bin/sh
# Baut den Host-Lauf gegen die Firmware-Quellen. FW=Pfad zu firmware/
set -e
H=$(cd "$(dirname "$0")" && pwd)
FW=${FW:-$H/../../firmware}
B=$FW/components/berry
gcc -O1 -w -DSCRIPT_DIR_TEST -I"$H/stubs" -I"$FW/src" -I"$B/src" -I"$B/port" -I"$B/generate" \
  "$H/run_sim.c" "$FW/src/scripting/berry_bindings.c" "$FW/src/scripting/berry_registry.c" \
  "$FW/src/scripting/script_loader.c" "$FW/src/can/can.c" "$FW/src/can/rate_limit.c" \
  "$FW/src/can/msg_codec.c" "$FW/src/can/tesla.c" $B/src/*.c "$B/port/be_modtab.c" "$B/port/be_port.c" \
  -lm -o "$H/run_sim"
# Reiner Syntax-Check fuer .be-Dateien: ./parse datei.be
gcc -O1 -w -I"$H/stubs" -I"$B/src" -I"$B/port" -I"$B/generate" "$H/parse.c" $B/src/*.c \
  "$B/port/be_modtab.c" "$B/port/be_port.c" -lm -o "$H/parse"
echo built "$H/run_sim" "$H/parse"
