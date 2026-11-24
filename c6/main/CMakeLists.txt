idf_component_register(
    SRCS         "main.c" "radio154.c" "zigbee_test.c" "slots.c"
    # aos_154_proto.h: what the P4 and the C6 say about the 802.15.4 radio
    INCLUDE_DIRS "." "../../components/aos_154/include"
    REQUIRES     nvs_flash esp_hosted ieee802154 esp_timer espressif__esp-zigbee-lib
                 spi_flash app_update esp_partition
)

# The repository's partition table (../partitions_eh_cp_ota_4m.csv) as the
# bytes the bootloader reads, built by the IDF's own gen_esp32part.py and
# carried in the image: slots.c writes it in the factory table's place.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    idf_build_get_property(idf_path IDF_PATH)
    set(csv ${CMAKE_CURRENT_LIST_DIR}/../partitions_eh_cp_ota_4m.csv)
    set(table ${CMAKE_CURRENT_BINARY_DIR}/p4os_table.bin)
    add_custom_command(OUTPUT ${table}
        COMMAND ${python} ${idf_path}/components/partition_table/gen_esp32part.py
                --flash-size 4MB --offset 0x8000 -q ${csv} ${table}
        DEPENDS ${csv}
        COMMENT "The C6's partition table, for slots.c")
    add_custom_target(p4os_table DEPENDS ${table})
    target_add_binary_data(${COMPONENT_LIB} ${table} BINARY DEPENDS p4os_table)
endif()
