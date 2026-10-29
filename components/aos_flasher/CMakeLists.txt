# The programmer service: its core (aos_flasher.c), the AVR Arduinos through
# their bootloaders (aos_flasher_avr.c), Cortex-M chips by SWD
# (aos_flasher_swd.c, on aos_io's SWD wire), STM32 by its serial bootloader
# (aos_flasher_stm32.c), the bridge that lends the port over TCP
# (aos_flasher_bridge.c, BSD sockets: lwIP here), the ESP32 family on top
# of Espressif's esp-serial-flasher (aos_flasher_esp.c), which comes from the
# component registry (idf_component.yml), the firmware found on the card
# (aos_flasher_src.c) and firmware files as regions (aos_flasher_img.c). HAL-clean: only aos_hal, aos_io, the library and
# aos_ui's aos_tr() (its messages are formatted here and shown by the app, so
# they are translated here), so the simulator compiles the same files
# (sim/CMakeLists.txt builds the library from managed_components/).
idf_component_register(
    SRCS "aos_flasher.c" "aos_flasher_avr.c" "aos_flasher_swd.c" "aos_flasher_stm32.c" "aos_flasher_bridge.c" "aos_flasher_esp.c" "aos_flasher_src.c" "aos_flasher_img.c"
    INCLUDE_DIRS "include"
    REQUIRES aos_hal aos_io
    PRIV_REQUIRES espressif__esp-serial-flasher aos_ui lwip
)
