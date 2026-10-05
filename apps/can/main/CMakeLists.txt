idf_component_register(
    SRCS "can.c" "can_bus.c" "can_frame.c" "can_spy.c" "can_send.c" "can_sheets.c" "can_sig.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
