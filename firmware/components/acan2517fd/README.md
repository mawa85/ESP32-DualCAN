# ACAN2517FD (vendored)

Upstream: [pierremolinaro/acan2517FD](https://github.com/pierremolinaro/acan2517FD) at `74e5b29fdbba16caeddac1d2535f49a24bb8b42c`.

License: MIT (`LICENSE` in this directory). Copyright (c) 2018 Pierre Molinaro. This component is not relicensed under the repository's CERN-OHL-W.

`src/` is the upstream library. Two `portYIELD_FROM_ISR()` calls in `ACAN2517FD.cpp` pass the woken-task flag, because ESP-IDF does not provide Arduino-ESP32's no-argument macro.

`port/` is the ESP-IDF shim (SPI master, GPIO, `millis`). The chip-select pin is still driven by the library. Defining `ARDUINO_ARCH_ESP32` privately selects the library's FreeRTOS handler task, which is the path Battery-Emulator uses on the LilyGo T-2CAN FD.

The firmware wrapper is `firmware/src/can/mcp2518fd_port.cpp`. It requests **Normal CAN 2.0B** (`Normal20B`) and programs the data phase at 1× the arbitration rate, so classic frames are what the rest of the firmware sees and a later FD mode change is not blocked on bit timing.
