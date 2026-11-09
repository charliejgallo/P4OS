idf_component_register(SRCS "main.c" "dmx.c" "rdm_resp.c" PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_driver_rmt esp_timer nvs_flash esp_hw_support)
