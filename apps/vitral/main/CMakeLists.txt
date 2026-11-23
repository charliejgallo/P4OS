idf_component_register(
    SRCS "vitral.c" "vt_game.c" "vt_gfx.c" "vt_view.c" "vt_input.c" "vt_menu.c" "vt_snd.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
