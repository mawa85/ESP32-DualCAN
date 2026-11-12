# Vendored Pierre Molinaro ACAN2517FD (MIT). ESP-IDF does not ship Arduino,
# so port/ supplies the SPI / GPIO / millis surface the library calls.
# ARDUINO_ARCH_ESP32 is private: it selects the library's FreeRTOS ISR task
# and must match the define in firmware/src/can/mcp2518fd_port.cpp.

idf_component_register(
    SRCS
        src/ACAN2517FD.cpp
        src/ACAN2517FDSettings.cpp
        port/idf_shim.cpp
    INCLUDE_DIRS src port
    REQUIRES driver esp_driver_gpio esp_driver_spi esp_timer freertos
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ARDUINO_ARCH_ESP32=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -include stdint.h -include stddef.h)
