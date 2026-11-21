idf_component_register(
    SRCS "sudoku.c" "sd_core.c" "sd_gen.c" "sd_game.c" "sd_board.c" "sd_ui.c" "sd_theme.c" "sd_save.c"
         "sd_snd.c" "sd_pak.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
