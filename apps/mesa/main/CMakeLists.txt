idf_component_register(
    SRCS "mesa.c" "jm_games.c" "jm_art.c" "jm_view.c" "jm_play.c" "jm_hud.c" "jm_setup.c" "jm_audio.c"
         "jm_save.c" "jm_gfx.c" "jm_pak.c"
         "g_ajedrez.c" "g_ajrules.c" "g_ajsearch.c" "g_damas.c" "g_reversi.c" "g_cuatro.c" "g_ludo.c"
         "g_backgammon.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the engines and the pixel loops is a #pragma in those files: the
# .so is compiled by elf_loader.cmake with -Os, and per-file options do not
# reach it.
