idf_component_register(
    SRCS "pwm.c" "pw_draw.c" "pw_engine.c" "pw_knob.c" "pw_sheets.c" "pw_store.c" "pw_ui.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
