# The header, its ports and modules (aos_io.c), the serial capture service
# (aos_serial.c), the I2C sensors (aos_sensors.c), modules.txt as a text
# (aos_modules.c), PWM/analog/IR/CAN (aos_io_signal.c) and the ESP-IDF
# backends (aos_io_p4.c, aos_io_signal_p4.c). The simulator compiles all but
# the backends with its own, sim/io_sim.c and sim/io_signal_sim.c.
idf_component_register(
    SRCS "aos_io.c" "aos_io_wire.c" "aos_leds.c" "aos_serial.c" "aos_sensors.c" "aos_modules.c" "aos_io_signal.c" "aos_io_p4.c" "aos_io_signal_p4.c"
    INCLUDE_DIRS "include"
    REQUIRES aos_hal
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_rmt esp_driver_ledc esp_driver_sdm esp_driver_twai esp_timer waveshare__esp32_p4_wifi6_touch_lcd_5
)
