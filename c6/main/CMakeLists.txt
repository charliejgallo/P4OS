idf_component_register(
    SRCS         "main.c" "radio154.c" "zigbee_test.c"
    # aos_154_proto.h: what the P4 and the C6 say about the 802.15.4 radio
    INCLUDE_DIRS "." "../../components/aos_154/include"
    REQUIRES     nvs_flash esp_hosted ieee802154 esp_timer espressif__esp-zigbee-lib
)
