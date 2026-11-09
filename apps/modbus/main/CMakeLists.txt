idf_component_register(
    SRCS "modbus.c" "mb_bus.c" "mb_proto.c" "mb_tabs.c" "mb_prof.c" "mb_ui_prof.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
