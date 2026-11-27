idf_component_register(
    SRCS "grabador.c" "gr_cfg.c" "gr_engine.c" "gr_motion.c" "gr_mp4.c" "gr_rtsp.c" "gr_ui.c" "gr_live.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
