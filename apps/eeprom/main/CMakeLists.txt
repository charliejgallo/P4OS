idf_component_register(
    SRCS "eeprom.c" "ee_chips.c" "ee_drv.c" "ee_hash.c" "ee_portal.c" "ee_store.c" "ee_ui.c"
         "ee_pg_chip.c" "ee_pg_mem.c" "ee_pg_ver.c" "ee_pg_wire.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
