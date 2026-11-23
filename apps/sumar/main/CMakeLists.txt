idf_component_register(
    SRCS "fusion.c" "fu_core.c" "fu_art.c" "fu_snd.c" "fu_ui.c" "fu_save.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
