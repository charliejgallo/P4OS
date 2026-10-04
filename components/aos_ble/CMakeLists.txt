# The BLE stack (NimBLE's host on the P4, the C6's controller through
# esp_hosted) and the HAL's Bluetooth functions on top of it (aos_bt_p4.c).
# WHOLE_ARCHIVE: those functions replace the weak stubs in aos_hal, and only
# a whole archive gets them linked in.
idf_component_register(SRCS "aos_ble.c" "aos_ancs.c" "aos_ams.c" "aos_bt_p4.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt nvs_flash esp_timer aos_hal espressif__esp_hosted
                       WHOLE_ARCHIVE)
