idf_component_register(
    SRCS "solitario.c" "so_game.c" "so_solve.c" "so_seeds.c" "so_art.c" "so_court.c"
         "so_es.c" "so_view.c" "so_win.c" "so_ui.c" "so_data.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
