# The header, its ports and modules (aos_io.c), the serial capture service
# (aos_serial.c), the sensor service (aos_sensors.c, its drivers in sensors/), modules.txt as a text
# (aos_modules.c), PWM/analog/IR/CAN (aos_io_signal.c), pulses and counts
# (aos_io_pulse.c) and the ESP-IDF backends (aos_io_p4.c,
# aos_io_signal_p4.c, aos_io_pulse_p4.c). The simulator compiles all but the
# backends with its own, sim/io_sim.c, sim/io_signal_sim.c and
# sim/io_pulse_sim.c. Ethernet on a W5500 of the header is aos_eth_p4.c
# (sim/eth_sim.c there).
# the sensor drivers, one file per family (the glob as aos_ui does it: no
# CONFIGURE_DEPENDS in ESP-IDF's script pass)
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB SENS_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/sensors/*.c)
else()
    file(GLOB SENS_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sensors/*.c)
endif()
idf_component_register(
    SRCS "aos_io.c" "aos_io_wire.c" "aos_leds.c" "aos_serial.c" "aos_sensors.c" "aos_modules.c" "aos_io_signal.c" "aos_io_pulse.c" "aos_io_dmx.c" "aos_pca9685.c" "aos_io_p4.c" "aos_io_signal_p4.c" "aos_io_pulse_p4.c" "aos_io_dmx_p4.c" "aos_eth_p4.c"
    ${SENS_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES aos_hal
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_rmt esp_driver_ledc esp_driver_sdm esp_driver_twai esp_driver_pcnt esp_timer esp_eth esp_netif esp_event waveshare__esp32_p4_wifi6_touch_lcd_5
)
