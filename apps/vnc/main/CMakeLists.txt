idf_component_register(
    SRCS "vnc.c" "vnc_cfg.c" "vnc_rfb.c" "vnc_fb.c" "vnc_kbd.c" "vnc_des.c" "tinfl.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
